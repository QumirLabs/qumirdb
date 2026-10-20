#include <qdb/plan/build.h>

#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/limit.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/sort.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/ops/union.h>
#include <qdb/plan/ops/cte_ref.h>
#include <qdb/plan/ops/window.h>

#include <qdb/plan/clone_expr.h>
#include <qdb/plan/sql_lowering.h>
#include <qdb/plan/passes/flatten_conjuncts.h>
#include <qdb/plan/passes/flatten_disjuncts.h>
#include <qdb/plan/passes/unbound_vars.h>

#include <qumir/parser/ast.h>
#include <qumir/parser/core/printer.h>
#include <qumir/parser/type.h>

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <functional>
#include <unordered_set>
#include <utility>
#include <vector>

namespace NQdb {

namespace {

namespace NAst = NQumir::NAst;
namespace NSql = NQdb::NSql;
using NQumir::TError;
using NQumir::TLocation;

bool IsReservedOutputColumnName(std::string_view name) {
    return name == InternalRowIdColumnName;
}

std::expected<void, TError> ValidateOutputColumnName(
    std::string_view name,
    const TLocation& location = {})
{
    if (IsReservedOutputColumnName(name)) {
        return std::unexpected(TError(
            location,
            "output column name '" + std::string(name) + "' is reserved"));
    }
    return {};
}

bool HasSubquery(const NAst::TExprPtr& expr) {
    if (!expr) {
        return false;
    }
    if (NAst::TMaybeNode<NSql::TSubqueryExpr>(expr)) {
        return true;
    }
    for (const auto& child : expr->Children()) {
        if (HasSubquery(child)) {
            return true;
        }
    }
    return false;
}

std::string ItemName(const NSql::TSqlSelectItem& item, size_t index) {
    if (!item.ColumnAliases.empty()) {
        return item.ColumnAliases.front();
    }
    if (item.Alias) {
        return *item.Alias;
    }
    if (auto ident = NAst::TMaybeNode<NAst::TIdentExpr>(item.Expr)) {
        // `SELECT t.col` (no alias) yields a column named `col` (unqualified), per SQL.
        // Keeping the `t.` prefix breaks later references to the bare name - notably CTE
        // columns referenced as `<cte>.col` / bare `col`, whose equi-joins then fail to
        // extract keys (keyless cross joins).
        const std::string& name = ident.Cast()->Name;
        auto dot = name.rfind('.');
        return dot == std::string::npos ? name : name.substr(dot + 1);
    }
    return "col" + std::to_string(index);
}

std::string TableName(const std::vector<std::string>& parts) {
    std::string name;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            name += '.';
        }
        name += parts[i];
    }
    return name;
}

// All distinct group keys (G) plus the grouping sets as index lists into them.
struct TGroupingParse {
    std::vector<TGroupKey> Keys;
    std::vector<std::vector<size_t>> Sets;
    bool HasGroupingSyntax = false;
};

bool IsGroupingSyntax(const NSql::TSqlNodePtr& element) {
    return NSql::TMaybeNode<NSql::TSqlRollUp>(element)
        || NSql::TMaybeNode<NSql::TSqlCube>(element)
        || NSql::TMaybeNode<NSql::TSqlGroupingSet>(element);
}

std::vector<NAst::TExprPtr> UnpackExprs(const NAst::TExprPtr& exprs) {
    std::vector<NAst::TExprPtr> out;
    if (auto block = NAst::TMaybeNode<NAst::TBlockExpr>(exprs)) {
        out = block.Cast()->Stmts;
    } else if (exprs) {
        out.push_back(exprs);
    }
    return out;
}

// One grouping element expands to a list of column-sets (a set = list of exprs).
std::expected<std::vector<std::vector<NAst::TExprPtr>>, TError>
ExpandGroupingElement(const NSql::TSqlNodePtr& element) {
    if (auto e = NSql::TMaybeNode<NSql::TSqlGroupingExprOrList>(element)) {
        return std::vector<std::vector<NAst::TExprPtr>>{ UnpackExprs(e.Cast()->Exprs) };
    }
    if (auto r = NSql::TMaybeNode<NSql::TSqlRollUp>(element)) {
        auto cols = UnpackExprs(r.Cast()->Exprs);
        std::vector<std::vector<NAst::TExprPtr>> sets;
        for (size_t k = cols.size() + 1; k-- > 0;) {
            sets.emplace_back(cols.begin(), cols.begin() + k);
        }
        return sets;
    }
    if (auto c = NSql::TMaybeNode<NSql::TSqlCube>(element)) {
        auto cols = UnpackExprs(c.Cast()->Exprs);
        if (cols.size() > 12) {
            return std::unexpected(TError("CUBE with more than 12 columns is not supported"));
        }
        std::vector<std::vector<NAst::TExprPtr>> sets;
        for (size_t mask = (size_t{1} << cols.size()); mask-- > 0;) {
            std::vector<NAst::TExprPtr> s;
            for (size_t i = 0; i < cols.size(); ++i) {
                if (mask & (size_t{1} << i)) s.push_back(cols[i]);
            }
            sets.push_back(std::move(s));
        }
        return sets;
    }
    if (auto gs = NSql::TMaybeNode<NSql::TSqlGroupingSet>(element)) {
        std::vector<std::vector<NAst::TExprPtr>> sets;
        for (const auto& item : gs.Cast()->Items) {
            auto sub = ExpandGroupingElement(item);
            if (!sub) return std::unexpected(sub.error());
            for (auto& s : *sub) sets.push_back(std::move(s));
        }
        return sets;
    }
    return std::unexpected(TError("unsupported grouping element"));
}

std::expected<TGroupingParse, TError> ParseGrouping(const NSql::TSqlGroupBy& groupBy) {
    // Multiple grouping elements combine as a cross product of their set-lists.
    std::vector<std::vector<NAst::TExprPtr>> combos = {{}};
    for (const auto& item : groupBy.Items) {
        auto elementSets = ExpandGroupingElement(item);
        if (!elementSets) {
            return std::unexpected(elementSets.error());
        }
        std::vector<std::vector<NAst::TExprPtr>> next;
        for (const auto& base : combos) {
            for (const auto& choice : *elementSets) {
                auto merged = base;
                merged.insert(merged.end(), choice.begin(), choice.end());
                next.push_back(std::move(merged));
            }
        }
        combos = std::move(next);
    }

    TGroupingParse result;
    for (const auto& item : groupBy.Items) {
        if (IsGroupingSyntax(item)) {
            result.HasGroupingSyntax = true;
            break;
        }
    }
    std::unordered_map<std::string, size_t> keyIndex; // PrintAst -> index in Keys
    auto keyFor = [&](const NAst::TExprPtr& expr) -> std::expected<size_t, TError> {
        std::string k = NAst::NCore::PrintAst(expr);
        if (auto it = keyIndex.find(k); it != keyIndex.end()) {
            return it->second;
        }
        TGroupKey gk;
        if (auto ident = NAst::TMaybeNode<NAst::TIdentExpr>(expr)) {
            gk = { .Name = ident.Cast()->Name, .Expression = Ident(expr->Location, ident.Cast()->Name) };
        } else if (HasSubquery(expr)) {
            return std::unexpected(TError(expr->Location, "GROUP BY on a subquery is not supported"));
        } else {
            gk = { .Name = "gb_" + std::to_string(result.Keys.size()), .Expression = expr, .Computed = true };
        }
        size_t idx = result.Keys.size();
        result.Keys.push_back(std::move(gk));
        keyIndex[k] = idx;
        return idx;
    };
    for (const auto& combo : combos) {
        std::vector<size_t> set;
        for (const auto& expr : combo) {
            auto idx = keyFor(expr);
            if (!idx) return std::unexpected(idx.error());
            if (std::find(set.begin(), set.end(), *idx) == set.end()) {
                set.push_back(*idx);
            }
        }
        result.Sets.push_back(std::move(set));
    }
    return result;
}

