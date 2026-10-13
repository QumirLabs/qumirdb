#include "reg_suite.h"
#include "plan_runner.h"

#include <qdb/io/parquet/source.h>
#include <qdb/plan/build.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/pipeline.h>
#include <qdb/plan/types/nullable.h>
#include <qdb/sql/parser.h>
#include <qumir/parser/core/parser.h>
#include <qumir/parser/core/printer.h>

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/writer.h>
#include <parquet/properties.h>

#include <llvm/Support/JSON.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/ADT/SmallString.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace NRegTest {
namespace fs = std::filesystem;
using namespace NQdb;
using namespace NQumir::NAst;
using TValue = llvm::json::Value;
using TObject = llvm::json::Object;
using TArray = llvm::json::Array;

namespace {

std::string ReadAll(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot read " + path.string());
    }
    return {std::istreambuf_iterator<char>(input), {}};
}

TValue ReadJson(const fs::path& path) {
    auto parsed = llvm::json::parse(ReadAll(path));
    if (!parsed) {
        throw std::runtime_error(path.string() + ": " + llvm::toString(parsed.takeError()));
    }
    return std::move(*parsed);
}

const TObject& Object(const TValue& value) {
    if (auto* object = value.getAsObject()) {
        return *object;
    }
    throw std::runtime_error("expected a JSON object");
}

const TArray& Array(const TValue& value) {
    if (auto* array = value.getAsArray()) {
        return *array;
    }
    throw std::runtime_error("expected a JSON array");
}

std::string String(const TValue& value) {
    if (auto string = value.getAsString()) {
        return string->str();
    }
    throw std::runtime_error("expected a JSON string");
}

const TValue& Required(const TObject& object, llvm::StringRef name) {
    if (auto* value = object.get(name)) {
        return *value;
    }
    throw std::runtime_error("missing field '" + name.str() + "'");
}

void Fields(const TObject& object, std::initializer_list<llvm::StringRef> allowed) {
    for (const auto& [key, value] : object) {
        if (std::find(allowed.begin(), allowed.end(), key.str()) == allowed.end()) {
            throw std::runtime_error("unknown field '" + key.str() + "'");
        }
    }
}

void Version(const TObject& object) {
    if (Required(object, "version").getAsInteger() != 1) {
        throw std::runtime_error("expected version 1");
    }
}

std::string Render(const TValue& value) {
    return llvm::formatv("{0:2}", value).str() + "\n";
}

std::string RenderResult(const TValue& value) {
    const auto& object = Object(value);
    const auto& rows = Array(Required(object, "rows"));
    std::ostringstream output;
    output << "{\n  \"version\": " << llvm::formatv("{0}", Required(object, "version")).str()
        << ",\n  \"schema\": " << llvm::formatv("{0}", Required(object, "schema")).str()
        << ",\n  \"rows\": [";
    for (size_t row = 0; row < rows.size(); ++row) {
        output << (row == 0 ? "\n" : ",\n") << "    [";
        const auto& cells = Array(rows[row]);
        for (size_t column = 0; column < cells.size(); ++column) {
            if (column) output << ", ";
            output << llvm::formatv("{0}", cells[column]).str();
        }
        output << ']';
    }
    if (!rows.empty()) output << "\n  ";
    output << "]\n}\n";
    return output.str();
}

void CheckArrow(const arrow::Status& status) {
    if (!status.ok()) {
        throw std::runtime_error(status.ToString());
    }
}

template <typename T>
T ArrowValue(arrow::Result<T> result) {
    if (!result.ok()) {
        throw std::runtime_error(result.status().ToString());
    }
    return std::move(*result);
}

class TTempDir {
public:
    TTempDir() {
        llvm::SmallString<128> path;
        if (auto error = llvm::sys::fs::createUniqueDirectory("qdb-regtest", path)) {
            throw std::runtime_error("cannot create regression fixture directory: " + error.message());
        }
        Path = path.str().str();
    }

    ~TTempDir() {
        std::error_code error;
        fs::remove_all(Path, error);
    }

    fs::path Path;
};

