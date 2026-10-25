#include <gtest/gtest.h>

#include "runtime_filter_test_util.h"

#include <qdb/io/parquet/row_id.h>
#include <qdb/io/parquet/source.h>
#include <qdb/scheduler/runtime_adapter.h>

#include <qumir/codegen/llvm/llvm_initializer.h>

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/writer.h>
#include <parquet/properties.h>

#include <cstring>
#include <filesystem>
#include <format>
#include <functional>
#include <numeric>
#include <optional>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include <unistd.h>

using namespace NQdb;

namespace {

// Every column type the scan reads, with nulls, empty and long strings at
// different periods, so that pages and batches mix them.
enum class EKind { I8, I16, U32, I64, F64, Date, Bool, String, Decimal };

struct TTestColumn {
    std::string Name;
    EKind Kind;
    std::function<std::optional<std::string>(int64_t)> Value;
};

std::optional<std::string> Text(int64_t row) {
    if (row % 7 == 0) {
        return std::nullopt;
    }
    if (row % 5 == 0) {
        return "";
    }
    if (row % 3 == 0) {
        return std::string(static_cast<size_t>(100 + row % 50), static_cast<char>('a' + row % 26));
    }
    return "s" + std::to_string(row);
}

std::vector<TTestColumn> TestColumns() {
    auto nullEvery = [](int64_t period, auto value) {
        return [period, value](int64_t row) -> std::optional<std::string> {
            if (row % period == 0) {
                return std::nullopt;
            }
            return value(row);
        };
    };
    return {
        {"k", EKind::I64, [](int64_t row) { return std::to_string(row); }},
        {"i8", EKind::I8, nullEvery(4, [](int64_t row) { return std::to_string(row % 128 - 64); })},
        {"i16", EKind::I16, [](int64_t row) { return std::to_string(row % 60000 - 30000); }},
        {"u32", EKind::U32, nullEvery(6, [](int64_t row) { return std::to_string(4000000000u - row); })},
        {"i64", EKind::I64, nullEvery(9, [](int64_t row) { return std::to_string(row * 1000003); })},
        {"f64", EKind::F64, [](int64_t row) { return std::format("{}", row * 0.5); }},
        {"dt", EKind::Date, nullEvery(10, [](int64_t row) { return std::to_string(10000 + row); })},
        {"b", EKind::Bool, nullEvery(11, [](int64_t row) { return std::string(row % 3 == 0 ? "true" : "false"); })},
        {"s", EKind::String, Text},
        {"ls", EKind::String, [](int64_t row) { return "L" + std::to_string(row); }},
        {"dec", EKind::Decimal, nullEvery(13, [](int64_t row) { return std::to_string(row * 7); })},
    };
}

template <typename TBuilder, typename TConvert>
std::shared_ptr<arrow::Array> Build(
    TBuilder builder, const TTestColumn& column, int64_t rows, TConvert convert)
{
    for (int64_t row = 0; row < rows; ++row) {
        const auto value = column.Value(row);
        const auto status = value ? builder.Append(convert(*value)) : builder.AppendNull();
        EXPECT_TRUE(status.ok()) << status.ToString();
    }
    return builder.Finish().ValueOrDie();
}

void WriteTestFile(
    const std::string& path,
    const std::vector<TTestColumn>& columns,
    int64_t rows,
    int64_t rowGroupRows,
    bool dictionary)
{
    std::vector<std::shared_ptr<arrow::Field>> fields;
    std::vector<std::shared_ptr<arrow::Array>> arrays;
    for (const auto& column : columns) {
        const bool nullable = !column.Value(0).has_value() || column.Name == "s";
        std::shared_ptr<arrow::Array> array;
        std::shared_ptr<arrow::DataType> type;
        switch (column.Kind) {
            case EKind::I8:
                type = arrow::int8();
                array = Build(arrow::Int8Builder(), column, rows,
                    [](const std::string& v) { return static_cast<int8_t>(std::stoi(v)); });
                break;
            case EKind::I16:
                type = arrow::int16();
                array = Build(arrow::Int16Builder(), column, rows,
                    [](const std::string& v) { return static_cast<int16_t>(std::stoi(v)); });
                break;
            case EKind::U32:
                type = arrow::uint32();
                array = Build(arrow::UInt32Builder(), column, rows,
                    [](const std::string& v) { return static_cast<uint32_t>(std::stoul(v)); });
                break;
            case EKind::I64:
                type = arrow::int64();
                array = Build(arrow::Int64Builder(), column, rows,
                    [](const std::string& v) { return std::stoll(v); });
                break;
            case EKind::F64:
                type = arrow::float64();
                array = Build(arrow::DoubleBuilder(), column, rows,
                    [](const std::string& v) { return std::stod(v); });
                break;
            case EKind::Date:
                type = arrow::date32();
                array = Build(arrow::Date32Builder(), column, rows,
                    [](const std::string& v) { return std::stoi(v); });
                break;
            case EKind::Bool:
                type = arrow::boolean();
                array = Build(arrow::BooleanBuilder(), column, rows,
                    [](const std::string& v) { return v == "true"; });
                break;
            case EKind::String:
                if (column.Name == "ls") {
                    type = arrow::large_utf8();
                    array = Build(arrow::LargeStringBuilder(), column, rows,
                        [](const std::string& v) { return v; });
                } else {
                    type = arrow::utf8();
                    array = Build(arrow::StringBuilder(), column, rows,
                        [](const std::string& v) { return v; });
                }
                break;
            case EKind::Decimal:
                type = arrow::decimal128(12, 2);
                array = Build(arrow::Decimal128Builder(type), column, rows,
                    [](const std::string& v) { return arrow::Decimal128(std::stoll(v)); });
                break;
        }
        fields.push_back(arrow::field(column.Name, type, nullable || array->null_count() > 0));
        arrays.push_back(std::move(array));
    }
    auto table = arrow::Table::Make(arrow::schema(fields), arrays, rows);
    auto output = arrow::io::FileOutputStream::Open(path).ValueOrDie();
    parquet::WriterProperties::Builder properties;
    // Small pages put page boundaries inside every batch.
    properties.max_rows_per_page(64)->data_pagesize(1024);
    if (!dictionary) {
        properties.disable_dictionary();
    }
    auto status = parquet::arrow::WriteTable(
        *table, arrow::default_memory_pool(), output, rowGroupRows,
        properties.build(),
        parquet::ArrowWriterProperties::Builder().store_schema()->build());
    ASSERT_TRUE(status.ok()) << status.ToString();
}

std::string Render(const TColumn& column, EKind kind, int64_t row) {
    if (column.Mask) {
        const int64_t bit = column.MaskBitOffset + row;
        if (((column.Mask[bit / 8] >> (bit % 8)) & 1) == 0) {
            return "null";
        }
    }
    auto at = [&]<typename T>(T) {
        T value;
        std::memcpy(&value, column.Data + row * sizeof(T), sizeof(T));
        return value;
    };
    switch (kind) {
        case EKind::I8:
            return std::to_string(at(int8_t{}));
        case EKind::I16:
            return std::to_string(at(int16_t{}));
        case EKind::U32:
            return std::to_string(at(uint32_t{}));
        case EKind::I64:
            return std::to_string(at(int64_t{}));
        case EKind::F64:
            return std::format("{}", at(double{}));
        case EKind::Date:
            return std::to_string(at(int32_t{}));
        case EKind::Bool: {
            const int64_t bit = column.DataBitOffset + row;
            return ((column.Data[bit / 8] >> (bit % 8)) & 1) ? "true" : "false";
        }
        case EKind::String: {
            auto offset = [&](int64_t i) -> int64_t {
                return column.OffsetWidth == 8
                    ? static_cast<const int64_t*>(column.Offsets)[i]
                    : static_cast<const int32_t*>(column.Offsets)[i];
            };
            const int64_t begin = offset(row) - offset(0);
            const int64_t end = offset(row + 1) - offset(0);
            return std::string(column.Data + begin, static_cast<size_t>(end - begin));
        }
        case EKind::Decimal: {
            int64_t low;
            std::memcpy(&low, column.Data + row * 16, sizeof(low));
            return std::to_string(low);
        }
    }
    return {};
}

// Keeps rows by their key, given in the first column it reads. Records what
// it saw so a test can check the scan's two stages.
class TKeyFilter final : public IScanFilter {
public:
    // keep returns nullopt for a batch it passes without a selection.
    using TKeep = std::function<std::optional<bool>(int64_t)>;