EJoinType MapJoinType(NSql::ESqlJoinType type) {
    switch (type) {
        case NSql::ESqlJoinType::Inner: return EJoinType::Inner;
        case NSql::ESqlJoinType::Left: return EJoinType::Left;
        case NSql::ESqlJoinType::Right: return EJoinType::Right;
        case NSql::ESqlJoinType::Full: return EJoinType::Full;
        case NSql::ESqlJoinType::LeftSemi: return EJoinType::LeftSemi;
        case NSql::ESqlJoinType::RightSemi: return EJoinType::RightSemi;
        case NSql::ESqlJoinType::Cross: return EJoinType::Inner;
    }
    return EJoinType::Inner;
}

std::expected<TOperatorPtr, TError> BuildQuery(
    const NSql::TSqlQuery& query,
    const TTableSourceFactory& sources);

bool HasWindowExpr(const NAst::TExprPtr& expr);
bool HasWindowExpr(const NSql::TSqlQuery& query);
bool HasWindowExpr(const NSql::TSqlNodePtr& node);

bool HasWindowExpr(const NSql::TSqlPtr<NSql::TSqlOrder>& order) {
    if (!order) {
        return false;
    }
    for (const auto& item : order->Items) {
        if (item && HasWindowExpr(item->Expr)) {
            return true;
        }
    }
    return false;
}

bool HasWindowExpr(const NAst::TExprPtr& expr) {
    if (!expr) {
        return false;
    }
    if (NAst::TMaybeNode<NSql::TWindowExpr>(expr)) {
        return true;
    }
    if (auto sub = NAst::TMaybeNode<NSql::TSubqueryExpr>(expr)) {
        if (sub.Cast()->Query && HasWindowExpr(*sub.Cast()->Query)) {
            return true;
        }
    }
    for (const auto& child : expr->Children()) {
        if (HasWindowExpr(child)) {
            return true;
        }
    }
    return false;
}

// True if `expr` contains a window function in its OWN scope - not descending
// into nested subqueries, whose windows belong to that subquery. Used to reject
// windows in WHERE/HAVING and in ORDER BY over an independent scalar subquery.
bool HasWindowExprLocal(const NAst::TExprPtr& expr) {
    if (!expr) {
        return false;
    }
    if (NAst::TMaybeNode<NSql::TWindowExpr>(expr)) {
        return true;
    }
    if (NAst::TMaybeNode<NSql::TSubqueryExpr>(expr)) {
        return false;
    }
    for (const auto& child : expr->Children()) {
        if (HasWindowExprLocal(child)) {
            return true;
        }
    }
    return false;
}

bool HasWindowExpr(const NSql::TSqlQuery& query) {
    if (query.WithClause) {
        for (const auto& cte : query.WithClause->Ctes) {
            if (cte && cte->Query && HasWindowExpr(*cte->Query)) {
                return true;
            }
        }
    }
    return HasWindowExpr(query.Body)
        || HasWindowExpr(query.OrderBy)
        || HasWindowExpr(query.Limit)
        || HasWindowExpr(query.Offset);
}

bool HasWindowExpr(const NSql::TSqlNodePtr& node) {
    if (!node) {
        return false;
    }
    if (auto query = NSql::TMaybeNode<NSql::TSqlQuery>(node)) {
        return HasWindowExpr(*query.Cast());
    }
    if (auto select = NSql::TMaybeNode<NSql::TSqlSelect>(node)) {
        auto s = select.Cast();
        if (s->SelectList) {
            for (const auto& item : s->SelectList->Items) {
                if (item && !item->Star && HasWindowExpr(item->Expr)) {
                    return true;
                }
            }
        }
        if (s->From) {
            for (const auto& item : s->From->Items) {
                if (HasWindowExpr(std::static_pointer_cast<NSql::TSqlNode>(item))) {
                    return true;
                }
            }
        }
        if (s->GroupBy) {
            for (const auto& item : s->GroupBy->Items) {
                if (HasWindowExpr(item)) {
                    return true;
                }
            }
        }
        return HasWindowExpr(s->Where) || HasWindowExpr(s->Having);
    }
    if (auto setOp = NSql::TMaybeNode<NSql::TSqlSetOp>(node)) {
        return HasWindowExpr(setOp.Cast()->Left) || HasWindowExpr(setOp.Cast()->Right);
    }
    if (auto sub = NSql::TMaybeNode<NSql::TSqlSubqueryTable>(node)) {
        return sub.Cast()->Query && HasWindowExpr(*sub.Cast()->Query);
    }
    if (auto join = NSql::TMaybeNode<NSql::TSqlJoin>(node)) {
        auto j = join.Cast();
        return HasWindowExpr(std::static_pointer_cast<NSql::TSqlNode>(j->Left))
            || HasWindowExpr(std::static_pointer_cast<NSql::TSqlNode>(j->Right))
            || (j->Condition && HasWindowExpr(j->Condition->On));
    }
    if (auto exprOrList = NSql::TMaybeNode<NSql::TSqlGroupingExprOrList>(node)) {
        return HasWindowExpr(exprOrList.Cast()->Exprs);
    }
    if (auto rollup = NSql::TMaybeNode<NSql::TSqlRollUp>(node)) {
        return HasWindowExpr(rollup.Cast()->Exprs);
    }
    if (auto cube = NSql::TMaybeNode<NSql::TSqlCube>(node)) {
        return HasWindowExpr(cube.Cast()->Exprs);
    }
    if (auto groupingSet = NSql::TMaybeNode<NSql::TSqlGroupingSet>(node)) {
        for (const auto& item : groupingSet.Cast()->Items) {
            if (HasWindowExpr(item)) {
                return true;
            }
        }
    }
    return false;
}

// Output aliases are bound against the typed flattened schema by the pass.
std::expected<TOperatorPtr, TError> ApplyColumnAliases(
    TOperatorPtr plan, const std::vector<std::string>& aliases)
{
    std::vector<TProjectionSpec> provisional;
    const auto fields = plan->OutputColumns()->Fields;
    for (size_t i = 0; i < aliases.size(); ++i) {
        if (auto valid = ValidateOutputColumnName(aliases[i]); !valid) {
            return std::unexpected(valid.error());
        }
        provisional.push_back({.Name = aliases[i],
            .Expression = Ident({}, i < fields.size() ? fields[i].first : aliases[i])});
    }
    auto project = std::make_shared<TProjectOperator>(std::move(plan), std::move(provisional));
    project->MutableOutputAliases() = TOutputAliasesBinding{.Columns = aliases};
    return project;
}

// Qualifies a subplan's output columns as `alias.col`. Source tables get this from
// AssignSourceAliases, but subqueries/CTEs used with an alias need it here so the
// outer query can reference `alias.col` (and equi-join keys on them are extracted).
TOperatorPtr AliasSubplan(TOperatorPtr plan, const std::string& alias) {
    std::vector<TProjectionSpec> provisional;
    for (const auto& [name, _] : plan->OutputColumns()->Fields) {
        const auto dot = name.rfind('.');
        const auto bare = dot == std::string::npos ? name : name.substr(dot + 1);
        provisional.push_back({.Name = alias + "." + bare, .Expression = Ident({}, name)});
    }
    auto project = std::make_shared<TProjectOperator>(std::move(plan), std::move(provisional));
    project->MutableOutputAliases() = TOutputAliasesBinding{.Qualifier = alias};
    return project;
}

