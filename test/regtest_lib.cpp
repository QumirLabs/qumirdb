#include "regtest_lib.h"
#include "mock_source.h"
#include "plan_runner.h"

#include <qdb/catalog/external_module.h>
#include <qdb/io/parquet/source.h>
#include <qdb/plan/build.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/pipeline.h>
#include <qdb/plan/plan_print.h>
#include <qdb/plan/types/decimal.h>
#include <qdb/plan/types/nullable.h>
#include <qdb/sql/parser.h>
#include <qdb/sql/printer.h>
#include <qdb/sexp/printer.h>
#include <qumir/error.h>
#include <qumir/parser/core/parser.h>
#include <qumir/parser/core/printer.h>

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <arrow/util/key_value_metadata.h>
#include <parquet/arrow/writer.h>
#include <parquet/properties.h>

#include <llvm/Support/JSON.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/ADT/SmallString.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <type_traits>
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

void WriteAll(const fs::path& path, const std::string& contents) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    if (!output || !(output << contents)) {
        throw std::runtime_error("cannot write " + path.string());
    }
}

std::string BuildAst(std::istream& input) {
    NSql::TTokenStream tokens(input);
    NSql::TParser parser;
    auto parsed = parser.Parse(tokens);
    if (!parsed) return parsed.error().ToString() + "\n";
    return NSql::PrintAst(*parsed);
}

std::unique_ptr<TMockSource> MakePlanSource(std::string_view table) {
    // Planning resolves and types columns, so plan-only cases need real schemas. One
    // fixed schema per table keeps fixtures independent of the query under test.
    if (table == "t") {
        return std::make_unique<TMockSource>(
            std::vector<std::string>{"a", "b", "c", "id", "k", "v"});
    }
    if (table == "s") {
        return std::make_unique<TMockSource>(std::vector<std::string>{"a", "b", "id"});
    }
    if (table == "u") {
        return std::make_unique<TMockSource>(std::vector<std::string>{"a"});
    }
    if (table == "customer_address") {
        return std::make_unique<TMockSource>(
            std::vector<std::string>{"ca_zip"},
            std::vector<TRowSet>{},
            std::vector<TTypePtr>{std::make_shared<TStringType>()});
    }
    throw std::runtime_error("unknown plan fixture table '" + std::string(table) + "'");
}