    TKeyFilter(std::vector<size_t> columns, std::vector<size_t> late, TKeep keep)
        : Columns_(std::move(columns))
        , Late_(std::move(late))
        , Keep_(std::move(keep))
    {}

    std::span<const size_t> Columns() const override {
        return Columns_;
    }

    size_t Select(const TRowSet& batch, TScanSelection& selection) override {
        ++Batches;
        for (size_t column : Late_) {
            LateDecodedEarly |= batch.Columns[column].Data != nullptr;
        }
        const auto* keys = reinterpret_cast<const int64_t*>(batch.Columns[Columns_[0]].Data);
        if (!Keep_(keys[0])) {
            return static_cast<size_t>(batch.RowCount);
        }
        size_t kept = 0;
        selection.Rows.resize(static_cast<size_t>(batch.RowCount));
        selection.Hashes.resize(static_cast<size_t>(batch.RowCount));
        for (int64_t row = 0; row < batch.RowCount; ++row) {
            const bool keep = Keep_(keys[row]).value_or(true);
            selection.Rows[row] = keep ? 0xff : 0;
            selection.Hashes[row] = static_cast<uint64_t>(keys[row]) * 31 + 7;
            kept += keep;
        }
        return kept;
    }

    size_t Batches = 0;
    bool LateDecodedEarly = false;

private:
    std::vector<size_t> Columns_;
    std::vector<size_t> Late_;
    TKeep Keep_;
};

struct TScanResult {
    // Rendered rows the scan kept, with their hash or "-" without one.
    std::vector<std::vector<std::string>> Rows;
    size_t Batches = 0;
    size_t EmptyBatches = 0;
};

size_t ColumnIndex(const TSchema& schema, std::string_view name) {
    for (size_t i = 0; i < schema.Columns.size(); ++i) {
        if (schema.Columns[i].Name == name) {
            return i;
        }
    }
    ADD_FAILURE() << "no column " << name;
    return 0;
}

TScanResult Scan(ISource& source, const std::vector<TTestColumn>& columns) {
    TScanResult result;
    TRowSet rowSet{};
    std::vector<size_t> indexes;
    for (const auto& column : columns) {
        indexes.push_back(ColumnIndex(source.Schema(), column.Name));
    }
    while (source.Next(rowSet)) {
        ++result.Batches;
        size_t kept = 0;
        for (int64_t row = 0; row < rowSet.RowCount; ++row) {
            if (rowSet.Selection && !rowSet.Selection[row]) {
                continue;
            }
            ++kept;
            std::vector<std::string> rendered;
            for (size_t i = 0; i < columns.size(); ++i) {
                rendered.push_back(Render(rowSet.Columns[indexes[i]], columns[i].Kind, row));
            }
            rendered.push_back(rowSet.Hash ? std::to_string(rowSet.Hash[row]) : "-");
            result.Rows.push_back(std::move(rendered));
        }
        result.EmptyBatches += kept == 0;
        Release(&rowSet);
        rowSet = {};
    }
    return result;
}

std::vector<std::vector<std::string>> Expected(
    const std::vector<TTestColumn>& columns,
    int64_t rows,
    const TKeyFilter::TKeep& keep)
{
    std::vector<std::vector<std::string>> expected;
    for (int64_t row = 0; row < rows; ++row) {
        const auto decision = keep(row);
        if (decision && !*decision) {
            continue;
        }
        std::vector<std::string> rendered;
        for (const auto& column : columns) {
            rendered.push_back(column.Value(row).value_or("null"));
        }
        rendered.push_back(decision ? std::to_string(row * 31 + 7) : "-");
        expected.push_back(std::move(rendered));
    }
    return expected;
}

class ParquetScanFilter : public testing::TestWithParam<bool> {
protected:
    std::string Path(std::string_view name) const {
        return (std::filesystem::temp_directory_path() /
            std::format("qdb_scan_filter_{}_{}_{}.parquet", getpid(), name,
                GetParam() ? "dict" : "plain")).string();
    }