std::shared_ptr<TStructType> ParseSchema(const TValue& value) {
    std::istringstream input(String(value));
    NCore::TTokenStream tokens(input);
    auto parsed = NCore::ParseType(tokens);
    if (!parsed) throw std::runtime_error(parsed.error().ToString());
    if (!tokens.Next().IsEof()) throw std::runtime_error("unexpected input after schema type");
    auto structure = TMaybeType<TStructType>(*parsed);
    if (!structure) throw std::runtime_error("schema must be a Qumir struct type");
    std::set<std::string> names;
    for (const auto& [name, type] : structure.Cast()->Fields) {
        if (name.empty() || !names.insert(name).second) {
            throw std::runtime_error("empty or duplicate schema field name");
        }
    }
    return structure.Cast();
}

std::pair<TTypePtr, bool> FieldType(const TTypePtr& type) {
    if (auto named = TMaybeType<TNamedType>(type)) {
        const auto& t = *named.Cast();
        if (t.Name == "Nullable" && !t.UnderlyingType && t.TypeArgs.size() == 1
            && t.TypeArgs[0].Kind == TGenericArg::EKind::Type && t.TypeArgs[0].Type)
        {
            return {t.TypeArgs[0].Type, true};
        }
    }
    return {type, false};
}

std::shared_ptr<arrow::DataType> ArrowType(const TTypePtr& type) {
    if (auto integer = TMaybeType<TIntegerType>(type)) {
        switch (integer.Cast()->Kind) {
            case TIntegerType::I8: return arrow::int8();
            case TIntegerType::I16: return arrow::int16();
            case TIntegerType::I32: return arrow::int32();
            case TIntegerType::I64: return arrow::int64();
            case TIntegerType::U8: return arrow::uint8();
            case TIntegerType::U16: return arrow::uint16();
            case TIntegerType::U32: return arrow::uint32();
            case TIntegerType::U64: return arrow::uint64();
            default: break;
        }
    }
    if (TMaybeType<TFloatType>(type)) return arrow::float64();
    if (TMaybeType<TBoolType>(type)) return arrow::boolean();
    if (TMaybeType<TStringType>(type)) return arrow::utf8();
    throw std::runtime_error("unsupported fixture type '" + NCore::PrintType(type) + "'");
}

std::string PrintSchema(const std::shared_ptr<TStructType>& schema) {
    std::vector<std::pair<std::string, TTypePtr>> fields;
    for (const auto& [name, type] : schema->Fields) {
        auto printed = type;
        // QDB nullable types are represented as a generic named Qumir type in
        // the fixture syntax; the core printer/parser require no QDB extension.
        if (IsNullableType(type)) {
            printed = std::make_shared<TNamedType>("Nullable", nullptr,
                std::vector<TGenericArg>{TGenericArg::TypeArg(UnwrapNullableType(type))});
        }
        fields.emplace_back(name, std::move(printed));
    }
    return NCore::PrintType(std::make_shared<TStructType>(std::move(fields)));
}

template <typename TInt>
TInt Integer(const TValue& value) {
    std::string text;
    if (auto string = value.getAsString()) {
        text = string->str();
    } else if constexpr (sizeof(TInt) < 8) {
        if (auto number = value.getAsInteger()) {
            text = std::to_string(*number);
        }
    }
    TInt parsed{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size()) {
        throw std::runtime_error("invalid or out-of-range integer (64-bit values require strings)");
    }
    return parsed;
}

double Float(const TValue& value) {
    if (auto number = value.getAsNumber()) {
        if (std::isfinite(*number)) return *number;
    }
    if (auto* tag = value.getAsObject()) {
        Fields(*tag, {"$float"});
        const auto name = String(Required(*tag, "$float"));
        if (name == "nan") return std::numeric_limits<double>::quiet_NaN();
        if (name == "+inf") return std::numeric_limits<double>::infinity();
        if (name == "-inf") return -std::numeric_limits<double>::infinity();
        if (name == "-0") return -0.0;
    }
    throw std::runtime_error("invalid f64 value");
}

TValue FloatJson(double value) {
    if (std::isnan(value)) return TObject{{"$float", "nan"}};
    if (std::isinf(value)) return TObject{{"$float", value < 0 ? "-inf" : "+inf"}};
    if (value == 0 && std::signbit(value)) return TObject{{"$float", "-0"}};
    return value;
}

template <typename TBuilder, typename TInt>
void AppendInteger(arrow::ArrayBuilder& builder, const TValue& value) {
    CheckArrow(static_cast<TBuilder&>(builder).Append(Integer<TInt>(value)));
}