// The plan text, or the text of the planning error.
std::expected<std::string, std::string> BuildPlanText(std::istream& input) {
    NSql::TTokenStream tokens(input);
    NSql::TParser parser;
    auto parsed = parser.Parse(tokens);
    if (!parsed) return std::unexpected(parsed.error().ToString() + "\n");
    std::vector<std::unique_ptr<TMockSource>> sources;
    auto factory = [&](std::string_view table)
        -> std::expected<TOperatorPtr, NQumir::TError>
    {
        auto& source = sources.emplace_back(MakePlanSource(table));
        return std::make_shared<TSourceOperator>(*source, std::string(table));
    };
    auto plan = BuildPlan(*parsed, factory);
    if (!plan) return std::unexpected(plan.error().ToString() + "\n");
    std::ostringstream output;
    NSexp::PrintRelPlan(output, *plan);
    output << '\n';
    return output.str();
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
    output << "{\n  \"schema\": " << llvm::formatv("{0}", Required(object, "schema")).str()
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

template <typename TArrowArray>
TObject ComputeNumericStats(const std::string& name, const arrow::Array& column) {
    const auto& array = static_cast<const TArrowArray&>(column);
    using T = decltype(array.Value(0));
    std::vector<T> values;
    for (int64_t row = 0; row < array.length(); ++row) {
        if (array.IsNull(row)) continue;
        const auto value = array.Value(row);
        if constexpr (std::is_floating_point_v<T>) {
            // NaN grouping/equality is not a portable exact-NDV contract.
            if (std::isnan(value)) return TObject{{"name", name}};
        }
        values.push_back(value);
    }
    std::sort(values.begin(), values.end());
    auto unique = values;
    unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
    TObject stats{{"name", name}, {"ndv", static_cast<int64_t>(unique.size())}, {"ndv_exact", true}};
    if constexpr (std::is_same_v<T, bool>) return stats;
    if (values.empty()) return stats;
    if constexpr (std::is_floating_point_v<T>) {
        if (!std::isfinite(values.front()) || !std::isfinite(values.back())) return stats;
    }
    if constexpr (std::is_same_v<T, uint64_t>) {
        // The current metadata reader accepts signed JSON integer boundaries.
        if (values.back() > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) return stats;
    }
    TArray histogram;
    const auto buckets = std::min<size_t>(32, std::max<size_t>(1, values.size() - 1));
    for (size_t bucket = 0; bucket <= buckets; ++bucket) {
        const auto index = bucket * (values.size() - 1) / buckets;
        histogram.push_back(TValue(static_cast<T>(values[index])));
    }
    stats["histogram"] = std::move(histogram);
    return stats;
}

TValue ComputeStats(const arrow::Schema& schema, const std::vector<std::shared_ptr<arrow::Array>>& columns) {
    TArray stats;
    for (size_t i = 0; i < columns.size(); ++i) {
        const auto& column = *columns[i];
        const auto name = schema.field(static_cast<int>(i))->name();
        switch (column.type_id()) {
            case arrow::Type::INT8: stats.push_back(ComputeNumericStats<arrow::Int8Array>(name, column)); break;
            case arrow::Type::INT16: stats.push_back(ComputeNumericStats<arrow::Int16Array>(name, column)); break;
            case arrow::Type::INT32: stats.push_back(ComputeNumericStats<arrow::Int32Array>(name, column)); break;
            case arrow::Type::INT64: stats.push_back(ComputeNumericStats<arrow::Int64Array>(name, column)); break;
            case arrow::Type::UINT8: stats.push_back(ComputeNumericStats<arrow::UInt8Array>(name, column)); break;
            case arrow::Type::UINT16: stats.push_back(ComputeNumericStats<arrow::UInt16Array>(name, column)); break;
            case arrow::Type::UINT32: stats.push_back(ComputeNumericStats<arrow::UInt32Array>(name, column)); break;
            case arrow::Type::UINT64: stats.push_back(ComputeNumericStats<arrow::UInt64Array>(name, column)); break;
            case arrow::Type::DOUBLE: stats.push_back(ComputeNumericStats<arrow::DoubleArray>(name, column)); break;
            case arrow::Type::BOOL: stats.push_back(ComputeNumericStats<arrow::BooleanArray>(name, column)); break;
            case arrow::Type::STRING: {
                const auto& array = static_cast<const arrow::StringArray&>(column);
                std::set<std::string> distinct;
                for (int64_t row = 0; row < array.length(); ++row) {
                    if (!array.IsNull(row)) distinct.insert(array.GetString(row));
                }
                stats.push_back(TObject{{"name", name}, {"ndv", static_cast<int64_t>(distinct.size())}, {"ndv_exact", true}});
                break;
            }
            default: throw std::runtime_error("unsupported column for stats computation");
        }
    }
    return TObject{{"columns", std::move(stats)}};
}

std::shared_ptr<arrow::Table> ReadTable(const fs::path& path) {
    auto json = ReadJson(path);
    const auto& object = Object(json);
    Fields(object, {"version", "schema", "rows", "stats"});
    Version(object);
    std::vector<std::shared_ptr<arrow::Field>> fields;
    std::vector<std::unique_ptr<arrow::ArrayBuilder>> builders;
    const auto schema = ParseSchema(Required(object, "schema"));
    std::set<std::string> names;
    for (const auto& [name, columnType] : schema->Fields) {
        if (name.empty() || !names.insert(name).second) {
            throw std::runtime_error("empty or duplicate schema field name");
        }
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
    auto arrowSchema = arrow::schema(std::move(fields));
    TValue computed = nullptr;
    const TValue* statsValue = object.get("stats");
    if (!statsValue || statsValue->getAsString() == "compute") {
        computed = ComputeStats(*arrowSchema, columns);
        statsValue = &computed;
    } else if (const auto mode = statsValue->getAsString()) {
        if (*mode != "footer") throw std::runtime_error("stats mode must be compute or footer");
        statsValue = nullptr;
    }
    if (statsValue) {
        const auto& stats = Object(*statsValue);
        Fields(stats, {"columns"});
        std::set<std::string> names;
        for (const auto& value : Array(Required(stats, "columns"))) {
            const auto& column = Object(value);
            Fields(column, {"name", "ndv", "ndv_exact", "null_count", "min", "max", "histogram"});
            const auto name = String(Required(column, "name"));
            const auto index = arrowSchema->GetFieldIndex(name);
            if (index < 0 || !names.insert(name).second) throw std::runtime_error("unknown or duplicate stats column '" + name + "'");
            for (const auto key : {"ndv", "null_count"}) {
                if (const auto* count = column.get(key)) {
                    const auto number = count->getAsInteger();
                    if (!number || *number < 0) throw std::runtime_error(std::string(key) + " must be a nonnegative integer");
                }
            }
            if (const auto* exact = column.get("ndv_exact"); exact && !exact->getAsBoolean()) {
                throw std::runtime_error("ndv_exact must be boolean");
            }
            const auto floating = arrowSchema->field(index)->type()->id() == arrow::Type::DOUBLE;
            auto numeric = [&](const TValue& boundary) {
                if (floating) {
                    const auto number = boundary.getAsNumber();
                    if (number && std::isfinite(*number)) return;
                } else if (boundary.getAsInteger()) return;
                throw std::runtime_error("stats boundaries must be finite numbers matching the column type");
            };
            for (const auto key : {"min", "max"}) {
                if (const auto* boundary = column.get(key)) numeric(*boundary);
            }
            if (const auto* histogram = column.get("histogram")) {
                if (arrowSchema->field(index)->type()->id() == arrow::Type::STRING) {
                    throw std::runtime_error("string histograms are not supported");
                }
                for (const auto& boundary : Array(*histogram)) numeric(boundary);
            }
        }
        arrowSchema = arrowSchema->WithMetadata(arrow::key_value_metadata(
            {"stats"}, {llvm::formatv("{0}", *statsValue).str()}));
    }
    return arrow::Table::Make(std::move(arrowSchema), std::move(columns), rowIndex);
}

std::map<std::string, std::string> Bindings(const TObject& object) {
    std::map<std::string, std::string> result;
    for (const auto& [name, value] : object) {
        if (name.str().empty()) throw std::runtime_error("empty table name");
        result.emplace(name.str(), String(value));
    }
    return result;
}

bool BoolOption(const TObject& object, llvm::StringRef name, bool fallback) {
    const auto* value = object.get(name);
    if (!value) return fallback;
    const auto enabled = value->getAsBoolean();
    if (!enabled) throw std::runtime_error(name.str() + " must be a boolean");
    return *enabled;
}

void ValidateTolerance(TFloatTolerance tol) {
    if (!std::isfinite(tol.Absolute) || tol.Absolute < 0
        || !std::isfinite(tol.Relative) || tol.Relative < 0)
    {
        throw std::runtime_error("tol.abs and tol.rel must be finite nonnegative numbers");
    }
}

TFloatTolerance ToleranceOption(const TObject& object, TFloatTolerance fallback) {
    const auto* value = object.get("tol");
    if (!value) return fallback;
    const auto& options = Object(*value);
    Fields(options, {"abs", "rel"});
    auto read = [&](llvm::StringRef name, double& target) {
        if (const auto* setting = options.get(name)) {
            const auto number = setting->getAsNumber();
            if (!number) throw std::runtime_error("tol." + name.str() + " must be a number");
            target = *number;
        }
    };
    read("abs", fallback.Absolute);
    read("rel", fallback.Relative);
    ValidateTolerance(fallback);
    return fallback;
}

// The variants this runner executes. "js" belongs to the browser runner
// (test/test_reg.mjs), which checks the same goldens; a native variant must
// remain, because only native runs canonize them.
std::set<std::string> NativeVariants(const TValue& value) {
    std::set<std::string> variants;
    for (const auto& item : Array(value)) {
        const auto variant = String(item);
        if ((variant != "single" && variant != "threaded" && variant != "js") || !variants.insert(variant).second) {
            throw std::runtime_error("unknown or duplicate execution variant '" + variant + "'");
        }
    }
    variants.erase("js");
    if (variants.empty()) throw std::runtime_error("variants need a native execution variant (single or threaded)");
    return variants;
}

fs::path File(const fs::path& dir, const TValue& value) {
    const auto path = (dir / String(value)).lexically_normal();
    if (!fs::is_regular_file(path)) throw std::runtime_error("missing file " + path.string());
    return path;
}

} // namespace

std::string TestName(const fs::path& path) {
    auto name = path.string();
    for (auto& c : name) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') c = '_';
    }
    return name;
}