// SELECT DISTINCT groups on every output column. These keys describe the built
// output; the binding pass replaces them with the bound columns.
TOperatorPtr ApplyDistinct(TOperatorPtr projected) {
    std::vector<std::string> keys;
    for (const auto& [name, _] : projected->OutputColumns()->Fields) {
        keys.push_back(name);
    }
    auto aggregate = std::make_shared<TAggregateOperator>(
        std::move(projected), std::move(keys), std::vector<TAggregateSpec>{});
    aggregate->SetGroupsAllColumns(true);
    return aggregate;
}

std::expected<TOperatorPtr, TError> BuildTableRef(
    const NSql::TSqlPtr<NSql::TSqlTableRef>& ref,
    const TTableSourceFactory& sources)
{
    if (auto table = NSql::TMaybeNode<NSql::TSqlTableName>(ref)) {
        auto node = table.Cast();
        auto source = sources(TableName(node->Name));
        if (!source) {
            return std::unexpected(source.error());
        }
        if (auto sourceOp = TMaybeOp<TSourceOperator>(*source)) {
            if (node->Alias) {
                sourceOp.Cast()->SetAlias(*node->Alias);
            }
            return *source;
        }
        // A CTE inlined as a subplan: qualify its columns by the reference alias, or -
        // when referenced bare - by the CTE name, so `<cte>.col` references resolve
        // (otherwise the inlined columns stay bare and equi-joins over them drop their
        // keys, producing keyless cross joins).
        return AliasSubplan(
            std::move(*source), node->Alias ? *node->Alias : node->Name.back());
    }

    if (auto sub = NSql::TMaybeNode<NSql::TSqlSubqueryTable>(ref)) {
        auto node = sub.Cast();
        auto plan = BuildQuery(*node->Query, sources);
        if (!plan) {
            return plan;
        }
        if (node->ColumnAliases) {
            plan = ApplyColumnAliases(std::move(*plan), node->ColumnAliases->Items);
            if (!plan) {
                return plan;
            }
        }
        if (node->Alias) {
            return AliasSubplan(std::move(*plan), *node->Alias);
        }
        return plan;
    }

    if (auto maybeJoin = NSql::TMaybeNode<NSql::TSqlJoin>(ref)) {
        auto join = maybeJoin.Cast();
        auto left = BuildTableRef(join->Left, sources);
        if (!left) {
            return std::unexpected(left.error());
        }
        auto right = BuildTableRef(join->Right, sources);
        if (!right) {
            return std::unexpected(right.error());
        }

        // The builder does not extract equi-keys from ON; the whole predicate
        // becomes a residual that the optimizer later turns into join keys.
        std::vector<TJoinKey> keys;
        NAst::TExprPtr residual;
        if (join->Condition) {
            if (join->Condition->On) {
                if (HasSubquery(join->Condition->On)) {
                    return std::unexpected(TError("subqueries are not supported yet"));
                }
                residual = join->Condition->On;
            } else if (join->Condition->UsingColumns) {
                for (const auto& column : join->Condition->UsingColumns->Items) {
                    keys.push_back({ .Left = column, .Right = column });
                }
            }
        }
        return std::make_shared<TJoinOperator>(
            *left, *right, std::move(keys), MapJoinType(join->Type), std::move(residual));
    }

    return std::unexpected(TError("unsupported table reference"));
}

std::expected<TOperatorPtr, TError> BuildFrom(
    const NSql::TSqlFrom& from,
    const TTableSourceFactory& sources)
{
    if (from.Items.empty()) {
        return std::unexpected(TError("empty FROM"));
    }
    auto node = BuildTableRef(from.Items[0], sources);
    if (!node) {
        return std::unexpected(node.error());
    }
    TOperatorPtr result = *node;

    // Comma-separated tables become a left-deep cross-join tree; the WHERE
    // predicate stays a single top-level filter (equi-join extraction is an
    // optimizer pass).
    for (size_t i = 1; i < from.Items.size(); ++i) {
        auto right = BuildTableRef(from.Items[i], sources);
        if (!right) {
            return std::unexpected(right.error());
        }
        result = std::make_shared<TJoinOperator>(
            std::move(result), *right, std::vector<TJoinKey>{}, EJoinType::Inner, nullptr);
    }
    return result;
}

NAst::TExprPtr Conjoin(const std::vector<NAst::TExprPtr>& parts) {
    NAst::TExprPtr result;
    for (const auto& part : parts) {
        result = result
            ? std::make_shared<NAst::TBinaryExpr>(part->Location, NAst::TOperator("&&"), result, part)
            : part;
    }
    return result;
}

struct TDecorrelation {
    EJoinType Type;
    bool IsIn;
    std::shared_ptr<NSql::TSubqueryExpr> Subquery;
};

// Recognizes a WHERE conjunct that decorrelates into a semi/anti join:
// EXISTS / IN -> semi, NOT EXISTS / NOT IN -> anti.
std::optional<TDecorrelation> AsDecorrelation(const NAst::TExprPtr& conjunct) {
    NAst::TExprPtr inner = conjunct;
    bool negated = false;
    if (auto unary = NAst::TMaybeNode<NAst::TUnaryExpr>(inner); unary && unary.Cast()->Operator == "!") {
        negated = true;
        inner = unary.Cast()->Operand;
    }
    auto sub = NAst::TMaybeNode<NSql::TSubqueryExpr>(inner);
    if (!sub) {
        return std::nullopt;
    }
    using EKind = NSql::TSubqueryExpr::EKind;
    EKind kind = sub.Cast()->Kind;
    if (kind != EKind::Exists && kind != EKind::In) {
        return std::nullopt;
    }
    return TDecorrelation{
        .Type = negated ? EJoinType::LeftAnti : EJoinType::LeftSemi,
        .IsIn = kind == EKind::In,
        .Subquery = sub.Cast(),
    };
}

std::expected<TOperatorPtr, TError> BuildQuery(
    const NSql::TSqlQuery& query,
    const TTableSourceFactory& sources);

// EXISTS: the subquery's FROM becomes the right side and its WHERE (correlation
// plus local predicates) becomes the join residual.
std::expected<TOperatorPtr, TError> DecorrelateExists(
    TOperatorPtr left,
    const NSql::TSubqueryExpr& subquery,
    EJoinType type,
    const TTableSourceFactory& sources)
{
    auto maybeSelect = NSql::TMaybeNode<NSql::TSqlSelect>(subquery.Query->Body);
    if (!maybeSelect) {
        return std::unexpected(TError("expected a SELECT in EXISTS subquery"));
    }
    auto select = maybeSelect.Cast();
    if (subquery.Query->WithClause || select->GroupBy || select->Having) {
        return std::unexpected(TError("aggregated/CTE EXISTS subquery is not supported yet"));
    }
    if (!select->From) {
        return std::unexpected(TError("EXISTS subquery requires FROM"));
    }
    auto right = BuildFrom(*select->From, sources);
    if (!right) {
        return std::unexpected(right.error());
    }
    if (select->Where && HasSubquery(select->Where)) {
        return std::unexpected(TError("nested subqueries are not supported yet"));
    }
    return std::make_shared<TJoinOperator>(
        std::move(left), *right, std::vector<TJoinKey>{}, type, select->Where);
}

