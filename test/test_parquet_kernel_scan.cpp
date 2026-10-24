#include <gtest/gtest.h>

#include <qdb/exec/runtime_context.h>
#include <qdb/io/parquet/kernel_scan.h>
#include <qdb/io/parquet/row_id.h>
#include <qdb/io/parquet/source.h>
#include <qdb/kernel/compiler.h>
#include <qdb/kernel/finalize.h>
#include <qdb/kernel/generated.h>

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/writer.h>
#include <parquet/properties.h>

#include <qumir/codegen/llvm/llvm_initializer.h>

#include <cstring>
#include <filesystem>
#include <numeric>
#include <random>
#include <unordered_set>
#include <string>
#include <vector>

using namespace NQdb;

namespace {

std::shared_ptr<TKernelSlot> Decoder() {
    static std::shared_ptr<TKernelSlot> slot = [] {
        std::vector<TGeneratedKernel> kernels;
        TKernelCompiler compiler(TKernelCompilerOptions{
            .Sink = &kernels,
            .BindNow = false,
            .RuntimeContext = std::make_shared<TRuntimeContext>(),
        });
        auto decoder = compiler.CompileParquetDecoder();
        JitFinalizeKernels(kernels);
        return decoder;
    }();
    return slot;
}

std::string TempPath(const std::string& name) {
    return (std::filesystem::temp_directory_path() / ("qdb_kernel_scan_" + name + ".parquet"))
        .string();
}

// Every type and shape the kernel decodes: required and optional columns,
// low-cardinality strings that stay dictionary encoded, long strings, and a
// unique column whose dictionary overflows into PLAIN pages.
std::shared_ptr<arrow::Table> MakeTable(int64_t rows, uint32_t seed) {
    std::mt19937 random(seed);
    arrow::Int32Builder i32;
    arrow::Int32Builder i32n;
    arrow::Date32Builder date;
    arrow::UInt32Builder u32;
    arrow::Int64Builder i64;
    arrow::Int64Builder i64n;
    arrow::UInt64Builder u64;
    arrow::DoubleBuilder f64;
    arrow::DoubleBuilder f64n;
    arrow::BooleanBuilder flag;
    arrow::BooleanBuilder flagn;
    arrow::StringBuilder mode;
    arrow::StringBuilder moden;
    arrow::StringBuilder text;
    arrow::StringBuilder unique;
    // Dictionary values up to 16 bytes take the kernel's single-copy path.
    const std::vector<std::string> shortModes = {"AIR", "MAIL", "SHIP", "TRUCK", "", "1-URGENT"};
    const std::vector<std::string> modes = {"AIR", "MAIL", "", "DELIVER IN PERSON",
        "TAKE BACK RETURN AND A LONG TAIL OF WORDS"};
    auto check = [](const arrow::Status& status) {
        if (!status.ok()) {
            throw std::runtime_error(status.ToString());
        }
    };
    for (int64_t row = 0; row < rows; ++row) {
        const bool null = random() % 5 == 0;
        check(i32.Append(static_cast<int32_t>(random())));
        check(null ? i32n.AppendNull() : i32n.Append(static_cast<int32_t>(random() % 100)));
        check(row % 3 == 0 ? date.AppendNull() : date.Append(static_cast<int32_t>(8000 + random() % 2000)));
        check(u32.Append(static_cast<uint32_t>(random())));
        check(i64.Append((static_cast<int64_t>(random()) << 32) ^ random()));
        check(null ? i64n.AppendNull() : i64n.Append(static_cast<int64_t>(random() % 1000) - 500));
        check(u64.Append((static_cast<uint64_t>(random()) << 32) | random()));
        check(f64.Append(static_cast<double>(random() % 10000) / 100.0));
        check(row % 7 == 0 ? f64n.AppendNull() : f64n.Append(static_cast<double>(random()) / 7.0));
        check(flag.Append(random() % 2 == 0));
        check(null ? flagn.AppendNull() : flagn.Append(random() % 3 == 0));
        check(mode.Append(modes[random() % modes.size()]));
        check(row % 4 == 1 ? moden.AppendNull() : moden.Append(shortModes[random() % shortModes.size()]));
        std::string value(random() % 70, 'a');
        for (auto& c : value) {
            c = static_cast<char>('a' + random() % 26);
        }
        check(text.Append(value));
        check(unique.Append("value-" + std::to_string(row) + "-" + std::to_string(random())));
    }
    std::vector<std::shared_ptr<arrow::Field>> fields;
    std::vector<std::shared_ptr<arrow::Array>> arrays;
    auto add = [&](const std::string& name, arrow::ArrayBuilder& builder, bool nullable) {
        std::shared_ptr<arrow::Array> array;
        check(builder.Finish(&array));
        fields.push_back(arrow::field(name, array->type(), nullable));
        arrays.push_back(array);
    };
    add("i32", i32, false);
    add("i32n", i32n, true);
    add("date", date, true);
    add("u32", u32, false);
    add("i64", i64, false);
    add("i64n", i64n, true);
    add("u64", u64, false);
    add("f64", f64, false);
    add("f64n", f64n, true);
    add("flag", flag, false);
    add("flagn", flagn, true);
    add("mode", mode, false);
    add("moden", moden, true);
    add("text", text, false);
    add("unique", unique, false);
    return arrow::Table::Make(arrow::schema(fields), arrays);
}

struct TWriteOptions {
    parquet::ParquetDataPageVersion PageVersion = parquet::ParquetDataPageVersion::V1;
    arrow::Compression::type Codec = arrow::Compression::ZSTD;
    bool Dictionary = true;
    int64_t RowGroupRows = 1000;
    int64_t PageBytes = 1024;
    int64_t DictionaryPageBytes = 4096;
};

void Write(const std::string& path, const arrow::Table& table, const TWriteOptions& options) {
    parquet::WriterProperties::Builder properties;
    properties.data_page_version(options.PageVersion)
        ->compression(options.Codec)
        ->data_pagesize(options.PageBytes)
        ->dictionary_pagesize_limit(options.DictionaryPageBytes)
        ->disable_write_page_index();
    if (!options.Dictionary) {
        properties.disable_dictionary();
    }
    auto output = arrow::io::FileOutputStream::Open(path).ValueOrDie();
    auto status = parquet::arrow::WriteTable(table, arrow::default_memory_pool(), output,
        options.RowGroupRows, properties.build());
    ASSERT_TRUE(status.ok()) << status.ToString();
    ASSERT_TRUE(output->Close().ok());
}

bool Bit(const uint8_t* bitmap, int64_t index) {
    return (bitmap[index >> 3] >> (index & 7)) & 1;
}

// Checks one batch column against rows [first, first + rows) of the array.
void ExpectColumn(const TColumn& column, const arrow::Array& expected,
    int64_t first, int64_t rows, const std::string& name)
{
    for (int64_t i = 0; i < rows; ++i) {
        const int64_t row = first + i;
        const bool valid = !column.Mask || Bit(column.Mask, column.MaskBitOffset + i);
        ASSERT_EQ(valid, expected.IsValid(row)) << name << " row " << row;
        if (!valid) {
            continue;
        }
        switch (expected.type_id()) {
            case arrow::Type::INT32:
            case arrow::Type::DATE32:
            case arrow::Type::UINT32: {
                uint32_t value;
                std::memcpy(&value, column.Data + i * 4, 4);
                const auto& typed = static_cast<const arrow::PrimitiveArray&>(expected);
                uint32_t want;
                std::memcpy(&want, typed.values()->data() + (typed.offset() + row) * 4, 4);
                ASSERT_EQ(value, want) << name << " row " << row;
                break;
            }
            case arrow::Type::INT64:
            case arrow::Type::UINT64:
            case arrow::Type::DOUBLE: {
                uint64_t value;
                std::memcpy(&value, column.Data + i * 8, 8);
                const auto& typed = static_cast<const arrow::PrimitiveArray&>(expected);
                uint64_t want;
                std::memcpy(&want, typed.values()->data() + (typed.offset() + row) * 8, 8);
                ASSERT_EQ(value, want) << name << " row " << row;
                break;
            }
            case arrow::Type::BOOL: {
                const auto& typed = static_cast<const arrow::BooleanArray&>(expected);
                ASSERT_EQ(Bit(reinterpret_cast<const uint8_t*>(column.Data),
                    column.DataBitOffset + i), typed.Value(row)) << name << " row " << row;
                break;
            }
            case arrow::Type::STRING: {
                const auto* offsets = static_cast<const int32_t*>(column.Offsets);
                ASSERT_EQ(column.OffsetWidth, 4);
                const std::string_view value(column.Data + (offsets[i] - offsets[0]),
                    static_cast<size_t>(offsets[i + 1] - offsets[i]));
                ASSERT_EQ(value, static_cast<const arrow::StringArray&>(expected).GetView(row))
                    << name << " row " << row;
                break;
            }
            default:
                FAIL() << "unexpected type " << expected.type()->ToString();
        }
    }
}

// Reads the whole file through the kernel and compares it with the table.
void ExpectKernelScan(const std::string& path, const arrow::Table& table,
    int64_t batchRows, const std::vector<std::string>& columns = {}, bool rowIds = false)
{
    TParquetSource file(path);
    std::vector<std::string> names = columns;
    if (names.empty()) {
        for (const auto& field : table.schema()->fields()) {
            names.push_back(field->name());
        }
    }
    std::unordered_set<std::string> restriction(names.begin(), names.end());
    if (rowIds) {
        restriction.insert(std::string(InternalRowIdColumnName));
    }
    file.RestrictColumns(restriction);
    std::vector<size_t> rowGroups;
    for (const auto& group : file.ScanRowGroups()) {
        rowGroups.push_back(group.RowGroup);
    }
    auto source = file.MakeRowGroupsSource(rowGroups,
        TParquetDecoding{.Kernel = Decoder(), .BatchRows = batchRows});
    ASSERT_TRUE(source->UsesKernelDecoding()) << source->KernelDecodingUnsupported().value_or("");

    // The kernel scan reports columns in file order.
    std::vector<std::string> ordered;
    for (const auto& field : table.schema()->fields()) {
        if (restriction.contains(field->name())) {
            ordered.push_back(field->name());
        }
    }
    const auto& schema = source->Schema();
    ASSERT_EQ(schema.Columns.size(), ordered.size() + (rowIds ? 1 : 0));

    std::vector<std::shared_ptr<arrow::Array>> expected;
    for (const auto& name : ordered) {
        auto chunked = table.GetColumnByName(name);
        expected.push_back(arrow::Concatenate(chunked->chunks()).ValueOrDie());
    }

    int64_t row = 0;
    int64_t groupRows = 0;
    size_t group = 0;
    TRowSet batch;
    while (source->Next(batch)) {
        ASSERT_EQ(batch.ColumnCount, static_cast<int64_t>(ordered.size() + (rowIds ? 1 : 0)));
        ASSERT_LE(batch.RowCount, batchRows);
        for (size_t c = 0; c < ordered.size(); ++c) {
            ExpectColumn(batch.Columns[c], *expected[c], row, batch.RowCount, ordered[c]);
        }
        if (rowIds) {
            const auto* ids = reinterpret_cast<const uint64_t*>(batch.Columns[ordered.size()].Data);
            for (int64_t i = 0; i < batch.RowCount; ++i) {
                ASSERT_EQ(ParquetRowGroup(ids[i]), rowGroups[group]);
                ASSERT_EQ(ParquetRowOffset(ids[i]), static_cast<uint32_t>(groupRows + i));
            }
        }
        row += batch.RowCount;
        groupRows += batch.RowCount;
        if (group < rowGroups.size()
            && groupRows == file.ScanRowGroups()[group].RowCount)
        {
            ++group;
            groupRows = 0;
        }
        Release(&batch);
    }
    EXPECT_EQ(row, table.num_rows());
}

struct TCase {
    std::string Name;
    TWriteOptions Options;
};

class ParquetKernelScan : public testing::TestWithParam<TCase> {};

TEST_P(ParquetKernelScan, MatchesWrittenValues) {
    const auto& param = GetParam();
    auto table = MakeTable(2777, 17);
    const auto path = TempPath(param.Name);
    Write(path, *table, param.Options);
    for (int64_t batchRows : {37, 1000, 16384}) {
        SCOPED_TRACE("batch rows " + std::to_string(batchRows));
        ExpectKernelScan(path, *table, batchRows);
    }
    std::filesystem::remove(path);
}

std::vector<TCase> Cases() {
    using V = parquet::ParquetDataPageVersion;
    std::vector<TCase> cases;
    for (auto version : {V::V1, V::V2}) {
        for (auto codec : {arrow::Compression::UNCOMPRESSED, arrow::Compression::SNAPPY,
                 arrow::Compression::ZSTD})
        {
            for (bool dictionary : {true, false}) {
                TWriteOptions options{
                    .PageVersion = version,
                    .Codec = codec,
                    .Dictionary = dictionary,
                };
                cases.push_back({
                    std::string(version == V::V1 ? "v1" : "v2") + "_"
                        + arrow::util::Codec::GetCodecAsString(codec) + "_"
                        + (dictionary ? "dict" : "plain"),
                    options,
                });
            }
        }
    }
    // One page per chunk, one row group, and large dictionaries.
    cases.push_back({"single_page", TWriteOptions{
        .RowGroupRows = 1 << 20, .PageBytes = 1 << 20, .DictionaryPageBytes = 1 << 20}});
    // Row groups shorter than a batch, including a 1-row tail.
    cases.push_back({"short_groups", TWriteOptions{.RowGroupRows = 2, .PageBytes = 64}});
    return cases;
}

INSTANTIATE_TEST_SUITE_P(Layouts, ParquetKernelScan, testing::ValuesIn(Cases()),
    [](const testing::TestParamInfo<TCase>& info) { return info.param.Name; });

TEST(ParquetKernelScanColumns, RestrictsColumnsAndAddsRowIds) {
    auto table = MakeTable(3001, 5);
    const auto path = TempPath("restrict");
    Write(path, *table, TWriteOptions{.RowGroupRows = 700});
    ExpectKernelScan(path, *table, 256, {"moden", "i64n", "flag"}, true);
    ExpectKernelScan(path, *table, 256, {"unique"}, false);
    std::filesystem::remove(path);
}

TEST(ParquetKernelScanColumns, ReadsRowCountsWithoutColumns) {
    auto table = MakeTable(1500, 9);
    const auto path = TempPath("no_columns");
    Write(path, *table, TWriteOptions{.RowGroupRows = 600});
    TParquetSource file(path);
    file.RestrictColumns({});
    auto source = file.MakeRowGroupsSource({0, 1, 2},
        TParquetDecoding{.Kernel = Decoder(), .BatchRows = 500});
    ASSERT_TRUE(source->UsesKernelDecoding());
    int64_t rows = 0;
    TRowSet batch;
    while (source->Next(batch)) {
        EXPECT_EQ(batch.ColumnCount, 0);
        rows += batch.RowCount;
        Release(&batch);
    }
    EXPECT_EQ(rows, 1500);
    std::filesystem::remove(path);
}

TEST(ParquetKernelScanColumns, EmptyFile) {
    auto table = MakeTable(0, 1);
    const auto path = TempPath("empty");
    Write(path, *table, TWriteOptions{});
    ExpectKernelScan(path, *table, 100);
    std::filesystem::remove(path);
}

TEST(ParquetKernelScanColumns, FallsBackForUnsupportedEncodingsAndTypes) {
    auto table = MakeTable(500, 3);
    const auto path = TempPath("fallback");
    parquet::WriterProperties::Builder properties;
    properties.disable_dictionary()->encoding("i64", parquet::Encoding::DELTA_BINARY_PACKED);
    auto output = arrow::io::FileOutputStream::Open(path).ValueOrDie();
    ASSERT_TRUE(parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), output,
        1000, properties.build()).ok());
    ASSERT_TRUE(output->Close().ok());

    TParquetSource file(path);
    file.RestrictColumns({"i64"});
    auto unsupported = file.KernelDecodingUnsupported();
    ASSERT_TRUE(unsupported);
    EXPECT_NE(unsupported->find("DELTA_BINARY_PACKED"), std::string::npos) << *unsupported;
    auto source = file.MakeRowGroupsSource({0},
        TParquetDecoding{.Kernel = Decoder(), .BatchRows = 100});
    EXPECT_FALSE(source->UsesKernelDecoding());
    int64_t rows = 0;
    TRowSet batch;
    while (source->Next(batch)) {
        rows += batch.RowCount;
        Release(&batch);
    }
    EXPECT_EQ(rows, 500);

    file.RestrictColumns({"i32", "mode"});
    EXPECT_FALSE(file.KernelDecodingUnsupported());
    std::filesystem::remove(path);
}

// The browser runs kernels as wasm64; the decoder must compile for it through
// the same object path as the plan export.
TEST(ParquetKernelScanColumns, DecoderCompilesForWasm64) {
    std::vector<TGeneratedKernel> kernels;
    TKernelCompiler compiler(TKernelCompilerOptions{
        .Sink = &kernels,
        .BindNow = false,
        .RuntimeContext = std::make_shared<TRuntimeContext>(),
    });
    compiler.CompileParquetDecoder();
    ASSERT_EQ(kernels.size(), 1u);
    auto options = KernelRunnerOptions();
    options.NativeCode = false;
    options.TargetTriple = "wasm64-unknown-unknown";
    NQumir::TLLVMRunner runner(std::move(options));
    std::string error;
    auto object = CompileKernelAstToObject(runner, kernels.front().Ast,
        kernels.front().Entrypoints, &error);
    ASSERT_TRUE(object) << error;
    EXPECT_GT(object->size(), 0u);
}

} // namespace

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    NQumir::NCodeGen::TLLVMInitializer llvmInit;
    return RUN_ALL_TESTS();
}