fs::path InitializeJitCache(const char* executable, const TRegTestOptions& options) {
    fs::path path;
    if (!options.JitCacheDir.empty()) {
        path = options.JitCacheDir;
    } else {
        static int anchor;
        const auto binary = llvm::sys::fs::getMainExecutable(executable, &anchor);
        if (binary.empty()) throw std::runtime_error("cannot locate test_reg executable");
        path = fs::path(binary).parent_path() / "test_reg_jit_cache";
    }
    fs::create_directories(path);
    return path;
}

void TEngineOverrides::Apply(NScheduler::TSettings& settings) const {
    if (JoinTable) settings.Join.Table = *JoinTable;
}

TRegTestOptions ParseRegTestOptions(int& argc, char** argv) {
    TRegTestOptions options;
    int kept = 1;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        const auto equals = arg.find('=');
        const auto name = arg.substr(0, equals);
        auto value = [&]() -> std::string_view {
            if (equals != std::string_view::npos) return arg.substr(equals + 1);
            if (i + 1 >= argc || std::string_view(argv[i + 1]).starts_with("--")) {
                throw std::invalid_argument(std::string(name) + " requires a value");
            }
            return argv[++i];
        };
        auto flag = [&]() {
            if (equals != std::string_view::npos) {
                throw std::invalid_argument(std::string(name) + " takes no value");
            }
        };
        if (name == "--join-table") {
            const auto text = value();
            options.Engine.JoinTable = NScheduler::ParseJoinTableKind(text);
            if (!options.Engine.JoinTable) {
                throw std::invalid_argument("unknown --join-table value '" + std::string(text)
                    + "'; accepted: " + std::string(NScheduler::JoinTableKindNames));
            }
        } else if (name == "--jit-cache-dir") {
            options.JitCacheDir = value();
        } else if (name == "--arrow-memory-pool") {
            const auto text = value();
            const auto pool = ParseArrowMemoryPool(text);
            if (!pool) {
                throw std::invalid_argument("unknown --arrow-memory-pool value '" + std::string(text)
                    + "'; accepted: " + std::string(ArrowMemoryPoolNames));
            }
            SetArrowMemoryPool(*pool);
        } else if (name == "--timing") {
            flag();
            options.Timing = true;
        } else if (name == "--trace-fixtures") {
            flag();
            options.TraceFixtures = true;
        } else if (name == "--dump-passes") {
            flag();
            options.DumpPasses = true;
        } else if (arg.starts_with("--") && !arg.starts_with("--gtest_")
            && name != "--canonize" && name != "--print" && name != "--help")
        {
            throw std::invalid_argument("unknown option '" + std::string(arg) + "'; known options: "
                "--join-table, --timing, --trace-fixtures, --jit-cache-dir, --dump-passes, "
                "--arrow-memory-pool, --canonize, --print, and the --gtest_ flags");
        } else {
            argv[kept++] = argv[i];
        }
    }
    argc = kept;
    argv[argc] = nullptr;
    return options;
}

TComparison CheckAst(const TQueryCase& test, bool canonize) {
    std::istringstream input(ReadAll(test.Sql));
    const auto actual = BuildAst(input);
    const auto golden = fs::path(test.PlanGolden).replace_extension(".ast");
    if (canonize) WriteAll(golden, actual);
    const auto expected = ReadAll(golden);
    return {actual == expected, actual, expected};
}