// IN: build the subquery as a full plan and join on `operand == <its only column>`.
std::expected<TOperatorPtr, TError> DecorrelateIn(
    TOperatorPtr left,
    const NSql::TSubqueryExpr& subquery,
    EJoinType type,
    const TTableSourceFactory& sources)
{
    if (!subquery.Operand || HasSubquery(subquery.Operand)) {
        return std::unexpected(TError("unsupported IN operand"));
    }
    auto right = BuildQuery(*subquery.Query, sources);
    if (!right) {
        return std::unexpected(right.error());
    }
    // Rename the subquery's output to a synthetic name so the residual's two sides
    // never share a name: otherwise `x IN (SELECT x ...)` yields `x == x`, which
    // qualification cannot bind to the left side and key extraction drops entirely,
    // leaving a keyless (unschedulable, condition-less) semi-join. The name is
    // internal - a semi/anti join exposes only its left side.
    const std::string name = "__qdb_in__";
    right = ApplyColumnAliases(std::move(*right), {name});
    if (!right) {
        return std::unexpected(right.error());
    }
    auto column = Ident(subquery.Operand->Location, name);
    auto residual = std::make_shared<NAst::TBinaryExpr>(
        subquery.Operand->Location, NAst::TOperator("=="), subquery.Operand, std::move(column));
    return std::make_shared<TJoinOperator>(
        std::move(left), *right, std::vector<TJoinKey>{}, type, std::move(residual));
}

// Qualified references belong to a FROM scope independently of its output types.
// Bare names need a complete schema; unresolved CTE/derived outputs can leave
// their classification unknown until binding.
enum class ESqlReferenceScope { Local, Outer, Unknown };

struct TLocalSqlScope {
    std::unordered_set<std::string> Qualifiers;
    std::unordered_set<std::string> BareColumns;
    bool CompleteBareColumns = true;

    ESqlReferenceScope Classify(const std::string& name) const {
        const auto dot = name.rfind('.');
        if (dot != std::string::npos) {
            return Qualifiers.contains(name.substr(0, dot))
                ? ESqlReferenceScope::Local
                : ESqlReferenceScope::Outer;
        }
        if (BareColumns.contains(name)) {
            return ESqlReferenceScope::Local;
        }
        return CompleteBareColumns
            ? ESqlReferenceScope::Outer
            : ESqlReferenceScope::Unknown;
    }
};

void CollectLocalColumns(
    const NSql::TSqlPtr<NSql::TSqlTableRef>& ref,
    const TTableSourceFactory& sources,
    TLocalSqlScope& out);

// Output names of a FROM subquery as the builder assigns them (ItemName). An
// unqualified star exposes the columns of its own FROM.
void CollectOutputNames(
    const NSql::TSqlNodePtr& body,
    const TTableSourceFactory& sources,
    TLocalSqlScope& out)
{
    if (auto query = NSql::TMaybeNode<NSql::TSqlQuery>(body)) {
        CollectOutputNames(query.Cast()->Body, sources, out);
    } else if (auto set = NSql::TMaybeNode<NSql::TSqlSetOp>(body)) {
        CollectOutputNames(set.Cast()->Left, sources, out);
    } else if (auto select = NSql::TMaybeNode<NSql::TSqlSelect>(body)) {
        const auto& items = select.Cast()->SelectList->Items;
        for (size_t i = 0; i < items.size(); ++i) {
            const auto& item = *items[i];
            if (!item.Star) {
                if (item.ColumnAliases.empty()) {
                    out.BareColumns.insert(ItemName(item, i));
                } else {
                    out.BareColumns.insert(item.ColumnAliases.begin(), item.ColumnAliases.end());
                }
            } else if (item.StarPrefix.empty() && select.Cast()->From) {
                TLocalSqlScope from;
                for (const auto& ref : select.Cast()->From->Items) {
                    CollectLocalColumns(ref, sources, from);
                }
                out.BareColumns.insert(from.BareColumns.begin(), from.BareColumns.end());
                out.CompleteBareColumns = out.CompleteBareColumns && from.CompleteBareColumns;
            } else {
                out.CompleteBareColumns = false;
            }
        }
    } else {
        out.CompleteBareColumns = false;
    }
}

void CollectLocalColumns(
    const NSql::TSqlPtr<NSql::TSqlTableRef>& ref,
    const TTableSourceFactory& sources,
    TLocalSqlScope& out)
{
    if (auto table = NSql::TMaybeNode<NSql::TSqlTableName>(ref)) {
        auto node = table.Cast();
        const std::string qualifier = node->Alias ? *node->Alias : node->Name.back();
        out.Qualifiers.insert(qualifier);
        if (auto src = sources(TableName(node->Name))) {
            if (auto source = TMaybeOp<TSourceOperator>(*src)) {
                for (const auto& col : source.Cast()->GetSource().Schema().Columns) {
                    out.BareColumns.insert(std::string(col.Name));
                }
            } else if (auto&& schema = (*src)->OutputColumns()) {
                // A CTE exposes its declared output names.
                for (const auto& [name, _] : schema->Fields) {
                    auto dot = name.rfind('.');
                    std::string bare = dot != std::string::npos ? name.substr(dot + 1) : name;
                    out.BareColumns.insert(bare);
                }
            } else {
                out.CompleteBareColumns = false;
            }
        } else {
            out.CompleteBareColumns = false;
        }
    } else if (auto derived = NSql::TMaybeNode<NSql::TSqlSubqueryTable>(ref)) {
        if (derived.Cast()->Alias) {
            out.Qualifiers.insert(*derived.Cast()->Alias);
        }
        if (derived.Cast()->ColumnAliases) {
            const auto& columns = derived.Cast()->ColumnAliases->Items;
            out.BareColumns.insert(columns.begin(), columns.end());
        } else {
            CollectOutputNames(derived.Cast()->Query, sources, out);
        }
    } else if (auto join = NSql::TMaybeNode<NSql::TSqlJoin>(ref)) {
        CollectLocalColumns(join.Cast()->Left, sources, out);
        CollectLocalColumns(join.Cast()->Right, sources, out);
    }
}

// Collects correlation equalities (outer_col == local_col) anywhere in a subquery
// predicate - including inside OR/AND - so a correlation buried in a disjunction is
// lifted too (not only top-level conjuncts).
void CollectCorrelations(
    const NAst::TExprPtr& expr,
    const TLocalSqlScope& local,
    std::vector<std::pair<std::string, std::string>>& out) // (outer, local)
{
    if (!expr) {
        return;
    }
    if (auto binary = NAst::TMaybeNode<NAst::TBinaryExpr>(expr);
        binary && binary.Cast()->Operator == "==")
    {
        auto left = NAst::TMaybeNode<NAst::TIdentExpr>(binary.Cast()->Left);
        auto right = NAst::TMaybeNode<NAst::TIdentExpr>(binary.Cast()->Right);
        if (left && right) {
            const std::string& ln = left.Cast()->Name;
            const std::string& rn = right.Cast()->Name;
            const auto leftScope = local.Classify(ln);
            const auto rightScope = local.Classify(rn);
            if (leftScope == ESqlReferenceScope::Local && rightScope == ESqlReferenceScope::Outer) {
                out.emplace_back(rn, ln);
                return;
            }
            if (rightScope == ESqlReferenceScope::Local && leftScope == ESqlReferenceScope::Outer) {
                out.emplace_back(ln, rn);
                return;
            }
        }
    }
    if (auto call = NAst::TMaybeNode<NAst::TCallExpr>(expr)) {
        for (const auto& arg : call.Cast()->Args) {
            CollectCorrelations(arg, local, out);
        }
        return;
    }
    for (auto* child : expr->MutableChildren()) {
        CollectCorrelations(*child, local, out);
    }
}

