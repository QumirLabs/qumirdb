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

std::expected<TGroupingParse, TError> ParseGrouping(
    const NSql::TSqlGroupBy& groupBy,
    TFreshNames& names)
{
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
            gk = { .Name = names.Next("gb"), .Expression = expr, .Computed = true };
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

// A built relation: a plan and the SQL label of each output column. The builder
// names columns by their SQL names, so the labels are the plan's column names.
struct TRelation {
    TOperatorPtr Plan;
    std::vector<std::string> Labels;
};

struct TBuildContext {
    TTableSourceFactory Sources;
    const NKernel::TAnnotationContext& Annotation;
};

std::vector<std::string> ColumnNames(const TOperatorPtr& plan) {
    std::vector<std::string> names;
    if (auto&& schema = plan->OutputColumns()) {
        for (const auto& [name, _] : schema->Fields) {
            names.push_back(name);
        }
    }
    return names;
}

TRelation Relation(TOperatorPtr plan) {
    auto labels = ColumnNames(plan);
    return {.Plan = std::move(plan), .Labels = std::move(labels)};
}

// Labels follow the join's output: both inputs, or the preserved side of a
// semi/anti join.
TRelation JoinRelations(
    TRelation left,
    TRelation right,
    std::vector<TJoinKey> keys,
    EJoinType type,
    NAst::TExprPtr residual)
{
    TRelation joined{.Plan = std::make_shared<TJoinOperator>(
        left.Plan, right.Plan, std::move(keys), type, std::move(residual))};
    switch (type) {
        case EJoinType::LeftSemi:
        case EJoinType::LeftAnti:
            joined.Labels = std::move(left.Labels);
            break;
        case EJoinType::RightSemi:
        case EJoinType::RightAnti:
            joined.Labels = std::move(right.Labels);
            break;
        default:
            joined.Labels = std::move(left.Labels);
            joined.Labels.insert(joined.Labels.end(), right.Labels.begin(), right.Labels.end());
            break;
    }
    return joined;
}

// Renames a built sub-plan's output columns to the given list (CTE column lists
// and derived-table column aliases). The sub-plan's top must be a projection.
std::expected<TRelation, TError> ApplyColumnAliases(
    TRelation relation, const std::vector<std::string>& aliases)
{
    auto project = TMaybeOp<TProjectOperator>(relation.Plan);
    if (!project || project.Cast()->Projections().size() != aliases.size()) {
        return std::unexpected(TError("column alias count does not match output columns"));
    }
    // Rebuild through the ctor so the operator's output Type is recomputed from
    // the renamed columns (mutating Projections in place would leave it stale).
    std::vector<TProjectionSpec> renamed = project.Cast()->Projections();
    for (size_t i = 0; i < renamed.size(); ++i) {
        if (auto valid = ValidateOutputColumnName(aliases[i]); !valid) {
            return std::unexpected(valid.error());
        }
        renamed[i].Name = aliases[i];
        renamed[i].ImplicitName = false;
    }
    return Relation(std::make_shared<TProjectOperator>(project.Cast()->Input(), std::move(renamed)));
}

// Qualifies a subplan's output columns as `alias.col`. Source tables get this from
// AssignSourceAliases, but subqueries/CTEs used with an alias need it here so the
// outer query can reference `alias.col` (and equi-join keys on them are extracted).
TRelation AliasSubplan(TRelation relation, const std::string& alias) {
    auto&& schema = relation.Plan->OutputColumns();
    if (!schema) {
        return relation;
    }
    std::vector<TProjectionSpec> projections;
    projections.reserve(schema->Fields.size());
    for (size_t i = 0; i < schema->Fields.size(); ++i) {
        const auto& label = relation.Labels[i];
        auto dot = label.rfind('.');
        std::string bare = dot != std::string::npos ? label.substr(dot + 1) : label;
        projections.push_back({
            .Name = alias + "." + bare, .Expression = Ident({}, schema->Fields[i].first) });
    }
    return Relation(std::make_shared<TProjectOperator>(std::move(relation.Plan), std::move(projections)));
}

// SELECT DISTINCT: dedup by grouping on every output column. A pass-through project
// on top keeps a project-typed root (scalar-subquery/CTE handling relies on it) and
// preserves column order.
TRelation ApplyDistinct(TRelation projected, const std::vector<std::string>& columns) {
    auto agg = std::make_shared<TAggregateOperator>(
        std::move(projected.Plan), columns, std::vector<TAggregateSpec>{});
    std::vector<TProjectionSpec> passthrough;
    passthrough.reserve(columns.size());
    for (const auto& col : columns) {
        passthrough.push_back({ .Name = col, .Expression = Ident({}, col) });
    }
    return Relation(std::make_shared<TProjectOperator>(std::move(agg), std::move(passthrough)));
}

std::expected<std::vector<std::string>, TError> OutputColumnNames(
    const TOperatorPtr& op,
    const std::string& context)
{
    auto&& output = op->OutputColumns();
    if (!output) {
        return std::unexpected(TError(context + " output schema must be a struct"));
    }
    std::vector<std::string> names;
    names.reserve(output->Fields.size());
    for (const auto& [name, _] : output->Fields) {
        names.push_back(name);
    }
    return names;
}

std::expected<TRelation, TError> BuildQuery(
    const NSql::TSqlQuery& query,
    TBuildContext& context);

std::expected<TRelation, TError> BuildTableRef(
    const NSql::TSqlPtr<NSql::TSqlTableRef>& ref,
    TBuildContext& context)
{
    if (auto table = NSql::TMaybeNode<NSql::TSqlTableName>(ref)) {
        auto node = table.Cast();
        auto source = context.Sources(TableName(node->Name));
        if (!source) {
            return std::unexpected(source.error());
        }
        if (auto sourceOp = TMaybeOp<TSourceOperator>(*source)) {
            if (node->Alias) {
                sourceOp.Cast()->SetAlias(*node->Alias);
            }
            return Relation(*source);
        }
        // A CTE inlined as a subplan: qualify its columns by the reference alias, or -
        // when referenced bare - by the CTE name, so `<cte>.col` references resolve
        // (otherwise the inlined columns stay bare and equi-joins over them drop their
        // keys, producing keyless cross joins).
        return AliasSubplan(
            Relation(std::move(*source)), node->Alias ? *node->Alias : node->Name.back());
    }

    if (auto sub = NSql::TMaybeNode<NSql::TSqlSubqueryTable>(ref)) {
        auto node = sub.Cast();
        auto plan = BuildQuery(*node->Query, context);
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
        auto left = BuildTableRef(join->Left, context);
        if (!left) {
            return std::unexpected(left.error());
        }
        auto right = BuildTableRef(join->Right, context);
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
        return JoinRelations(std::move(*left), std::move(*right), std::move(keys),
            MapJoinType(join->Type), std::move(residual));
    }

    return std::unexpected(TError("unsupported table reference"));
}

std::expected<TRelation, TError> BuildFrom(
    const NSql::TSqlFrom& from,
    TBuildContext& context)
{
    if (from.Items.empty()) {
        return std::unexpected(TError("empty FROM"));
    }
    auto node = BuildTableRef(from.Items[0], context);
    if (!node) {
        return std::unexpected(node.error());
    }
    TRelation result = std::move(*node);

    // Comma-separated tables become a left-deep cross-join tree; the WHERE
    // predicate stays a single top-level filter (equi-join extraction is an
    // optimizer pass).
    for (size_t i = 1; i < from.Items.size(); ++i) {
        auto right = BuildTableRef(from.Items[i], context);
        if (!right) {
            return std::unexpected(right.error());
        }
        result = JoinRelations(std::move(result), std::move(*right), {}, EJoinType::Inner, nullptr);
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

// EXISTS: the subquery's FROM becomes the right side and its WHERE (correlation
// plus local predicates) becomes the join residual.
std::expected<TRelation, TError> DecorrelateExists(
    TRelation left,
    const NSql::TSubqueryExpr& subquery,
    EJoinType type,
    TBuildContext& context)
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
    auto right = BuildFrom(*select->From, context);
    if (!right) {
        return std::unexpected(right.error());
    }
    if (select->Where && HasSubquery(select->Where)) {
        return std::unexpected(TError("nested subqueries are not supported yet"));
    }
    return JoinRelations(std::move(left), std::move(*right), {}, type, select->Where);
}

// IN: build the subquery as a full plan and join on `operand == <its only column>`.
std::expected<TRelation, TError> DecorrelateIn(
    TRelation left,
    const NSql::TSubqueryExpr& subquery,
    EJoinType type,
    TBuildContext& context)
{
    if (!subquery.Operand || HasSubquery(subquery.Operand)) {
        return std::unexpected(TError("unsupported IN operand"));
    }
    auto right = BuildQuery(*subquery.Query, context);
    if (!right) {
        return std::unexpected(right.error());
    }
    auto project = TMaybeOp<TProjectOperator>(right->Plan);
    if (!project || project.Cast()->Projections().size() != 1) {
        return std::unexpected(TError("IN subquery must return exactly one column"));
    }
    // Rename the subquery's output to a synthetic name so the residual's two sides
    // never share a name: otherwise `x IN (SELECT x ...)` yields `x == x`, which
    // qualification cannot bind to the left side and key extraction drops entirely,
    // leaving a keyless (unschedulable, condition-less) semi-join. The name is
    // internal - a semi/anti join exposes only its left side.
    auto& spec = project.Cast()->MutableProjections()[0];
    spec.Name = "__qdb_in__" + spec.Name;
    spec.ImplicitName = false;
    auto column = Ident(subquery.Operand->Location, spec.Name);
    auto residual = std::make_shared<NAst::TBinaryExpr>(
        subquery.Operand->Location, NAst::TOperator("=="), subquery.Operand, std::move(column));
    return JoinRelations(std::move(left), std::move(*right), {}, type, std::move(residual));
}

// Columns produced by a subquery's own FROM tables (base tables, recursing into
// joins). Used to tell a subquery's local columns from outer (correlated) ones.
void CollectLocalColumns(
    const NSql::TSqlPtr<NSql::TSqlTableRef>& ref,
    const TTableSourceFactory& sources,
    std::unordered_set<std::string>& out)
{
    if (auto table = NSql::TMaybeNode<NSql::TSqlTableName>(ref)) {
        auto node = table.Cast();
        if (auto src = sources(TableName(node->Name))) {
            // Qualify by the table's alias (or its name), so a local column of
            // `item j` (`j.i_category`) is distinguishable from an outer column
            // of another alias of the same table (`i.i_category`); a bare name
            // is also kept for unqualified references.
            const std::string prefix =
                node->Alias ? *node->Alias : node->Name.back();
            if (auto source = TMaybeOp<TSourceOperator>(*src)) {
                for (const auto& col : source.Cast()->GetSource().Schema().Columns) {
                    out.insert(std::string(col.Name));
                    out.insert(prefix + "." + std::string(col.Name));
                }
            } else if (auto&& schema = (*src)->OutputColumns()) {
                // CTE / subquery table inlined as a subplan: its columns are the
                // subplan's output (projection) names; qualify them by the reference
                // alias exactly as AliasSubplan does, so a correlated `ctr2.ctr_state`
                // matches the outer `ctr1.ctr_state`.
                for (const auto& [name, _] : schema->Fields) {
                    auto dot = name.rfind('.');
                    std::string bare = dot != std::string::npos ? name.substr(dot + 1) : name;
                    out.insert(bare);
                    out.insert(prefix + "." + bare);
                }
            }
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
    const std::unordered_set<std::string>& local,
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
            bool lLocal = local.count(ln) > 0;
            bool rLocal = local.count(rn) > 0;
            if (lLocal && !rLocal) {
                out.emplace_back(rn, ln);
                return;
            }
            if (rLocal && !lLocal) {
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
std::expected<std::optional<TRelation>, TError> DecorrelateExistsOr(
    const TRelation& left,
    const std::vector<std::shared_ptr<NSql::TSubqueryExpr>>& existsList,
    TBuildContext& context)
{
    std::optional<std::string> outerKey;
    std::vector<TOperatorPtr> branches;
    for (const auto& sub : existsList) {
        auto maybeSelect = NSql::TMaybeNode<NSql::TSqlSelect>(sub->Query->Body);
        if (!maybeSelect) {
            return std::optional<TRelation>{};
        }
        auto select = maybeSelect.Cast();
        if (sub->Query->WithClause || select->GroupBy || select->Having || !select->From
            || !select->Where || HasSubquery(select->Where))
        {
            return std::optional<TRelation>{};
        }
        std::unordered_set<std::string> local;
        for (const auto& item : select->From->Items) {
            CollectLocalColumns(item, context.Sources, local);
        }
        std::vector<std::pair<std::string, std::string>> corrs;
        CollectCorrelations(select->Where, local, corrs);
        std::set<std::pair<std::string, std::string>> uniq(corrs.begin(), corrs.end());
        if (uniq.size() != 1) {
            return std::optional<TRelation>{}; // need exactly one correlation
        }
        const auto& [outer, inner] = *uniq.begin();
        if (!outerKey) {
            outerKey = outer;
        } else if (*outerKey != outer) {
            return std::optional<TRelation>{}; // branches must share the outer key
        }
        auto body = BuildFrom(*select->From, context);
        if (!body) {
            return std::unexpected(body.error());
        }
        auto where = CloneExpr(select->Where);
        RenameIdents(where, {{outer, inner}}); // correlation -> inner=inner (tautology)
        TOperatorPtr filtered = std::make_shared<TFilterOperator>(std::move(body->Plan), std::move(where));
        std::vector<TProjectionSpec> projs{
            { .Name = "__exists_key__", .Expression = Ident(sub->Location, inner) }};
        branches.push_back(std::make_shared<TProjectOperator>(
            std::move(filtered), std::move(projs)));
    }
    auto un = Relation(std::make_shared<TUnionAllOperator>(std::move(branches)));
    // Keyed left-semi (not a residual) so cost-based join reordering, which runs before
    // equi-key extraction, sees this as an equi-join and does not cross-join around it.
    std::vector<TJoinKey> keys{ TJoinKey{ *outerKey, "__exists_key__" } };
    return std::optional<TRelation>{
        JoinRelations(left, std::move(un), std::move(keys), EJoinType::LeftSemi, nullptr)};
}

// True if any identifier leaf is not a local column (a leftover outer reference not
// covered by a correlation equality - that shape we cannot decorrelate).
bool HasUnmappedOuter(
    const NAst::TExprPtr& expr,
    const std::unordered_set<std::string>& local)
{
    for (const auto& name : FindUnboundVars(expr)) {
        if (!local.count(name)) {
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
    TRelation& node,
    TBuildContext& context,
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
            // Only uncorrelated IN is supported here: a correlated body would leave
            // outer columns unbound in the standalone subquery plan. (`!local.empty()`
            // avoids misfiring on empty-schema mock builds - see the scalar guard.)
            if (auto sel = NSql::TMaybeNode<NSql::TSqlSelect>(scalar->Query->Body);
                sel && sel.Cast()->From && sel.Cast()->Where)
            {
                std::unordered_set<std::string> local;
                for (const auto& item : sel.Cast()->From->Items) {
                    CollectLocalColumns(item, context.Sources, local);
                }
                if (!local.empty() && HasUnmappedOuter(sel.Cast()->Where, local)) {
                    return std::unexpected(TError(
                        "correlated IN subquery in a boolean expression is not supported"));
                }
            }
            auto plan = BuildQuery(*scalar->Query, context);
            if (!plan) {
                return std::unexpected(plan.error());
            }
            auto project = TMaybeOp<TProjectOperator>(plan->Plan);
            if (!project || project.Cast()->Projections().size() != 1) {
                return std::unexpected(TError("IN subquery must return exactly one column"));
            }
            std::string markName = "__in_mark_" + std::to_string(counter++) + "__";
            auto& projection = project.Cast()->MutableProjections()[0];
            projection.Name = markName;
            projection.ImplicitName = false;
            // Dedup so each outer row matches at most one right row (no duplication).
            auto distinct = ApplyDistinct(std::move(*plan), { markName });
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
            node = JoinRelations(std::move(node), std::move(distinct), std::move(keys),
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
            std::unordered_set<std::string> local;
            for (const auto& item : sel->From->Items) {
                CollectLocalColumns(item, context.Sources, local);
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

                auto plan = BuildQuery(*scalar->Query, context);
                if (!plan) {
                    return std::unexpected(plan.error());
                }
                auto project = TMaybeOp<TProjectOperator>(plan->Plan);
                if (!project
                    || project.Cast()->Projections().size() != correlation.size() + 1)
                {
                    return std::unexpected(
                        TError("correlated scalar subquery must return one value"));
                }
                std::string scalarName = "__scalar_" + std::to_string(id) + "__";
                auto& projection = project.Cast()->MutableProjections().back();
                projection.Name = scalarName;
                projection.ImplicitName = false;
                // LEFT join preserves outer rows; a missing match yields NULL, so
                // the surrounding comparison filters the row out (SQL semantics).
                node = JoinRelations(
                    std::move(node), std::move(*plan), std::move(joinKeys), EJoinType::Left, nullptr);
                return Ident(expr->Location, std::move(scalarName));
            }

            // Correlation present but not liftable to an equi-key (a non-equality
            // correlation, or an equality that isn't ident==ident): the outer column
            // would be left unbound in the subquery. Fail loudly rather than silently
            // cross-joining and dropping rows. Only when `local` is known (non-empty):
            // if the subquery's own columns couldn't be resolved (e.g. an unschema'd
            // source), every ident looks "outer" and we must not misfire - fall back.
            if (!local.empty() && HasUnmappedOuter(sel->Where, local)) {
                return std::unexpected(TError(
                    "unsupported correlated scalar subquery "
                    "(correlation must be an equality between subquery and outer columns)"));
            }
        }

        // Uncorrelated: the subquery yields one row, broadcast via a cross join.
        auto plan = BuildQuery(*scalar->Query, context);
        if (!plan) {
            return std::unexpected(plan.error());
        }
        auto project = TMaybeOp<TProjectOperator>(plan->Plan);
        if (!project || project.Cast()->Projections().size() != 1) {
            return std::unexpected(TError("scalar subquery must return exactly one column"));
        }
        std::string name = "__scalar_" + std::to_string(counter++) + "__";
        auto& projection = project.Cast()->MutableProjections()[0];
        projection.Name = name;
        projection.ImplicitName = false;
        node = JoinRelations(std::move(node), std::move(*plan), {}, EJoinType::Inner, nullptr);
        return Ident(expr->Location, std::move(name));
    }
    for (NAst::TExprPtr* child : expr->MutableChildren()) {
        auto rewritten = ExtractScalarSubqueries(*child, node, context, counter);
        if (!rewritten) {
            return std::unexpected(rewritten.error());
        }
        *child = std::move(*rewritten);
    }
    return expr;
}

std::expected<TRelation, TError> BuildSelect(
    const NSql::TSqlSelect& select,
    TBuildContext& context)
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
        [&](std::vector<TProjectionSpec>& projs, TRelation& n)
        -> std::expected<void, TError> {
        for (auto& p : projs) {
            auto rewritten = ExtractScalarSubqueries(p.Expression, n, context, scalarCounter);
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
    auto buildInput = [&]() -> std::expected<TRelation, TError> {
        auto base = BuildFrom(*select.From, context);
        if (!base) {
            return std::unexpected(base.error());
        }
        TRelation node = std::move(*base);
        if (select.Where) {
            std::vector<NAst::TExprPtr> conjuncts;
            FlattenConjuncts(select.Where, conjuncts);

            std::vector<NAst::TExprPtr> residual;
            for (const auto& conjunct : conjuncts) {
                if (auto decorrelation = AsDecorrelation(conjunct)) {
                    auto joined = decorrelation->IsIn
                        ? DecorrelateIn(node, *decorrelation->Subquery, decorrelation->Type, context)
                        : DecorrelateExists(node, *decorrelation->Subquery, decorrelation->Type, context);
                    if (!joined) {
                        return std::unexpected(joined.error());
                    }
                    node = std::move(*joined);
                    continue;
                }
                if (auto existsList = AsExistsOrDisjunction(conjunct); !existsList.empty()) {
                    auto joined = DecorrelateExistsOr(node, existsList, context);
                    if (!joined) {
                        return std::unexpected(joined.error());
                    }
                    if (*joined) {
                        node = std::move(**joined);
                        continue;
                    }
                    // Unsupported EXISTS-OR shape: fall through to the residual path.
                }
                auto rewritten = ExtractScalarSubqueries(conjunct, node, context, scalarCounter);
                if (!rewritten) {
                    return std::unexpected(rewritten.error());
                }
                residual.push_back(std::move(*rewritten));
            }

            if (!residual.empty()) {
                node.Plan = std::make_shared<TFilterOperator>(std::move(node.Plan), Conjoin(residual));
            }
        }
        return node;
    };

    auto base = buildInput();
    if (!base) {
        return std::unexpected(base.error());
    }
    TRelation node = std::move(*base);

    if (!select.SelectList) {
        return std::unexpected(TError("empty select list"));
    }

    // A GROUP BY alias names its key, which HAVING and GROUPING() refer to, so
    // generated names avoid every SELECT alias as well as the input columns.
    TFreshNames names;
    for (const auto& column : ColumnNames(node.Plan)) {
        names.Reserve(BareColumnName(column));
    }
    for (const auto& item : select.SelectList->Items) {
        if (item->Alias) {
            names.Reserve(*item->Alias);
        }
    }

    std::optional<TGroupingParse> parsedGrouping;
    if (select.GroupBy) {
        auto parsed = ParseGrouping(*select.GroupBy, names);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        parsedGrouping = std::move(*parsed);
    }
    const bool multiSet = parsedGrouping && parsedGrouping->Sets.size() > 1;

    auto inputSchemaType = node.Plan->OutputColumns();
    auto inputSchema = NAst::TMaybeType<NAst::TStructType>(inputSchemaType);
    // Grouping-sets execution masks key columns for sets that omit them. Keep
    // computed aggregate arguments materialized separately: collapsing `sum(k+1)`
    // to reducers over `k` would make the subtotal consume the masked key.
    TAggCollector collector(
        inputSchema ? inputSchema.Cast().get() : nullptr,
        !multiSet,
        names,
        context.Annotation);
    const bool distinct = select.Quantifier == NSql::ESetQuantifier::Distinct;
    std::vector<TProjectionSpec> projections;
    std::unordered_map<std::string, NAst::TExprPtr> selectAliases;
    std::unordered_set<std::string> ambiguousSelectAliases;
    for (size_t i = 0; i < select.SelectList->Items.size(); ++i) {
        const auto& item = select.SelectList->Items[i];
        if (item->Star) {
            auto&& schema = node.Plan->OutputColumns();
            if (!schema) {
                return std::unexpected(TError("'*' over an unresolved input"));
            }
            std::string prefix;
            for (const auto& part : item->StarPrefix) {
                prefix += part + ".";
            }
            for (size_t column = 0; column < schema->Fields.size(); ++column) {
                const auto& label = node.Labels[column];
                if (!prefix.empty() && label.rfind(prefix, 0) != 0) {
                    continue;
                }
                auto dot = label.rfind('.');
                std::string bare = dot != std::string::npos ? label.substr(dot + 1) : label;
                projections.push_back({
                    .Name = bare, .Expression = Ident({}, schema->Fields[column].first) });
            }
            continue;
        }
        const auto outputLocation = item->Expr
            ? item->Expr->Location
            : TLocation{};
        if (item->Alias) {
            if (auto valid = ValidateOutputColumnName(
                    *item->Alias, outputLocation); !valid)
            {
                return std::unexpected(valid.error());
            }
        }
        for (const auto& alias : item->ColumnAliases) {
            if (auto valid = ValidateOutputColumnName(
                    alias, outputLocation); !valid)
            {
                return std::unexpected(valid.error());
            }
        }
        std::string name = ItemName(*item, i);
        auto expr = collector.Rewrite(item->Expr);
        if (!expr) {
            return std::unexpected(expr.error());
        }
        if (item->Alias) {
            auto [_, inserted] = selectAliases.emplace(*item->Alias, *expr);
            if (!inserted) {
                ambiguousSelectAliases.insert(*item->Alias);
            }
        }
        projections.push_back({
            .Name = std::move(name),
            .Expression = std::move(*expr),
            .ColumnAliases = item->ColumnAliases,
            .ImplicitName = !item->Alias && item->ColumnAliases.empty()
                && !NAst::TMaybeNode<NAst::TIdentExpr>(item->Expr),
            .RejectUnaliasedStructForDistinct =
                distinct && item->ColumnAliases.empty(),
        });
    }

    std::vector<std::string> outputNames;
    for (const auto& p : projections) {
        if (p.ColumnAliases.empty()) {
            outputNames.push_back(p.Name);
        } else {
            outputNames.insert(
                outputNames.end(), p.ColumnAliases.begin(), p.ColumnAliases.end());
        }
    }
    // A scalar subquery in HAVING is left intact by the aggregate collector
    // (it is opaque) and decorrelated below, once the aggregate node exists.
    NAst::TExprPtr having;
    if (select.Having) {
        auto rewritten = collector.Rewrite(select.Having);
        if (!rewritten) {
            return std::unexpected(rewritten.error());
        }
        having = std::move(*rewritten);
    }

    // Window functions run after aggregation/HAVING and before the final project.
    // Shared by both the aggregated and non-aggregated paths.
    auto applyWindows = [&](TOperatorPtr n) -> std::expected<TOperatorPtr, TError> {
        TWindowCollector windows;
        for (auto& projection : projections) {
            auto rewritten = windows.Rewrite(projection.Expression);
            if (!rewritten) {
                return std::unexpected(rewritten.error());
            }
            projection.Expression = std::move(*rewritten);
        }
        if (windows.Empty()) {
            return n;
        }
        return windows.Build(std::move(n));
    };

    bool aggregated = select.GroupBy != nullptr || !collector.Empty();
    if (!aggregated) {
        for (const auto& projection : projections) {
            if (HasGroupingCall(projection.Expression)) {
                return std::unexpected(TError(projection.Expression->Location,
                    "GROUPING() requires GROUPING SETS, ROLLUP, or CUBE"));
            }
        }
        if (having) {
            return std::unexpected(TError("HAVING requires aggregation"));
        }
        auto windowed = applyWindows(std::move(node.Plan));
        if (!windowed) {
            return std::unexpected(windowed.error());
        }
        node = Relation(std::move(*windowed));
        if (auto ok = extractProjectionSubqueries(projections, node); !ok) {
            return std::unexpected(ok.error());
        }
        auto projected =
            Relation(std::make_shared<TProjectOperator>(std::move(node.Plan), std::move(projections)));
        return distinct ? ApplyDistinct(std::move(projected), outputNames) : projected;
    }

    std::vector<TGroupKey> groupKeys;
    std::vector<std::vector<size_t>> groupingSets;
    bool hasGroupingSyntax = false;
    if (parsedGrouping) {
        groupKeys = std::move(parsedGrouping->Keys);
        groupingSets = std::move(parsedGrouping->Sets);
        hasGroupingSyntax = parsedGrouping->HasGroupingSyntax;
    }
    auto inputHasColumn = [&](const std::string& name) {
        if (!inputSchema) {
            return false;
        }
        return std::ranges::any_of(inputSchema.Cast()->Fields, [&](const auto& field) {
            return field.first == name
                || (name.find('.') == std::string::npos && BareColumnName(field.first) == name);
        });
    };
    const bool inputSchemaKnown = inputSchema && !inputSchema.Cast()->Fields.empty();
    for (auto& key : groupKeys) {
        // PostgreSQL GROUP BY resolves an input-column name before a SELECT alias.
        if (key.Computed || inputHasColumn(key.Name)) {
            continue;
        }
        if (ambiguousSelectAliases.contains(key.Name)) {
            return std::unexpected(TError(
                key.Expression->Location,
                "GROUP BY alias '" + key.Name + "' is ambiguous"));
        }
        auto alias = selectAliases.find(key.Name);
        if (alias == selectAliases.end()) {
            continue;
        }
        if (HasSubquery(alias->second) || HasWindowExprLocal(alias->second)) {
            return std::unexpected(TError(
                key.Expression->Location,
                "GROUP BY alias '" + key.Name +
                "' must refer to a scalar input expression"));
        }
        const auto refs = FindUnboundVars(alias->second);
        if (!inputSchemaKnown && std::ranges::any_of(refs, [&](const auto& ref) {
                const auto dot = ref.rfind('.');
                return ref.substr(dot == std::string::npos ? 0 : dot + 1) == key.Name;
            }))
        {
            // Schema-less golden plans cannot prove the input-column match.
            continue;
        }
        for (const auto& ref : refs) {
            if (!inputHasColumn(ref)) {
                return std::unexpected(TError(
                    key.Expression->Location,
                    "GROUP BY alias '" + key.Name +
                    "' must refer to a scalar input expression"));
            }
        }
        if (auto ident = NAst::TMaybeNode<NAst::TIdentExpr>(alias->second)) {
            key.Name = ident.Cast()->Name;
            key.Expression = CloneExpr(alias->second);
            continue;
        }
        key.Expression = CloneExpr(alias->second);
        key.Computed = true;
    }
    NarrowPlainGroupKeys(groupKeys, groupingSets, hasGroupingSyntax);
    // A single set over all keys is a plain aggregate; >1 set needs the kernel's
    // grouping-sets machinery (keys become nullable, masked per set).
    std::unordered_map<std::string, std::string> computedKeys; // PrintAst -> name
    for (const auto& key : groupKeys) {
        if (key.Computed) {
            computedKeys.emplace(NAst::NCore::PrintAst(key.Expression), key.Name);
        }
    }
    if (!computedKeys.empty()) {
        for (auto& projection : projections) {
            projection.Expression = SubstituteGroupKeys(projection.Expression, computedKeys);
        }
        having = SubstituteGroupKeys(having, computedKeys);
    }

    auto groupingInfo = MakeGroupingRewriteInfo(groupKeys, groupingSets, hasGroupingSyntax);
    for (auto& projection : projections) {
        auto rewritten = RewriteGroupingCalls(projection.Expression, groupingInfo);
        if (!rewritten) {
            return std::unexpected(rewritten.error());
        }
        projection.Expression = std::move(*rewritten);
    }
    if (having) {
        auto rewritten = RewriteGroupingCalls(having, groupingInfo);
        if (!rewritten) {
            return std::unexpected(rewritten.error());
        }
        having = std::move(*rewritten);
    }

    std::vector<std::string> keys;
    keys.reserve(groupKeys.size());
    for (const auto& key : groupKeys) {
        keys.push_back(key.Name);
    }

    auto specs = collector.TakeSpecs();
    for (const auto& spec : specs) {
        if (HasGroupingCall(spec.Arg)) {
            return std::unexpected(TError(spec.Arg->Location,
                "GROUPING() inside aggregate arguments is not supported"));
        }
    }

    // count(distinct col): dedup with an inner aggregate on (group keys + col),
    // then count over the deduped rows above - the double aggregation the hand
    // plans use.
    if (multiSet && collector.DistinctColumn()) {
        return std::unexpected(
            TError("GROUPING SETS with COUNT(DISTINCT) is not supported yet"));
    }

    if (auto distinctColumn = collector.DistinctColumn()) {
        if (!computedKeys.empty()) {
            return std::unexpected(
                TError("GROUP BY on an expression with COUNT(DISTINCT) is not supported yet"));
        }
        // Inner grouping keys: the group keys plus the distinct column, so each distinct
        // value is one inner group (the dedup the hand plans use).
        std::vector<std::string> dedupKeys = keys;
        if (std::find(dedupKeys.begin(), dedupKeys.end(), *distinctColumn) == dedupKeys.end()) {
            dedupKeys.push_back(*distinctColumn);
        }

        if (collector.HasNonDistinct()) {
            // Mixed DISTINCT + non-DISTINCT (single distinct column): one input scan, a
            // double aggregation with partials. The inner (grouped by group keys + col)
            // computes the non-distinct aggregates per (group, distinct value); the outer
            // (grouped by group keys) merges them - sum->sum, count->sum, min/max->min/max -
            // alongside count(col) for the distinct. No join, no input clone.
            std::vector<TAggregateSpec> innerSpecs;
            std::vector<TAggregateSpec> outerSpecs;
            for (auto& spec : specs) {
                if (collector.IsDistinctSpec(spec.Name)) {
                    // col is an inner group key; count(col) in the outer counts distinct
                    // non-null values.
                    outerSpecs.push_back(std::move(spec));
                    continue;
                }
                if (spec.Arg && !NAst::TMaybeNode<NAst::TIdentExpr>(spec.Arg)) {
                    return std::unexpected(TError(
                        "mixing DISTINCT with a computed aggregate argument is not supported"));
                }
                const std::string innerName = names.Next("partial");
                const std::string mergeFunc = spec.Func == "count" ? "sum" : spec.Func;
                innerSpecs.push_back({ .Name = innerName, .Func = spec.Func, .Arg = std::move(spec.Arg) });
                outerSpecs.push_back({ .Name = spec.Name, .Func = mergeFunc, .Arg = Ident({}, innerName) });
            }
            node = Relation(std::make_shared<TAggregateOperator>(
                std::move(node.Plan), std::move(dedupKeys), std::move(innerSpecs)));
            // The normal construction below builds the OUTER aggregate over `node`
            // (the inner result) with these merge specs, grouped by the group keys.
            specs = std::move(outerSpecs);
        } else {
            // Lone count(distinct): dedup, then count over the deduped rows below.
            node = Relation(std::make_shared<TAggregateOperator>(
                std::move(node.Plan), std::move(dedupKeys), std::vector<TAggregateSpec>{}));
        }
    }

    // A global aggregate (no GROUP BY) still needs a grouping-key descriptor, so
    // synthesize a constant key column and group by it, as the hand-written plans do.
    {
    bool global = keys.empty();

    // The aggregate executor requires column-reference arguments, so materialize
    // computed arguments (and pass the group keys through) in a project below.
    // Grouping sets also force it so the input is exactly [keys..., args...], which
    // the masked-batch driver relies on.
    bool needsArgProject = global || !computedKeys.empty() || multiSet;
    for (const auto& spec : specs) {
        if (spec.Arg && !NAst::TMaybeNode<NAst::TIdentExpr>(spec.Arg)) {
            needsArgProject = true;
            break;
        }
    }
    if (needsArgProject) {
        std::vector<TProjectionSpec> argProjections;
        std::set<std::string> projected;
        auto passthrough = [&](const std::string& name) {
            if (projected.insert(name).second) {
                argProjections.push_back({ .Name = name, .Expression = Ident({}, name) });
            }
        };
        for (const auto& key : groupKeys) {
            if (projected.insert(key.Name).second) {
                argProjections.push_back({ .Name = key.Name, .Expression = key.Expression });
            }
        }
        for (auto& spec : specs) {
            if (!spec.Arg) {
                continue;
            }
            // The constant global key keeps its name, which the scheduler and the
            // estimates recognize, so an input `__group__` argument is
            // materialized under a generated name instead of passed through.
            if (auto ident = NAst::TMaybeNode<NAst::TIdentExpr>(spec.Arg);
                ident && !(global && BareColumnName(ident.Cast()->Name) == "__group__"))
            {
                passthrough(ident.Cast()->Name);
            } else {
                std::string name = names.Next("arg");
                argProjections.push_back({ .Name = name, .Expression = spec.Arg });
                spec.Arg = Ident(spec.Arg->Location, name);
            }
        }
        if (global) {
            argProjections.push_back({
                .Name = "__group__",
                .Expression = std::make_shared<NAst::TNumberExpr>(TLocation{}, static_cast<int64_t>(1)),
            });
            keys.push_back("__group__");
        }
        node = Relation(std::make_shared<TProjectOperator>(std::move(node.Plan), std::move(argProjections)));
    }

    auto aggregate = std::make_shared<TAggregateOperator>(
        std::move(node.Plan), std::move(keys), std::move(specs));
    if (multiSet) {
        aggregate->MutableGroupingSets() = std::move(groupingSets);
        auto inputSchema = aggregate->Input()->OutputColumns();
        aggregate->Type = std::make_shared<NAst::TFunctionType>(
            std::vector<NAst::TTypePtr>{inputSchema},
            ComputeAggregateOutputType(inputSchema, aggregate->GroupKeys(), aggregate->Aggs(),
                !aggregate->GroupingSets().empty()));
    }
    node = Relation(aggregate);
    }
    if (having) {
        auto rewritten = ExtractScalarSubqueries(having, node, context, scalarCounter);
        if (!rewritten) {
            return std::unexpected(rewritten.error());
        }
        node.Plan = std::make_shared<TFilterOperator>(std::move(node.Plan), std::move(*rewritten));
    }
    {
        auto windowed = applyWindows(std::move(node.Plan));
        if (!windowed) {
            return std::unexpected(windowed.error());
        }
        node = Relation(std::move(*windowed));
    }
    if (auto ok = extractProjectionSubqueries(projections, node); !ok) {
        return std::unexpected(ok.error());
    }
    auto projected =
        Relation(std::make_shared<TProjectOperator>(std::move(node.Plan), std::move(projections)));
    return distinct ? ApplyDistinct(std::move(projected), outputNames) : projected;
}

std::expected<TRelation, TError> ApplyOrderBy(
    const NSql::TSqlQuery& query,
    TRelation relation,
    const TBuildContext& context)
{
    if (!query.OrderBy || query.OrderBy->Items.empty()) {
        return relation;
    }

    // BuildSelect always tops the plan with a projection. An ORDER BY item is
    // resolved in order of preference:
    //   1. a plain output-column identifier -> sort on it directly;
    //   2. an expression already computed in the select list (matched
    //      structurally) -> sort on that output column, so aggregates and other
    //      computed columns are reused rather than re-evaluated;
    //   3. a pure-scalar expression over the projection's input -> materialized
    //      as a hidden column, sorted on, then stripped by a final projection so
    //      the output schema is unchanged.
    // An aggregate not present in the select list cannot be sorted on (it would
    // be re-applied as a scalar over already-grouped rows), so it is rejected.
    auto topProject = TMaybeOp<TProjectOperator>(relation.Plan);

    // Index the original (pre-lowering) select-list expressions by structure, so
    // ORDER BY can match one and reuse its output column. The plan's projection
    // expressions are unusable here: the aggregate collector has already
    // rewritten e.g. sum(x) into a reference to a synthetic column.
    struct TOutputRef {
        std::string Name;
        std::optional<size_t> ProjectionIndex;
    };
    std::unordered_map<std::string, TOutputRef> outputByExpr;
    if (auto select = NSql::TMaybeNode<NSql::TSqlSelect>(query.Body);
        select && select.Cast()->SelectList) {
        const auto& items = select.Cast()->SelectList->Items;
        for (size_t i = 0; i < items.size(); ++i) {
            // A window select item is referenced by its output alias (handled by
            // the ident branch below); it also cannot be core-printed for a
            // structural match, so skip it here.
            if (!items[i]->Star && !HasWindowExpr(items[i]->Expr)) {
                auto name = ItemName(*items[i], i);
                std::optional<size_t> projectionIndex;
                if (!items[i]->Alias && items[i]->ColumnAliases.empty()
                    && !NAst::TMaybeNode<NAst::TIdentExpr>(items[i]->Expr)
                    && topProject)
                {
                    const auto& projections = topProject.Cast()->Projections();
                    for (size_t p = 0; p < projections.size(); ++p) {
                        if (projections[p].ImplicitName && projections[p].Name == name) {
                            projectionIndex = p;
                            break;
                        }
                    }
                }
                outputByExpr.emplace(
                    NAst::NCore::PrintAst(items[i]->Expr),
                    TOutputRef{std::move(name), projectionIndex});
            }
        }
    }

    std::vector<TSortKey> keys;
    keys.reserve(query.OrderBy->Items.size());
    std::vector<TProjectionSpec> hidden;
    int sortCounter = 0;

    for (const auto& item : query.OrderBy->Items) {
        std::string column;
        std::optional<size_t> projectionIndex;
        auto ident = NAst::TMaybeNode<NAst::TIdentExpr>(item->Expr);
        if (ident && std::ranges::find(relation.Labels, ident.Cast()->Name) != relation.Labels.end()) {
            column = ident.Cast()->Name;
        } else if (HasWindowExprLocal(item->Expr)) {
            return std::unexpected(TError(item->Expr->Location,
                "ORDER BY on a window expression must reference its select alias"));
        } else if (auto it = outputByExpr.find(NAst::NCore::PrintAst(item->Expr));
                   it != outputByExpr.end()) {
            column = it->second.Name;
            projectionIndex = it->second.ProjectionIndex;
        } else {
            if (!topProject) {
                return std::unexpected(TError(item->Expr->Location,
                    "ORDER BY on an expression is not supported here"));
            }
            if (HasGroupingCall(item->Expr)) {
                return std::unexpected(TError(item->Expr->Location,
                    "ORDER BY GROUPING() expression must appear in the select list"));
            }
            // Reuse the aggregate collector purely to detect aggregate calls:
            // a non-empty result means the expression aggregates but is not in
            // the select list, which cannot be materialized in a scalar project.
            TFreshNames names;
            TAggCollector probe(nullptr, true, names, context.Annotation);
            auto scalar = probe.Rewrite(item->Expr);
            if (!scalar) {
                return std::unexpected(scalar.error());
            }
            if (!probe.Empty()) {
                return std::unexpected(TError(item->Expr->Location,
                    "ORDER BY on an aggregate not in the select list is not supported"));
            }
            column = "__sort_" + std::to_string(sortCounter++);
            hidden.push_back({ .Name = column, .Expression = item->Expr });
        }

        keys.push_back({
            .Column = column,
            .Direction = item->Desc ? ESortDirection::Desc : ESortDirection::Asc,
            .Nulls = ConvertSortNulls(item->NullOrder),
            .ProjectionIndex = projectionIndex,
        });
    }

    if (hidden.empty()) {
        relation.Plan = std::make_shared<TSortOperator>(std::move(relation.Plan), std::move(keys));
        return relation;
    }

    // Pass-through projection over the original outputs, captured before the
    // hidden columns are appended; it becomes the strip projection above the sort.
    std::vector<TProjectionSpec> outputProjections;
    outputProjections.reserve(topProject.Cast()->Projections().size());
    for (const auto& p : topProject.Cast()->Projections()) {
        outputProjections.push_back({ .Name = p.Name, .Expression = Ident({}, p.Name) });
    }

    auto& projections = topProject.Cast()->MutableProjections();
    for (auto& h : hidden) {
        projections.push_back(std::move(h));
    }

    TOperatorPtr sorted = std::make_shared<TSortOperator>(std::move(relation.Plan), std::move(keys));
    return Relation(std::make_shared<TProjectOperator>(std::move(sorted), std::move(outputProjections)));
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

std::expected<TRelation, TError> ApplyLimit(
    const NSql::TSqlQuery& query,
    TRelation relation)
{
    if (!query.Limit && !query.Offset) {
        return relation;
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
    relation.Plan = std::make_shared<TLimitOperator>(std::move(relation.Plan), *limit, offsetValue);
    return relation;
}

std::expected<TRelation, TError> BuildSetOp(
    const NSql::TSqlSetOp& setOp,
    TBuildContext& context);

// Builds a query body (a SELECT, a set operation, or a parenthesized query).
std::expected<TRelation, TError> BuildQueryBody(
    const NSql::TSqlNodePtr& body,
    TBuildContext& context)
{
    if (auto select = NSql::TMaybeNode<NSql::TSqlSelect>(body)) {
        return BuildSelect(*select.Cast(), context);
    }
    if (auto setOp = NSql::TMaybeNode<NSql::TSqlSetOp>(body)) {
        return BuildSetOp(*setOp.Cast(), context);
    }
    if (auto sub = NSql::TMaybeNode<NSql::TSqlQuery>(body)) {
        return BuildQuery(*sub.Cast(), context);
    }
    return std::unexpected(TError("unsupported query body"));
}

std::expected<TRelation, TError> BuildSetOp(
    const NSql::TSqlSetOp& setOp,
    TBuildContext& context)
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
            auto branch = BuildQueryBody(node, context);
            if (!branch) {
                return std::unexpected(branch.error());
            }
            branches.push_back(std::move(branch->Plan));
            return {};
        };
        if (auto l = collect(setOp.Left); !l) return std::unexpected(l.error());
        if (auto r = collect(setOp.Right); !r) return std::unexpected(r.error());

        auto unionAll = Relation(std::make_shared<TUnionAllOperator>(std::move(branches)));
        if (setOp.Quantifier == NSql::ESetQuantifier::All) {
            return unionAll;
        }

        auto outputNames = OutputColumnNames(unionAll.Plan, "UNION");
        if (!outputNames) {
            return std::unexpected(outputNames.error());
        }
        return ApplyDistinct(std::move(unionAll), *outputNames);
    }

    if (setOp.Op == NSql::TSqlSetOp::EOp::Intersect
        || setOp.Op == NSql::TSqlSetOp::EOp::Except)
    {
        const bool isIntersect = setOp.Op == NSql::TSqlSetOp::EOp::Intersect;
        const std::string opName = isIntersect ? "INTERSECT" : "EXCEPT";
        if (setOp.Quantifier == NSql::ESetQuantifier::All) {
            return std::unexpected(TError(opName + " ALL is not supported yet"));
        }

        auto left = BuildQueryBody(setOp.Left, context);
        if (!left) {
            return std::unexpected(left.error());
        }
        auto right = BuildQueryBody(setOp.Right, context);
        if (!right) {
            return std::unexpected(right.error());
        }

        auto leftNames = OutputColumnNames(left->Plan, opName + " left branch");
        if (!leftNames) {
            return std::unexpected(leftNames.error());
        }
        auto rightNames = OutputColumnNames(right->Plan, opName + " right branch");
        if (!rightNames) {
            return std::unexpected(rightNames.error());
        }
        if (leftNames->size() != rightNames->size()) {
            return std::unexpected(TError(
                opName + " branches must have the same number of columns"));
        }

        auto leftDistinct = ApplyDistinct(std::move(*left), *leftNames);
        auto rightDistinct = ApplyDistinct(std::move(*right), *rightNames);

        std::vector<std::pair<std::string, std::string>> keys;
        keys.reserve(leftNames->size());
        for (size_t i = 0; i < leftNames->size(); ++i) {
            keys.emplace_back((*leftNames)[i], (*rightNames)[i]);
        }

        auto join = MakeJoin(
            std::move(leftDistinct.Plan), std::move(rightDistinct.Plan),
            std::move(keys),
            isIntersect ? EJoinType::LeftSemi : EJoinType::LeftAnti);
        if (!join) {
            return std::unexpected(join.error());
        }
        // Set membership uses IS NOT DISTINCT FROM semantics, including NULLs.
        for (auto& key : TMaybeOp<TJoinOperator>(*join).Cast()->MutableKeys()) {
            key.NullsEqual = true;
        }
        return Relation(std::move(*join));
    }

    return std::unexpected(TError("unsupported set operation"));
}

std::expected<TRelation, TError> BuildQuery(
    const NSql::TSqlQuery& query,
    TBuildContext& context)
{
    if (!query.WithClause) {
        auto plan = BuildQueryBody(query.Body, context);
        if (!plan) {
            return plan;
        }
        auto ordered = ApplyOrderBy(query, std::move(*plan), context);
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
    TBuildContext inner{.Annotation = context.Annotation};
    inner.Sources = [&](std::string_view name) -> std::expected<TOperatorPtr, TError> {
        const std::string lower = ToLower(std::string(name));
        auto it = ctes.find(lower);
        if (it == ctes.end()) {
            return context.Sources(name);
        }
        auto cached = builtDefs.find(lower);
        if (cached == builtDefs.end()) {
            auto plan = BuildQuery(*it->second->Query, inner);
            if (!plan) {
                return std::unexpected(plan.error());
            }
            if (it->second->Columns) {
                plan = ApplyColumnAliases(std::move(*plan), it->second->Columns->Items);
                if (!plan) {
                    return std::unexpected(plan.error());
                }
            }
            auto definition = std::make_shared<TCteDefinition>();
            definition->OutputType = plan->Plan->OutputColumns();
            definition->Plan = std::move(plan->Plan);
            cached = builtDefs.emplace(lower, std::move(definition)).first;
        }
        return std::make_shared<TCteRef>(cached->second);
    };

    auto plan = BuildQueryBody(query.Body, inner);
    if (!plan) {
        return plan;
    }
    auto ordered = ApplyOrderBy(query, std::move(*plan), inner);
    if (!ordered) {
        return ordered;
    }
    return ApplyLimit(query, std::move(*ordered));
}

} // namespace

std::expected<TOperatorPtr, TError> BuildPlan(
    const NQdb::NSql::TSqlNodePtr& query,
    const TTableSourceFactory& sources,
    const NKernel::TAnnotationContext& annotation)
{
    auto root = NSql::TMaybeNode<NSql::TSqlQuery>(query);
    if (!root) {
        return std::unexpected(TError("expected a query"));
    }
    TBuildContext context{.Sources = sources, .Annotation = annotation};
    auto relation = BuildQuery(*root.Cast(), context);
    if (!relation) {
        return std::unexpected(relation.error());
    }
    AssignCteIds(relation->Plan);
    return relation->Plan;
}

} // namespace NQdb