struct TSuite {
    struct TFixture {
        fs::path Json;
        fs::path Parquet;
        fs::path PreparedPath;
        int64_t RowGroupRows = 128;
        std::shared_ptr<TParquetFile> File;
        std::string Error;
    };

    fs::path Manifest;
    std::unique_ptr<TTempDir> Temp;
    std::map<std::string, TFixture> Fixtures;
    std::map<fs::path, std::shared_ptr<arrow::Table>> Parsed;
    // Same input/layout, even under two fixture names, shares a generated file.
    std::map<std::pair<fs::path, int64_t>, std::pair<fs::path, std::shared_ptr<TParquetFile>>> Files;
    std::map<fs::path, TValue> Canonized;
    std::map<fs::path, std::string> CanonizedPlans;
    std::map<fs::path, std::string> CanonizedErrors;
    // Result goldens of the cases checked in canonize mode; a filter skips the rest.
    std::set<fs::path> Ran;

    void Prepare(const std::string& name, bool traceFixtures) {
        auto& fixture = Fixtures.at(name);
        if (fixture.File || !fixture.Error.empty()) return;
        try {
            if (!fixture.Parquet.empty()) {
                fixture.File = std::make_shared<TParquetFile>(fixture.Parquet.string());
                fixture.PreparedPath = fixture.Parquet;
                if (traceFixtures) std::cerr << "[regtest-fixture] opened " << fixture.Parquet << '\n';
                return;
            }
            const auto identity = std::make_pair(fixture.Json, fixture.RowGroupRows);
            auto& [path, file] = Files[identity];
            if (!file) {
                auto& table = Parsed[fixture.Json];
                if (!table) {
                    table = ReadTable(fixture.Json);
                    if (traceFixtures) std::cerr << "[regtest-fixture] parsed " << fixture.Json << '\n';
                }
                if (!Temp) Temp = std::make_unique<TTempDir>();
                path = Temp->Path / (std::to_string(Files.size()) + ".parquet");
                auto output = ArrowValue(arrow::io::FileOutputStream::Open(path.string()));
                parquet::WriterProperties::Builder properties;
                properties.compression(parquet::Compression::UNCOMPRESSED);
                properties.disable_dictionary();
                properties.enable_statistics();
                auto writer = ArrowValue(parquet::arrow::FileWriter::Open(*table->schema(),
                    arrow::default_memory_pool(), output, properties.build()));
                if (const auto& metadata = table->schema()->metadata()) {
                    CheckArrow(writer->AddKeyValueMetadata(metadata));
                }
                CheckArrow(writer->WriteTable(*table, fixture.RowGroupRows));
                CheckArrow(writer->Close());
                CheckArrow(output->Close());
                if (traceFixtures) {
                    std::cerr << "[regtest-fixture] generated " << fixture.Json << " -> " << path
                        << " rows=" << table->num_rows() << " row_group_rows=" << fixture.RowGroupRows << '\n';
                }
                file = std::make_shared<TParquetFile>(path.string());
            }
            fixture.File = file;
            fixture.PreparedPath = path;
        } catch (const std::exception& error) {
            fixture.Error = Manifest.string() + ": fixture '" + name + "': " + error.what();
        }
    }
};