// Renames identifier leaves in place per `rename` (outer column -> local column).
void RenameIdents(
    const NAst::TExprPtr& expr,
    const std::unordered_map<std::string, std::string>& rename)
{
    if (!expr) {
        return;
    }
    if (auto ident = NAst::TMaybeNode<NAst::TIdentExpr>(expr)) {
        if (auto it = rename.find(ident.Cast()->Name); it != rename.end()) {
            ident.Cast()->Name = it->second;
        }
        return;
    }
    if (auto call = NAst::TMaybeNode<NAst::TCallExpr>(expr)) {
        for (auto& arg : call.Cast()->Args) {
            RenameIdents(arg, rename);
        }
        return;
    }
    for (auto* child : expr->MutableChildren()) {
        RenameIdents(*child, rename);
    }
}

// A conjunct that is a disjunction of positive EXISTS subqueries (`exists(A) OR
// exists(B) [OR ...]`). Returns the EXISTS subqueries, or empty if the shape doesn't match.
std::vector<std::shared_ptr<NSql::TSubqueryExpr>> AsExistsOrDisjunction(
    const NAst::TExprPtr& conjunct)
{
    std::vector<NAst::TExprPtr> disjuncts;
    FlattenDisjuncts(conjunct, disjuncts);
    if (disjuncts.size() < 2) {
        return {};
    }
    std::vector<std::shared_ptr<NSql::TSubqueryExpr>> existsList;
    for (const auto& d : disjuncts) {
        auto sub = NAst::TMaybeNode<NSql::TSubqueryExpr>(d);
        if (!sub || sub.Cast()->Kind != NSql::TSubqueryExpr::EKind::Exists) {
            return {};
        }
        existsList.push_back(sub.Cast());
    }
    return existsList;
}

// `exists(A) OR exists(B) OR ...`, where every EXISTS is correlated to the SAME outer
// column by a single equality, is equivalent to `outer IN (A_key UNION ALL B_key ...)`:
// a row satisfies the OR iff its outer key matches any branch. Each branch's correlation
// is neutralized and its inner key projected under a common name; the union feeds one
// left-semi join. Returns nullopt when the shape isn't supported (caller falls back).
std::expected<std::optional<TOperatorPtr>, TError> DecorrelateExistsOr(
    TOperatorPtr left,
    const std::vector<std::shared_ptr<NSql::TSubqueryExpr>>& existsList,
    const TTableSourceFactory& sources)
{
    std::optional<std::string> outerKey;
    std::vector<TOperatorPtr> branches;
    for (const auto& sub : existsList) {
        auto maybeSelect = NSql::TMaybeNode<NSql::TSqlSelect>(sub->Query->Body);
        if (!maybeSelect) {
            return std::optional<TOperatorPtr>{};
        }
        auto select = maybeSelect.Cast();
        if (sub->Query->WithClause || select->GroupBy || select->Having || !select->From
            || !select->Where || HasSubquery(select->Where))
        {
            return std::optional<TOperatorPtr>{};
        }
        TLocalSqlScope local;
        for (const auto& item : select->From->Items) {
            CollectLocalColumns(item, sources, local);
        }
        std::vector<std::pair<std::string, std::string>> corrs;
        CollectCorrelations(select->Where, local, corrs);
        std::set<std::pair<std::string, std::string>> uniq(corrs.begin(), corrs.end());
        if (uniq.size() != 1) {
            return std::optional<TOperatorPtr>{}; // need exactly one correlation
        }
        const auto& [outer, inner] = *uniq.begin();
        if (!outerKey) {
            outerKey = outer;
        } else if (*outerKey != outer) {
            return std::optional<TOperatorPtr>{}; // branches must share the outer key
        }
        auto body = BuildFrom(*select->From, sources);
        if (!body) {
            return std::unexpected(body.error());
        }
        auto where = CloneExpr(select->Where);
        RenameIdents(where, {{outer, inner}}); // correlation -> inner=inner (tautology)
        TOperatorPtr filtered = std::make_shared<TFilterOperator>(std::move(*body), std::move(where));
        std::vector<TProjectionSpec> projs{
            { .Name = "__exists_key__", .Expression = Ident(sub->Location, inner) }};
        branches.push_back(std::make_shared<TProjectOperator>(
            std::move(filtered), std::move(projs)));
    }
    auto un = std::make_shared<TUnionAllOperator>(std::move(branches));
    // Keyed left-semi (not a residual) so cost-based join reordering, which runs before
    // equi-key extraction, sees this as an equi-join and does not cross-join around it.
    std::vector<TJoinKey> keys{ TJoinKey{ *outerKey, "__exists_key__" } };
    return std::optional<TOperatorPtr>{std::make_shared<TJoinOperator>(
        std::move(left), std::move(un), std::move(keys),
        EJoinType::LeftSemi, nullptr)};
}

// True if any identifier leaf is not a local column (a leftover outer reference not
// covered by a correlation equality - that shape we cannot decorrelate).
bool HasUnmappedOuter(
    const NAst::TExprPtr& expr,
    const TLocalSqlScope& local)
{
    for (const auto& name : FindUnboundVars(expr)) {
        if (local.Classify(name) != ESqlReferenceScope::Local) {
            return true;
        }
    }
    return false;
}

