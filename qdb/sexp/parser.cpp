#include <qdb/sexp/parser.h>

#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/late_materialize.h>
#include <qdb/plan/ops/limit.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/union.h>
#include <qdb/plan/ops/sort.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/ops/window.h>

#include <limits>
#include <optional>

namespace NQdb {
namespace NSexp {

using namespace NQumir::NAst::NCore;
using namespace NQumir::NAst;
using NQumir::TError;
using NQumir::TLocation;

std::optional<std::string> ReadIdentifier(IParseHandle& h, TToken token) {
    if (token.Type != TToken::Identifier) {
        return std::nullopt;
    }
    std::string result = token.Name;
    while (true) {
        auto dash = h.Next();
        const bool isDash = IParseHandle::IsOp(dash, '-')
            || (dash.Type == TToken::Identifier && dash.Name == "-");
        if (!isDash) {
            h.Unget(dash);
            break;
        }
        auto part = h.Next();
        if (part.Type != TToken::Identifier) {
            h.Unget(part);
            h.Unget(dash);
            break;
        }
        result += "-";
        result += part.Name;
    }
    return result;
}

std::optional<ESortDirection> ParseSortDirection(std::string_view name) {
    if (name == "asc") return ESortDirection::Asc;
    if (name == "desc") return ESortDirection::Desc;
    return std::nullopt;
}

std::optional<ESortNulls> ParseSortNulls(std::string_view name) {
    if (name == "nulls-default") return ESortNulls::Default;
    if (name == "nulls-first") return ESortNulls::First;
    if (name == "nulls-last") return ESortNulls::Last;
    return std::nullopt;
}

std::optional<EWindowFrameMode> ParseFrameMode(std::string_view name) {
    if (name == "rows") return EWindowFrameMode::Rows;
    if (name == "range") return EWindowFrameMode::Range;
    return std::nullopt;
}

std::optional<EFrameBoundKind> ParseFrameBoundKind(std::string_view name) {
    if (name == "unbounded-preceding") return EFrameBoundKind::UnboundedPreceding;
    if (name == "preceding") return EFrameBoundKind::Preceding;
    if (name == "current-row") return EFrameBoundKind::CurrentRow;
    if (name == "following") return EFrameBoundKind::Following;
    if (name == "unbounded-following") return EFrameBoundKind::UnboundedFollowing;
    return std::nullopt;
}

bool CanStartExpression(const TToken& token) {
    return token.Type == TToken::Integer
        || token.Type == TToken::Float
        || token.Type == TToken::String
        || token.Type == TToken::Char
        || token.Type == TToken::Boolean
        || token.Type == TToken::Identifier
        || IParseHandle::IsOp(token, '(');
}

namespace {

TAstTask ParseJoinResidual(IParseHandle& h)
{
    auto residual = co_await h.Expr();
    co_await h.Take(')');
    co_return residual;
}

NQumir::TExpectedTask<TRuntimeFilterSpec, TError, TLocation>
ParseJoinEmitFilter(IParseHandle& h)
{
    auto idTok = h.Next();
    if (idTok.Type != TToken::Integer
        || idTok.Value.i64 <= 0
        || idTok.Value.i64 > std::numeric_limits<uint32_t>::max()) {
        co_return IParseHandle::MakeError(
            idTok, "(emit-filter) expects a positive uint32 id");
    }
    auto sideTok = h.Next();
    auto sideName = ReadIdentifier(h, sideTok);
    auto side = sideName ? ParseJoinFilterSide(*sideName) : std::nullopt;
    if (!side) {
        co_return IParseHandle::MakeError(
            sideTok, "(emit-filter) expects build side 'left' or 'right'");
    }
    co_await h.Take(')');
    co_return TRuntimeFilterSpec{
        .Id = static_cast<uint32_t>(idTok.Value.i64),
        .BuildSide = *side,
    };
}

} // namespace

TNodeParserMap MakeRelParsers(TRelParserOptions options) {
    TNodeParserMap parsers;
    parsers["rel"] = [opts = options](IParseHandle& h, TLocation loc) -> TAstTask {
        auto nameTok = h.Next();
        auto relName = ReadIdentifier(h, nameTok);
        if (!relName) {
            co_return IParseHandle::MakeError(nameTok, "expected rel operator name after 'rel'");
        }

        if (*relName == TSourceOperator::OpId) {
            auto pathTok = h.Next();
            if (pathTok.Type != TToken::String) {
                co_return IParseHandle::MakeError(pathTok, "(rel source) expects a path string");
            }
            // Optional explicit alias and row-group predicate hint:
            // (rel source "path" "alias" (> x 5))
            std::string explicitAlias;
            TExprPtr rowGroupPredicate;
            auto peek = h.Next();
            if (peek.Type == TToken::String) {
                explicitAlias = peek.Name;
                peek = h.Next();
            }
            if (!IParseHandle::IsOp(peek, ')')) {
                if (!CanStartExpression(peek)) {
                    co_return IParseHandle::MakeError(
                        peek,
                        "(rel source) expects ')' or alias string, optionally "
                        "followed by a row-group predicate expression");
                }
                h.Unget(peek);
                rowGroupPredicate = co_await h.Expr();
                co_await h.Take(')');
            }
            if (!opts.SourceFactory) {
                co_return TError(loc, "(rel source) requires a SourceFactory");
            }
            auto opExpr = opts.SourceFactory(std::string_view(pathTok.Name), loc);
            if (!opExpr) co_return TError(loc, "(rel source) factory returned null");
            if (!explicitAlias.empty()) {
                if (auto* src = dynamic_cast<TSourceOperator*>(opExpr.get())) {
                    src->SetAlias(std::move(explicitAlias));
                }
            }
            if (rowGroupPredicate) {
                if (auto* src = dynamic_cast<TSourceOperator*>(opExpr.get())) {
                    src->SetRowGroupPredicate(std::move(rowGroupPredicate));
                }
            }
            co_return opExpr;
        }

        if (*relName == TCteRef::OpId) {
            auto idTok = h.Next();
            if (idTok.Type != TToken::Integer) {
                co_return IParseHandle::MakeError(idTok, "(rel cte-ref) expects an integer id");
            }
            co_await h.Take(')');
            if (!opts.CteRegistry) {
                co_return TError(loc, "(rel cte-ref) requires a CTE registry");
            }
            auto it = opts.CteRegistry->find(static_cast<uint32_t>(idTok.Value.i64));
            if (it == opts.CteRegistry->end()) {
                co_return TError(loc, "(rel cte-ref) references unknown CTE id");
            }
            co_return std::make_shared<TCteRef>(it->second);
        }

        if (*relName == TFilterOperator::OpId) {
            auto inputExpr = co_await h.Expr();
            auto input = std::static_pointer_cast<IOperator>(inputExpr);
            auto pred = co_await h.Expr();
            co_await h.Take(')');
            co_return std::make_shared<TFilterOperator>(std::move(input), std::move(pred));
        }

        if (*relName == TProjectOperator::OpId) {
            auto inputExpr = co_await h.Expr();
            auto input = std::static_pointer_cast<IOperator>(inputExpr);
            std::vector<TProjectionSpec> specs;
            while (true) {
                auto tok = h.Next();
                if (IParseHandle::IsOp(tok, ')')) break;
                if (!IParseHandle::IsOp(tok, '(')) {
                    co_return IParseHandle::MakeError(tok, "expected '(' before projection spec");
                }
                auto projName = h.Next();
                if (projName.Type != TToken::Identifier) {
                    co_return IParseHandle::MakeError(projName, "expected projection output name");
                }
                auto projExpr = co_await h.Expr();
                co_await h.Take(')');
                specs.push_back({projName.Name, std::move(projExpr)});
            }
            co_return std::make_shared<TProjectOperator>(std::move(input), std::move(specs));
        }

        if (*relName == TLateMaterializeOperator::OpId) {
            auto inputExpr = co_await h.Expr();
            auto input = std::static_pointer_cast<IOperator>(inputExpr);

            co_await h.Take('(');
            auto locatorTagTok = h.Next();
            auto locatorTag = ReadIdentifier(h, locatorTagTok);
            if (!locatorTag || *locatorTag != "locator") {
                co_return IParseHandle::MakeError(locatorTagTok, "expected '(locator <column>)'");
            }
            auto locatorTok = h.Next();
            auto locator = ReadIdentifier(h, locatorTok);
            if (!locator) {
                co_return IParseHandle::MakeError(locatorTok, "expected locator column name");
            }
            co_await h.Take(')');

            std::vector<TLateMaterializeColumn> columns;
            while (true) {
                auto tok = h.Next();
                if (IParseHandle::IsOp(tok, ')')) {
                    break;
                }
                if (!IParseHandle::IsOp(tok, '(')) {
                    co_return IParseHandle::MakeError(tok, "expected '(column <output> <physical> <type>)'");
                }
                auto columnTagTok = h.Next();
                auto columnTag = ReadIdentifier(h, columnTagTok);
                if (!columnTag || *columnTag != "column") {
                    co_return IParseHandle::MakeError(columnTagTok, "expected 'column'");
                }
                auto outputTok = h.Next();
                auto output = ReadIdentifier(h, outputTok);
                if (!output) {
                    co_return IParseHandle::MakeError(outputTok, "expected output column name");
                }
                auto physicalTok = h.Next();
                auto physical = ReadIdentifier(h, physicalTok);
                if (!physical) {
                    co_return IParseHandle::MakeError(physicalTok, "expected physical column name");
                }
                auto type = co_await h.Type();
                co_await h.Take(')');
                columns.push_back({
                    .PhysicalName = std::move(*physical),
                    .OutputName = std::move(*output),
                    .Type = std::move(type),
                });
            }
            if (columns.empty()) {
                co_return TError(
                    loc, "(rel late-materialize) requires at least one column");
            }
            co_return std::make_shared<TLateMaterializeOperator>(
                std::move(input), std::move(*locator), std::move(columns));
        }

        if (*relName == TUnionAllOperator::OpId) {
            std::vector<TOperatorPtr> inputs;
            while (true) {
                auto tok = h.Next();
                if (IParseHandle::IsOp(tok, ')')) break;
                h.Unget(tok);
                auto branch = co_await h.Expr();
                inputs.push_back(std::static_pointer_cast<IOperator>(std::move(branch)));
            }
            co_return std::make_shared<TUnionAllOperator>(std::move(inputs));
        }

        if (*relName == TSortOperator::OpId || *relName == TTopSortOperator::OpId) {
            const bool isTopSort = *relName == TTopSortOperator::OpId;
            auto inputExpr = co_await h.Expr();
            auto input = std::static_pointer_cast<IOperator>(inputExpr);
            std::vector<TSortKey> keys;
            std::optional<int64_t> limit;

            while (true) {
                auto tok = h.Next();
                if (IParseHandle::IsOp(tok, ')')) break;
                if (!IParseHandle::IsOp(tok, '(')) {
                    co_return IParseHandle::MakeError(tok, "expected '(' before sort key or limit");
                }
                auto keyTok = h.Next();
                auto keyName = ReadIdentifier(h, keyTok);
                if (!keyName) {
                    co_return IParseHandle::MakeError(keyTok, "expected sort key column or 'limit'");
                }
                if (*keyName == "limit") {
                    auto limitTok = h.Next();
                    if (limitTok.Type != TToken::Integer) {
                        co_return IParseHandle::MakeError(limitTok, "expected integer top-sort limit");
                    }
                    limit = limitTok.Value.i64;
                    co_await h.Take(')');
                    continue;
                }

                auto directionTok = h.Next();
                auto directionName = ReadIdentifier(h, directionTok);
                if (!directionName) {
                    co_return IParseHandle::MakeError(directionTok, "expected sort direction");
                }
                auto direction = ParseSortDirection(*directionName);
                if (!direction) {
                    co_return IParseHandle::MakeError(directionTok, "unknown sort direction: " + *directionName);
                }
                auto nullsTok = h.Next();
                auto nullsName = ReadIdentifier(h, nullsTok);
                if (!nullsName) {
                    co_return IParseHandle::MakeError(nullsTok, "expected sort nulls mode");
                }
                auto nulls = ParseSortNulls(*nullsName);
                if (!nulls) {
                    co_return IParseHandle::MakeError(nullsTok, "unknown sort nulls mode: " + *nullsName);
                }
                co_await h.Take(')');
                keys.push_back({*keyName, *direction, *nulls});
            }

            if (isTopSort) {
                if (!limit) {
                    co_return TError(loc, "(rel top-sort) requires (limit N)");
                }
                co_return std::make_shared<TTopSortOperator>(std::move(input), std::move(keys), *limit);
            }
            if (limit) {
                co_return TError(loc, "(rel sort) does not accept (limit N); use (rel top-sort)");
            }
            co_return std::make_shared<TSortOperator>(std::move(input), std::move(keys));
        }

        if (*relName == TLimitOperator::OpId) {
            auto inputExpr = co_await h.Expr();
            auto input = std::static_pointer_cast<IOperator>(inputExpr);
            std::optional<int64_t> limit;
            int64_t offset = 0;

            while (true) {
                auto tok = h.Next();
                if (IParseHandle::IsOp(tok, ')')) break;
                if (!IParseHandle::IsOp(tok, '(')) {
                    co_return IParseHandle::MakeError(tok, "expected '(' before limit option");
                }
                auto optTok = h.Next();
                auto optName = ReadIdentifier(h, optTok);
                if (!optName) {
                    co_return IParseHandle::MakeError(optTok, "expected limit option name");
                }
                auto valueTok = h.Next();
                if (valueTok.Type != TToken::Integer) {
                    co_return IParseHandle::MakeError(valueTok, "expected integer limit option value");
                }
                if (*optName == "limit") {
                    limit = valueTok.Value.i64;
                } else if (*optName == "offset") {
                    offset = valueTok.Value.i64;
                } else {
                    co_return IParseHandle::MakeError(optTok, "unknown limit option: " + *optName);
                }
                co_await h.Take(')');
            }
            if (!limit) {
                co_return TError(loc, "(rel limit) requires (limit N)");
            }
            co_return std::make_shared<TLimitOperator>(std::move(input), *limit, offset);
        }

        if (*relName == TAggregateOperator::OpId) {
            auto inputExpr = co_await h.Expr();
            auto input = std::static_pointer_cast<IOperator>(inputExpr);

            co_await h.Take('(');
            auto keysTok = h.Next();
            if (keysTok.Type != TToken::Identifier || keysTok.Name != "keys") {
                co_return IParseHandle::MakeError(keysTok, "expected 'keys' after '(' in (rel aggregate ...)");
            }
            std::vector<std::string> groupKeys;
            while (true) {
                auto tok = h.Next();
                if (IParseHandle::IsOp(tok, ')')) break;
                if (tok.Type != TToken::Identifier) {
                    co_return IParseHandle::MakeError(tok, "expected key column name");
                }
                groupKeys.push_back(tok.Name);
            }

            std::vector<TAggregateSpec> aggs;
            while (true) {
                auto tok = h.Next();
                if (IParseHandle::IsOp(tok, ')')) break;
                if (!IParseHandle::IsOp(tok, '(')) {
                    co_return IParseHandle::MakeError(tok, "expected '(' before aggregate spec");
                }
                auto aggTok = h.Next();
                if (aggTok.Type != TToken::Identifier || aggTok.Name != "agg") {
                    co_return IParseHandle::MakeError(aggTok, "expected 'agg'");
                }
                auto outName = h.Next();
                if (outName.Type != TToken::Identifier) {
                    co_return IParseHandle::MakeError(outName, "expected aggregate output name");
                }
                auto funcTok = h.Next();
                if (funcTok.Type != TToken::Identifier) {
                    co_return IParseHandle::MakeError(funcTok, "expected aggregate function name");
                }
                TExprPtr arg;
                auto closeTok = h.Next();
                if (!IParseHandle::IsOp(closeTok, ')')) {
                    h.Unget(closeTok);
                    arg = co_await h.Expr();
                    co_await h.Take(')');
                }
                aggs.push_back({outName.Name, funcTok.Name, std::move(arg)});
            }

            co_return std::make_shared<TAggregateOperator>(std::move(input), std::move(groupKeys), std::move(aggs));
        }

        if (*relName == TJoinOperator::OpId) {
            auto leftExpr = co_await h.Expr();
            auto left = std::static_pointer_cast<IOperator>(leftExpr);
            auto rightExpr = co_await h.Expr();
            auto right = std::static_pointer_cast<IOperator>(rightExpr);

            // Key pairs optionally carry NULL-equal comparison semantics.
            co_await h.Take('(');
            std::vector<TJoinKey> keys;
            while (true) {
                auto tok = h.Next();
                if (IParseHandle::IsOp(tok, ')')) break; // end of key list
                if (!IParseHandle::IsOp(tok, '(')) {
                    co_return IParseHandle::MakeError(tok, "expected '(' before join key pair");
                }
                auto leftKey = h.Next();
                if (leftKey.Type != TToken::Identifier) {
                    co_return IParseHandle::MakeError(leftKey, "expected left key column");
                }
                auto rightKey = h.Next();
                if (rightKey.Type != TToken::Identifier) {
                    co_return IParseHandle::MakeError(rightKey, "expected right key column");
                }
                bool nullsEqual = false;
                auto keyEnd = h.Next();
                if (keyEnd.Type == TToken::Identifier && keyEnd.Name == "nulls_equal") {
                    nullsEqual = true;
                    co_await h.Take(')');
                } else if (!IParseHandle::IsOp(keyEnd, ')')) {
                    co_return IParseHandle::MakeError(keyEnd, "expected ')' or nulls_equal after join keys");
                }
                keys.push_back({leftKey.Name, rightKey.Name, nullsEqual});
            }
            // Join type as a bare keyword: (inner)
            co_await h.Take('(');
            auto typeTok = h.Next();
            if (typeTok.Type != TToken::Identifier) {
                co_return IParseHandle::MakeError(typeTok, "expected join type name");
            }
            auto parsedType = ParseJoinType(typeTok.Name);
            if (!parsedType) {
                co_return IParseHandle::MakeError(typeTok, "unknown join type: " + typeTok.Name);
            }
            co_await h.Take(')');
            EJoinType type = *parsedType;
            if (keys.empty() && type != EJoinType::Inner) {
                co_return TError(loc, "cross join (empty key list) only supports inner type");
            }

            // Canonical plans tag the residual. Accept the former untagged
            // form as input so stored plans remain readable.
            TExprPtr filter;
            size_t residualCount = 0;
            std::optional<TRuntimeFilterSpec> runtimeFilter;
            for (;;) {
                auto tok = h.Next();
                if (IParseHandle::IsOp(tok, ')')) {
                    break;
                }
                if (IParseHandle::IsOp(tok, '(')) {
                    auto tagTok = h.Next();
                    auto tag = ReadIdentifier(h, tagTok);
                    if (tag && *tag == "residual") {
                        if (++residualCount > 1) {
                            co_return IParseHandle::MakeError(
                                tok, "(rel join) accepts at most one (residual …)");
                        }
                        filter = co_await ParseJoinResidual(h);
                        continue;
                    }
                    if (tag && *tag == "emit-filter") {
                        if (runtimeFilter) {
                            co_return IParseHandle::MakeError(
                                tok, "(rel join) accepts at most one (emit-filter …)");
                        }
                        runtimeFilter = co_await ParseJoinEmitFilter(h);
                        continue;
                    }
                    h.Unget(tagTok);
                }
                if (++residualCount > 1) {
                    co_return IParseHandle::MakeError(
                        tok, "(rel join) accepts at most one (residual …)");
                }
                h.Unget(tok);
                filter = co_await h.Expr();
            }

            auto joinOp = std::make_shared<TJoinOperator>(
                std::move(left), std::move(right), std::move(keys), type, std::move(filter));
            joinOp->MutableRuntimeFilter() = std::move(runtimeFilter);
            co_return joinOp;
        }

        if (*relName == TWindowOperator::OpId) {
            auto inputExpr = co_await h.Expr();
            auto input = std::static_pointer_cast<IOperator>(inputExpr);
            std::vector<std::string> partitionKeys;
            std::vector<TSortKey> orderKeys;
            std::optional<TWindowFrame> frame;
            std::vector<TWindowFunc> funcs;

            while (true) {
                auto tok = h.Next();
                if (IParseHandle::IsOp(tok, ')')) break;
                if (!IParseHandle::IsOp(tok, '(')) {
                    co_return IParseHandle::MakeError(tok, "expected '(' in (rel window ...)");
                }
                auto sectionTok = h.Next();
                auto section = ReadIdentifier(h, sectionTok);
                if (!section) {
                    co_return IParseHandle::MakeError(sectionTok, "expected window section name");
                }

                if (*section == "partition") {
                    while (true) {
                        auto keyTok = h.Next();
                        if (IParseHandle::IsOp(keyTok, ')')) break;
                        if (keyTok.Type != TToken::Identifier) {
                            co_return IParseHandle::MakeError(keyTok, "expected partition key column");
                        }
                        partitionKeys.push_back(keyTok.Name);
                    }
                } else if (*section == "order") {
                    while (true) {
                        auto keyTok = h.Next();
                        if (IParseHandle::IsOp(keyTok, ')')) break;
                        if (!IParseHandle::IsOp(keyTok, '(')) {
                            co_return IParseHandle::MakeError(keyTok, "expected '(' before window order key");
                        }
                        auto colTok = h.Next();
                        auto colName = ReadIdentifier(h, colTok);
                        if (!colName) {
                            co_return IParseHandle::MakeError(colTok, "expected order key column");
                        }
                        auto dirTok = h.Next();
                        auto dirName = ReadIdentifier(h, dirTok);
                        auto direction = dirName ? ParseSortDirection(*dirName) : std::nullopt;
                        if (!direction) {
                            co_return IParseHandle::MakeError(dirTok, "expected sort direction");
                        }
                        auto nullsTok = h.Next();
                        auto nullsName = ReadIdentifier(h, nullsTok);
                        auto nulls = nullsName ? ParseSortNulls(*nullsName) : std::nullopt;
                        if (!nulls) {
                            co_return IParseHandle::MakeError(nullsTok, "expected sort nulls mode");
                        }
                        co_await h.Take(')');
                        orderKeys.push_back({*colName, *direction, *nulls});
                    }
                } else if (*section == "frame") {
                    auto modeTok = h.Next();
                    auto modeName = ReadIdentifier(h, modeTok);
                    auto mode = modeName ? ParseFrameMode(*modeName) : std::nullopt;
                    if (!mode) {
                        co_return IParseHandle::MakeError(modeTok, "expected window frame mode");
                    }
                    TWindowFrame parsed;
                    parsed.Mode = *mode;
                    // Two bounds: (start <kind> <expr>?) (end <kind> <expr>?)
                    for (int side = 0; side < 2; ++side) {
                        co_await h.Take('(');
                        h.Next(); // bound tag: 'start' / 'end' (positional)
                        auto kindTok = h.Next();
                        auto kindName = ReadIdentifier(h, kindTok);
                        auto kind = kindName ? ParseFrameBoundKind(*kindName) : std::nullopt;
                        if (!kind) {
                            co_return IParseHandle::MakeError(kindTok, "expected frame bound kind");
                        }
                        TFrameBound bound;
                        bound.Kind = *kind;
                        auto next = h.Next();
                        if (!IParseHandle::IsOp(next, ')')) {
                            h.Unget(next);
                            bound.Offset = co_await h.Expr();
                            co_await h.Take(')');
                        }
                        (side == 0 ? parsed.Start : parsed.End) = std::move(bound);
                    }
                    co_await h.Take(')');
                    frame = std::move(parsed);
                } else if (*section == "fn") {
                    auto nameTok = h.Next();
                    if (nameTok.Type != TToken::Identifier) {
                        co_return IParseHandle::MakeError(nameTok, "expected window function output name");
                    }
                    auto funcTok = h.Next();
                    if (funcTok.Type != TToken::Identifier) {
                        co_return IParseHandle::MakeError(funcTok, "expected window function name");
                    }
                    TExprPtr arg;
                    auto closeTok = h.Next();
                    if (!IParseHandle::IsOp(closeTok, ')')) {
                        h.Unget(closeTok);
                        arg = co_await h.Expr();
                        co_await h.Take(')');
                    }
                    funcs.push_back({nameTok.Name, funcTok.Name, std::move(arg)});
                } else {
                    co_return IParseHandle::MakeError(sectionTok, "unknown window section: " + *section);
                }
            }

            co_return std::make_shared<TWindowOperator>(
                std::move(input), std::move(partitionKeys), std::move(orderKeys),
                std::move(frame), std::move(funcs));
        }

        co_return IParseHandle::MakeError(nameTok, "unknown rel operator: " + *relName);
    };

    // (query (cte <id> <plan>) ... (main <plan>)): register each definition (so
    // later (rel cte-ref <id>) resolves), then return the main plan.
    parsers["query"] = [opts = std::move(options)](IParseHandle& h, TLocation loc) -> TAstTask {
        if (!opts.CteRegistry) {
            co_return TError(loc, "(query) requires a CTE registry");
        }
        TOperatorPtr main;
        while (true) {
            auto open = h.Next();
            if (IParseHandle::IsOp(open, ')')) {
                break;
            }
            if (!IParseHandle::IsOp(open, '(')) {
                co_return IParseHandle::MakeError(open, "expected '(cte ...)' or '(main ...)'");
            }
            auto headTok = h.Next();
            auto head = ReadIdentifier(h, headTok);
            if (head && *head == "cte") {
                auto idTok = h.Next();
                if (idTok.Type != TToken::Integer) {
                    co_return IParseHandle::MakeError(idTok, "(cte) expects an integer id");
                }
                auto planExpr = co_await h.Expr();
                co_await h.Take(')');
                auto def = std::make_shared<TCteDefinition>();
                def->Id = static_cast<uint32_t>(idTok.Value.i64);
                def->Plan = std::static_pointer_cast<IOperator>(std::move(planExpr));
                def->OutputType = def->Plan->OutputColumns();
                auto [it, inserted] = opts.CteRegistry->emplace(def->Id, def);
                if (!inserted) {
                    co_return TError(loc, "duplicate CTE id");
                }
            } else if (head && *head == "main") {
                auto planExpr = co_await h.Expr();
                co_await h.Take(')'); // close (main ...)
                main = std::static_pointer_cast<IOperator>(std::move(planExpr));
                co_await h.Take(')'); // close (query ...)
                break;
            } else {
                co_return IParseHandle::MakeError(headTok, "expected 'cte' or 'main'");
            }
        }
        if (!main) {
            co_return TError(loc, "(query) missing (main ...)");
        }
        co_return main;
    };

    return parsers;
}

} // namespace NSexp
} // namespace NQdb