std::vector<TQueryCase> CollectCases(const fs::path& root, const fs::path& goldensDir) {
    std::vector<fs::path> manifests;
    std::vector<fs::path> queriesWithoutSuite;
    if (!fs::exists(root)) return {};
    const auto rootDir = fs::absolute(root).lexically_normal();
    const auto goldenRoot = fs::absolute(goldensDir).lexically_normal();
    for (const auto& entry : fs::recursive_directory_iterator(rootDir)) {
        if (entry.is_regular_file() && entry.path().filename() == "suite.json") {
            manifests.push_back(entry.path());
        } else if (entry.is_regular_file() && entry.path().extension() == ".sql"
            && !fs::exists(entry.path().parent_path() / "suite.json"))
        {
            queriesWithoutSuite.push_back(entry.path());
        }
    }
    std::sort(manifests.begin(), manifests.end());
    std::vector<TQueryCase> result;
    for (const auto& sql : queriesWithoutSuite) {
        const auto relative = fs::relative(sql, rootDir);
        result.push_back({fs::path(relative).replace_extension(), sql,
            fs::path(goldenRoot / relative).replace_extension(".plan"),
            fs::path(goldenRoot / relative).replace_extension(".result.json"),
            fs::path(goldenRoot / relative).replace_extension(".error"),
            nullptr, {}, "plan", true});
    }
    for (const auto& manifest : manifests) {
        try {
            auto json = ReadJson(manifest);
            const auto& object = Object(json);
            Fields(object, {"version", "fixtures", "tables", "variants", "cases", "execute", "tol", "expect_error"});
            const bool suiteExpectError = BoolOption(object, "expect_error", false);
            Version(object);
            const bool suiteExecute = BoolOption(object, "execute", true);
            const auto suiteTol = ToleranceOption(object, {});
            auto suite = std::make_shared<TSuite>();
            suite->Manifest = manifest;
            const auto dir = manifest.parent_path();
            for (const auto& [name, value] : Object(Required(object, "fixtures"))) {
                const auto& fixture = Object(value);
                Fields(fixture, {"json", "parquet", "parquet_file"});
                TSuite::TFixture definition;
                const auto* jsonFile = fixture.get("json");
                const auto* parquetFile = fixture.get("parquet_file");
                if (static_cast<bool>(jsonFile) == static_cast<bool>(parquetFile)) {
                    throw std::runtime_error("fixture needs exactly one of json or parquet_file");
                }
                if (jsonFile) definition.Json = File(dir, *jsonFile);
                else {
                    if (fixture.get("parquet")) throw std::runtime_error("parquet_file fixtures cannot have writer options");
                    definition.Parquet = File(dir, *parquetFile);
                }
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
            const auto variants = NativeVariants(Required(object, "variants"));
            std::set<fs::path> queries;
            for (const auto& [name, value] : Object(Required(object, "cases"))) {
                const auto& definition = Object(value);
                Fields(definition, {"tables", "order", "execute", "tol", "variants", "expect_error"});
                const bool execute = BoolOption(definition, "execute", suiteExecute);
                const auto tol = ToleranceOption(definition, suiteTol);
                const bool expectError = BoolOption(definition, "expect_error", suiteExpectError);
                const auto* caseVariants = definition.get("variants");
                const auto executed = caseVariants ? NativeVariants(*caseVariants) : variants;
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
                const auto order = definition.get("order") ? String(*definition.get("order")) : "ordered";
                if (order != "ordered" && order != "unordered") throw std::runtime_error("invalid result order");
                const auto relativeSql = fs::relative(sql, rootDir);
                const auto base = fs::path(relativeSql).replace_extension();
                const auto golden = fs::path(goldenRoot / relativeSql).replace_extension(".result.json");
                const auto planGolden = fs::path(goldenRoot / relativeSql).replace_extension(".plan");
                const auto errorGolden = fs::path(goldenRoot / relativeSql).replace_extension(".error");
                const auto runs = bindings.empty() || !execute
                    ? std::set<std::string>{"plan"} : executed;
                for (const auto& variant : runs) {
                    result.push_back({base, sql, planGolden, golden, errorGolden,
                        suite, bindings, variant, order == "ordered", execute, tol, expectError});
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

void PrepareCases(std::span<const TQueryCase> tests, const TRegTestOptions& options) {
    const auto started = std::chrono::steady_clock::now();
    std::map<TSuite*, std::set<std::string>> selected;
    for (const auto& test : tests) {
        if (!test.Suite) continue;
        for (const auto& [table, fixture] : test.Tables) {
            if (selected[test.Suite.get()].insert(fixture).second) test.Suite->Prepare(fixture, options.TraceFixtures);
        }
    }
    if (options.TraceFixtures) {
        const auto elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        std::cerr << "[regtest-fixture] preparation cases=" << tests.size() << " elapsed_ms=" << elapsed << '\n';
    }
}

void PrepareCase(const TQueryCase& test, const TRegTestOptions& options) {
    PrepareCases(std::span(&test, 1), options);
}

TStatsPtr TableStats(const TQueryCase& test, const std::string& table) {
    const auto& fixture = test.Suite->Fixtures.at(test.Tables.at(table));
    if (!fixture.Error.empty()) throw std::runtime_error(fixture.Error);
    if (!fixture.File) throw std::runtime_error("fixture was not prepared");
    return fixture.File->MakeSource()->Stats();
}

fs::path TableFile(const TQueryCase& test, const std::string& table) {
    const auto& fixture = test.Suite->Fixtures.at(test.Tables.at(table));
    if (!fixture.Error.empty()) throw std::runtime_error(fixture.Error);
    if (!fixture.File) throw std::runtime_error("fixture was not prepared");
    return fixture.PreparedPath;
}

namespace {

struct TParsedQuery {
    NSql::TSqlNodePtr Query;
    NKernel::TAnnotationContext Annotation;
};

// A suite query may follow CREATE MODULE and CREATE FUNCTION definitions, as in
// the CLI and the plan exporter; planning and execution use their catalog.
TParsedQuery ParseQuery(const fs::path& sql) {
    std::istringstream input(ReadAll(sql));
    NSql::TTokenStream tokens(input);
    NSql::TParser parser;
    auto statements = parser.ParseAll(tokens);
    if (!statements) throw statements.error();
    if (statements->empty()) throw std::runtime_error("SQL file has no query");
    TExternalModuleCatalog catalog;
    for (size_t i = 0; i + 1 < statements->size(); ++i) {
        const auto& statement = (*statements)[i];
        std::expected<std::monostate, NQumir::TError> applied;
        if (auto module = NSql::TMaybeNode<NSql::TSqlExternalModule>(statement)) {
            applied = catalog.Apply(*module.Cast());
        } else if (auto function = NSql::TMaybeNode<NSql::TSqlExternalFunction>(statement)) {
            applied = catalog.Apply(*function.Cast());
        } else {
            throw std::runtime_error("only CREATE MODULE and CREATE FUNCTION may precede the query");
        }
        if (!applied) throw applied.error();
    }
    return {
        .Query = statements->back(),
        .Annotation = {.ExternalCatalog = statements->size() > 1 ? catalog.Snapshot() : nullptr},
    };
}

// A text golden. Canonizing a suite case records the text and CommitGoldens
// writes it once every variant agreed; a variant that renders other text than
// the first one fails the comparison.
TComparison CompareText(const TQueryCase& test, const fs::path& golden,
    std::map<fs::path, std::string> TSuite::* canonized, const std::string& actual, bool canonize)
{
    std::string expected;
    if (canonize) {
        if (test.Suite) {
            auto [it, inserted] = ((*test.Suite).*canonized).emplace(golden, actual);
            expected = it->second;
        } else {
            WriteAll(golden, actual);
            expected = actual;
        }
    } else if (!fs::is_regular_file(golden)) {
        return {false, actual, "Missing golden file: " + golden.string() + "\n"};
    } else expected = ReadAll(golden);
    return {actual == expected, actual, expected};
}

TComparison ComparePlan(const TQueryCase& test, const std::string& actual, bool canonize) {
    return CompareText(test, test.PlanGolden, &TSuite::CanonizedPlans, actual, canonize);
}

// A failure of the harness itself (a broken fixture, an unmapped table) is not an
// error of the query: it is never compared with a golden.
class THarnessError : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Runs a stage of a case and returns the text of its error, if any. A TError
// renders as `Error: <message> @ Line: L, Byte: B, Column: C`, like Qumir's
// regression tests. Any other exception has no location and renders as
// `Runtime error: <message>`.
template<typename TFunc>
std::optional<std::string> CaptureError(TFunc&& stage) {
    try {
        stage();
    } catch (const THarnessError&) {
        throw;
    } catch (const NQumir::TError& error) {
        return error.ToString() + "\n";
    } catch (const std::exception& error) {
        return std::string("Runtime error: ") + error.what() + "\n";
    }
    return std::nullopt;
}

} // namespace

namespace {

// The exact text of a DECIMAL, as DuckDB prints it: the scaled integer with the
// decimal point placed `scale` digits from the right. A result is compared as
// this string, so no digit is lost to a binary float.
std::string DecimalText(const char* storage, int32_t scale) {
    __int128 value;
    std::memcpy(&value, storage, sizeof(value));
    const bool negative = value < 0;
    auto magnitude = static_cast<unsigned __int128>(negative ? -value : value);
    std::string digits;
    do {
        digits.push_back(static_cast<char>('0' + static_cast<int>(magnitude % 10)));
        magnitude /= 10;
    } while (magnitude != 0);
    if (digits.size() <= static_cast<size_t>(scale)) digits.resize(scale + 1, '0');
    if (scale > 0) digits.insert(digits.begin() + scale, '.');
    if (negative) digits.push_back('-');
    return std::string(digits.rbegin(), digits.rend());
}

TValue Cell(const TColumn& column, int64_t row, const TTypePtr& type) {
    if (column.Mask) {
        const auto bit = column.MaskBitOffset + row;
        if (!(column.Mask[bit / 8] & (uint8_t{1} << (bit % 8)))) return nullptr;
    }
    const auto valueType = UnwrapNullableType(type);
    if (const auto decimal = DecimalSpecOfValueType(valueType)) {
        return DecimalText(column.Data + row * 16, decimal->Scale);
    }
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
    return TObject{{"schema", PrintSchema(output.Cast())}, {"rows", std::move(rows)}};
}

void Normalize(TValue& value, bool ordered) {
    if (!value.getAsObject()) throw std::runtime_error("result must be an object");
    auto& object = *value.getAsObject();
    Fields(object, {"schema", "rows"});
    const auto schema = ParseSchema(Required(object, "schema"));
    object["schema"] = PrintSchema(schema);
    auto* rows = object.getArray("rows");
    if (!rows) throw std::runtime_error("result rows must be an array");
    for (auto& row : *rows) {
        auto* values = row.getAsArray();
        if (!values || values->size() != schema->Fields.size()) {
            throw std::runtime_error("wrong result row width");
        }
        for (size_t column = 0; column < values->size(); ++column) {
            auto& cell = (*values)[column];
            if (!cell.getAsNull() && TMaybeType<TFloatType>(FieldType(schema->Fields[column].second).first)) {
                cell = FloatJson(Float(cell));
            }
        }
    }
    if (!ordered) {
        std::sort(rows->begin(), rows->end(), [](const auto& a, const auto& b) {
            return Render(a) < Render(b);
        });
    }
}

bool Close(double actual, double expected, TFloatTolerance tol) {
    const double difference = std::abs(actual - expected);
    if (difference <= tol.Absolute) return true;
    const double scale = std::max(std::abs(actual), std::abs(expected));
    // Scale the comparison to avoid overflow in either the difference or the
    // relative threshold, including values with opposite signs near DBL_MAX.
    const double scaledDifference = std::isfinite(difference)
        ? difference / scale : std::abs(actual / scale - expected / scale);
    return scaledDifference <= tol.Absolute / scale + tol.Relative;
}

bool ResultsMatch(const TValue& actual, const TValue& expected, bool ordered, TFloatTolerance tol) {
    ValidateTolerance(tol);
    if (actual == expected) return true;
    const auto& a = Object(actual);
    const auto& e = Object(expected);
    if (Required(a, "schema") != Required(e, "schema")) return false;
    const auto& actualRows = Array(Required(a, "rows"));
    const auto& expectedRows = Array(Required(e, "rows"));
    if (actualRows.size() != expectedRows.size()) return false;
    const auto schema = ParseSchema(Required(a, "schema"));
    std::vector<bool> floats;
    for (const auto& [name, type] : schema->Fields) {
        floats.push_back(static_cast<bool>(TMaybeType<TFloatType>(FieldType(type).first)));
    }
    if (std::none_of(floats.begin(), floats.end(), [](bool value) { return value; })) return false;
    auto rowMatches = [&](const TValue& actualRow, const TValue& expectedRow) {
        const auto& av = Array(actualRow);
        const auto& ev = Array(expectedRow);
        for (size_t column = 0; column < floats.size(); ++column) {
            if (av[column] == ev[column]) continue;
            if (!floats[column]) return false;
            const auto an = av[column].getAsNumber();
            const auto en = ev[column].getAsNumber();
            // NULL and $float tags (NaN, infinities, signed zero) stay exact.
            if (!an || !en || !Close(*an, *en, tol)) return false;
        }
        return true;
    };
    if (ordered) {
        for (size_t row = 0; row < actualRows.size(); ++row) {
            if (!rowMatches(actualRows[row], expectedRows[row])) return false;
        }
        return true;
    }
    // Approximate equality is not transitive: neither sorting and zipping nor
    // greedy pairing can compare unordered results correctly. Find a complete
    // one-to-one matching so that every occurrence, including duplicates, counts.
    const size_t count = actualRows.size();
    std::vector<std::vector<size_t>> candidates(count);
    for (size_t row = 0; row < count; ++row) {
        for (size_t other = 0; other < count; ++other) {
            if (rowMatches(actualRows[row], expectedRows[other])) candidates[row].push_back(other);
        }
        if (candidates[row].empty()) return false;
    }
    std::vector<size_t> matched(count, count);
    std::vector<bool> visited(count);
    auto augment = [&](auto&& self, size_t row) -> bool {
        for (const auto other : candidates[row]) {
            if (!visited[other] && matched[other] == count) {
                visited[other] = true;
                matched[other] = row;
                return true;
            }
        }
        for (const auto other : candidates[row]) {
            if (visited[other]) continue;
            visited[other] = true;
            if (self(self, matched[other])) {
                matched[other] = row;
                return true;
            }
        }
        return false;
    };
    for (size_t row = 0; row < count; ++row) {
        std::fill(visited.begin(), visited.end(), false);
        if (!augment(augment, row)) return false;
    }
    return true;
}

} // namespace

TComparison CompareResults(const std::string& actual, const std::string& expected,
    bool ordered, TFloatTolerance tol)
{
    auto parse = [&](const std::string& text) {
        auto value = llvm::json::parse(text);
        if (!value) throw std::runtime_error(llvm::toString(value.takeError()));
        Normalize(*value, ordered);
        return std::move(*value);
    };
    const auto a = parse(actual);
    const auto e = parse(expected);
    return {ResultsMatch(a, e, ordered, tol), RenderResult(a), RenderResult(e)};
}

TCaseResult CheckCase(const TQueryCase& test, bool canonize, const TRegTestOptions& options) {
    if (!test.HasTables()) {
        std::istringstream input(ReadAll(test.Sql));
        const auto plan = BuildPlanText(input);
        // Cases without a manifest cannot declare `expect_error` and record either outcome.
        if (test.Suite) {
            const std::string where = " (" + test.Sql.string() + ", " + test.Variant + ")";
            if (!plan && !test.ExpectError) {
                throw std::runtime_error("unexpected planning error" + where
                    + "; declare \"expect_error\" if it is intended:\n" + plan.error());
            }
            if (plan && test.ExpectError) {
                throw std::runtime_error("expected a planning error but the plan was built" + where);
            }
        }
        return {ComparePlan(test, plan ? *plan : plan.error(), canonize), std::nullopt, std::nullopt};
    }
    if (canonize) test.Suite->Ran.insert(test.Golden);
    const bool timing = options.Timing;
    const auto started = std::chrono::steady_clock::now();
    auto point = started;
    auto mark = [&]() {
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration<double, std::milli>(now - point).count();
        point = now;
        return elapsed;
    };
    std::vector<std::unique_ptr<TParquetSource>> sources;
    std::optional<TParsedQuery> parsed;
    TOperatorPtr plan;
    // Parsing, binding and optimization are the planning stage: its error is the
    // plan output of an invalid query, as in plan-only suites.
    double parseMs = 0;
    double buildMs = 0;
    double optimizeMs = 0;
    const auto planError = CaptureError([&] {
        parsed.emplace(ParseQuery(test.Sql));
        parseMs = mark();
        auto built = BuildPlan(parsed->Query, [&](std::string_view table)
            -> std::expected<TOperatorPtr, NQumir::TError>
        {
            const auto binding = test.Tables.find(std::string(table));
            if (binding == test.Tables.end()) {
                throw THarnessError("unknown suite table '" + std::string(table) + "'");
            }
            auto& fixture = test.Suite->Fixtures.at(binding->second);
            if (!fixture.Error.empty()) throw THarnessError(fixture.Error);
            if (!fixture.File) throw THarnessError("fixture was not prepared");
            auto& source = sources.emplace_back(fixture.File->MakeSource());
            return std::make_shared<TSourceOperator>(*source, std::string(table));
        }, parsed->Annotation);
        buildMs = mark();
        if (!built) throw built.error();
        plan = std::move(*built);
        ApplyPlanPasses(plan, {
            .Annotation = parsed->Annotation,
            .DumpPasses = options.DumpPasses ? &std::cerr : nullptr,
        });
        optimizeMs = mark();
    });
    const std::string where = " (" + test.Sql.string() + ", " + test.Variant + ")";
    if (planError && !test.ExpectError) {
        // Canonizing must not turn a case into an error golden.
        throw std::runtime_error("unexpected planning error" + where
            + "; declare \"expect_error\" if it is intended:\n" + *planError);
    }
    std::ostringstream planOutput;
    if (planError) {
        planOutput << *planError;
    } else {
        PrintPlanTreeWithCtes(planOutput, plan);
    }
    const auto planComparison = ComparePlan(test, planOutput.str(), canonize);
    const auto planGoldenMs = mark();
    if (planError && test.ExpectError) {
        return {planComparison, std::nullopt, std::nullopt};
    }
    if (!planError && !test.Execute && test.ExpectError) {
        throw std::runtime_error("expected a planning error but the plan was built" + where);
    }
    auto printTiming = [&](const TPlanRunTimings& runTimings, double resultGoldenMs) {
        if (!timing) return;
        const auto totalMs = std::chrono::duration<double, std::milli>(point - started).count();
        std::cerr << "[regtest-timing] query=" << test.Base.string() << " variant=" << test.Variant
            << " read_parse_ms=" << parseMs << " build_plan_ms=" << buildMs
            << " optimize_ms=" << optimizeMs << " plan_golden_ms=" << planGoldenMs
            << " lower_ms=" << runTimings.LowerMs << " jit_ms=" << runTimings.JitMs
            << " execute_ms=" << runTimings.ExecuteMs << " result_golden_ms=" << resultGoldenMs
            << " total_ms=" << totalMs << '\n';
    };
    if (!test.Execute) {
        printTiming({}, 0);
        return {planComparison, std::nullopt, std::nullopt};
    }
    NScheduler::TSettings settings;
    settings.Scheduler.Mode = test.Variant == "single"
        ? NScheduler::EExecutionMode::SingleThreadedScheduler
        : NScheduler::EExecutionMode::ThreadedScheduler;
    settings.Scheduler.WorkerCount = test.Variant == "single" ? 1 : 4;
    settings.ScanSplit.Strategy = NScheduler::EScanSplitStrategy::RowGroupRange;
    settings.ScanSplit.MaxScanTasks = test.Variant == "single" ? 1 : 4;
    options.Engine.Apply(settings);
    settings.Jit.CacheDir = options.JitCacheDir;
    TPlanRunTimings runTimings;
    TValue actual = nullptr;
    // Lowering, JIT compilation and the run itself.
    const auto runError = CaptureError([&] {
        auto runtime = RunPlan(
            plan, settings, parsed->Annotation.ExternalCatalog, nullptr, timing ? &runTimings : nullptr);
        mark();
        actual = Result(*runtime);
    });
    if (runError) {
        if (!test.ExpectError) {
            throw std::runtime_error("unexpected error" + where
                + "; declare \"expect_error\" if it is intended:\n" + *runError);
        }
        return {planComparison, std::nullopt,
            CompareText(test, test.ErrorGolden, &TSuite::CanonizedErrors, *runError, canonize)};
    }
    if (test.ExpectError) {
        throw std::runtime_error("expected an error but the case succeeded" + where);
    }
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
    auto comparison = TComparison{ResultsMatch(actual, expected, test.Ordered, test.Tol),
        RenderResult(actual), RenderResult(expected)};
    const auto resultGoldenMs = mark();
    printTiming(runTimings, resultGoldenMs);
    return {planComparison, std::move(comparison), std::nullopt};
}

void CommitGoldens(const std::vector<TQueryCase>& tests) {
    std::set<TSuite*> seen;
    for (const auto& test : tests) {
        if (!test.Suite) continue;
        if (!seen.insert(test.Suite.get()).second) continue;
        for (const auto& [path, result] : test.Suite->Canonized) {
            WriteAll(path, RenderResult(result));
        }
        for (const auto& [path, plan] : test.Suite->CanonizedPlans) WriteAll(path, plan);
        for (const auto& [path, error] : test.Suite->CanonizedErrors) WriteAll(path, error);
    }
    // A case holds a result or an error, never both: when the outcome changes,
    // the golden of the previous one is stale. Report what is removed.
    std::set<fs::path> removed;
    for (const auto& test : tests) {
        if (!test.Suite) continue;
        const auto& suite = *test.Suite;
        if (!suite.Ran.contains(test.Golden)) continue;
        const bool hasResult = suite.Canonized.contains(test.Golden);
        const bool hasError = suite.CanonizedErrors.contains(test.ErrorGolden);
        for (const auto& [stale, wanted] : {std::pair{test.Golden, hasResult},
                std::pair{test.ErrorGolden, hasError}})
        {
            if (!wanted && fs::exists(stale) && removed.insert(stale).second) {
                fs::remove(stale);
                std::cerr << "[regtest] removed stale golden " << stale.string() << '\n';
            }
        }
    }
}

void RemoveOrphanGoldens(const std::vector<TQueryCase>& tests, const fs::path& goldensDir) {
    if (!fs::exists(goldensDir)) return;
    std::set<fs::path> owned;
    for (const auto& test : tests) {
        owned.insert(test.PlanGolden);
        owned.insert(fs::path(test.PlanGolden).replace_extension(".ast"));
        owned.insert(test.Golden);
        owned.insert(test.ErrorGolden);
    }
    const auto root = fs::absolute(goldensDir).lexically_normal();
    std::set<fs::path> orphans;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (entry.is_regular_file() && !owned.contains(entry.path())) orphans.insert(entry.path());
    }
    for (const auto& orphan : orphans) {
        fs::remove(orphan);
        std::cerr << "[regtest] removed orphan golden " << orphan.string() << '\n';
    }
}

} // namespace NRegTest
