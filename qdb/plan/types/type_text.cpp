#include <qdb/plan/types/type_text.h>

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
        result.TypePrinters[TNullable::TypeId] =
            [](TType& type, NCore::TPrinter& printer, int level) {
                TNamedType named(
                    "Nullable", nullptr,
                    {TGenericArg::TypeArg(static_cast<TNullable&>(type).UnderlyingType)});
                printer.PrintType(std::make_shared<TNamedType>(std::move(named)), level);
            };
        return result;
    }();
    return options;
}

std::expected<TTypePtr, std::string> BuildNullable(TArgs args) {
    if (args.size() != 1 || args[0].Kind != TGenericArg::EKind::Type || !args[0].Type) {
        return std::unexpected("Nullable expects one type argument");
    }
    return std::make_shared<TNullable>(std::move(args[0].Type));
}

std::expected<TTypePtr, std::string> BuildDecimal(TArgs args) {
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
        return std::make_shared<TDecimal>(*precision, *scale);
    } catch (const std::invalid_argument& e) {
        return std::unexpected(e.what());
    }
}

const NCore::TTypeParserMap& TypeParsers() {
    // "Decimal" is the spelling the plan exporter schemas use.
    static const NCore::TTypeParserMap parsers = {
        {"Nullable", BuildNullable},
        {"DECIMAL", [](TArgs args) {
            // A bare DECIMAL means the default precision and scale.
            return args.empty() ? std::expected<TTypePtr, std::string>(std::make_shared<TDecimal>())
                                : BuildDecimal(std::move(args));
        }},
        {"Decimal", BuildDecimal},
    };
    return parsers;
}

} // namespace

std::string PrintTypeText(const TTypePtr& type) {
    return NCore::PrintType(type, PrintOptions());
}

TTypePtr ParseTypeText(std::string_view text) {
    std::istringstream input{std::string(text)};
    NCore::TTokenStream tokens(input);
    auto parsed = NCore::ParseType(tokens, TypeParsers());
    if (!parsed) {
        throw std::runtime_error(
            "invalid type '" + std::string(text) + "': " + parsed.error().ToString());
    }
    if (!tokens.Next().IsEof()) {
        throw std::runtime_error(
            "invalid type '" + std::string(text) + "': unexpected trailing input");
    }
    return std::move(*parsed);
}

} // namespace NQdb