    std::unique_ptr<TParquetSource> FilteredScan(
        const std::string& path,
        const std::unordered_set<std::string>& names,
        TKeyFilter::TKeep keep,
        std::shared_ptr<TKeyFilter>& filter)
    {
        TParquetSource source(path);
        source.RestrictColumns(names);
        std::vector<size_t> rowGroups;
        for (const auto& group : source.ScanRowGroups()) {
            rowGroups.push_back(group.RowGroup);
        }
        auto scan = source.MakeRowGroupsSource(rowGroups);
        std::vector<size_t> late;
        for (size_t i = 0; i < scan->Schema().Columns.size(); ++i) {
            const auto name = scan->Schema().Columns[i].Name;
            if (name == "s" || name == "ls" || name == "i64") {
                late.push_back(i);
            }
        }
        filter = std::make_shared<TKeyFilter>(
            std::vector<size_t>{ColumnIndex(scan->Schema(), "k")}, late, std::move(keep));
        scan->SetScanFilter(filter);
        return scan;
    }
};

TEST_P(ParquetScanFilter, MaterializesKeptRowsOfEveryColumnType) {
    constexpr int64_t rowGroupRows = 1000;
    constexpr int64_t rows = 4500;
    const auto columns = TestColumns();
    const auto path = Path("types");
    WriteTestFile(path, columns, rows, rowGroupRows, GetParam());

    // One batch per row group: rejected entirely, kept entirely, mostly
    // kept, sparsely kept, and passed without a selection.
    const TKeyFilter::TKeep keep = [](int64_t key) -> std::optional<bool> {
        switch (key / rowGroupRows) {
            case 0: return false;
            case 1: return true;
            case 2: return key % 3 != 1;
            case 3: return key % 37 == 4;
            default: return std::nullopt;
        }
    };
    std::unordered_set<std::string> names;
    for (const auto& column : columns) {
        names.insert(column.Name);
    }
    std::shared_ptr<TKeyFilter> filter;
    auto scan = FilteredScan(path, names, keep, filter);
    const auto result = Scan(*scan, columns);

    EXPECT_EQ(result.Rows, Expected(columns, rows, keep));
    EXPECT_EQ(filter->Batches, 5u);
    EXPECT_EQ(result.Batches, 4u);
    EXPECT_EQ(result.EmptyBatches, 0u);
    EXPECT_FALSE(filter->LateDecodedEarly);
}

TEST_P(ParquetScanFilter, LaterBatchesOfARowGroupFollowSkippedOnes) {
    // The Arrow reader cuts a row group into batches of 1 << 18 rows.
    constexpr int64_t batchRows = 1 << 18;
    constexpr int64_t rows = 2 * batchRows + 1000;
    const auto all = TestColumns();
    const std::vector<TTestColumn> columns = {all[0], all[4], all[8]};
    const auto path = Path("batches");
    WriteTestFile(path, columns, rows, rows, GetParam());

    const TKeyFilter::TKeep keep = [](int64_t key) -> std::optional<bool> {
        switch (key / batchRows) {
            case 0: return key % 97 == 5;
            case 1: return false;
            default: return true;
        }
    };
    std::shared_ptr<TKeyFilter> filter;
    auto scan = FilteredScan(path, {"k", "i64", "s"}, keep, filter);
    const auto result = Scan(*scan, columns);

    EXPECT_EQ(result.Rows, Expected(columns, rows, keep));
    EXPECT_EQ(filter->Batches, 3u);
    EXPECT_EQ(result.Batches, 2u);
    EXPECT_FALSE(filter->LateDecodedEarly);
}

TEST_P(ParquetScanFilter, RowIdsLocateKeptRows) {
    constexpr int64_t rowGroupRows = 100;
    constexpr int64_t rows = 250;
    const auto all = TestColumns();
    const std::vector<TTestColumn> columns = {all[0], all[8]};
    const auto path = Path("row_ids");
    WriteTestFile(path, columns, rows, rowGroupRows, GetParam());

    const TKeyFilter::TKeep keep = [](int64_t key) -> std::optional<bool> {
        return key % 4 == 1;
    };
    std::shared_ptr<TKeyFilter> filter;
    auto scan = FilteredScan(path,
        {"k", "s", std::string(InternalRowIdColumnName)}, keep, filter);
    const size_t rowIdColumn = ColumnIndex(scan->Schema(), InternalRowIdColumnName);
    const size_t keyColumn = ColumnIndex(scan->Schema(), "k");

    size_t kept = 0;
    TRowSet rowSet{};
    while (scan->Next(rowSet)) {
        const auto* keys = reinterpret_cast<const int64_t*>(rowSet.Columns[keyColumn].Data);
        const auto* rowIds = reinterpret_cast<const uint64_t*>(rowSet.Columns[rowIdColumn].Data);
        for (int64_t row = 0; row < rowSet.RowCount; ++row) {
            if (!rowSet.Selection[row]) {
                continue;
            }
            ++kept;
            EXPECT_EQ(rowIds[row], MakeParquetRowId(
                static_cast<uint64_t>(keys[row] / rowGroupRows),
                static_cast<uint64_t>(keys[row] % rowGroupRows)));
        }
        Release(&rowSet);
        rowSet = {};
    }
    EXPECT_EQ(kept, static_cast<size_t>((rows + 2) / 4));
}

INSTANTIATE_TEST_SUITE_P(Encodings, ParquetScanFilter, testing::Bool(),
    [](const testing::TestParamInfo<bool>& info) {
        return info.param ? "Dictionary" : "Plain";
    });

// A scan applying real runtime filters. The hash kernel is replaced by the
// key itself, so the test chooses which keys a filter holds.
class ParquetRuntimeFilterScan : public NTest::TRuntimeFilterTest {
protected:
    static constexpr int64_t BatchRows = 8192;
    static constexpr int64_t Batches = 11;