void Append(arrow::ArrayBuilder& builder, const TValue& value) {
    switch (builder.type()->id()) {
        case arrow::Type::INT8: return AppendInteger<arrow::Int8Builder, int8_t>(builder, value);
        case arrow::Type::INT16: return AppendInteger<arrow::Int16Builder, int16_t>(builder, value);
        case arrow::Type::INT32: return AppendInteger<arrow::Int32Builder, int32_t>(builder, value);
        case arrow::Type::INT64: return AppendInteger<arrow::Int64Builder, int64_t>(builder, value);
        case arrow::Type::UINT8: return AppendInteger<arrow::UInt8Builder, uint8_t>(builder, value);
        case arrow::Type::UINT16: return AppendInteger<arrow::UInt16Builder, uint16_t>(builder, value);
        case arrow::Type::UINT32: return AppendInteger<arrow::UInt32Builder, uint32_t>(builder, value);
        case arrow::Type::UINT64: return AppendInteger<arrow::UInt64Builder, uint64_t>(builder, value);
        case arrow::Type::DOUBLE:
            return CheckArrow(static_cast<arrow::DoubleBuilder&>(builder).Append(Float(value)));
        case arrow::Type::BOOL:
            if (auto boolean = value.getAsBoolean()) {
                return CheckArrow(static_cast<arrow::BooleanBuilder&>(builder).Append(*boolean));
            }
            throw std::runtime_error("invalid bool value");
        case arrow::Type::STRING:
            return CheckArrow(static_cast<arrow::StringBuilder&>(builder).Append(String(value)));
        default: throw std::runtime_error("unsupported Arrow fixture type");
    }
}

std::shared_ptr<arrow::Table> ReadTable(const fs::path& path) {
    auto json = ReadJson(path);
    const auto& object = Object(json);
    Fields(object, {"version", "schema", "rows"});
    Version(object);
    std::vector<std::shared_ptr<arrow::Field>> fields;
    std::vector<std::unique_ptr<arrow::ArrayBuilder>> builders;
    const auto schema = ParseSchema(Required(object, "schema"));
    for (const auto& [name, columnType] : schema->Fields) {
        const auto [valueType, nullable] = FieldType(columnType);
        auto type = ArrowType(valueType);
        fields.push_back(arrow::field(name, type, nullable));
        builders.push_back(ArrowValue(arrow::MakeBuilder(type)));
    }
    if (fields.empty()) throw std::runtime_error("fixture schema must not be empty");
    size_t rowIndex = 0;
    for (const auto& row : Array(Required(object, "rows"))) {
        const auto& values = Array(row);
        if (values.size() != fields.size()) {
            throw std::runtime_error("row " + std::to_string(rowIndex) + ": wrong column count");
        }
        for (size_t column = 0; column < fields.size(); ++column) {
            try {
                if (values[column].getAsNull()) {
                    if (!fields[column]->nullable()) throw std::runtime_error("NULL in non-nullable column");
                    CheckArrow(builders[column]->AppendNull());
                } else {
                    Append(*builders[column], values[column]);
                }
            } catch (const std::exception& error) {
                throw std::runtime_error("row " + std::to_string(rowIndex) + ", column '"
                    + fields[column]->name() + "': " + error.what());
            }
        }
        ++rowIndex;
    }
    std::vector<std::shared_ptr<arrow::Array>> columns;
    for (auto& builder : builders) columns.push_back(ArrowValue(builder->Finish()));
    return arrow::Table::Make(arrow::schema(std::move(fields)), std::move(columns), rowIndex);
}

std::map<std::string, std::string> Bindings(const TObject& object) {
    std::map<std::string, std::string> result;
    for (const auto& [name, value] : object) {
        if (name.str().empty()) throw std::runtime_error("empty table name");
        result.emplace(name.str(), String(value));
    }
    return result;
}

fs::path File(const fs::path& dir, const TValue& value) {
    const auto path = (dir / String(value)).lexically_normal();
    if (!fs::is_regular_file(path)) throw std::runtime_error("missing file " + path.string());
    return path;
}

} // namespace

struct TSuite {
    struct TFixture {
        fs::path Json;
        int64_t RowGroupRows = 128;
        std::shared_ptr<TParquetFile> File;
        std::string Error;
    };

