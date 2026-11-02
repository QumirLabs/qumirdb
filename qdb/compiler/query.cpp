#include "query.h"

#include <qdb/plan/ops/cte_consumer.h>
#include <qdb/plan/plan_print.h>
#include <qdb/sexp/parser.h>
#include <qdb/sexp/printer.h>
#include <qdb/sql/parser.h>

#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>

#include <cctype>
#include <iterator>
#include <sstream>

namespace NQdb {
namespace NCompiler {
namespace {

std::expected<TOperatorPtr, NQumir::TError> ParseSexpr(
    std::istream& in,
    const TTableSourceFactory& makeSource)
{
    std::optional<NQumir::TError> sourceError;
    NSexp::TRelParserOptions opts;
    opts.SourceFactory = [&](std::string_view path, NQumir::TLocation) {
        auto source = makeSource(path);
        if (!source) {
            sourceError = source.error();
            return TOperatorPtr{};
        }
        return *source;
    };

    NQumir::NAst::NCore::TParser parser;
    for (auto& [key, value] : NSexp::MakeRelParsers(std::move(opts))) {
        parser.NodeParsers[key] = std::move(value);
    }

    NQumir::NAst::NCore::TTokenStream tokens(in);
    auto parsed = parser.Parse(tokens);
    if (sourceError) {
        return std::unexpected(*sourceError);
    }
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    auto plan = std::dynamic_pointer_cast<IOperator>(*parsed);
    if (!plan) {
        return std::unexpected(NQumir::TError("input must be a relational plan (rel ...)"));
    }
    return plan;
}

std::expected<std::vector<NSql::TSqlNodePtr>, NQumir::TError>
ParseSqlStatements(std::istream& in)
{
    NSql::TTokenStream tokens(in);
    NSql::TParser parser;
    return parser.ParseAll(tokens);
}

bool StripExplain(std::string& text) {
    size_t start = text.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return false;
    }
    constexpr std::string_view keyword = "explain";
    if (text.size() - start < keyword.size()) {
        return false;
    }
    for (size_t i = 0; i < keyword.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(text[start + i])) != keyword[i]) {
            return false;
        }
    }
    size_t after = start + keyword.size();
    if (after < text.size() && !std::isspace(static_cast<unsigned char>(text[after]))) {
        return false;
    }
    text = text.substr(after);
    return true;
}

} // namespace

std::expected<TParsedQuery, NQumir::TError> TQueryCompiler::Parse(
    std::istream& input,
    ESyntax syntax) const
{
    std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    TParsedQuery query;
    query.Explain = StripExplain(text);
    std::istringstream stream(text);
    if (syntax == ESyntax::Sql) {
        auto statements = ParseSqlStatements(stream);
        if (!statements) {
            return std::unexpected(statements.error());
        }
        if (query.Explain && statements->size() != 1) {
            return std::unexpected(NQumir::TError("EXPLAIN requires exactly one SELECT statement"));
        }
        for (auto& statement : *statements) {
            query.Statements.emplace_back(std::move(statement));
        }
    } else {
        auto plan = ParseSexpr(stream, Sources_);
        if (!plan) {
            return std::unexpected(plan.error());
        }
        query.Statements.emplace_back(std::move(*plan));
    }
    return query;
}

TQueryCompiler::TQueryCompiler(
    TExternalModuleCatalog& catalog,
    TTableSourceFactory sources,
    TPlanPassOptions options)
    : Catalog_(catalog)
    , Sources_(std::move(sources))
    , Options_(std::move(options))
{}

std::expected<std::optional<TPreparedQuery>, NQumir::TError> TQueryCompiler::Prepare(
    const TStatement& statement,
    bool explain) const
{
    TPreparedQuery prepared;
    if (auto sql = std::get_if<NSql::TSqlNodePtr>(&statement)) {
        auto module = NSql::TMaybeNode<NSql::TSqlExternalModule>(*sql);
        auto function = NSql::TMaybeNode<NSql::TSqlExternalFunction>(*sql);
        if (module || function) {
            if (explain) {
                return std::unexpected(NQumir::TError("EXPLAIN requires a SELECT statement"));
            }
            auto applied = module
                ? Catalog_.Apply(*module.Cast())
                : Catalog_.Apply(*function.Cast());
            if (!applied) {
                return std::unexpected(applied.error());
            }
            return std::nullopt;
        }
        prepared.Annotation.ExternalCatalog = Catalog_.Snapshot();
        auto plan = BuildPlan(*sql, Sources_, prepared.Annotation);
        if (!plan) {
            return std::unexpected(plan.error());
        }
        prepared.Plan = std::move(*plan);
    } else {
        prepared.Plan = std::get<TOperatorPtr>(statement);
    }
    auto options = Options_;
    options.Annotation = prepared.Annotation;
    options.Diagnostics = &prepared.Diagnostics;
    ApplyPlanPasses(prepared.Plan, std::move(options));
    return prepared;
}

void Explain(std::ostream& output, const TOperatorPtr& plan, EExplainMode mode) {
    const bool sexprAvailable = CollectMaterializations(plan).empty();
    const bool wantSexpr = mode != EExplainMode::Text && sexprAvailable;
    const bool wantText = mode != EExplainMode::Sexpr || !sexprAvailable;
    if (wantSexpr) {
        NSexp::PrintRelPlan(output, plan);
        output << (wantText ? "\n\n" : "\n");
    }
    if (wantText) {
        PrintPlanTreeWithCtes(output, plan);
    }
}

} // namespace NCompiler
} // namespace NQdb
