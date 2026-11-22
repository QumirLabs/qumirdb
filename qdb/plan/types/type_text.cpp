#include <qdb/plan/types/type_text.h>

#include <qdb/plan/types/date.h>
#include <qdb/plan/types/decimal.h>
#include <qdb/plan/types/nullable.h>

#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>
#include <qumir/parser/core/printer.h>

#include <expected>
#include <sstream>
#include <stdexcept>

namespace NQdb {

namespace {

using namespace NQumir::NAst;

using TArgs = std::vector<TGenericArg>;

const NCore::TPrintOptions& PrintOptions() {
    static const NCore::TPrintOptions options = [] {
        NCore::TPrintOptions result;
        result.Pretty = false;
        result.ShortNamedTypes = PlanNamedTypes();
        return result;
    }();
    return options;
}

std::expected<TTypePtr, std::string> BuildNullable(TTypeFactory& types, TArgs args) {
    if (args.size() != 1 || args[0].Kind != TGenericArg::EKind::Type || !args[0].Type) {
        return std::unexpected("Nullable expects one type argument");
    }
    return MakeNullableType(types, args[0].Type);
}

std::expected<TTypePtr, std::string> BuildDate(TTypeFactory& types, TArgs args) {
    if (args.empty()) {
        return DateCastTarget(types);
    }
    auto maybeStorage = args.size() == 1 && args[0].Kind == TGenericArg::EKind::Type
        ? TMaybeType<TIntegerType>(args[0].Type)
        : TMaybeType<TIntegerType>(nullptr);
    if (!maybeStorage) {
        return std::unexpected("DATE expects an i32 or i64 storage argument");
    }
    const auto kind = maybeStorage.Cast()->Kind;
    if (kind != TIntegerType::I32 && kind != TIntegerType::I64) {
        return std::unexpected("DATE expects an i32 or i64 storage argument");
    }
    return MakeDateType(types, kind);
}

std::expected<TTypePtr, std::string> BuildDecimal(TTypeFactory& types, TArgs args) {
    if (args.size() != 2 || args[0].Kind != TGenericArg::EKind::Value ||
        args[1].Kind != TGenericArg::EKind::Value)
    {
        return std::unexpected("DECIMAL expects precision and scale value arguments");
    }
    auto precision = ParseDecimalArg(args[0].Value);
    auto scale = ParseDecimalArg(args[1].Value);
    if (!precision || !scale) {
        return std::unexpected("DECIMAL precision and scale must be integers");
    }
    try {
        return MakeDecimalType(types, {.Precision = *precision, .Scale = *scale});
    } catch (const std::invalid_argument& e) {
        return std::unexpected(e.what());
    }
}

const NCore::TTypeParserMap& TypeParsers() {
    // "Decimal" is the spelling the plan exporter schemas use.
    static const NCore::TTypeParserMap parsers = {
        {"Nullable", BuildNullable},
        {"DATE", BuildDate},
        {"DECIMAL", [](TTypeFactory& types, TArgs args) {
            // A bare DECIMAL means the default precision and scale.
            return args.empty()
                ? std::expected<TTypePtr, std::string>(MakeDecimalType(types, {}))
                : BuildDecimal(types, std::move(args));
        }},
        {"Decimal", BuildDecimal},
    };
    return parsers;
}

} // namespace

const std::set<std::string>& PlanNamedTypes() {
    static const std::set<std::string> names = {"Nullable", "DATE", "StringView", "OwnedString", "BinInt"};
    return names;
}

const TStructType* RecordFields(const TTypePtr& type) {
    auto maybeNamed = TMaybeType<TNamedType>(type);
    if (!maybeNamed) {
        return TMaybeType<TStructType>(type).Cast();
    }
    auto named = maybeNamed.Cast();
    return PlanNamedTypes().contains(named->Name)
        ? nullptr
        : TMaybeType<TStructType>(named->UnderlyingType()).Cast();
}

std::string SqlTypeName(const TTypePtr& type) {
    const TTypePtr value = UnwrapNullableType(type);
    if (IsDateType(value)) {
        return "DATE";
    }
    if (auto decimal = DecimalSpecOfValueType(value)) {
        return "DECIMAL(" + std::to_string(decimal->Precision) + "," + std::to_string(decimal->Scale) + ")";
    }
    const TTypePtr plain = UnwrapNamedValueType(value);
    if (auto integer = TMaybeType<TIntegerType>(plain)) {
        return integer.Cast()->BitWidth() == 64 ? "BIGINT" : "INTEGER";
    }
    if (auto floating = TMaybeType<TFloatType>(plain)) {
        return floating.Cast()->Kind == TFloatType::F32 ? "REAL" : "DOUBLE";
    }
    return TypeDiagnosticName(value);
}

std::string PrintTypeText(const TTypePtr& type) {
    return NCore::PrintType(type, PrintOptions());
}

TTypePtr ParseTypeText(NQumir::NAst::TTypeFactory& types, std::string_view text) {
    std::istringstream input{std::string(text)};
    NCore::TTokenStream tokens(input);
    auto parsed = NCore::ParseType(tokens, TypeParsers(), types);
    if (!parsed) {
        throw std::runtime_error(
            "invalid type '" + std::string(text) + "': " + parsed.error().ToString());
    }
    if (!tokens.Next().IsEof()) {
        throw std::runtime_error(
            "invalid type '" + std::string(text) + "': unexpected trailing input");
    }
    return *parsed;
}

} // namespace NQdb