    fs::path Manifest;
    std::unique_ptr<TTempDir> Temp;
    std::map<std::string, TFixture> Fixtures;
    std::map<fs::path, std::shared_ptr<arrow::Table>> Parsed;
    // Same input/layout, even under two fixture names, shares a generated file.
    std::map<std::pair<fs::path, int64_t>, std::shared_ptr<TParquetFile>> Files;
    std::map<fs::path, TValue> Canonized;

    void Prepare(const std::string& name) {
        auto& fixture = Fixtures.at(name);
        if (fixture.File || !fixture.Error.empty()) return;
        try {
            const auto identity = std::make_pair(fixture.Json, fixture.RowGroupRows);
            auto& file = Files[identity];
            if (!file) {
                auto& table = Parsed[fixture.Json];
                if (!table) table = ReadTable(fixture.Json);
                if (!Temp) Temp = std::make_unique<TTempDir>();
                const auto path = Temp->Path / (std::to_string(Files.size()) + ".parquet");
                auto output = ArrowValue(arrow::io::FileOutputStream::Open(path.string()));
                parquet::WriterProperties::Builder properties;
                properties.compression(parquet::Compression::UNCOMPRESSED);
                properties.disable_dictionary();
                properties.enable_statistics();
                CheckArrow(parquet::arrow::WriteTable(*table, arrow::default_memory_pool(),
                    output, fixture.RowGroupRows, properties.build()));
                CheckArrow(output->Close());
                file = std::make_shared<TParquetFile>(path.string());
            }
            fixture.File = file;
        } catch (const std::exception& error) {
            fixture.Error = Manifest.string() + ": fixture '" + name + "': " + error.what();
        }
    }
};

std::vector<TExecCase> CollectExecCases(const fs::path& root, const fs::path& goldensDir) {
    std::vector<fs::path> manifests;
    if (!fs::exists(root)) return {};
    const auto rootDir = fs::absolute(root).lexically_normal();
    const auto goldenRoot = fs::absolute(goldensDir).lexically_normal();
    for (const auto& entry : fs::recursive_directory_iterator(rootDir)) {
        if (entry.is_regular_file() && entry.path().filename() == "suite.json") {
            manifests.push_back(entry.path());
        }
    }
    std::sort(manifests.begin(), manifests.end());
    std::vector<TExecCase> result;
    for (const auto& manifest : manifests) {
        try {
            auto json = ReadJson(manifest);
            const auto& object = Object(json);
            Fields(object, {"version", "fixtures", "tables", "variants", "cases"});
            Version(object);
            auto suite = std::make_shared<TSuite>();
            suite->Manifest = manifest;
            const auto dir = manifest.parent_path();
            for (const auto& [name, value] : Object(Required(object, "fixtures"))) {
                const auto& fixture = Object(value);
                Fields(fixture, {"json", "parquet"});
                TSuite::TFixture definition;
                definition.Json = File(dir, Required(fixture, "json"));
                if (auto* options = fixture.getObject("parquet")) {
                    Fields(*options, {"row_group_rows"});
                    auto size = Required(*options, "row_group_rows").getAsInteger();
                    if (!size || *size <= 0) throw std::runtime_error("row_group_rows must be positive");
                    definition.RowGroupRows = *size;
                } else if (fixture.get("parquet")) {
                    throw std::runtime_error("parquet options must be an object");
                }
                suite->Fixtures.emplace(name.str(), std::move(definition));
            }
            const auto tables = Bindings(Object(Required(object, "tables")));
            auto validateTables = [&](const auto& bindings) {
                for (const auto& [table, fixture] : bindings) {
                    if (!suite->Fixtures.contains(fixture)) {
                        throw std::runtime_error("table '" + table + "' references unknown fixture '" + fixture + "'");
                    }
                }
            };
            validateTables(tables);
            std::set<std::string> variants;
            for (const auto& value : Array(Required(object, "variants"))) {
                const auto variant = String(value);
                if ((variant != "single" && variant != "threaded") || !variants.insert(variant).second) {
                    throw std::runtime_error("unknown or duplicate execution variant '" + variant + "'");
                }
            }
            if (variants.empty()) throw std::runtime_error("suite needs at least one execution variant");
            std::set<fs::path> queries;
            for (const auto& [name, value] : Object(Required(object, "cases"))) {
                const auto& definition = Object(value);
                Fields(definition, {"tables", "order"});
                const auto sql = File(dir, TValue(name.str()));
                if (sql.extension() != ".sql" || sql.parent_path() != dir) {
                    throw std::runtime_error("suite SQL cases must be .sql files in the manifest directory");
                }
                queries.insert(sql);
                auto bindings = tables;
                if (auto* overrides = definition.get("tables")) {
                    for (const auto& [table, fixture] : Bindings(Object(*overrides))) bindings[table] = fixture;
                }
                validateTables(bindings);
                const auto order = String(Required(definition, "order"));
                if (order != "ordered" && order != "unordered") throw std::runtime_error("invalid result order");
                const auto relativeSql = fs::relative(sql, rootDir);
                const auto base = fs::path(relativeSql).replace_extension();
                const auto golden = fs::path(goldenRoot / relativeSql).replace_extension(".result.json");
                for (const auto& variant : variants) {
                    result.push_back({base, sql,
                        golden, suite, bindings, variant, order == "ordered"});
                }
            }
            for (const auto& entry : fs::directory_iterator(dir)) {
                if (entry.is_regular_file() && entry.path().extension() == ".sql" && !queries.contains(entry.path())) {
                    throw std::runtime_error("SQL file has no suite case: " + entry.path().filename().string());
                }
            }
        } catch (const std::exception& error) {
            throw std::runtime_error(manifest.string() + ": " + error.what());
        }
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return std::tie(a.Base, a.Variant) < std::tie(b.Base, b.Variant);
    });
    return result;
}