// Replaces each scalar subquery in `expr` with a reference to a fresh column.
// Uncorrelated subqueries broadcast via a cross join (a single row). Correlated
// ones are decorrelated: the subquery is grouped by its correlation columns and
// LEFT-joined onto `node` (Apply elimination).
std::expected<NAst::TExprPtr, TError> ExtractScalarSubqueries(
    NAst::TExprPtr expr,
    TOperatorPtr& node,
    const TTableSourceFactory& sources,
    int& counter)
{
    if (!expr) {
        return expr;
    }
    if (auto sub = NAst::TMaybeNode<NSql::TSubqueryExpr>(expr)) {
        auto scalar = sub.Cast();
        if (scalar->Kind == NSql::TSubqueryExpr::EKind::In) {
            // An uncorrelated IN-subquery nested inside a boolean expression (e.g. one
            // side of an OR) cannot be a semi-join - that would drop rows the OR keeps.
            // Rewrite it as a mark join: LEFT-join `node` to the DISTINCT subquery on
            // the IN key, then replace `x IN (subquery)` with `marker IS NOT NULL`.
            if (!scalar->Operand || HasSubquery(scalar->Operand)) {
                return std::unexpected(TError("unsupported IN operand"));
            }
            // Only uncorrelated IN is supported here: outer references must not
            // survive in a standalone subquery plan.
            if (auto sel = NSql::TMaybeNode<NSql::TSqlSelect>(scalar->Query->Body);
                sel && sel.Cast()->From && sel.Cast()->Where)
            {
                TLocalSqlScope local;
                for (const auto& item : sel.Cast()->From->Items) {
                    CollectLocalColumns(item, sources, local);
                }
                if (HasUnmappedOuter(sel.Cast()->Where, local)) {
                    return std::unexpected(TError(
                        "correlated IN subquery in a boolean expression is not supported"));
                }
            }
            auto plan = BuildQuery(*scalar->Query, sources);
            if (!plan) {
                return std::unexpected(plan.error());
            }
            std::string markName = "__in_mark_" + std::to_string(counter++) + "__";
            plan = ApplyColumnAliases(std::move(*plan), {markName});
            if (!plan) {
                return std::unexpected(plan.error());
            }
            // Dedup so each outer row matches at most one right row (no duplication).
            auto distinct = ApplyDistinct(*plan);
            // Left join null-extends `markName` to NULL on non-match - the marker.
            std::vector<TJoinKey> keys;
            NAst::TExprPtr residual;
            if (auto ident = NAst::TMaybeNode<NAst::TIdentExpr>(scalar->Operand)) {
                keys.push_back(TJoinKey{ ident.Cast()->Name, markName });
            } else {
                residual = std::make_shared<NAst::TBinaryExpr>(
                    scalar->Operand->Location, NAst::TOperator("=="),
                    scalar->Operand, Ident(scalar->Operand->Location, markName));
            }
            node = std::make_shared<TJoinOperator>(
                std::move(node), std::move(distinct), std::move(keys),
                EJoinType::Left, std::move(residual));
            // Replace `x IN (subquery)` with `!(qdb_is_null(marker))`.
            return std::make_shared<NAst::TUnaryExpr>(
                expr->Location, NAst::TOperator("!"),
                Call(expr->Location, "qdb_is_null", { Ident(expr->Location, markName) }));
        }
        if (scalar->Kind != NSql::TSubqueryExpr::EKind::Scalar) {
            return std::unexpected(TError("subquery is not supported in this position"));
        }

        auto maybeSelect = NSql::TMaybeNode<NSql::TSqlSelect>(scalar->Query->Body);
        if (maybeSelect && !scalar->Query->WithClause
            && maybeSelect.Cast()->From && maybeSelect.Cast()->Where)
        {
            auto sel = maybeSelect.Cast();
            TLocalSqlScope local;
            for (const auto& item : sel->From->Items) {
                CollectLocalColumns(item, sources, local);
            }

            // Correlation = an equality with one local and one outer column, found
            // anywhere in the predicate (deduplicated by the (outer, local) pair).
            std::vector<std::pair<std::string, std::string>> correlation; // (outer, local)
            {
                std::vector<std::pair<std::string, std::string>> found;
                CollectCorrelations(sel->Where, local, found);
                std::set<std::pair<std::string, std::string>> seen;
                for (auto& pair : found) {
                    if (seen.insert(pair).second) {
                        correlation.push_back(pair);
                    }
                }
            }

            if (!correlation.empty()) {
                int id = counter++;
                if (!sel->GroupBy) {
                    sel->GroupBy = std::make_shared<NSql::TSqlGroupBy>();
                }
                std::vector<NSql::TSqlPtr<NSql::TSqlSelectItem>> prepend;
                std::vector<TJoinKey> joinKeys;
                std::unordered_map<std::string, std::string> outerToLocal;
                for (size_t k = 0; k < correlation.size(); ++k) {
                    const auto& [outerCol, localCol] = correlation[k];
                    std::string corrName =
                        "__corr_" + std::to_string(id) + "_" + std::to_string(k) + "__";
                    auto item = std::make_shared<NSql::TSqlSelectItem>();
                    item->Expr = Ident({}, localCol);
                    item->Alias = corrName;
                    item->Hidden = true;
                    prepend.push_back(std::move(item));
                    sel->GroupBy->Items.push_back(
                        std::make_shared<NSql::TSqlGroupingExprOrList>(Ident({}, localCol)));
                    joinKeys.push_back({ .Left = outerCol, .Right = corrName });
                    outerToLocal[outerCol] = localCol;
                }
                sel->SelectList->Items.insert(
                    sel->SelectList->Items.begin(), prepend.begin(), prepend.end());
                // Rewrite outer refs to their correlated local columns: a correlation
                // buried in a disjunction (`a = out AND P1) OR (a = out AND P2)`)
                // becomes fully local; the correlation itself is enforced by the
                // group-by + join keys above.
                RenameIdents(sel->Where, outerToLocal);
                if (HasUnmappedOuter(sel->Where, local)) {
                    return std::unexpected(
                        TError("unsupported correlated subquery (outer column outside an equality)"));
                }

                auto plan = BuildQuery(*scalar->Query, sources);
                if (!plan) {
                    return std::unexpected(plan.error());
                }
                std::string scalarName = "__scalar_" + std::to_string(id) + "__";
                std::vector<std::string> aliases;
                for (const auto& key : joinKeys) {
                    aliases.push_back(key.Right);
                }
                aliases.push_back(scalarName);
                plan = ApplyColumnAliases(std::move(*plan), aliases);
                if (!plan) {
                    return std::unexpected(plan.error());
                }
                // LEFT join preserves outer rows; a missing match yields NULL, so
                // the surrounding comparison filters the row out (SQL semantics).
                node = std::make_shared<TJoinOperator>(
                    std::move(node), *plan, std::move(joinKeys), EJoinType::Left, nullptr);
                return Ident(expr->Location, std::move(scalarName));
            }

            // Unknown bare references cannot safely be treated as uncorrelated.
            // Full outer-scope binding belongs to algebraic decorrelation.
            if (HasUnmappedOuter(sel->Where, local)) {
                return std::unexpected(TError(
                    "unsupported correlated scalar subquery "
                    "(correlation must be an equality between subquery and outer columns)"));
            }
        }

        // Uncorrelated: the subquery yields one row, broadcast via a cross join.
        auto plan = BuildQuery(*scalar->Query, sources);
        if (!plan) {
            return std::unexpected(plan.error());
        }
        std::string name = "__scalar_" + std::to_string(counter++) + "__";
        plan = ApplyColumnAliases(std::move(*plan), {name});
        if (!plan) {
            return std::unexpected(plan.error());
        }
        node = std::make_shared<TJoinOperator>(
            std::move(node), *plan, std::vector<TJoinKey>{}, EJoinType::Inner, nullptr);
        return Ident(expr->Location, std::move(name));
    }
    for (NAst::TExprPtr* child : expr->MutableChildren()) {
        auto rewritten = ExtractScalarSubqueries(*child, node, sources, counter);
        if (!rewritten) {
            return std::unexpected(rewritten.error());
        }
        *child = std::move(*rewritten);
    }
    return expr;
}

