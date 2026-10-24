#include "parser.h"

#include <qdb/modules/qumirdb_runtime.h>
#include <qdb/plan/types/decimal.h>

#include <qumir/optional.h>

#include <cctype>
#include <map>
#include <set>
#include <stdexcept>
#include <string>

/*

<statement> ::=
    ( <select_stmt> | <create_module_stmt> | <create_function_stmt> ) [ ";" ]

<create_module_stmt> ::=
    "CREATE" [ "OR" "REPLACE" ] "MODULE" <ident>
    "LANGUAGE" <ident>
    "AS" <dollar_quoted_source>

// A single token produced by the lexer; its value is the text between delimiters.
<dollar_quoted_source> ::=
    "$$" { any_char_sequence_not_containing_terminating_"$$" } "$$"

<create_function_stmt> ::=
    "CREATE" [ "OR" "REPLACE" ] "FUNCTION" <ident>
    "(" [ <function_arg> { "," <function_arg> } ] ")"
    "RETURNS" <function_return_type>
    <external_function_options>

<function_arg> ::=
    [ <ident> ] <type_name>

<function_return_type> ::=
      <type_name>
    | "(" <type_name> { "," <type_name> } ")"

// MODULE is required. SYMBOL is optional and defaults to the SQL function name.
// When both are present, either order is accepted.
<external_function_options> ::=
      <set_module_clause> [ <set_symbol_clause> ]
    | <set_symbol_clause> <set_module_clause>

<set_module_clause> ::=
    "SET" "MODULE" ( "TO" | "=" ) <ident>

<set_symbol_clause> ::=
    "SET" "SYMBOL" ( "TO" | "=" ) <ident>

<select_stmt> ::=
    [ <with_clause> ]
    <query_expr>
    [ <order_by_clause> ]
    [ <limit_clause> ]
    [ <offset_clause> ]

<query_expr> ::=
    <query_term>
    {
        ( "UNION" | "EXCEPT" )
        [ <set_quantifier> ]
        <query_term>
    }

<query_term> ::=
    <query_primary>
    {
        "INTERSECT"
        [ <set_quantifier> ]
        <query_primary>
    }

<query_primary> ::=
      <select_core>
    | "(" <select_stmt> ")"

<set_quantifier> ::= "ALL" | "DISTINCT"

<with_clause> ::=
    "WITH" [ "RECURSIVE" ] <cte_def> { "," <cte_def> }

<cte_def> ::=
    <ident> [ "(" <ident_list> ")" ] "AS" "(" <select_stmt> ")"

<select_core> ::=
    "SELECT" [ <set_quantifier> ] <select_list>
    [ <from_clause> ]
    [ <where_clause> ]
    [ <group_by_clause> ]
    [ <having_clause> ]

<set_quantifier> ::= "ALL" | "DISTINCT"

<select_list> ::=
      "*"
    | <select_item> { "," <select_item> }

<select_item> ::=
      <expr> [ [ "AS" ] <ident> | "AS" "(" <ident> { "," <ident> } ")" ]
    | <qualified_name> "." "*"

<from_clause> ::=
    "FROM" <table_ref> { "," <table_ref> }

<table_ref> ::=
    <table_factor> { <join_clause> }

<table_factor> ::=
      <qualified_name> [ [ "AS" ] <ident> ]
    | "(" <select_stmt> ")" [ [ "AS" ] <ident> ]
    | "(" <table_ref> ")"

<join_clause> ::=
    [ <join_type> ] "JOIN" <table_factor> <join_condition>
    | "NATURAL" [ <join_type> ] "JOIN" <table_factor>

<join_type> ::=
      "INNER"
    | "LEFT" [ "OUTER" ]
    | "RIGHT" [ "OUTER" ]
    | "FULL" [ "OUTER" ]
    | "CROSS"

<join_condition> ::=
      "ON" <expr>
    | "USING" "(" <ident_list> ")"
    | <empty>

<where_clause> ::= "WHERE" <expr>

<group_by_clause> ::=
    "GROUP" "BY" <grouping_element> { "," <grouping_element> }

<grouping_element> ::=
      <expr>
    | "ROLLUP" "(" [ <expr_list> ] ")"
    | "CUBE" "(" [ <expr_list> ] ")"
    | "GROUPING" "SETS" "(" <grouping_set> { "," <grouping_set> } ")"

<grouping_set> ::=
      "(" [ <expr_list> ] ")"
    | <expr>
    | "ROLLUP" "(" [ <expr_list> ] ")"
    | "CUBE" "(" [ <expr_list> ] ")"

<having_clause> ::= "HAVING" <expr>

<order_by_clause> ::=
    "ORDER" "BY" <order_item> { "," <order_item> }

<order_item> ::=
    <expr> [ "ASC" | "DESC" ] [ "NULLS" ( "FIRST" | "LAST" ) ]

<limit_clause> ::= "LIMIT" <expr>

<offset_clause> ::= "OFFSET" <expr>

// exprs:
<expr> ::= <or_expr>

<or_expr> ::=
    <and_expr> { "OR" <and_expr> }

<and_expr> ::=
    <not_expr> { "AND" <not_expr> }

<not_expr> ::=
      "NOT" <not_expr>
    | <comparison_expr>

<comparison_expr> ::=
    <add_expr>
    [
        <comparison_op> <concat_expr>
      | "IS" [ "NOT" ] "NULL"
      | [ "NOT" ] "IN" "(" <expr_list_or_subquery> ")"
      | [ "NOT" ] "BETWEEN" <concat_expr> "AND" <concat_expr>
      | [ "NOT" ] "LIKE" <concat_expr>
    ]

<comparison_op> ::=
      "=" | "<>" | "!="
    | "<" | "<=" | ">" | ">="

<concat_expr> ::=
    <add_expr> { "||" <add_expr> }

<add_expr> ::=
    <mul_expr> { ( "+" | "-" ) <mul_expr> }

<mul_expr> ::=
    <unary_expr> { ( "*" | "/" | "%" ) <unary_expr> }

<unary_expr> ::=
      ( "+" | "-" ) <unary_expr>
    | <postfix_expr>

<postfix_expr> ::=
    <primary_expr>
    [ "OVER" <window_spec> ]
    {
        "::" <type_name>
    }

<window_spec> ::=
    "("
        [ "PARTITION" "BY" <expr_list> ]
        [ <order_by_clause> ]
        [ <window_frame> ]
    ")"

<window_frame> ::=
    ( "ROWS" | "RANGE" )
    (
        <frame_bound>
      | "BETWEEN" <frame_bound> "AND" <frame_bound>
    )

<frame_bound> ::=
      "UNBOUNDED" "PRECEDING"
    | "UNBOUNDED" "FOLLOWING"
    | "CURRENT" "ROW"
    | <expr> "PRECEDING"
    | <expr> "FOLLOWING"

<primary_expr> ::=
      <literal>
    | <qualified_name>
    | <function_call>
    | <case_expr>
    | "CAST" "(" <expr> "AS" <type_name> ")"
    | "EXISTS" "(" <select_stmt> ")"
    | "GROUPING" "(" <expr> ")"
    | "(" <expr> ")"
    | "(" <select_stmt> ")"

<function_call> ::=
    <qualified_name> "(" [ <function_args> ] ")"

<function_args> ::=
      "*"
    | <expr> { "," <expr> }

<case_expr> ::=
    "CASE" [ <expr> ]
        { "WHEN" <expr> "THEN" <expr> }
        [ "ELSE" <expr> ]
    "END"

<expr_list_or_subquery> ::=
      <select_stmt>
    | <expr> { "," <expr> }

<expr_list> ::= <expr> { "," <expr> }

// literals:
<qualified_name> ::= <ident> { "." <ident> }

<ident_list> ::= <ident> { "," <ident> }

<ident> ::= <regular_ident> | <quoted_ident>

<regular_ident> ::= /[A-Za-z_][A-Za-z0-9_]*/

/*
<quoted_ident> ::= '"' { any_char_except_quote | '""' } '"'

<literal> ::=
      <number_literal>
    | <string_literal>
    | "NULL"
    | "TRUE"
    | "FALSE"

<number_literal> ::= /[0-9]+(\.[0-9]+)?([eE][+-]?[0-9]+)?/

<string_literal> ::= "'" { any_char_except_quote | "''" } "'"

<type_name> ::=
      "BOOL"
    | "BOOLEAN"
    | "INT"
    | "INTEGER"
    | "BIGINT"
    | "FLOAT"
    | "DOUBLE"
    | "TEXT"
    | "VARCHAR"
    | "DATE"
    | "TIMESTAMP"
    | "DECIMAL" [ "(" <integer> [ "," <integer> ] ")" ]

<integer> ::= /[0-9]+/

*/

namespace NQdb {
namespace NSql {

using NQumir::TError;
using NQumir::TLocation;
using NQumir::NAst::TOperator;
using NQumir::NAst::TToken;
using NQumir::NAst::TWrappedTokenStream;

using namespace NQumir::NAst::NLiterals;

namespace {

template<class T>
using TAstTask = NQumir::TExpectedTask<TSqlPtr<T>, TError, TLocation>;

using TAstExprTask = NQumir::TExpectedTask<NQumir::NAst::TExprPtr, TError, TLocation>;
using TAstTypeTask = NQumir::TExpectedTask<NQumir::NAst::TTypePtr, TError, TLocation>;
using TVoidTask = NQumir::TExpectedTask<std::monostate, TError, TLocation>;

struct TParserContext {
    TWrappedTokenStream& Stream;