    void SetUp() override {
        NTest::TRuntimeFilterTest::SetUp();
        for (uint64_t key = 1; key <= 200; ++key) {
            Keys_.push_back(static_cast<int64_t>(key * 0x9e3779b97f4a7c15ull >> 1));
        }
        const auto all = TestColumns();
        Columns_ = {
            {"k", EKind::I64, [this](int64_t row) {
                return std::to_string(Keys_[static_cast<size_t>(row % 200)]);
            }},
            all[5],
            all[8],
        };
        Path_ = (std::filesystem::temp_directory_path() /
            std::format("qdb_runtime_filter_scan_{}.parquet", getpid())).string();
        WriteTestFile(Path_, Columns_, BatchRows * Batches, BatchRows, true);
    }

    std::shared_ptr<TRuntimeFilter> Publish(std::vector<int64_t> keys, size_t maxKeys, size_t bloomBytes) {
        auto filter = std::make_shared<TRuntimeFilter>(Context_, maxKeys, bloomBytes);
        auto builder = filter->MakeBuilder();
        builder.Add(std::vector<uint64_t>(keys.begin(), keys.end()));
        filter->FinishProducer(std::move(builder).TakePartial());
        return filter;
    }

    // Scans the file through filter; returns whether each batch carried a
    // selection.
    std::vector<bool> ScanWith(std::shared_ptr<TRuntimeFilter> filter, TScanResult& result) {
        auto code = std::make_shared<NScheduler::TRuntimeFilterCode>();
        code->Probe = std::move(filter);
        code->Hash = [](TRowSet* batch, uint64_t* hashes) {
            const auto* keys = reinterpret_cast<const int64_t*>(batch->Columns[0].Data);
            for (int64_t row = 0; row < batch->RowCount; ++row) {
                hashes[row] = static_cast<uint64_t>(keys[row]);
            }
            return true;
        };
        TParquetSource source(Path_);
        source.RestrictColumns({"k", "f64", "s"});
        std::vector<size_t> rowGroups(Batches);
        std::iota(rowGroups.begin(), rowGroups.end(), size_t{0});
        auto scan = source.MakeRowGroupsSource(rowGroups);
        scan->SetScanFilter(std::make_shared<NScheduler::TRuntimeFilterScan>(
            std::vector<std::shared_ptr<const NScheduler::TRuntimeFilterCode>>{code},
            std::vector<size_t>{0}));

        std::vector<bool> selected;
        TRowSet rowSet{};
        while (scan->Next(rowSet)) {
            selected.push_back(rowSet.Selection != nullptr);
            EXPECT_EQ(rowSet.Selection != nullptr, rowSet.Hash != nullptr);
            for (int64_t row = 0; row < rowSet.RowCount; ++row) {
                if (rowSet.Selection && !rowSet.Selection[row]) {
                    continue;
                }
                std::vector<std::string> rendered;
                for (size_t column = 0; column < Columns_.size(); ++column) {
                    rendered.push_back(Render(rowSet.Columns[column], Columns_[column].Kind, row));
                }
                result.Rows.push_back(std::move(rendered));
            }
            Release(&rowSet);
            rowSet = {};
        }
        return selected;
    }