std::expected<TOperatorPtr, TError> BuildSelect(
    const NSql::TSqlSelect& select,
    const TTableSourceFactory& sources)
{
    if (!select.From) {
        return std::unexpected(TError("SELECT without FROM is not supported yet"));
    }
    // Window functions are only valid in the SELECT list and ORDER BY. Elsewhere
    // they are left as raw TWindowExpr nodes (applyWindows rewrites projections
    // only), so reject them with a clear message instead of crashing later in
    // type annotation. Use the local check so a window inside an independent
    // subquery in WHERE/HAVING (its own scope) is not mistaken for one here.
    if (HasWindowExprLocal(select.Where)) {
        return std::unexpected(TError("window functions are not allowed in WHERE"));
    }
    if (HasWindowExprLocal(select.Having)) {
        return std::unexpected(TError("window functions are not allowed in HAVING"));
    }
    int scalarCounter = 0;
    // Extracts scalar subqueries out of a finished projection list, broadcasting
    // each onto `n` (uncorrelated -> keyless cross join; correlated -> decorrelated
    // left join). Shares scalarCounter with the WHERE/HAVING extraction so the
    // __scalar_N__ names stay unique. Called just before the final project so the
    // broadcast joins attach above any aggregate (matching HAVING).
    auto extractProjectionSubqueries =
        [&](std::vector<TProjectionSpec>& projs, TOperatorPtr& n)
        -> std::expected<void, TError> {
        for (auto& p : projs) {
            auto rewritten = ExtractScalarSubqueries(p.Expression, n, sources, scalarCounter);
            if (!rewritten) {
                return std::unexpected(rewritten.error());
            }
            p.Expression = std::move(*rewritten);
        }
        return {};
    };
    // Builds the pre-aggregate input (FROM + joins + WHERE). Called once per
    // aggregation branch so a mixed DISTINCT/non-DISTINCT split gets independent
    // subtrees (there is no deep operator clone; rebuilding from the AST is the
    // simplest way to duplicate the input).
    TOperatorPtr fromInput;
    auto buildInput = [&]() -> std::expected<TOperatorPtr, TError> {
        auto base = BuildFrom(*select.From, sources);
        if (!base) {
            return std::unexpected(base.error());
        }
        TOperatorPtr node = *base;
        fromInput = node;
        if (select.Where) {
            std::vector<NAst::TExprPtr> conjuncts;
            FlattenConjuncts(select.Where, conjuncts);

            std::vector<NAst::TExprPtr> residual;
            for (const auto& conjunct : conjuncts) {
                if (auto decorrelation = AsDecorrelation(conjunct)) {
                    auto joined = decorrelation->IsIn
                        ? DecorrelateIn(node, *decorrelation->Subquery, decorrelation->Type, sources)
                        : DecorrelateExists(node, *decorrelation->Subquery, decorrelation->Type, sources);
                    if (!joined) {
                        return std::unexpected(joined.error());
                    }
                    node = std::move(*joined);
                    continue;
                }
                if (auto existsList = AsExistsOrDisjunction(conjunct); !existsList.empty()) {
                    auto joined = DecorrelateExistsOr(node, existsList, sources);
                    if (!joined) {
                        return std::unexpected(joined.error());
                    }
                    if (*joined) {
                        node = std::move(**joined);
                        continue;
                    }
                    // Unsupported EXISTS-OR shape: fall through to the residual path.
                }
                auto rewritten = ExtractScalarSubqueries(conjunct, node, sources, scalarCounter);
                if (!rewritten) {
                    return std::unexpected(rewritten.error());
                }
                residual.push_back(std::move(*rewritten));
            }

            if (!residual.empty()) {
                node = std::make_shared<TFilterOperator>(std::move(node), Conjoin(residual));
            }
        }
        return node;
    };

    auto base = buildInput();
    if (!base) {
        return std::unexpected(base.error());
    }
    auto node = std::move(*base);
    if (!select.SelectList) {
        return std::unexpected(TError("empty select list"));
    }
    std::optional<TGroupingParse> grouping;
    if (select.GroupBy) {
        auto parsed = ParseGrouping(*select.GroupBy);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        grouping = std::move(*parsed);
    }
    auto scope = std::make_shared<TSelectBinding>();
    scope->Input = fromInput;
    std::vector<TProjectionSpec> projections;
    for (size_t i = 0; i < select.SelectList->Items.size(); ++i) {
        const auto& item = select.SelectList->Items[i];
        if (item->Star) {
            std::string prefix;
            for (const auto& part : item->StarPrefix) {
                prefix += part + ".";
            }
            projections.push_back({.Name = "*", .Expression = Ident({}, "*"), .StarPrefix = prefix});
            scope->Items.push_back({.Name = "*", .Expression = Ident({}, "*"), .StarPrefix = prefix});
            continue;
        }
        const auto location = item->Expr->Location;
        if (item->Alias) {
            if (auto valid = ValidateOutputColumnName(*item->Alias, location); !valid) {
                return std::unexpected(valid.error());
            }
        }
        for (const auto& alias : item->ColumnAliases) {
            if (auto valid = ValidateOutputColumnName(alias, location); !valid) {
                return std::unexpected(valid.error());
            }
        }
        const auto name = ItemName(*item, i);
        const bool implicit = !item->Alias && item->ColumnAliases.empty()
            && !NAst::TMaybeNode<NAst::TIdentExpr>(item->Expr);
        // Window and subquery items are SQL-only nodes: binding never matches
        // them structurally.
        const bool opaque = HasWindowExpr(item->Expr) || HasSubquery(item->Expr);
        scope->Items.push_back({.Name = name, .ColumnAliases = item->ColumnAliases,
            .ImplicitName = implicit, .Opaque = opaque, .Hidden = item->Hidden});
        projections.push_back({.Name = name, .Expression = item->Expr,
            .ColumnAliases = item->ColumnAliases, .ImplicitName = implicit});
    }
    const bool aggregated = grouping.has_value() || HasAggregateCall(select.Having)
        || std::ranges::any_of(projections, [](const auto& spec) {
            return HasAggregateCall(spec.Expression);
        });
    if (!aggregated && select.Having) {
        return std::unexpected(TError("HAVING requires aggregation"));
    }
    if (aggregated) {
        TAggregateBinding binding{.Select = scope};
        if (grouping) {
            binding.Keys = std::move(grouping->Keys);
            binding.Sets = std::move(grouping->Sets);
            binding.HasGroupingSyntax = grouping->HasGroupingSyntax;
        }
        std::vector<std::string> keys;
        for (const auto& key : binding.Keys) {
            keys.push_back(key.Name);
        }
        auto aggregate = std::make_shared<TAggregateOperator>(node, std::move(keys), std::vector<TAggregateSpec>{});
        aggregate->MutableBinding() = std::move(binding);
        node = std::move(aggregate);
    }
    if (select.Having) {
        auto rewritten = ExtractScalarSubqueries(select.Having, node, sources, scalarCounter);
        if (!rewritten) {
            return std::unexpected(rewritten.error());
        }
        scope->Having = *rewritten;
        node = std::make_shared<TFilterOperator>(std::move(node), std::move(*rewritten));
    }
    if (auto ok = extractProjectionSubqueries(projections, node); !ok) {
        return std::unexpected(ok.error());
    }
    // Scope items see the final projection expressions; binding clones them
    // before it rewrites anything.
    for (size_t i = 0; i < projections.size(); ++i) {
        scope->Items[i].Expression = projections[i].Expression;
    }
    auto project = std::make_shared<TProjectOperator>(std::move(node), std::move(projections));
    project->SetSelectBinding(std::move(scope));
    if (select.Quantifier == NSql::ESetQuantifier::Distinct) {
        return ApplyDistinct(project);
    }
    return project;
}

std::expected<TOperatorPtr, TError> ApplyOrderBy(
    const NSql::TSqlQuery& query, TOperatorPtr plan)
{
    if (!query.OrderBy || query.OrderBy->Items.empty()) {
        return plan;
    }
    auto sort = std::make_shared<TSortOperator>(std::move(plan), std::vector<TSortKey>{});
    for (const auto& item : query.OrderBy->Items) {
        sort->MutableUnresolvedKeys().push_back({.Expression = item->Expr,
            .Direction = item->Desc ? ESortDirection::Desc : ESortDirection::Asc,
            .Nulls = ConvertSortNulls(item->NullOrder)});
    }
    return sort;
}

std::expected<int64_t, TError> ConstNonNegativeI64(
    const NAst::TExprPtr& expr,
    std::string_view clauseName)
{
    if (!expr) {
        return int64_t{0};
    }
    auto number = NAst::TMaybeNode<NAst::TNumberExpr>(expr);
    if (!number || number.Cast()->IsFloat()) {
        return std::unexpected(TError(expr->Location,
            std::string(clauseName) + " currently requires an integer literal"));
    }
    if (number.Cast()->IntValue < 0) {
        return std::unexpected(TError(expr->Location,
            std::string(clauseName) + " must be non-negative"));
    }
    return number.Cast()->IntValue;
}