    TParserContext(TWrappedTokenStream& stream)
        : Stream(stream)
    { }
};

TError Error(const TToken& token, const std::string& message) {
    return TError(token.Location, message);
}

bool IsEof(const TToken& token) {
    return token.Type == TToken::Operator && token.Value.i64 == -1;
}

bool IsOp(const TToken& token, char op) {
    return token.Type == TToken::Operator && token.Value.i64 == static_cast<int64_t>(op);
}

bool IsOp(const TToken& token, TOperator op) {
    return token.Type == TToken::Operator && token.Value.i64 == static_cast<int64_t>(op);
}

bool IsKeyword(const TToken& tok, const std::string& kw) {
    return tok.Type == TToken::Keyword && tok.Name == kw;
}

std::string ToLowerStr(std::string s) {
    for (auto& c : s) {
        c = std::tolower(c);
    }
    return s;
}

bool IsWord(const TToken& tok, const std::string& word) {
    if (tok.Type == TToken::Keyword) {
        return tok.Name == word;
    }
    if (tok.Type == TToken::Identifier) {
        return tok.Name == ToLowerStr(word);
    }
    return false;
}

bool IsIntervalUnit(const std::string& name) {
    std::string u = ToLowerStr(name);
    return u == "day" || u == "days" || u == "month" || u == "months"
        || u == "year" || u == "years";
}

NQumir::NAst::TExprPtr binary(TLocation loc, TOperator op, NQumir::NAst::TExprPtr left, NQumir::NAst::TExprPtr right)
{
    return std::make_shared<NQumir::NAst::TBinaryExpr>(loc, op, left, right);
}

NQumir::NAst::TExprPtr unary(TLocation loc, TOperator op, NQumir::NAst::TExprPtr expr)
{
    return std::make_shared<NQumir::NAst::TUnaryExpr>(loc, op, expr);
}

NQumir::NAst::TExprPtr call(TLocation loc, const std::string& name, std::vector<NQumir::NAst::TExprPtr> args) {
    return std::make_shared<NQumir::NAst::TCallExpr>(loc,
        std::make_shared<NQumir::NAst::TIdentExpr>(loc, name),
        std::move(args)
    );
}

NQumir::NAst::TExprPtr i32_literal(TLocation loc, int32_t value) {
    auto ret = std::make_shared<NQumir::NAst::TNumberExpr>(loc, static_cast<int64_t>(value));
    ret->Type = std::make_shared<NQumir::NAst::TIntegerType>(NQumir::NAst::TIntegerType::I32);
    return ret;
}

NQumir::NAst::TExprPtr cast(TLocation loc, NQumir::NAst::TExprPtr expr, NQumir::NAst::TTypePtr type) {
    return std::make_shared<NQumir::NAst::TCastExpr>(loc, std::move(expr), std::move(type));
}

// Wraps a string's bytes as a qdb_string_view for the parse-time date/interval
// constant folders (the referenced string must outlive the call).
qdb_string_view AsStringView(const std::string& s) {
    return {reinterpret_cast<const uint8_t*>(s.data()), static_cast<int64_t>(s.size())};
}

NQumir::NAst::TExprPtr list(TLocation loc, std::vector<NQumir::NAst::TExprPtr> items) {
    return std::make_shared<NQumir::NAst::TBlockExpr>(loc, std::move(items));
}

NQumir::NAst::TExprPtr BuildInListPredicate(
    TLocation loc,
    NQumir::NAst::TExprPtr lhs,
    const std::vector<NQumir::NAst::TExprPtr>& items)
{
    std::vector<NQumir::NAst::TExprPtr> args;
    args.reserve(items.size() + 1);
    args.push_back(std::move(lhs));
    for (const auto& item : items) {
        args.push_back(item);
    }
    // Keep the IN boundary until kernel type expansion. Expanding to OR here loses
    // the fact that one nullable lhs guard can cover every comparison in the list.
    return call(loc, "qdb_in_list", std::move(args));
}

// builds a column reference as a single dotted ident (a.b.c -> "a.b.c"), the
// qualified form the logical layer (QualifyColumns, equijoin, pushdown) expects.
NQumir::NAst::TExprPtr qualified_ref(TLocation loc, const std::vector<std::string>& parts) {
    std::string name = parts.front();
    for (size_t i = 1; i < parts.size(); ++i) {
        name += '.';
        name += parts[i];
    }
    return std::make_shared<NQumir::NAst::TIdentExpr>(loc, std::move(name));
}

// <ident> ::= <regular_ident> | <quoted_ident>
std::expected<std::string, TError> ident(TParserContext& ctx) {
    auto token = ctx.Stream.Next();
    if (token.Type == TToken::Identifier) {
        return token.Name;
    }
    return std::unexpected(Error(token, "identifier expected"));
}

// <qualified_name> ::= <ident> { "." <ident> }
std::expected<std::vector<std::string>, TError> qualified_name(TParserContext& ctx) {
    std::vector<std::string> parts;

    auto first = ident(ctx);
    if (!first) {
        return std::unexpected(first.error());
    }
    parts.push_back(std::move(*first));

    while (true) {
        auto dot = ctx.Stream.Next();
        if (!IsOp(dot, '.')) {
            ctx.Stream.Unget(dot);
            break;
        }
        auto next = ident(ctx);
        if (!next) {
            return std::unexpected(next.error());
        }
        parts.push_back(std::move(*next));
    }

    return parts;
}

// [ [ "AS" ] <ident> ]
std::expected<std::optional<std::string>, TError> opt_alias(TParserContext& ctx) {
    auto token = ctx.Stream.Next();
    if (IsKeyword(token, "AS")) {
        auto name = ident(ctx);
        if (!name) {
            return std::unexpected(name.error());
        }
        return std::optional<std::string>(std::move(*name));
    }
    if (token.Type == TToken::Identifier) {
        return std::optional<std::string>(token.Name);
    }
    ctx.Stream.Unget(token);
    return std::optional<std::string>{};
}

bool StartsQueryAfterOpenParen(TParserContext& ctx) {
    // A table subquery is `("(" <select_stmt> ")")`, and <select_stmt> may
    // start with nested parentheses via <query_primary>.
    std::vector<TToken> consumed;
    auto token = ctx.Stream.Next();
    consumed.push_back(token);
    while (IsOp(token, '(')) {
        token = ctx.Stream.Next();
        consumed.push_back(token);
    }
    const bool startsQuery = IsKeyword(token, "SELECT") || IsKeyword(token, "WITH");
    for (auto it = consumed.rbegin(); it != consumed.rend(); ++it) {
        ctx.Stream.Unget(*it);
    }
    return startsQuery;
}

template<class F>
using TTask = std::invoke_result_t<F&, TParserContext&>;

template<typename T>
TTask<T> TryKeywords(T&& lambda, TParserContext& ctx, const std::vector<std::string>& kws) {
    using TResult = typename decltype(lambda(ctx).result())::value_type;

    std::vector<TToken> consumed;
    for (const auto& kw : kws) {
        auto token = ctx.Stream.Next();
        consumed.push_back(token);
        if (!IsKeyword(token, kw)) {
            for (auto it = consumed.rbegin(); it != consumed.rend(); ++it) {
                ctx.Stream.Unget(*it);
            }
            co_return TResult{};
        }
    }

    co_return co_await lambda(ctx);
}

TAstTask<TSqlNode> query_expr(TParserContext& ctx);
TAstTask<TSqlNode> query_term(TParserContext& ctx);
TAstTask<TSqlNode> query_primary(TParserContext& ctx);

TAstTask<TSqlNode> statement(TParserContext& ctx);
TAstTask<TSqlExternalModule> create_module_stmt(TParserContext& ctx);
TAstTask<TSqlExternalFunction> create_function_stmt(TParserContext& ctx);
TAstTask<TSqlQuery> select_stmt(TParserContext& ctx);
TAstTask<TSqlNode> select_core(TParserContext& ctx);
TAstTask<TSqlSelectList> select_list(TParserContext& ctx);
TAstTask<TSqlSelectItem> select_item(TParserContext& ctx);
TAstTask<TSqlOrder> order_by_clause(TParserContext& ctx);
TAstTask<TSqlOrderItem> order_item(TParserContext& ctx);
TAstTask<TSqlWithClause> with_clause(TParserContext& ctx);
TAstTask<TSqlCte> cte_def(TParserContext& ctx);
TAstTask<TSqlFrom> from_clause(TParserContext& ctx);
TAstTask<TSqlGroupBy> group_by_clause(TParserContext& ctx);
TAstTask<TSqlNode> grouping_element(TParserContext& ctx);
TAstTask<TSqlNode> grouping_set(TParserContext& ctx);
TAstTask<TSqlTableRef> table_ref(TParserContext& ctx);
TAstTask<TSqlTableRef> table_factor(TParserContext& ctx);
TAstTask<TJoinCondition> join_condition(TParserContext& ctx);
TAstTask<TIdentList> ident_list(TParserContext& ctx);
TAstTask<TSqlWindowFrame> window_frame(TParserContext& ctx);
TAstTask<TSqlWindowSpec> window_spec(TParserContext& ctx);
TAstTask<TSqlFrameBound> frame_bound(TParserContext& ctx);
TAstExprTask limit_clause(TParserContext& ctx);
TAstExprTask offset_clause(TParserContext& ctx);
TAstExprTask where_clause(TParserContext& ctx);
TAstExprTask having_clause(TParserContext& ctx);


TAstExprTask expr(TParserContext& ctx);
TAstExprTask or_expr(TParserContext& ctx);
TAstExprTask and_expr(TParserContext& ctx);
TAstExprTask not_expr(TParserContext& ctx);
TAstExprTask comparison_expr(TParserContext& ctx);
TAstExprTask concat_expr(TParserContext& ctx);
TAstExprTask add_expr(TParserContext& ctx);
TAstExprTask mul_expr(TParserContext& ctx);
TAstExprTask unary_expr(TParserContext& ctx);
TAstExprTask postfix_expr(TParserContext& ctx);
TAstExprTask primary_expr(TParserContext& ctx);
TAstExprTask function_call(TParserContext& ctx);
TAstExprTask function_args(TParserContext& ctx);
TAstExprTask case_expr(TParserContext& ctx);
TAstExprTask expr_list_or_subquery(TParserContext& ctx);
TAstExprTask expr_list(TParserContext& ctx);

TAstTypeTask type_name(TParserContext& ctx);

TAstTask<TSqlNode> statement(TParserContext& ctx) {
    auto first = ctx.Stream.Next();

    TSqlNodePtr node;
    if (IsWord(first, "CREATE")) {
        std::vector<TToken> consumed{first};
        auto kind = ctx.Stream.Next();
        consumed.push_back(kind);
        if (IsKeyword(kind, "OR")) {
            consumed.push_back(ctx.Stream.Next()); // REPLACE
            kind = ctx.Stream.Next();
            consumed.push_back(kind);
        }
        for (auto it = consumed.rbegin(); it != consumed.rend(); ++it) {
            ctx.Stream.Unget(*it);
        }
        if (IsWord(kind, "FUNCTION")) {
            node = co_await create_function_stmt(ctx);
        } else {
            node = co_await create_module_stmt(ctx);
        }
    } else {
        ctx.Stream.Unget(first);
        node = co_await select_stmt(ctx);
    }

    auto token = ctx.Stream.Next();
    if (IsEof(token) || IsOp(token, ';')) {
        co_return node;
    }

    co_return Error(token, "expected ';' or end of file");
}

TAstTask<TSqlExternalModule> create_module_stmt(TParserContext& ctx) {
    auto token = ctx.Stream.Next();
    if (!IsWord(token, "CREATE")) {
        co_return Error(token, "`CREATE' required");
    }

    bool replace = false;
    token = ctx.Stream.Next();
    if (IsKeyword(token, "OR")) {
        token = ctx.Stream.Next();
        if (!IsWord(token, "REPLACE")) {
            co_return Error(token, "`REPLACE' required after `OR'");
        }
        replace = true;
        token = ctx.Stream.Next();
    }
    if (!IsWord(token, "MODULE")) {
        co_return Error(token, "`MODULE' required");
    }

    auto name = co_await ident(ctx);
    token = ctx.Stream.Next();
    if (!IsWord(token, "LANGUAGE")) {
        co_return Error(token, "`LANGUAGE' required");
    }
    auto language = co_await ident(ctx);
    token = ctx.Stream.Next();
    if (!IsKeyword(token, "AS")) {
        co_return Error(token, "`AS' required");
    }
    auto code = ctx.Stream.Next();
    if (code.Type != TToken::String) {
        co_return Error(code, "dollar-quoted module source expected");
    }

    co_return std::make_shared<TSqlExternalModule>(
        std::move(name), std::move(language), std::move(code.Name), replace);
}

TAstTask<TSqlExternalFunction> create_function_stmt(TParserContext& ctx) {
    using namespace NQumir::NAst;

    auto token = ctx.Stream.Next();
    auto location = token.Location;
    if (!IsWord(token, "CREATE")) {
        co_return Error(token, "`CREATE' required");
    }

    bool replace = false;
    token = ctx.Stream.Next();
    if (IsKeyword(token, "OR")) {
        token = ctx.Stream.Next();
        if (!IsWord(token, "REPLACE")) {
            co_return Error(token, "`REPLACE' required after `OR'");
        }
        replace = true;
        token = ctx.Stream.Next();
    }
    if (!IsWord(token, "FUNCTION")) {
        co_return Error(token, "`FUNCTION' required");
    }

    auto name = co_await ident(ctx);
    token = ctx.Stream.Next();
    if (!IsOp(token, '(')) {
        co_return Error(token, "`(' expected");
    }

    std::vector<TParam> params;
    token = ctx.Stream.Next();
    if (!IsOp(token, ')')) {
        ctx.Stream.Unget(token);
        while (true) {
            auto first = ctx.Stream.Next();
            auto second = ctx.Stream.Next();
            std::string paramName;
            if (first.Type == TToken::Identifier
                && (second.Type == TToken::Identifier || second.Type == TToken::Keyword))
            {
                paramName = first.Name;
                ctx.Stream.Unget(second);
            } else {
                ctx.Stream.Unget(second);
                ctx.Stream.Unget(first);
                paramName = "__external_arg_" + std::to_string(params.size());
            }
            auto paramType = co_await type_name(ctx);
            params.push_back(std::make_shared<TVarStmt>(
                first.Location, std::move(paramName), std::move(paramType)));

            token = ctx.Stream.Next();
            if (IsOp(token, ')')) {
                break;
            }
            if (!IsOp(token, ',')) {
                co_return Error(token, "`,' or `)' expected");
            }
        }
    }

    token = ctx.Stream.Next();
    if (!IsWord(token, "RETURNS")) {
        co_return Error(token, "`RETURNS' required");
    }

    TTypePtr returnType;
    token = ctx.Stream.Next();
    if (IsOp(token, '(')) {
        std::vector<std::pair<std::string, TTypePtr>> fields;
        while (true) {
            auto fieldType = co_await type_name(ctx);
            fields.emplace_back(
                "field" + std::to_string(fields.size() + 1), std::move(fieldType));
            token = ctx.Stream.Next();
            if (IsOp(token, ')')) {
                break;
            }
            if (!IsOp(token, ',')) {
                co_return Error(token, "`,' or `)' expected");
            }
        }
        returnType = std::make_shared<TStructType>(std::move(fields));
    } else {
        ctx.Stream.Unget(token);
        returnType = co_await type_name(ctx);
    }

    std::optional<std::string> moduleName;
    std::optional<std::string> symbol;
    while (true) {
        token = ctx.Stream.Next();
        if (!IsWord(token, "SET")) {
            ctx.Stream.Unget(token);
            break;
        }
        auto option = ctx.Stream.Next();
        auto separator = ctx.Stream.Next();
        if (!IsWord(separator, "TO") && !IsOp(separator, '=')) {
            co_return Error(separator, "`TO' or `=' expected");
        }
        auto value = co_await ident(ctx);
        if (IsWord(option, "MODULE") && !moduleName) {
            moduleName = std::move(value);
        } else if (IsWord(option, "SYMBOL") && !symbol) {
            symbol = std::move(value);
        } else {
            co_return Error(option, "duplicate or unknown external function option");
        }
    }
    if (!moduleName) {
        co_return Error(token, "`SET MODULE' required");
    }
    if (!symbol) {
        symbol = name;
    }

    auto body = std::make_shared<TBlockExpr>(location, std::vector<TExprPtr>{});
    auto function = std::make_shared<TFunDecl>(
        location, std::move(name), std::vector<TGenericParam>{},
        std::move(params), std::move(body), std::move(returnType));
    function->MangledName = std::move(*symbol);
    std::vector<TTypePtr> paramTypes;
    for (const auto& param : function->Params) {
        paramTypes.push_back(param->Type);
    }
    function->Type = std::make_shared<TFunctionType>(
        std::move(paramTypes), function->RetType);

    auto result = std::make_shared<TSqlExternalFunction>(replace);
    result->ModuleName = std::move(*moduleName);
    result->Func = std::move(function);
    co_return result;
}

TAstTask<TSqlQuery> select_stmt(TParserContext& ctx) {
    TSqlPtr<TSqlQuery> q = std::make_shared<TSqlQuery>();

    q->WithClause = co_await TryKeywords(with_clause, ctx, {"WITH"});

    q->Body = co_await query_expr(ctx);

    q->OrderBy = co_await TryKeywords(order_by_clause, ctx, {"ORDER", "BY"});
    q->Limit = co_await TryKeywords(limit_clause, ctx, {"LIMIT"});
    q->Offset = co_await TryKeywords(offset_clause, ctx, {"OFFSET"});

    co_return q;
}

TAstTask<TSqlNode> query_expr(TParserContext& ctx) {
    auto node = co_await query_term(ctx);
    while (true) {
        auto token = ctx.Stream.Next();
        if (IsKeyword(token, "UNION") || IsKeyword(token, "EXCEPT")) {
            auto op = token.Name == "UNION"
                ? TSqlSetOp::EOp::Union
                : TSqlSetOp::EOp::Except;

            auto quantifierTok = ctx.Stream.Next();
            ESetQuantifier quantifier = ESetQuantifier::Distinct;
            if (IsKeyword(quantifierTok, "ALL")) {
                quantifier = ESetQuantifier::All;
            } else if (IsKeyword(quantifierTok, "DISTINCT")) {
                quantifier = ESetQuantifier::Distinct;
            } else {
                ctx.Stream.Unget(quantifierTok);
            }
            auto right = co_await query_term(ctx);
            node = std::make_shared<TSqlSetOp>(std::move(node), std::move(right), op, quantifier);
        } else {
            ctx.Stream.Unget(token);
            break;
        }
    }

    co_return node;
}

TAstTask<TSqlNode> query_term(TParserContext& ctx) {
    auto node = co_await query_primary(ctx);
    while (true) {
        auto token = ctx.Stream.Next();
        if (IsKeyword(token, "INTERSECT")) {
            auto quantifierTok = ctx.Stream.Next();
            ESetQuantifier quantifier = ESetQuantifier::Distinct;
            if (IsKeyword(quantifierTok, "ALL")) {
                quantifier = ESetQuantifier::All;
            } else if (IsKeyword(quantifierTok, "DISTINCT")) {
                quantifier = ESetQuantifier::Distinct;
            } else {
                ctx.Stream.Unget(quantifierTok);
            }
            auto right = co_await query_primary(ctx);
            node = std::make_shared<TSqlSetOp>(std::move(node), std::move(right), TSqlSetOp::EOp::Intersect, quantifier);
        } else {
            ctx.Stream.Unget(token);
            break;
        }
    }

    co_return node;
}

TAstTask<TSqlNode> query_primary(TParserContext& ctx) {
    auto token = ctx.Stream.Next();
    if (IsKeyword(token, "SELECT")) {
        ctx.Stream.Unget(token);
        co_return co_await select_core(ctx);
    }
    if (IsOp(token, '(')) {
        auto inner = co_await select_stmt(ctx);
        auto close = ctx.Stream.Next();
        if (!IsOp(close, ')')) {
            co_return Error(close, "')' expected");
        }
        co_return inner;
    }
    co_return Error(token, "expected SELECT or '('");
}

TAstTask<TSqlWithClause> with_clause(TParserContext& ctx) {
    auto with = std::make_shared<TSqlWithClause>();
    auto token = ctx.Stream.Next();
    if (IsKeyword(token, "RECURSIVE")) {
        with->Recursive = true;
    } else {
        ctx.Stream.Unget(token);
    }

    do {
        auto cte = co_await cte_def(ctx);
        with->Ctes.emplace_back(std::move(cte));
        token = ctx.Stream.Next();
    } while (IsOp(token, ','));
    ctx.Stream.Unget(token);

    co_return with;
}

TAstTask<TSqlNode> select_core(TParserContext& ctx) {
    auto select = std::make_shared<TSqlSelect>();
    auto token = ctx.Stream.Next();
    if (!IsKeyword(token, "SELECT")) {
        co_return Error(token, "`SELECT' required");
    }
    token = ctx.Stream.Next();
    if (IsKeyword(token, "DISTINCT")) {
        select->Quantifier = ESetQuantifier::Distinct;
    } else if (IsKeyword(token, "ALL")) {
        select->Quantifier = ESetQuantifier::All;
    } else {
        ctx.Stream.Unget(token);
    }
    select->SelectList = co_await select_list(ctx);

    select->From = co_await TryKeywords(from_clause, ctx, {"FROM"});
    select->Where = co_await TryKeywords(where_clause, ctx, {"WHERE"});
    select->GroupBy = co_await TryKeywords(group_by_clause, ctx, {"GROUP", "BY"});
    select->Having = co_await TryKeywords(having_clause, ctx, {"HAVING"});

    co_return select;
}

// <select_list> ::= "*" | <select_item> { "," <select_item> }
TAstTask<TSqlSelectList> select_list(TParserContext& ctx) {
    auto list = std::make_shared<TSqlSelectList>();
    TToken token;
    do {
        auto item = co_await select_item(ctx);
        list->Items.emplace_back(std::move(item));
        token = ctx.Stream.Next();
    } while (IsOp(token, ','));
    ctx.Stream.Unget(token);
    co_return list;
}

// <select_item> ::=
//     <expr> [ [ "AS" ] <ident> | "AS" "(" <ident> { "," <ident> } ")" ]
//   | <qualified_name> "." "*"
TAstTask<TSqlSelectItem> select_item(TParserContext& ctx) {
    auto item = std::make_shared<TSqlSelectItem>();

    auto first = ctx.Stream.Next();

    // bare "*"
    if (IsOp(first, '*')) {
        item->Star = true;
        co_return item;
    }

    // try <qualified_name> "." "*", buffering tokens so we can fall back to <expr>
    if (first.Type == TToken::Identifier) {
        std::vector<TToken> buffer{first};
        std::vector<std::string> prefix{first.Name};
        bool isStar = false;

        while (true) {
            auto dot = ctx.Stream.Next();
            if (!IsOp(dot, '.')) {
                ctx.Stream.Unget(dot);
                break;
            }
            buffer.push_back(dot);

            auto next = ctx.Stream.Next();
            buffer.push_back(next);
            if (IsOp(next, '*')) {
                isStar = true;
                break;
            }
            if (next.Type == TToken::Identifier) {
                prefix.push_back(next.Name);
                continue;
            }
            break;
        }

        if (isStar) {
            item->Star = true;
            item->StarPrefix = std::move(prefix);
            co_return item;
        }

        // not a star item: restore tokens and parse as a general expression
        for (auto it = buffer.rbegin(); it != buffer.rend(); ++it) {
            ctx.Stream.Unget(*it);
        }
    } else {
        ctx.Stream.Unget(first);
    }

    item->Expr = co_await expr(ctx);
    auto alias = ctx.Stream.Next();
    if (IsKeyword(alias, "AS")) {
        auto next = ctx.Stream.Next();
        if (IsOp(next, '(')) {
            next = ctx.Stream.Next();
            if (IsOp(next, ')')) {
                co_return Error(next, "column alias list cannot be empty");
            }
            ctx.Stream.Unget(next);
            while (true) {
                item->ColumnAliases.push_back(co_await ident(ctx));
                next = ctx.Stream.Next();
                if (IsOp(next, ')')) {
                    break;
                }
                if (!IsOp(next, ',')) {
                    co_return Error(next, "`,' or `)' expected in column alias list");
                }
            }
        } else {
            ctx.Stream.Unget(next);
            item->Alias = co_await ident(ctx);
        }
    } else if (alias.Type == TToken::Identifier) {
        item->Alias = std::move(alias.Name);
    } else {
        ctx.Stream.Unget(alias);
    }
    co_return item;
}

TAstTask<TSqlFrom> from_clause(TParserContext& ctx) {
    auto from = std::make_shared<TSqlFrom>();
    TToken token;
    do {
        auto table = co_await table_ref(ctx);
        from->Items.emplace_back(std::move(table));
        token = ctx.Stream.Next();
    } while (IsOp(token, ','));
    ctx.Stream.Unget(token);
    co_return from;
}

TAstTask<TSqlGroupBy> group_by_clause(TParserContext& ctx) {
    auto group = std::make_shared<TSqlGroupBy>();
    TToken token;
    do {
        auto item = co_await grouping_element(ctx);
        group->Items.emplace_back(std::move(item));
        token = ctx.Stream.Next();
    } while (IsOp(token, ','));
    ctx.Stream.Unget(token);
    co_return group;
}

TAstTask<TSqlNode> grouping_element(TParserContext& ctx) {
    auto token = ctx.Stream.Next();
    if (IsKeyword(token, "ROLLUP")) {
        auto lparen = ctx.Stream.Next();
        if (!IsOp(lparen, '(')) {
            co_return Error(lparen, "expected '(' after ROLLUP");
        }
        auto list = co_await TryKeywords(expr_list, ctx, {});
        auto rparen = ctx.Stream.Next();
        if (!IsOp(rparen, ')')) {
            co_return Error(rparen, "expected ')' after ROLLUP list");
        }
        co_return std::make_shared<TSqlRollUp>(std::move(list));
    }
    if (IsKeyword(token, "CUBE")) {
        auto lparen = ctx.Stream.Next();
        if (!IsOp(lparen, '(')) {
            co_return Error(lparen, "expected '(' after CUBE");
        }
        auto list = co_await TryKeywords(expr_list, ctx, {});
        auto rparen = ctx.Stream.Next();
        if (!IsOp(rparen, ')')) {
            co_return Error(rparen, "expected ')' after CUBE list");
        }
        co_return std::make_shared<TSqlCube>(std::move(list));
    }
    if (IsKeyword(token, "GROUPING")) {
        auto setsTok = ctx.Stream.Next();
        if (!IsKeyword(setsTok, "SETS")) {
            co_return Error(setsTok, "expected 'SETS' after GROUPING");
        }
        auto lparen = ctx.Stream.Next();
        if (!IsOp(lparen, '(')) {
            co_return Error(lparen, "expected '(' after GROUPING SETS");
        }
        auto groupingSets = std::make_shared<TSqlGroupingSet>();
        TToken next;
        do {
            auto set = co_await grouping_set(ctx);
            groupingSets->Items.emplace_back(std::move(set));
            next = ctx.Stream.Next();
        } while (IsOp(next, ','));
        if (!IsOp(next, ')')) {
            co_return Error(next, "expected ')' after GROUPING SETS list");
        }
        co_return groupingSets;
    }

    // fallback to <expr>
    ctx.Stream.Unget(token);
    co_return std::make_shared<TSqlGroupingExprOrList>(co_await expr(ctx));
}

TAstTask<TSqlNode> grouping_set(TParserContext& ctx) {
    auto token = ctx.Stream.Next();
    if (IsOp(token, '(')) {
        auto list = co_await TryKeywords(expr_list, ctx, {});
        auto rparen = ctx.Stream.Next();
        if (!IsOp(rparen, ')')) {
            co_return Error(rparen, "expected ')' after grouping set");
        }
        co_return std::make_shared<TSqlGroupingExprOrList>(std::move(list));
    }
    if (IsKeyword(token, "ROLLUP")) {
        auto lparen = ctx.Stream.Next();
        if (!IsOp(lparen, '(')) {
            co_return Error(lparen, "expected '(' after ROLLUP");
        }
        auto list = co_await TryKeywords(expr_list, ctx, {});
        auto rparen = ctx.Stream.Next();
        if (!IsOp(rparen, ')')) {
            co_return Error(rparen, "expected ')' after ROLLUP list");
        }
        co_return std::make_shared<TSqlRollUp>(std::move(list));
    }
    if (IsKeyword(token, "CUBE")) {
        auto lparen = ctx.Stream.Next();
        if (!IsOp(lparen, '(')) {
            co_return Error(lparen, "expected '(' after CUBE");
        }
        auto list = co_await TryKeywords(expr_list, ctx, {});
        auto rparen = ctx.Stream.Next();
        if (!IsOp(rparen, ')')) {
            co_return Error(rparen, "expected ')' after CUBE list");
        }
        co_return std::make_shared<TSqlCube>(std::move(list));
    }

    // fallback to <expr>
    ctx.Stream.Unget(token);
    co_return std::make_shared<TSqlGroupingExprOrList>(co_await expr(ctx));
}

TAstTask<TSqlTableRef> table_ref(TParserContext& ctx) {
    auto left = co_await table_factor(ctx);
    const std::vector<std::pair<std::string, ESqlJoinType>> joinStart = {
        {"INNER", ESqlJoinType::Inner},
        {"LEFT", ESqlJoinType::Left},
        {"RIGHT", ESqlJoinType::Right},
        {"FULL", ESqlJoinType::Full},
        {"CROSS", ESqlJoinType::Cross},
    };

    const std::set<ESqlJoinType> outerJoins = {
        ESqlJoinType::Left,
        ESqlJoinType::Right,
        ESqlJoinType::Full
    };

    const std::map<ESqlJoinType, ESqlJoinType> semiJoins = {
        {ESqlJoinType::Left, ESqlJoinType::LeftSemi},
        {ESqlJoinType::Right, ESqlJoinType::RightSemi}
    };

    ESqlJoinType joinType = ESqlJoinType::Inner;
    bool natural = false;
    TLocation naturalLocation;
    auto isJoinStart = [&]() -> NQumir::TExpectedTask<bool, TError, TLocation> {
        auto first = ctx.Stream.Next();
        joinType = ESqlJoinType::Inner;
        natural = IsKeyword(first, "NATURAL");
        if (natural) {
            naturalLocation = first.Location;
            first = ctx.Stream.Next();
            if (IsKeyword(first, "CROSS")) {
                co_return Error(first, "NATURAL CROSS JOIN is not supported");
            }
        }

        for (const auto& [kw, type] : joinStart) {
            if (IsKeyword(first, kw)) {
                joinType = type;

                auto next = ctx.Stream.Next();
                if (IsKeyword(next, "OUTER")) {
                    if (outerJoins.count(joinType) == 0) {
                        co_return Error(first, "Unknown `OUTER' join type");
                    }
                    co_return true;
                }

                if (IsKeyword(next, "SEMI")) {
                    auto it = semiJoins.find(joinType);
                    if (it == semiJoins.end()) {
                        co_return Error(first, "Unknown `SEMI' join type");
                    }
                    joinType = it->second;
                    co_return true;
                }

                ctx.Stream.Unget(next);

                co_return true;
            }
        }

        auto ret = IsKeyword(first, "JOIN");
        if (natural && !ret) {
            co_return Error(first, "`JOIN' keyword expected after NATURAL");
        }
        ctx.Stream.Unget(first);
        co_return ret;
    };

    while (co_await isJoinStart()) {
        auto next = ctx.Stream.Next();
        if (!IsKeyword(next, "JOIN")) {
            co_return Error(next, "`JOIN' keyword expected");
        }

        auto join = std::make_shared<TSqlJoin>();
        join->Type = joinType;
        join->Natural = natural;
        join->NaturalLocation = naturalLocation;

        join->Left = left;
        join->Right = co_await table_factor(ctx);
        if (natural) {
            auto after = ctx.Stream.Next();
            if (IsKeyword(after, "ON") || IsKeyword(after, "USING")) {
                co_return Error(after, "NATURAL JOIN cannot have an ON or USING clause");
            }
            ctx.Stream.Unget(after);
        } else {
            join->Condition = co_await join_condition(ctx);
        }

        left = join;
    }
    co_return left;
}

TAstTask<TJoinCondition> join_condition(TParserContext& ctx) {
    auto join_cond = std::make_shared<TJoinCondition>();
    auto next = ctx.Stream.Next();
    if (IsKeyword(next, "ON")) {
        join_cond->On = co_await expr(ctx);
        co_return join_cond;
    }
    if (IsKeyword(next, "USING")) {
        auto lparen = ctx.Stream.Next();
        if (!IsOp(lparen, '(')) {
            co_return Error(lparen, "expected '(' after USING");
        }
        join_cond->UsingColumns = co_await ident_list(ctx);
        auto rparen = ctx.Stream.Next();
        if (!IsOp(rparen, ')')) {
            co_return Error(rparen, "expected ')' after USING column list");
        }
        co_return join_cond;
    }

    ctx.Stream.Unget(next);
    co_return join_cond;
}

// <table_factor> ::=
//       <qualified_name> [ [ "AS" ] <ident> ]
//     | "(" <select_stmt> ")" [ [ "AS" ] <ident> ]
//     | "(" <table_ref> ")"
TAstTask<TSqlTableRef> table_factor(TParserContext& ctx) {
    auto token = ctx.Stream.Next();

    if (IsOp(token, '(')) {
        if (StartsQueryAfterOpenParen(ctx)) {
            auto sub = std::make_shared<TSqlSubqueryTable>();
            sub->Query = co_await select_stmt(ctx);

            auto rparen = ctx.Stream.Next();
            if (!IsOp(rparen, ')')) {
                co_return Error(rparen, "expected ')' after subquery");
            }
            sub->Alias = co_await opt_alias(ctx);

            // optional column rename list: ( <ident_list> )
            auto cols = ctx.Stream.Next();
            if (IsOp(cols, '(')) {
                sub->ColumnAliases = co_await ident_list(ctx);
                auto rp = ctx.Stream.Next();
                if (!IsOp(rp, ')')) {
                    co_return Error(rp, "expected ')' after column alias list");
                }
            } else {
                ctx.Stream.Unget(cols);
            }
            co_return sub;
        }

        auto inner = co_await table_ref(ctx);
        auto rparen = ctx.Stream.Next();
        if (!IsOp(rparen, ')')) {
            co_return Error(rparen, "expected ')' after table reference");
        }
        co_return inner;
    }

    ctx.Stream.Unget(token);

    auto table = std::make_shared<TSqlTableName>();
    table->Name = co_await qualified_name(ctx);
    table->Alias = co_await opt_alias(ctx);
    co_return table;
}

// <ident_list> ::= <ident> { "," <ident> }
TAstTask<TIdentList> ident_list(TParserContext& ctx) {
    auto list = std::make_shared<TIdentList>();
    TToken token;
    do {
        auto name = co_await ident(ctx);
        list->Items.emplace_back(std::move(name));
        token = ctx.Stream.Next();
    } while (IsOp(token, ','));
    ctx.Stream.Unget(token);
    co_return list;
}

// <where_clause> ::= "WHERE" <expr>
TAstExprTask where_clause(TParserContext& ctx) {
    co_return co_await expr(ctx);
}

// <having_clause> ::= "HAVING" <expr>
TAstExprTask having_clause(TParserContext& ctx) {
    co_return co_await expr(ctx);
}

// <order_by_clause> ::= "ORDER" "BY" <order_item> { "," <order_item> }
TAstTask<TSqlOrder> order_by_clause(TParserContext& ctx) {
    auto order = std::make_shared<TSqlOrder>();
    TToken token;
    do {
        auto item = co_await order_item(ctx);
        order->Items.emplace_back(std::move(item));
        token = ctx.Stream.Next();
    } while (IsOp(token, ','));
    ctx.Stream.Unget(token);
    co_return order;
}

// <order_item> ::= <expr> [ "ASC" | "DESC" ] [ "NULLS" ( "FIRST" | "LAST" ) ]
TAstTask<TSqlOrderItem> order_item(TParserContext& ctx) {
    auto item = std::make_shared<TSqlOrderItem>();
    item->Expr = co_await expr(ctx);

    auto token = ctx.Stream.Next();
    if (IsKeyword(token, "ASC")) {
        item->Desc = false;
    } else if (IsKeyword(token, "DESC")) {
        item->Desc = true;
    } else {
        ctx.Stream.Unget(token);
    }

    token = ctx.Stream.Next();
    if (IsKeyword(token, "NULLS")) {
        auto dir = ctx.Stream.Next();
        if (IsKeyword(dir, "FIRST")) {
            item->NullOrder = TSqlOrderItem::ENullOrder::First;
        } else if (IsKeyword(dir, "LAST")) {
            item->NullOrder = TSqlOrderItem::ENullOrder::Last;
        } else {
            co_return Error(dir, "expected FIRST or LAST after NULLS");
        }
    } else {
        ctx.Stream.Unget(token);
    }

    co_return item;
}

// <limit_clause> ::= "LIMIT" <expr>
TAstExprTask limit_clause(TParserContext& ctx) {
    co_return co_await expr(ctx);
}

// <offset_clause> ::= "OFFSET" <expr>
TAstExprTask offset_clause(TParserContext& ctx) {
    co_return co_await expr(ctx);
}

// <cte_def> ::= <ident> [ "(" <ident_list> ")" ] "AS" "(" <select_stmt> ")"
TAstTask<TSqlCte> cte_def(TParserContext& ctx) {
    auto cte = std::make_shared<TSqlCte>();
    cte->Name = co_await ident(ctx);

    auto token = ctx.Stream.Next();
    if (IsOp(token, '(')) {
        cte->Columns = co_await ident_list(ctx);
        auto rparen = ctx.Stream.Next();
        if (!IsOp(rparen, ')')) {
            co_return Error(rparen, "expected ')' after CTE column list");
        }
        token = ctx.Stream.Next();
    }

    if (!IsKeyword(token, "AS")) {
        co_return Error(token, "expected `AS' in CTE definition");
    }

    auto lparen = ctx.Stream.Next();
    if (!IsOp(lparen, '(')) {
        co_return Error(lparen, "expected '(' before CTE query");
    }
    cte->Query = co_await select_stmt(ctx);
    auto rparen = ctx.Stream.Next();
    if (!IsOp(rparen, ')')) {
        co_return Error(rparen, "expected ')' after CTE query");
    }
    co_return cte;
}

/* exprs */

TAstExprTask expr(TParserContext& ctx) {
    co_return co_await or_expr(ctx);
}

template<typename Func>
TAstExprTask binary_op_kw_helper(TParserContext& ctx, Func prev_parser, const std::string& kw, TOperator op) {
    auto ret = co_await prev_parser(ctx);
    TLocation loc = ret->Location;
    auto isKw = [&] () {
        auto next = ctx.Stream.Next();
        loc = next.Location;
        if (IsKeyword(next, kw)) {
            return true;
        }
        ctx.Stream.Unget(next);
        return false;
    };
    while (isKw()) {
        auto next_expr = co_await prev_parser(ctx);
        ret = binary(loc, op, ret, next_expr);
    }
    co_return ret;
}

template<typename Func, typename... TOps>
TAstExprTask binary_op_helper(TParserContext& ctx, Func prev_parser, TOps... ops)
{
    auto ret = co_await prev_parser(ctx);
    auto loc = ret->Location;
    auto isOp = [&] -> std::optional<uint64_t> {
        auto token = ctx.Stream.Next();
        if (token.Type == TToken::Operator
            && ((token.Value.i64 == (uint64_t)ops) || ...))
        {
            return token.Value.i64;
        } else {
            ctx.Stream.Unget(token);
            return std::nullopt;
        }
    };

    while (auto op = isOp()) {
        auto next_expr = co_await prev_parser(ctx);
        ret = binary(loc, *op, ret, next_expr);
    }
    co_return ret;
}

TAstExprTask or_expr(TParserContext& ctx) {
    co_return co_await binary_op_kw_helper(ctx, and_expr, "OR", "||"_op);
}

TAstExprTask and_expr(TParserContext& ctx) {
    co_return co_await binary_op_kw_helper(ctx, not_expr, "AND", "&&"_op);
}

TAstExprTask not_expr(TParserContext& ctx) {
    auto token = ctx.Stream.Next();
    if (IsKeyword(token, "NOT")) {
        co_return unary(token.Location, "!"_op, co_await not_expr(ctx));
    }
    ctx.Stream.Unget(token);
    co_return co_await comparison_expr(ctx);
}

TAstExprTask comparison_expr(TParserContext& ctx) {
    auto ret = co_await concat_expr(ctx);
    auto token = ctx.Stream.Next();
    static std::vector<std::pair<TOperator, TOperator>> sql2qumir = {
        {"="_op, "=="_op},
        {"<>"_op, "!="_op},
        {"!="_op, "!="_op},
        {"<"_op, "<"_op},
        {"<="_op, "<="_op},
        {">"_op, ">"_op},
        {">="_op, ">="_op},
    };

    for (auto [from, to] : sql2qumir) {
        if (IsOp(token, from)) {
            co_return binary(token.Location, to, ret, co_await concat_expr(ctx));
        }
    }

    if (IsKeyword(token, "IS")) {
        bool notNull = false;
        auto next = ctx.Stream.Next();
        if (IsKeyword(next, "NOT")) {
            notNull = true;
            next = ctx.Stream.Next();
        }
        if (!IsKeyword(next, "NULL")) {
            co_return Error(next, "`NULL' expected after `IS'");
        }
        auto isNull = call(token.Location, "qdb_is_null", { ret });
        co_return notNull ? unary(token.Location, "!"_op, isNull) : isNull;
    }

    // optional NOT before IN / BETWEEN / LIKE
    bool negative = false;
    auto opToken = token;
    if (IsKeyword(token, "NOT")) {
        negative = true;
        opToken = ctx.Stream.Next();
    }

    auto loc = opToken.Location;
    if (IsKeyword(opToken, "IN")) {
        auto next = ctx.Stream.Next();
        if (!IsOp(next, '(')) {
            co_return Error(next, "'(' expected after `IN'");
        }
        auto items = co_await expr_list_or_subquery(ctx);
        next = ctx.Stream.Next();
        if (!IsOp(next, ')')) {
            co_return Error(next, "')' expected");
        }
        if (auto sub = NQumir::NAst::TMaybeNode<TSubqueryExpr>(items)) {
            // ret IN ( <select> )
            auto node = sub.Cast();
            node->Operand = ret;
            ret = node;
        } else {
            // Keep scalar IN as a pseudo-call until typed kernel expansion.
            auto block = std::static_pointer_cast<NQumir::NAst::TBlockExpr>(items);
            ret = BuildInListPredicate(loc, std::move(ret), block->Stmts);
        }
    } else if (IsKeyword(opToken, "BETWEEN")) {
        auto left = co_await concat_expr(ctx);
        auto next = ctx.Stream.Next();
        if (!IsKeyword(next, "AND")) {
            co_return Error(next, "`AND' expected");
        }
        auto right = co_await concat_expr(ctx);
        // left <= ret && ret <= right
        ret = binary(loc, "&&"_op,
            binary(loc, "<="_op, left, ret),
            binary(loc, "<="_op, ret, right));
    } else if (IsKeyword(opToken, "LIKE")) {
        ret = call(loc, "qdb_string_view_sql_like",
            {ret, co_await concat_expr(ctx)});
    } else {
        // nothing matched: restore the consumed token(s) in reverse order
        if (negative) {
            ctx.Stream.Unget(opToken);
        }
        ctx.Stream.Unget(token);
        co_return ret;
    }

    if (negative) {
        ret = unary(loc, "!"_op, ret);
    }
    co_return ret;
}

// `a || b` is string concatenation. It shares the `||` token with logical OR, so
// instead of a binary op (which collides with OR's operator string) it lowers to a
// `strcat(a, b)` pseudo-call: the typer types it as StringView and ExpandNullable
// rewrites it into the real qdb_string_concat runtime call.
TAstExprTask concat_expr(TParserContext& ctx) {
    auto ret = co_await add_expr(ctx);
    auto loc = ret->Location;
    while (true) {
        auto token = ctx.Stream.Next();
        if (!IsOp(token, "||"_op)) {
            ctx.Stream.Unget(token);
            break;
        }
        auto rhs = co_await add_expr(ctx);
        ret = call(loc, "strcat", {std::move(ret), std::move(rhs)});
    }
    co_return ret;
}

TAstExprTask add_expr(TParserContext& ctx) {
    co_return co_await binary_op_helper(ctx, mul_expr, "+"_op, "-"_op);
}

TAstExprTask mul_expr(TParserContext& ctx) {
    co_return co_await binary_op_helper(ctx, unary_expr, "*"_op, "/"_op, "%"_op);
}

TAstExprTask unary_expr(TParserContext& ctx) {
    auto token = ctx.Stream.Next();
    if (IsOp(token, '+')) {
        co_return co_await unary_expr(ctx);
    }

    if (IsOp(token, '-')) {
        co_return unary(token.Location, "-"_op, co_await unary_expr(ctx));
    }

    ctx.Stream.Unget(token);
    co_return co_await postfix_expr(ctx);
}

TAstExprTask postfix_expr(TParserContext& ctx) {
    auto ret = co_await primary_expr(ctx);
    TLocation loc = ret->Location;
    auto isCast = [&]() {
        auto token = ctx.Stream.Next();
        loc = token.Location;
        if (IsOp(token, "::"_op)) {
            return true;
        }
        ctx.Stream.Unget(token);
        return false;
    };

    auto token = ctx.Stream.Next();
    TSqlPtr<TSqlWindowSpec> windowSpec;
    if (IsWord(token, "OVER")
        && NQumir::NAst::TMaybeNode<NQumir::NAst::TCallExpr>(ret))
    {
        auto open = ctx.Stream.Next();
        if (IsOp(open, '(')) {
            ctx.Stream.Unget(open);
            windowSpec = co_await window_spec(ctx);
        } else {
            ctx.Stream.Unget(open);
            ctx.Stream.Unget(token);
        }
    } else {
        ctx.Stream.Unget(token);
    }

    if (windowSpec) {
        ret = std::make_shared<TWindowExpr>(std::move(ret), std::move(windowSpec));
    }

    while (isCast()) {
        ret = cast(loc, ret, co_await type_name(ctx));
    }
    co_return ret;
}

TAstTask<TSqlWindowSpec> window_spec(TParserContext& ctx) {
    auto token = ctx.Stream.Next();
    if (!IsOp(token, '(')) {
        co_return Error(token, "'(' expected after `OVER'");
    }

    auto partitionBy = co_await TryKeywords(expr_list, ctx, {"PARTITION", "BY"});
    auto orderBy = co_await TryKeywords(order_by_clause, ctx, {"ORDER", "BY"});
    auto frame = co_await window_frame(ctx);
    token = ctx.Stream.Next();
    if (!IsOp(token, ')')) {
        co_return Error(token, "')' expected in window specification");
    }

    // Materialize the default frame (RANGE UNBOUNDED PRECEDING AND ...): the end
    // is CURRENT ROW when ORDER BY is present, otherwise UNBOUNDED FOLLOWING (the
    // whole partition).
    if (!frame) {
        auto end = orderBy
            ? std::make_shared<TSqlFrameBound>(TSqlFrameBound::EType::CurrentRow, nullptr)
            : std::make_shared<TSqlFrameBound>(TSqlFrameBound::EType::UnboundedFollowing, nullptr);
        frame = std::make_shared<TSqlWindowFrame>(
            TSqlWindowFrame::EType::Range,
            std::make_shared<TSqlFrameBound>(TSqlFrameBound::EType::UnboundedPreceding, nullptr),
            std::move(end));
    }

    co_return std::make_shared<TSqlWindowSpec>(
        std::move(partitionBy),
        std::move(orderBy),
        std::move(frame)
    );
}

TAstTask<TSqlWindowFrame> window_frame(TParserContext& ctx) {
    auto token = ctx.Stream.Next();
    TSqlWindowFrame::EType frameType;
    if (IsWord(token, "ROWS")) {
        frameType = TSqlWindowFrame::EType::Rows;
    } else if (IsWord(token, "RANGE")) {
        frameType = TSqlWindowFrame::EType::Range;
    } else {
        ctx.Stream.Unget(token);
        co_return nullptr;
    }
    token = ctx.Stream.Next();
    if (IsKeyword(token, "BETWEEN")) {
        auto start = co_await frame_bound(ctx);
        auto andTok = ctx.Stream.Next();
        if (!IsKeyword(andTok, "AND")) {
            co_return Error(andTok, "`AND' expected in window frame");
        }
        auto end = co_await frame_bound(ctx);
        co_return std::make_shared<TSqlWindowFrame>(frameType, std::move(start), std::move(end));
    } else {
        ctx.Stream.Unget(token);
        co_return std::make_shared<TSqlWindowFrame>(
            frameType, co_await frame_bound(ctx), nullptr
        );
    }
}

TAstTask<TSqlFrameBound> frame_bound(TParserContext& ctx) {
    auto token = ctx.Stream.Next();
    if (IsWord(token, "UNBOUNDED")) {
        auto next = ctx.Stream.Next();
        if (IsWord(next, "PRECEDING")) {
            co_return std::make_shared<TSqlFrameBound>(TSqlFrameBound::EType::UnboundedPreceding, nullptr);
        } else if (IsWord(next, "FOLLOWING")) {
            co_return std::make_shared<TSqlFrameBound>(TSqlFrameBound::EType::UnboundedFollowing, nullptr);
        } else {
            ctx.Stream.Unget(next);
            ctx.Stream.Unget(token);
        }
    } else if (IsWord(token, "CURRENT")) {
        auto next = ctx.Stream.Next();
        if (IsWord(next, "ROW")) {
            co_return std::make_shared<TSqlFrameBound>(TSqlFrameBound::EType::CurrentRow, nullptr);
        } else {
            ctx.Stream.Unget(next);
            ctx.Stream.Unget(token);
        }
    } else {
        ctx.Stream.Unget(token);
    }

    auto e = co_await expr(ctx);
    token = ctx.Stream.Next();
    if (IsWord(token, "PRECEDING")) {
        co_return std::make_shared<TSqlFrameBound>(TSqlFrameBound::EType::Preceding, std::move(e));
    } else if (IsWord(token, "FOLLOWING")) {
        co_return std::make_shared<TSqlFrameBound>(TSqlFrameBound::EType::Following, std::move(e));
    } else {
        co_return Error(token, "`PRECEDING' or `FOLLOWING' expected after expression");
    }
}

TAstExprTask primary_expr(TParserContext& ctx) {
    auto token = ctx.Stream.Next();
    auto loc = token.Location;

    // literals
    if (token.Type == TToken::Integer) {
        // interval shorthand: `<integer> <unit>` (e.g. `60 days`), the unit check
        // keeps an implicit alias like `SELECT 5 x` from being eaten as a unit.
        auto unit = ctx.Stream.Next();
        if ((unit.Type == TToken::Identifier || unit.Type == TToken::Keyword)
            && IsIntervalUnit(unit.Name)) {
            co_return i32_literal(loc, qdb_sql_interval(
                AsStringView(std::to_string(token.Value.i64)), AsStringView(ToLowerStr(unit.Name))));
        }
        ctx.Stream.Unget(unit);
        co_return std::make_shared<NQumir::NAst::TNumberExpr>(loc, token.Value.i64);
    } else if (token.Type == TToken::Float) {
        co_return std::make_shared<NQumir::NAst::TNumberExpr>(loc, token.Value.f64);
    } else if (token.Type == TToken::String) {
        // TODO: call struct constructor inplace!
        auto literal = std::make_shared<NQumir::NAst::TStringLiteralExpr>(loc, token.Name);
        auto target = std::make_shared<NQumir::NAst::TNamedType>("StringView", nullptr);
        co_return std::make_shared<NQumir::NAst::TCastExpr>(loc, std::move(literal), std::move(target));
    } else if (token.Type == TToken::Char) {
        auto ret = std::make_shared<NQumir::NAst::TNumberExpr>(loc, token.Value.i64);
        ret->Type = std::make_shared<NQumir::NAst::TIntegerType>(NQumir::NAst::TIntegerType::U8);
        co_return ret;
    } else if (token.Type == TToken::Boolean) {
        co_return std::make_shared<NQumir::NAst::TNumberExpr>(loc, (bool)token.Value.i64);
    }

    // keyword literals
    if (IsKeyword(token, "TRUE")) {
        co_return std::make_shared<NQumir::NAst::TNumberExpr>(loc, true);
    } else if (IsKeyword(token, "FALSE")) {
        co_return std::make_shared<NQumir::NAst::TNumberExpr>(loc, false);
    } else if (IsKeyword(token, "NULL")) {
        co_return call(loc, "qdb_sql_null", {});
    }

    // <date_literal> ::= "DATE" <string_literal>
    if (IsKeyword(token, "DATE")) {
        auto value = ctx.Stream.Next();
        if (value.Type != TToken::String) {
            co_return Error(value, "string literal expected after `DATE'");
        }
        // TODO: constant functions should be expanded by Qumir constant folding,
        // not special-cased in the SQL parser.
        co_return i32_literal(loc, qdb_sql_date(AsStringView(value.Name)));
    }

    // <interval_literal> ::= "INTERVAL" <string_literal> <unit>
    if (IsKeyword(token, "INTERVAL")) {
        auto value = ctx.Stream.Next();
        if (value.Type != TToken::String) {
            co_return Error(value, "string literal expected after `INTERVAL'");
        }
        auto unit = ctx.Stream.Next();
        if (unit.Type != TToken::Identifier) {
            co_return Error(unit, "interval unit expected (e.g. day, month, year)");
        }
        // TODO: constant functions should be expanded by Qumir constant folding,
        // not special-cased in the SQL parser.
        co_return i32_literal(loc, qdb_sql_interval(AsStringView(value.Name), AsStringView(unit.Name)));
    }

    if (token.Type == TToken::Identifier) {
        ctx.Stream.Unget(token);
        co_return co_await function_call(ctx);
    }

    if (IsKeyword(token, "GROUPING")) {
        auto next = ctx.Stream.Next();
        if (!IsOp(next, '(')) {
            co_return Error(next, "'(' expected after GROUPING");
        }

        auto e = co_await expr(ctx);

        next = ctx.Stream.Next();
        if (!IsOp(next, ')')) {
            co_return Error(next, "')' expected after GROUPING argument");
        }

        co_return call(loc, "grouping", {std::move(e)});
    }

    if (IsKeyword(token, "CASE")) {
        co_return co_await case_expr(ctx);
    }

    if (IsKeyword(token, "CAST")) {
        auto next = ctx.Stream.Next();
        if (!IsOp(next, '(')) {
            co_return Error(next, "'(' expected");
        }

        auto operand = co_await expr(ctx);

        next = ctx.Stream.Next();
        if (!IsKeyword(next, "AS")) {
            co_return Error(next, "`AS' expected");
        }

        auto type = co_await type_name(ctx);

        next = ctx.Stream.Next();
        if (!IsOp(next, ')')) {
            co_return Error(next, "')' expected");
        }

        co_return cast(loc, std::move(operand), std::move(type));
    }

    // <extract> ::= "EXTRACT" "(" <field> "FROM" <expr> ")"
    if (IsKeyword(token, "EXTRACT")) {
        auto next = ctx.Stream.Next();
        if (!IsOp(next, '(')) {
            co_return Error(next, "'(' expected");
        }

        auto field = ctx.Stream.Next();
        if (field.Type != TToken::Identifier && field.Type != TToken::Keyword) {
            co_return Error(field, "extract field expected (e.g. year)");
        }
        std::string fieldName;
        for (char c : field.Name) {
            fieldName += std::toupper(static_cast<unsigned char>(c));
        }

        next = ctx.Stream.Next();
        if (!IsKeyword(next, "FROM")) {
            co_return Error(next, "`FROM' expected in EXTRACT");
        }

        auto operand = co_await expr(ctx);

        next = ctx.Stream.Next();
        if (!IsOp(next, ')')) {
            co_return Error(next, "')' expected");
        }

        if (fieldName == "YEAR") {
            co_return call(loc, "qdb_date_year", { std::move(operand) });
        }
        co_return Error(field, "unsupported extract field `" + fieldName + "'");
    }

    // <substring> ::= "SUBSTRING" "(" <expr> "FROM" <expr> "FOR" <expr> ")"
    if (IsKeyword(token, "SUBSTRING")) {
        auto i32type = std::make_shared<NQumir::NAst::TIntegerType>(NQumir::NAst::TIntegerType::I32);

        auto next = ctx.Stream.Next();
        if (!IsOp(next, '(')) {
            co_return Error(next, "'(' expected");
        }

        auto str = co_await expr(ctx);

        next = ctx.Stream.Next();
        if (!IsKeyword(next, "FROM")) {
            co_return Error(next, "`FROM' expected in SUBSTRING");
        }
        auto start = co_await expr(ctx);
        start = cast(next.Location, start, i32type);

        next = ctx.Stream.Next();
        if (!IsKeyword(next, "FOR")) {
            co_return Error(next, "`FOR' expected in SUBSTRING");
        }
        auto length = co_await expr(ctx);
        length = cast(next.Location, length, i32type);

        next = ctx.Stream.Next();
        if (!IsOp(next, ')')) {
            co_return Error(next, "')' expected");
        }

        co_return call(loc, "qdb_substring",
            { std::move(str), std::move(start), std::move(length) });
    }

    if (IsKeyword(token, "EXISTS")) {
        auto next = ctx.Stream.Next();
        if (!IsOp(next, '(')) {
            co_return Error(next, "'(' expected");
        }

        auto query = co_await select_stmt(ctx);

        next = ctx.Stream.Next();
        if (!IsOp(next, ')')) {
            co_return Error(next, "')' expected");
        }

        co_return std::make_shared<TSubqueryExpr>(
            loc, TSubqueryExpr::EKind::Exists, query);
    }

    if (IsOp(token, '(')) {
        auto next = ctx.Stream.Next(); ctx.Stream.Unget(next);
        NQumir::NAst::TExprPtr ret = nullptr;
        if (IsKeyword(next, "WITH") || IsKeyword(next, "SELECT")) {
            auto query = co_await select_stmt(ctx);
            ret = std::make_shared<TSubqueryExpr>(
                loc, TSubqueryExpr::EKind::Scalar, query);
        } else {
            ret = co_await expr(ctx);
        }
        next = ctx.Stream.Next();
        if (!IsOp(next, ')')) {
            co_return Error(next, "')' expected");
        }
        co_return ret;
    }

    co_return Error(token, "Unsupported expression");
}

// <function_call> ::= <qualified_name> "(" [ <function_args> ] ")"
// Falls back to a plain <qualified_name> reference when no "(" follows.
TAstExprTask function_call(TParserContext& ctx) {
    auto head = ctx.Stream.Next();
    auto loc = head.Location;
    ctx.Stream.Unget(head);

    auto name = co_await qualified_name(ctx);

    auto token = ctx.Stream.Next();
    if (!IsOp(token, '(')) {
        ctx.Stream.Unget(token);
        co_return qualified_ref(loc, name);
    }

    std::vector<NQumir::NAst::TExprPtr> args;
    auto next = ctx.Stream.Next();
    if (!IsOp(next, ')')) {
        ctx.Stream.Unget(next);
        auto block = co_await function_args(ctx);
        args = std::static_pointer_cast<NQumir::NAst::TBlockExpr>(block)->Stmts;
        next = ctx.Stream.Next();
        if (!IsOp(next, ')')) {
            co_return Error(next, "')' expected");
        }
    }

    std::string fname;
    for (size_t i = 0; i < name.size(); ++i) {
        if (i) {
            fname += '.';
        }
        fname += name[i];
    }
    co_return call(loc, fname, std::move(args));
}

// <function_args> ::= "*" | [ "DISTINCT" ] <expr> { "," <expr> }
TAstExprTask function_args(TParserContext& ctx) {
    auto token = ctx.Stream.Next();
    if (IsOp(token, '*')) {
        // represent star argument as a synthetic identifier "*"
        co_return list(token.Location,
            { std::make_shared<NQumir::NAst::TIdentExpr>(token.Location, "*") });
    }

    if (IsKeyword(token, "DISTINCT")) {
        // there is no DISTINCT aggregate; mark the arguments with a synthetic
        // `distinct(...)` call for the planner to expand via double aggregation
        auto block = co_await expr_list(ctx);
        auto args = std::static_pointer_cast<NQumir::NAst::TBlockExpr>(block)->Stmts;
        co_return list(token.Location,
            { call(token.Location, "distinct", std::move(args)) });
    }

    ctx.Stream.Unget(token);
    co_return co_await expr_list(ctx);
}

// <case_expr> ::= "CASE" [ <expr> ] { "WHEN" <expr> "THEN" <expr> } [ "ELSE" <expr> ] "END"
// (the leading "CASE" is consumed by <primary_expr>)
// Desugars to a chain of if-expressions. A simple CASE is the searched CASE whose
// conditions compare the operand with each WHEN value, as the SQL standard defines
// it, so a NULL operand matches no WHEN; the comparisons share the operand node.
TAstExprTask case_expr(TParserContext& ctx) {
    std::vector<std::pair<NQumir::NAst::TExprPtr, NQumir::NAst::TExprPtr>> branches;

    auto token = ctx.Stream.Next();
    auto loc = token.Location;
    NQumir::NAst::TExprPtr operand;
    if (!IsKeyword(token, "WHEN")) {
        ctx.Stream.Unget(token);
        operand = co_await expr(ctx);
        token = ctx.Stream.Next();
    }

    while (IsKeyword(token, "WHEN")) {
        auto cond = co_await expr(ctx);
        if (operand) {
            cond = binary(token.Location, "=="_op, operand, cond);
        }

        auto then = ctx.Stream.Next();
        if (!IsKeyword(then, "THEN")) {
            co_return Error(then, "`THEN' expected");
        }
        auto result = co_await expr(ctx);
        branches.emplace_back(std::move(cond), std::move(result));

        token = ctx.Stream.Next();
    }

    if (branches.empty()) {
        co_return Error(token, "`WHEN' expected in CASE");
    }

    NQumir::NAst::TExprPtr ret = nullptr;
    if (IsKeyword(token, "ELSE")) {
        ret = co_await expr(ctx);
        token = ctx.Stream.Next();
    } else {
        ret = call(loc, "qdb_sql_null", {});
    }

    if (!IsKeyword(token, "END")) {
        co_return Error(token, "`END' expected");
    }

    for (auto it = branches.rbegin(); it != branches.rend(); ++it) {
        // A WHEN condition may be Nullable[bool] (SQL: NULL is not TRUE); qdb_is_true
        // coerces it to a plain bool for the `if` (identity for a non-nullable bool).
        auto cond = call(loc, "qdb_is_true", {it->first});
        ret = std::make_shared<NQumir::NAst::TIfExpr>(loc, std::move(cond), it->second, ret);
    }
    co_return ret;
}

// <expr_list> ::= <expr> { "," <expr> }
TAstExprTask expr_list(TParserContext& ctx) {
    std::vector<NQumir::NAst::TExprPtr> items;
    TToken token;
    TLocation loc;
    do {
        auto item = co_await expr(ctx);
        if (items.empty()) {
            loc = item->Location;
        }
        items.emplace_back(std::move(item));
        token = ctx.Stream.Next();
    } while (IsOp(token, ','));
    ctx.Stream.Unget(token);
    co_return list(loc, std::move(items));
}

// <expr_list_or_subquery> ::= <select_stmt> | <expr> { "," <expr> }
TAstExprTask expr_list_or_subquery(TParserContext& ctx) {
    auto token = ctx.Stream.Next();
    bool subquery = IsKeyword(token, "SELECT") || IsKeyword(token, "WITH");
    ctx.Stream.Unget(token);

    if (subquery) {
        auto query = co_await select_stmt(ctx);
        co_return std::make_shared<TSubqueryExpr>(
            token.Location, TSubqueryExpr::EKind::In, query);
    }
    co_return co_await expr_list(ctx);
}

// <type_name> ::= "BOOL" | "BOOLEAN" | "INT" | "INTEGER" | "BIGINT"
//   | "FLOAT" | "DOUBLE" | "TEXT" | "VARCHAR" | "DATE" | "TIMESTAMP"
//   | "DECIMAL" [ "(" <integer> [ "," <integer> ] ")" ]
TAstTypeTask type_name(TParserContext& ctx) {
    using namespace NQumir::NAst;

    auto token = ctx.Stream.Next();
    if (token.Type != TToken::Identifier && token.Type != TToken::Keyword) {
        co_return Error(token, "type name expected");
    }

    // type names are case-insensitive; keywords already arrive upper-cased
    std::string name;
    for (char c : token.Name) {
        name += std::toupper(static_cast<unsigned char>(c));
    }

    if (name == "BOOL" || name == "BOOLEAN") {
        co_return std::make_shared<TBoolType>();
    } else if (name == "INT" || name == "INTEGER") {
        co_return std::make_shared<TIntegerType>(TIntegerType::I32);
    } else if (name == "BIGINT") {
        co_return std::make_shared<TIntegerType>(TIntegerType::I64);
    } else if (name == "FLOAT" || name == "DOUBLE") {
        co_return std::make_shared<TFloatType>();
    } else if (name == "TEXT" || name == "VARCHAR") {
        co_return std::make_shared<TStringType>();
    } else if (name == "DATE" || name == "TIMESTAMP") {
        co_return std::make_shared<TNamedType>(name, nullptr);
    } else if (name == "DECIMAL") {
        int32_t precisionValue = DefaultDecimalPrecision;
        int32_t scaleValue = DefaultDecimalScale;
        auto next = ctx.Stream.Next();
        if (IsOp(next, '(')) {
            auto precision = ctx.Stream.Next();
            if (precision.Type != TToken::Integer) {
                co_return Error(precision, "integer expected in DECIMAL");
            }
            precisionValue = static_cast<int32_t>(precision.Value.i64);
            next = ctx.Stream.Next();
            if (IsOp(next, ',')) {
                auto scale = ctx.Stream.Next();
                if (scale.Type != TToken::Integer) {
                    co_return Error(scale, "integer expected in DECIMAL");
                }
                scaleValue = static_cast<int32_t>(scale.Value.i64);
                next = ctx.Stream.Next();
            }
            if (!IsOp(next, ')')) {
                co_return Error(next, "')' expected");
            }
        } else {
            ctx.Stream.Unget(next);
        }
        try {
            co_return std::make_shared<NQdb::TDecimal>(precisionValue, scaleValue);
        } catch (const std::invalid_argument& e) {
            co_return Error(token, e.what());
        }
    }

    co_return Error(token, "unknown type name `" + name + "'");
}


} // namespace

std::expected<TSqlNodePtr, NQumir::TError> TParser::Parse(TTokenStream& stream)
{
    TWrappedTokenStream wrappedStream(stream, /*windowSize = */ 10);
    TParserContext context(wrappedStream);
    auto task = statement(context);
    auto result = task.result();
    if (!result) {
        return std::unexpected(result.error());
    }
    return TSqlNodePtr(result.value());
}

std::expected<std::vector<TSqlNodePtr>, NQumir::TError> TParser::ParseAll(
    TTokenStream& stream)
{
    TWrappedTokenStream wrappedStream(stream, /*windowSize = */ 10);
    TParserContext context(wrappedStream);
    std::vector<TSqlNodePtr> statements;
    while (true) {
        auto token = context.Stream.Next();
        if (IsEof(token)) {
            break;
        }
        context.Stream.Unget(token);
        auto task = statement(context);
        auto result = task.result();
        if (!result) {
            return std::unexpected(result.error());
        }
        statements.emplace_back(result.value());
    }
    return statements;
}

} // namespace NSql
} // namespace NQdb