void PrepareExecCase(const TExecCase& test) {
    for (const auto& [table, fixture] : test.Tables) test.Suite->Prepare(fixture);
}

namespace {

TValue Cell(const TColumn& column, int64_t row, const TTypePtr& type) {
    if (column.Mask) {
        const auto bit = column.MaskBitOffset + row;
        if (!(column.Mask[bit / 8] & (uint8_t{1} << (bit % 8)))) return nullptr;
    }
    const auto valueType = UnwrapNullableType(type);
    if (auto integer = TMaybeType<TIntegerType>(valueType)) {
        switch (integer.Cast()->Kind) {
            case TIntegerType::I8: return reinterpret_cast<const int8_t*>(column.Data)[row];
            case TIntegerType::I16: return reinterpret_cast<const int16_t*>(column.Data)[row];
            case TIntegerType::I32: return reinterpret_cast<const int32_t*>(column.Data)[row];
            case TIntegerType::I64: return std::to_string(reinterpret_cast<const int64_t*>(column.Data)[row]);
            case TIntegerType::U8: return reinterpret_cast<const uint8_t*>(column.Data)[row];
            case TIntegerType::U16: return reinterpret_cast<const uint16_t*>(column.Data)[row];
            case TIntegerType::U32: return reinterpret_cast<const uint32_t*>(column.Data)[row];
            case TIntegerType::U64: return std::to_string(reinterpret_cast<const uint64_t*>(column.Data)[row]);
            default: throw std::runtime_error("128-bit result integers are not supported yet");
        }
    }
    if (TMaybeType<TFloatType>(valueType)) return FloatJson(reinterpret_cast<const double*>(column.Data)[row]);
    if (TMaybeType<TBoolType>(valueType)) {
        const auto bit = column.DataBitOffset + row;
        return (reinterpret_cast<const uint8_t*>(column.Data)[bit / 8] & (uint8_t{1} << (bit % 8))) != 0;
    }
    if (TMaybeType<TStringType>(valueType)) {
        int64_t start, end;
        if (column.OffsetWidth == 4) {
            const auto* offsets = static_cast<const int32_t*>(column.Offsets);
            start = offsets[row] - offsets[0];
            end = offsets[row + 1] - offsets[0];
        } else if (column.OffsetWidth == 8) {
            const auto* offsets = static_cast<const int64_t*>(column.Offsets);
            start = offsets[row] - offsets[0];
            end = offsets[row + 1] - offsets[0];
        } else {
            throw std::runtime_error("invalid result string offset width");
        }
        return std::string(column.Data + start, end - start);
    }
    throw std::runtime_error("unsupported result type " + type->ToString());
}

TValue Result(TTestRuntime& runtime) {
    auto output = TMaybeType<TStructType>(runtime.OutputType());
    if (!output) throw std::runtime_error("query output is not a struct");
    TArray rows;
    TRowSet batch{};
    while (runtime.Next(batch)) {
        try {
            if (static_cast<size_t>(batch.ColumnCount) != output.Cast()->Fields.size()) {
                throw std::runtime_error("result schema/rowset column count mismatch");
            }
            for (int64_t row = 0; row < batch.RowCount; ++row) {
                if (batch.Selection && !batch.Selection[row]) continue;
                TArray values;
                for (int64_t column = 0; column < batch.ColumnCount; ++column) {
                    values.push_back(Cell(batch.Columns[column], row, output.Cast()->Fields[column].second));
                }
                rows.push_back(std::move(values));
            }
        } catch (...) {
            Release(&batch);
            throw;
        }
        Release(&batch);
    }
    return TObject{{"version", 1}, {"schema", PrintSchema(output.Cast())}, {"rows", std::move(rows)}};
}

void Normalize(TValue& value, bool ordered) {
    auto& object = *value.getAsObject();
    Fields(object, {"version", "schema", "rows"});
    Version(object);
    const auto schema = ParseSchema(Required(object, "schema"));
    object["schema"] = PrintSchema(schema);
    auto* rows = object.getArray("rows");
    if (!rows) throw std::runtime_error("result rows must be an array");
    for (auto& row : *rows) {
        const auto& values = Array(row);
        if (values.size() != schema->Fields.size()) throw std::runtime_error("wrong result row width");
    }
    if (!ordered) {
        std::sort(rows->begin(), rows->end(), [](const auto& a, const auto& b) {
            return Render(a) < Render(b);
        });
    }
}

} // namespace