std::expected<TOperatorPtr, TError> ApplyLimit(
    const NSql::TSqlQuery& query,
    TOperatorPtr plan)
{
    if (!query.Limit && !query.Offset) {
        return plan;
    }
    if (!query.Limit) {
        return std::unexpected(TError(query.Offset->Location,
            "OFFSET without LIMIT is not supported yet"));
    }

    auto limit = ConstNonNegativeI64(query.Limit, "LIMIT");
    if (!limit) {
        return std::unexpected(limit.error());
    }
    int64_t offsetValue = 0;
    if (query.Offset) {
        auto offset = ConstNonNegativeI64(query.Offset, "OFFSET");
        if (!offset) {
            return std::unexpected(offset.error());
        }
        offsetValue = *offset;
    }
    return std::make_shared<TLimitOperator>(std::move(plan), *limit, offsetValue);
}

std::expected<TOperatorPtr, TError> BuildSetOp(
    const NSql::TSqlSetOp& setOp,
    const TTableSourceFactory& sources);

// Builds a query body (a SELECT, a set operation, or a parenthesized query).
std::expected<TOperatorPtr, TError> BuildQueryBody(
    const NSql::TSqlNodePtr& body,
    const TTableSourceFactory& sources)
{
    if (auto select = NSql::TMaybeNode<NSql::TSqlSelect>(body)) {
        return BuildSelect(*select.Cast(), sources);
    }
    if (auto setOp = NSql::TMaybeNode<NSql::TSqlSetOp>(body)) {
        return BuildSetOp(*setOp.Cast(), sources);
    }
    if (auto sub = NSql::TMaybeNode<NSql::TSqlQuery>(body)) {
        return BuildQuery(*sub.Cast(), sources);
    }
    return std::unexpected(TError("unsupported query body"));
}

std::expected<TOperatorPtr, TError> BuildSetOp(
    const NSql::TSqlSetOp& setOp,
    const TTableSourceFactory& sources)
{
    if (setOp.Op == NSql::TSqlSetOp::EOp::Union) {
        // Flatten only a nested chain of the same UNION quantifier. Mixed chains such
        // as `(a UNION b) UNION ALL c` must keep the inner DISTINCT boundary.
        std::vector<TOperatorPtr> branches;
        std::function<std::expected<void, TError>(const NSql::TSqlNodePtr&)> collect =
            [&](const NSql::TSqlNodePtr& node) -> std::expected<void, TError> {
            auto inner = NSql::TMaybeNode<NSql::TSqlSetOp>(node);
            if (inner
                && inner.Cast()->Op == NSql::TSqlSetOp::EOp::Union
                && inner.Cast()->Quantifier == setOp.Quantifier)
            {
                if (auto l = collect(inner.Cast()->Left); !l) return l;
                return collect(inner.Cast()->Right);
            }
            auto branch = BuildQueryBody(node, sources);
            if (!branch) {
                return std::unexpected(branch.error());
            }
            branches.push_back(std::move(*branch));
            return {};
        };
        if (auto l = collect(setOp.Left); !l) return std::unexpected(l.error());
        if (auto r = collect(setOp.Right); !r) return std::unexpected(r.error());

        auto unionAll = std::make_shared<TUnionAllOperator>(std::move(branches));
        if (setOp.Quantifier == NSql::ESetQuantifier::All) {
            return unionAll;
        }

        return ApplyDistinct(std::move(unionAll));
    }

    if (setOp.Op == NSql::TSqlSetOp::EOp::Intersect
        || setOp.Op == NSql::TSqlSetOp::EOp::Except)
    {
        const bool isIntersect = setOp.Op == NSql::TSqlSetOp::EOp::Intersect;
        const std::string opName = isIntersect ? "INTERSECT" : "EXCEPT";
        if (setOp.Quantifier == NSql::ESetQuantifier::All) {
            return std::unexpected(TError(opName + " ALL is not supported yet"));
        }

        auto left = BuildQueryBody(setOp.Left, sources);
        if (!left) {
            return std::unexpected(left.error());
        }
        auto right = BuildQueryBody(setOp.Right, sources);
        if (!right) {
            return std::unexpected(right.error());
        }

        auto leftDistinct = ApplyDistinct(std::move(*left));
        auto rightDistinct = ApplyDistinct(std::move(*right));
        auto join = std::make_shared<TJoinOperator>(
            std::move(leftDistinct), std::move(rightDistinct), std::vector<TJoinKey>{},
            isIntersect ? EJoinType::LeftSemi : EJoinType::LeftAnti, nullptr);
        join->MutableBinding() = TSetOperationBinding{};
        return join;
    }

    return std::unexpected(TError("unsupported set operation"));
}

std::expected<TOperatorPtr, TError> BuildQuery(
    const NSql::TSqlQuery& query,
    const TTableSourceFactory& sources)
{
    if (!query.WithClause) {
        auto plan = BuildQueryBody(query.Body, sources);
        if (!plan) {
            return plan;
        }
        auto ordered = ApplyOrderBy(query, std::move(*plan));
        if (!ordered) {
            return ordered;
        }
        return ApplyLimit(query, std::move(*ordered));
    }
    if (query.WithClause->Recursive) {
        return std::unexpected(TError("WITH RECURSIVE is not supported yet"));
    }

    std::map<std::string, NSql::TSqlPtr<NSql::TSqlCte>> ctes;
    for (const auto& cte : query.WithClause->Ctes) {
        ctes[ToLower(cte->Name)] = cte;
    }

    // CTE-aware factory: a CTE name is built once as a canonical definition
    // (shared by every reference via TCteRef); other names fall back to the base
    // sources. Nested CTEs memoize into a DAG. Stack-local - definitions survive
    // via the shared_ptr the TCteRef holds.
    std::map<std::string, TCteDefinitionPtr> builtDefs;
    TTableSourceFactory factory =
        [&](std::string_view name) -> std::expected<TOperatorPtr, TError> {
        const std::string lower = ToLower(std::string(name));
        auto it = ctes.find(lower);
        if (it == ctes.end()) {
            return sources(name);
        }
        auto cached = builtDefs.find(lower);
        if (cached == builtDefs.end()) {
            auto plan = BuildQuery(*it->second->Query, factory);
            if (!plan) {
                return std::unexpected(plan.error());
            }
            TOperatorPtr def = std::move(*plan);
            if (it->second->Columns) {
                auto aliased = ApplyColumnAliases(std::move(def), it->second->Columns->Items);
                if (!aliased) {
                    return std::unexpected(aliased.error());
                }
                def = std::move(*aliased);
            }
            auto definition = std::make_shared<TCteDefinition>();
            definition->OutputType = def->OutputColumns();
            definition->Plan = std::move(def);
            cached = builtDefs.emplace(lower, std::move(definition)).first;
        }
        return std::make_shared<TCteRef>(cached->second);
    };

    auto plan = BuildQueryBody(query.Body, factory);
    if (!plan) {
        return plan;
    }
    auto ordered = ApplyOrderBy(query, std::move(*plan));
    if (!ordered) {
        return ordered;
    }
    return ApplyLimit(query, std::move(*ordered));
}

} // namespace

std::expected<TOperatorPtr, TError> BuildPlan(
    const NQdb::NSql::TSqlNodePtr& query,
    const TTableSourceFactory& sources)
{
    auto root = NSql::TMaybeNode<NSql::TSqlQuery>(query);
    if (!root) {
        return std::unexpected(TError("expected a query"));
    }
    auto plan = BuildQuery(*root.Cast(), sources);
    if (plan) {
        AssignCteIds(*plan);
    }
    return plan;
}

} // namespace NQdb