    std::vector<std::vector<std::string>> Expected(const std::function<bool(int64_t)>& keep) const {
        std::vector<std::vector<std::string>> expected;
        for (int64_t row = 0; row < BatchRows * Batches; ++row) {
            if (!keep(row)) {
                continue;
            }
            std::vector<std::string> rendered;
            for (const auto& column : Columns_) {
                rendered.push_back(column.Value(row).value_or("null"));
            }
            expected.push_back(std::move(rendered));
        }
        return expected;
    }

    std::vector<int64_t> Keys_;
    std::vector<TTestColumn> Columns_;
    std::string Path_;
};

TEST_F(ParquetRuntimeFilterScan, ExactFilterKeepsItsKeys) {
    auto filter = Publish({Keys_[3], Keys_[150]}, TRuntimeFilter::DefaultMaxKeys, 1024);
    TScanResult result;
    const auto selected = ScanWith(filter, result);
    EXPECT_EQ(result.Rows, Expected([](int64_t row) {
        return row % 200 == 3 || row % 200 == 150;
    }));
    EXPECT_EQ(selected, std::vector<bool>(Batches, true));
    const auto stats = filter->StatsSnapshot();
    EXPECT_EQ(stats.ProbeRows, static_cast<uint64_t>(BatchRows * Batches));
    EXPECT_EQ(stats.ProbeRows - stats.RejectedRows, result.Rows.size());
}

TEST_F(ParquetRuntimeFilterScan, PassAllFilterKeepsEveryBatchWhole) {
    // More keys than the exact set holds and no Bloom storage.
    auto filter = Publish(Keys_, 8, 0);
    ASSERT_TRUE(filter->StatsSnapshot().BloomDisabled);
    TScanResult result;
    const auto selected = ScanWith(filter, result);
    EXPECT_EQ(result.Rows, Expected([](int64_t) { return true; }));
    EXPECT_EQ(selected, std::vector<bool>(Batches, false));
    EXPECT_EQ(filter->StatsSnapshot().SkippedProbeRows, static_cast<uint64_t>(BatchRows * Batches));
}

TEST_F(ParquetRuntimeFilterScan, WeakBloomFilterPausesAndResumes) {
    // Every key is in the filter, so it rejects nothing: after six probed
    // batches it pauses for four and then probes again.
    auto filter = Publish(Keys_, 8, 512);
    ASSERT_TRUE(filter->UsesBloom());
    TScanResult result;
    const auto selected = ScanWith(filter, result);
    EXPECT_EQ(result.Rows, Expected([](int64_t) { return true; }));
    EXPECT_EQ(selected, (std::vector<bool>{
        true, true, true, true, true, true, false, false, false, false, true}));
    EXPECT_EQ(filter->StatsSnapshot().PausedProbeRows, static_cast<uint64_t>(BatchRows * 4));
}

} // namespace

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    NQumir::NCodeGen::TLLVMInitializer initializer;
    return RUN_ALL_TESTS();
}