TComparison ExecuteCase(const TExecCase& test, bool canonize) {
    std::vector<std::unique_ptr<TParquetSource>> sources;
    std::istringstream input(ReadAll(test.Sql));
    NSql::TTokenStream tokens(input);
    NSql::TParser parser;
    auto parsed = parser.Parse(tokens);
    if (!parsed) throw std::runtime_error(parsed.error().ToString());
    auto plan = BuildPlan(*parsed, [&](std::string_view table)
        -> std::expected<TOperatorPtr, NQumir::TError>
    {
        const auto binding = test.Tables.find(std::string(table));
        if (binding == test.Tables.end()) {
            throw std::runtime_error("unknown suite table '" + std::string(table) + "'");
        }
        auto& fixture = test.Suite->Fixtures.at(binding->second);
        if (!fixture.Error.empty()) throw std::runtime_error(fixture.Error);
        if (!fixture.File) throw std::runtime_error("fixture was not prepared");
        auto& source = sources.emplace_back(fixture.File->MakeSource());
        return std::make_shared<TSourceOperator>(*source, std::string(table));
    });
    if (!plan) throw std::runtime_error(plan.error().ToString());
    ApplyPlanPasses(*plan);
    NScheduler::TSettings settings;
    settings.Scheduler.Mode = test.Variant == "single"
        ? NScheduler::EExecutionMode::SingleThreadedScheduler
        : NScheduler::EExecutionMode::ThreadedScheduler;
    settings.Scheduler.WorkerCount = test.Variant == "single" ? 1 : 4;
    settings.ScanSplit.Strategy = NScheduler::EScanSplitStrategy::RowGroupRange;
    settings.ScanSplit.MaxScanTasks = test.Variant == "single" ? 1 : 4;
    auto runtime = RunPlan(*plan, settings);
    auto actual = Result(*runtime);
    Normalize(actual, test.Ordered);
    TValue expected = nullptr;
    if (canonize) {
        auto [it, inserted] = test.Suite->Canonized.emplace(test.Golden, actual);
        expected = it->second;
    } else {
        expected = ReadJson(test.Golden);
        if (!expected.getAsObject()) throw std::runtime_error("result golden must be an object");
        Normalize(expected, test.Ordered);
    }
    return {actual == expected, RenderResult(actual), RenderResult(expected)};
}

void CommitExecGoldens(const std::vector<TExecCase>& tests) {
    std::set<TSuite*> seen;
    for (const auto& test : tests) {
        if (!seen.insert(test.Suite.get()).second) continue;
        for (const auto& [path, result] : test.Suite->Canonized) {
            fs::create_directories(path.parent_path());
            std::ofstream output(path, std::ios::binary);
            if (!output || !(output << RenderResult(result))) throw std::runtime_error("cannot write " + path.string());
        }
    }
}

} // namespace NRegTest
