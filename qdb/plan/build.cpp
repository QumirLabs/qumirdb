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
#include <qdb/plan/passes/typing.h>
#include <qdb/plan/passes/unbound_vars.h>

#include <qumir/parser/ast.h>
#include <qumir/parser/core/printer.h>
#include <qumir/parser/type.h>

#include <algorithm>
#include <initializer_list>
#include <iterator>
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

// A built relation: a typed plan and the SQL label of each output column.
// Labels are the names SQL references see, qualified by a FROM alias where one
// applies. Physical names only need to be unique where relations meet.
struct TRelation {
    TOperatorPtr Plan;
    std::vector<std::string> Labels;
};

// Naming state shared by all scopes of one query.
struct TBuildState {
    // Source aliases taken in the CTE definition or main query being built.
    std::unordered_set<std::string> SourceAliases;
    size_t NextColumn = 0;
    std::unordered_map<const TCteDefinition*, std::vector<std::string>> CteLabels;
};

struct TBuildContext {
    TTableSourceFactory Sources;
    const NKernel::TAnnotationContext& Annotation;
    TBuildState& State;
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

std::string BareName(const std::string& name) {
    const auto dot = name.rfind('.');
    return dot == std::string::npos ? name : name.substr(dot + 1);
}

// Types an operator built over typed inputs. Kernel type annotation reports
// errors as exceptions; the builder returns them as values.
std::expected<void, TError> Annotate(const TOperatorPtr& plan, const TBuildContext& context) {
    try {
        AnnotateOperatorType(plan, context.Annotation);
    } catch (const TError& error) {
        return std::unexpected(error);
    }
    return {};
}

// A physical name for an output whose label is not unique where it is used.
std::string NextColumnName(
    std::string_view prefix,
    const std::unordered_set<std::string>& taken,
    TBuildState& state)
{
    std::string name;
    do {
        name = std::string(prefix) + std::to_string(state.NextColumn++);
    } while (taken.contains(name));
    return name;
}

// Rewrites SQL column references to the physical names of the inputs. A label
// matches by its qualified name or by its bare column part. Other names are left
// for an enclosing scope, except physical names hidden from SQL, which are unknown
// unless `allowHidden` lets the caller resolve them otherwise. Returns whether
// every name was resolved.
std::expected<bool, TError> BindColumns(
    NAst::TExprPtr& expression,
    std::initializer_list<std::reference_wrapper<const TRelation>> inputs,
    bool allowHidden = false)
{
    std::unordered_map<std::string, std::optional<std::string>> names;
    std::unordered_set<std::string> physical;
    auto addName = [&](const std::string& label, const std::string& column) {
        if (!names.emplace(label, column).second) {
            names[label].reset();
        }
    };
    for (const TRelation& input : inputs) {
        const auto& fields = input.Plan->OutputColumns()->Fields;
        for (size_t i = 0; i < input.Labels.size(); ++i) {
            physical.insert(fields[i].first);
            addName(input.Labels[i], fields[i].first);
            if (input.Labels[i].find('.') != std::string::npos) {
                addName(BareName(input.Labels[i]), fields[i].first);
            }
        }
    }
    bool resolved = true;
    std::optional<TError> error;
    // The parser shares subtrees, e.g. the operand of BETWEEN; each binds once.
    std::unordered_set<const NAst::TExpr*> visited;
    auto rewrite = [&](const auto& self, NAst::TExprPtr& node) -> void {
        if (!node || error || !visited.insert(node.get()).second) {
            return;
        }
        if (auto ident = NAst::TMaybeNode<NAst::TIdentExpr>(node)) {
            const auto& name = ident.Cast()->Name;
            if (auto found = names.find(name); found != names.end()) {
                if (found->second) {
                    ident.Cast()->Name = *found->second;
                } else {
                    error = TError(node->Location, "input column is ambiguous: " + name);
                }
            } else if (!allowHidden && physical.contains(name)) {
                error = TError(node->Location, "unknown input column: " + name);
            } else {
                resolved = false;
            }
            return;
        }
        if (auto call = NAst::TMaybeNode<NAst::TCallExpr>(node)) {
            for (auto& argument : call.Cast()->Args) {
                self(self, argument);
            }
            return;
        }
        for (auto* child : node->MutableChildren()) {
            self(self, *child);
        }
    };
    rewrite(rewrite, expression);
    if (error) {
        return std::unexpected(std::move(*error));
    }
    return resolved;
}

// Binds a column name that must exist in `input`.
std::expected<std::string, TError> BindColumn(const std::string& name, const TRelation& input) {
    NAst::TExprPtr column = Ident({}, name);
    auto bound = BindColumns(column, {input});
    if (!bound) {
        return std::unexpected(bound.error());
    }
    if (!*bound) {
        return std::unexpected(TError("unknown input column: " + name));
    }
    return NAst::TMaybeNode<NAst::TIdentExpr>(column).Cast()->Name;
}

// Renames the outputs of a plan. A top projection is renamed in place.
std::expected<TOperatorPtr, TError> RenameOutputs(
    TOperatorPtr plan,
    const std::vector<std::string>& names,
    const TBuildContext& context)
{
    if (auto project = TMaybeOp<TProjectOperator>(plan)) {
        size_t offset = 0;
        for (auto& spec : project.Cast()->MutableProjections()) {
            spec.Name = names[offset];
            for (size_t field = 0; field < spec.ColumnAliases.size(); ++field) {
                spec.ColumnAliases[field] = names[offset + field];
            }
            offset += FlattenedProjectionArity(spec);
        }
    } else {
        const auto fields = plan->OutputColumns()->Fields;
        std::vector<TProjectionSpec> renamed;
        for (size_t i = 0; i < fields.size(); ++i) {
            renamed.push_back({.Name = names[i], .Expression = Ident({}, fields[i].first)});
        }
        plan = std::make_shared<TProjectOperator>(std::move(plan), std::move(renamed));
    }
    if (auto annotated = Annotate(plan, context); !annotated) {
        return std::unexpected(annotated.error());
    }
    return plan;
}

// Gives a relation new labels. Physical names follow them unless labels repeat.
std::expected<TRelation, TError> Relabel(
    TRelation relation,
    std::vector<std::string> labels,
    TBuildContext& context)
{
    if (labels.size() != relation.Plan->OutputColumns()->Fields.size()) {
        return std::unexpected(TError("column alias count does not match query output"));
    }
    auto names = labels;
    std::unordered_set<std::string> unique(names.begin(), names.end());
    if (unique.size() != names.size()) {
        for (auto& name : names) {
            name = NextColumnName("__scope_", unique, context.State);
        }
    }
    auto plan = RenameOutputs(std::move(relation.Plan), names, context);
    if (!plan) {
        return std::unexpected(plan.error());
    }
    return TRelation{.Plan = std::move(*plan), .Labels = std::move(labels)};
}

// Renames a built sub-plan's output columns to the given list (CTE column lists,
// derived-table column aliases and internal subquery outputs).
std::expected<TRelation, TError> ApplyColumnAliases(
    TRelation relation,
    const std::vector<std::string>& aliases,
    TBuildContext& context)
{
    for (const auto& alias : aliases) {
        if (auto valid = ValidateOutputColumnName(alias); !valid) {
            return std::unexpected(valid.error());
        }
    }
    return Relabel(std::move(relation), aliases, context);
}

// Qualifies a subplan's output columns as `alias.col`, so the outer query can
// reference `alias.col` (and equi-join keys on them are extracted).
std::expected<TRelation, TError> AliasSubplan(
    TRelation relation,
    const std::string& alias,
    TBuildContext& context)
{
    std::vector<std::string> labels;
    for (const auto& label : relation.Labels) {
        labels.push_back(alias + "." + BareName(label));
    }
    return Relabel(std::move(relation), std::move(labels), context);
}

// Deduplicates rows by grouping on every output column (SELECT DISTINCT, UNION,
// INTERSECT, EXCEPT and IN marks).
TRelation ApplyDistinct(TRelation relation) {
    auto keys = ColumnNames(relation.Plan);
    relation.Plan = std::make_shared<TAggregateOperator>(
        std::move(relation.Plan), std::move(keys), std::vector<TAggregateSpec>{});
    return relation;
}

// FROM items and subqueries may expose equal physical names, for example two
// unaliased derived tables. The right side gets unique names; labels stay.
std::expected<TRelation, TError> SeparateRight(
    const TRelation& left,
    TRelation right,
    TBuildContext& context)
{
    const auto leftNames = ColumnNames(left.Plan);
    std::unordered_set<std::string> taken(leftNames.begin(), leftNames.end());
    const auto names = ColumnNames(right.Plan);
    if (std::ranges::none_of(names, [&](const auto& name) { return taken.contains(name); })) {
        return right;
    }
    const auto collisions = taken;
    taken.insert(names.begin(), names.end());
    std::vector<TProjectionSpec> renamed;
    for (const auto& name : names) {
        renamed.push_back({
            .Name = collisions.contains(name) ? NextColumnName("__scope_", taken, context.State) : name,
            .Expression = Ident({}, name)});
    }
    right.Plan = std::make_shared<TProjectOperator>(std::move(right.Plan), std::move(renamed));
    if (auto annotated = Annotate(right.Plan, context); !annotated) {
        return std::unexpected(annotated.error());
    }
    return right;
}

// A subquery's outputs under internal names, ready to join onto `node`.
std::expected<TRelation, TError> SubqueryOutputs(
    const TRelation& node,
    TRelation subquery,
    const std::vector<std::string>& outputs,
    TBuildContext& context)
{
    auto aliased = ApplyColumnAliases(std::move(subquery), outputs, context);
    if (!aliased) {
        return aliased;
    }
    return SeparateRight(node, std::move(*aliased), context);
}

// Unaliased sources are named by their table, numbered when the name repeats
// within one CTE definition or the main query.
std::string SourceAlias(const NSql::TSqlTableName& table, TBuildState& state) {
    if (table.Alias) {
        state.SourceAliases.insert(*table.Alias);
        return *table.Alias;
    }
    const auto stem = ToLower(table.Name.back());
    auto alias = stem;
    for (size_t i = 1; !state.SourceAliases.insert(alias).second; ++i) {
        alias = stem + "_" + std::to_string(i);
    }
    return alias;
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
        const auto qualifier = node->Alias ? *node->Alias : node->Name.back();
        if (auto sourceOp = TMaybeOp<TSourceOperator>(*source)) {
            sourceOp.Cast()->SetAlias(SourceAlias(*node, context.State));
            TRelation relation{.Plan = *source};
            for (const auto& column : sourceOp.Cast()->GetSource().Schema().Columns) {
                relation.Labels.push_back(qualifier + "." + std::string(column.Name));
            }
            return relation;
        }
        // A CTE exposes its declared output names, qualified by the reference
        // alias or - when referenced bare - by the CTE name.
        auto relation = Relation(*source);
        if (auto cte = TMaybeOp<TCteRef>(*source)) {
            if (auto found = context.State.CteLabels.find(cte.Cast()->Def().get());
                found != context.State.CteLabels.end())
            {
                relation.Labels = found->second;
            }
        }
        return AliasSubplan(std::move(relation), qualifier, context);
    }

    if (auto sub = NSql::TMaybeNode<NSql::TSqlSubqueryTable>(ref)) {
        auto node = sub.Cast();
        auto plan = BuildQuery(*node->Query, context);
        if (!plan) {
            return plan;
        }
        if (node->ColumnAliases) {
            plan = ApplyColumnAliases(std::move(*plan), node->ColumnAliases->Items, context);
            if (!plan) {
                return plan;
            }
        }
        if (node->Alias) {
            return AliasSubplan(std::move(*plan), *node->Alias, context);
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
        right = SeparateRight(*left, std::move(*right), context);
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
                if (auto bound = BindColumns(residual, {*left, *right}); !bound) {
                    return std::unexpected(bound.error());
                }
            } else if (join->Condition->UsingColumns) {
                for (const auto& column : join->Condition->UsingColumns->Items) {
                    auto leftKey = BindColumn(column, *left);
                    if (!leftKey) {
                        return std::unexpected(leftKey.error());
                    }
                    auto rightKey = BindColumn(column, *right);
                    if (!rightKey) {
                        return std::unexpected(rightKey.error());
                    }
                    keys.push_back({ .Left = *leftKey, .Right = *rightKey });
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
        right = SeparateRight(result, std::move(*right), context);
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
    right = SeparateRight(left, std::move(*right), context);
    if (!right) {
        return std::unexpected(right.error());
    }
    NAst::TExprPtr residual = select->Where;
    if (auto bound = BindColumns(residual, {left, *right}); !bound) {
        return std::unexpected(bound.error());
    }
    return JoinRelations(std::move(left), std::move(*right), {}, type, std::move(residual));
}

// IN: build the subquery as a full plan and join on `operand == <its only column>`.
// The operand is bound with the rest of the WHERE clause.
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
    // Rename the subquery's output to a synthetic name so the residual's two sides
    // never share a name: otherwise `x IN (SELECT x ...)` yields `x == x`, which
    // key extraction drops entirely, leaving a keyless (unschedulable,
    // condition-less) semi-join. The name is internal - a semi/anti join exposes
    // only its left side.
    right = SubqueryOutputs(left, std::move(*right), {"__qdb_in__"}, context);
    if (!right) {
        return std::unexpected(right.error());
    }
    auto residual = std::make_shared<NAst::TBinaryExpr>(
        subquery.Operand->Location, NAst::TOperator("=="), subquery.Operand,
        Ident(subquery.Operand->Location, ColumnNames(right->Plan).front()));
    return JoinRelations(std::move(left), std::move(*right), {}, type, std::move(residual));
}

// Qualified references belong to a FROM scope independently of its output types.
// Bare names need a complete schema; derived-table outputs of a star or a set
// operation can leave their classification unknown.
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
        TLocalSqlScope local;
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
        NAst::TExprPtr key = Ident(sub->Location, inner);
        for (auto* expression : {&where, &key}) {
            if (auto bound = BindColumns(*expression, {*body}); !bound) {
                return std::unexpected(bound.error());
            }
        }
        TOperatorPtr filtered = std::make_shared<TFilterOperator>(std::move(body->Plan), std::move(where));
        std::vector<TProjectionSpec> projs{
            { .Name = "__exists_key__", .Expression = std::move(key) }};
        branches.push_back(std::make_shared<TProjectOperator>(
            std::move(filtered), std::move(projs)));
        if (auto annotated = Annotate(branches.back(), context); !annotated) {
            return std::unexpected(annotated.error());
        }
    }
    auto un = Relation(std::make_shared<TUnionAllOperator>(std::move(branches)));
    if (auto annotated = Annotate(un.Plan, context); !annotated) {
        return std::unexpected(annotated.error());
    }
    auto outer = BindColumn(*outerKey, left);
    if (!outer) {
        return std::unexpected(outer.error());
    }
    auto right = SeparateRight(left, std::move(un), context);
    if (!right) {
        return std::unexpected(right.error());
    }
    // Keyed left-semi (not a residual) so cost-based join reordering, which runs before
    // equi-key extraction, sees this as an equi-join and does not cross-join around it.
    std::vector<TJoinKey> keys{ TJoinKey{ *outer, ColumnNames(right->Plan).front() } };
    return std::optional<TRelation>{
        JoinRelations(left, std::move(*right), std::move(keys), EJoinType::LeftSemi, nullptr)};
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

// Replaces each scalar subquery in a bound expression with a reference to a fresh
// column. Uncorrelated subqueries broadcast via a cross join (a single row).
// Correlated ones are decorrelated: the subquery is grouped by its correlation
// columns and LEFT-joined onto `node` (Apply elimination).
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
            // Only uncorrelated IN is supported here: outer references must not
            // survive in a standalone subquery plan.
            if (auto sel = NSql::TMaybeNode<NSql::TSqlSelect>(scalar->Query->Body);
                sel && sel.Cast()->From && sel.Cast()->Where)
            {
                TLocalSqlScope local;
                for (const auto& item : sel.Cast()->From->Items) {
                    CollectLocalColumns(item, context.Sources, local);
                }
                if (HasUnmappedOuter(sel.Cast()->Where, local)) {
                    return std::unexpected(TError(
                        "correlated IN subquery in a boolean expression is not supported"));
                }
            }
            auto plan = BuildQuery(*scalar->Query, context);
            if (!plan) {
                return std::unexpected(plan.error());
            }
            auto right = SubqueryOutputs(
                node, std::move(*plan), {"__in_mark_" + std::to_string(counter++) + "__"}, context);
            if (!right) {
                return std::unexpected(right.error());
            }
            const auto mark = ColumnNames(right->Plan).front();
            // Left join null-extends the marker to NULL on non-match.
            std::vector<TJoinKey> keys;
            NAst::TExprPtr residual;
            if (auto ident = NAst::TMaybeNode<NAst::TIdentExpr>(scalar->Operand)) {
                keys.push_back(TJoinKey{ ident.Cast()->Name, mark });
            } else {
                residual = std::make_shared<NAst::TBinaryExpr>(
                    scalar->Operand->Location, NAst::TOperator("=="),
                    scalar->Operand, Ident(scalar->Operand->Location, mark));
            }
            // Dedup so each outer row matches at most one right row (no duplication).
            node = JoinRelations(std::move(node), ApplyDistinct(std::move(*right)), std::move(keys),
                EJoinType::Left, std::move(residual));
            // Replace `x IN (subquery)` with `!(qdb_is_null(marker))`.
            return std::make_shared<NAst::TUnaryExpr>(
                expr->Location, NAst::TOperator("!"),
                Call(expr->Location, "qdb_is_null", { Ident(expr->Location, mark) }));
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
                std::vector<std::string> outputs;
                std::vector<std::string> outerColumns;
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
                    auto outer = BindColumn(outerCol, node);
                    if (!outer) {
                        return std::unexpected(outer.error());
                    }
                    outerColumns.push_back(std::move(*outer));
                    outputs.push_back(std::move(corrName));
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
                outputs.push_back("__scalar_" + std::to_string(id) + "__");
                auto right = SubqueryOutputs(node, std::move(*plan), outputs, context);
                if (!right) {
                    return std::unexpected(right.error());
                }
                const auto columns = ColumnNames(right->Plan);
                std::vector<TJoinKey> keys;
                for (size_t k = 0; k < outerColumns.size(); ++k) {
                    keys.push_back({ .Left = outerColumns[k], .Right = columns[k] });
                }
                // LEFT join preserves outer rows; a missing match yields NULL, so
                // the surrounding comparison filters the row out (SQL semantics).
                node = JoinRelations(
                    std::move(node), std::move(*right), std::move(keys), EJoinType::Left, nullptr);
                return Ident(expr->Location, columns.back());
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
        auto plan = BuildQuery(*scalar->Query, context);
        if (!plan) {
            return std::unexpected(plan.error());
        }
        auto right = SubqueryOutputs(
            node, std::move(*plan), {"__scalar_" + std::to_string(counter++) + "__"}, context);
        if (!right) {
            return std::unexpected(right.error());
        }
        const auto column = ColumnNames(right->Plan).front();
        node = JoinRelations(std::move(node), std::move(*right), {}, EJoinType::Inner, nullptr);
        return Ident(expr->Location, column);
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

// How expressions above an aggregate reach its group keys and aggregate slots.
struct TGrouping {
    TRelation Input;
    std::shared_ptr<TAggCollector> Aggregates;
    std::unordered_map<std::string, std::string> Computed;
    TGroupingRewriteInfo Rewrite;
};

// The expression must already be bound against the grouping input.
std::expected<NAst::TExprPtr, TError> RewriteGroupedExpression(
    NAst::TExprPtr expression,
    const TGrouping& grouping)
{
    auto aggregated = grouping.Aggregates->Rewrite(std::move(expression));
    if (!aggregated) {
        return aggregated;
    }
    return RewriteGroupingCalls(
        SubstituteGroupKeys(std::move(*aggregated), grouping.Computed), grouping.Rewrite);
}

// One SELECT list column before binding.
struct TSelectItem {
    std::string Name;
    // The SQL expression, or a reference to a physical column of a star.
    NAst::TExprPtr Expression;
    std::vector<std::string> ColumnAliases;
    bool ImplicitName = false;
    // Window and subquery items are SQL-only nodes: they never match structurally.
    bool Opaque = false;
    bool Hidden = false;
    bool Star = false;
};

std::expected<TRelation, TError> BuildAggregate(
    const TRelation& input,
    const std::vector<TSelectItem>& items,
    const NAst::TExprPtr& having,
    std::optional<TGroupingParse> parsed,
    std::shared_ptr<const TGrouping>& grouping,
    TBuildContext& context)
{
    const auto schema = input.Plan->OutputColumns();
    std::vector<TGroupKey> keys;
    std::vector<std::vector<size_t>> sets;
    bool hasGroupingSyntax = false;
    if (parsed) {
        keys = std::move(parsed->Keys);
        sets = std::move(parsed->Sets);
        hasGroupingSyntax = parsed->HasGroupingSyntax;
    }
    // Grouping-sets execution masks key columns for sets that omit them. Keep
    // computed aggregate arguments materialized separately: collapsing `sum(k+1)`
    // to reducers over `k` would make the subtotal consume the masked key.
    auto collector = std::make_shared<TAggCollector>(schema.get(), sets.size() <= 1, context.Annotation);
    auto bind = [&](NAst::TExprPtr& expression) -> std::expected<void, TError> {
        if (auto bound = BindColumns(expression, {input}); !bound) {
            return std::unexpected(bound.error());
        }
        return {};
    };
    for (const auto& item : items) {
        if (!item.Star) {
            if (auto collected = collector->Collect(item.Expression, bind); !collected) {
                return std::unexpected(collected.error());
            }
        }
    }
    if (auto collected = collector->Collect(having, bind); !collected) {
        return std::unexpected(collected.error());
    }
    auto specs = collector->TakeSpecs();
    for (const auto& spec : specs) {
        if (HasGroupingCall(spec.Arg)) {
            return std::unexpected(TError(spec.Arg->Location,
                "GROUPING() inside aggregate arguments is not supported"));
        }
    }
    std::unordered_map<std::string, NAst::TExprPtr> aliases;
    std::unordered_set<std::string> ambiguous;
    for (const auto& item : items) {
        if (item.Hidden || item.ImplicitName || !item.ColumnAliases.empty()) {
            continue;
        }
        auto expression = item.Expression;
        if (!item.Opaque && !item.Star) {
            expression = CloneExpr(expression);
            if (auto bound = BindColumns(expression, {input}); !bound) {
                return std::unexpected(bound.error());
            }
        }
        if (!aliases.emplace(item.Name, expression).second) {
            ambiguous.insert(item.Name);
        }
    }
    std::vector<TGroupKey> resolved;
    std::vector<size_t> remap;
    std::unordered_map<std::string, size_t> expressions;
    const auto inputNames = ColumnNames(input.Plan);
    std::unordered_set<std::string> reserved(inputNames.begin(), inputNames.end());
    for (const auto& spec : specs) {
        reserved.insert(spec.Name);
    }
    size_t nextGroupKey = 0;
    for (auto& key : keys) {
        if (HasOpaqueExpression(key.Expression)) {
            return std::unexpected(TError(key.Expression->Location,
                "GROUP BY key must refer to a scalar input expression"));
        }
        key.Expression = CloneExpr(key.Expression);
        // Input columns take precedence; only an unknown name may be a SELECT alias.
        auto ident = NAst::TMaybeNode<NAst::TIdentExpr>(key.Expression);
        auto bound = BindColumns(key.Expression, {input}, static_cast<bool>(ident));
        if (!bound) {
            return std::unexpected(bound.error());
        }
        if (!*bound && ident) {
            const auto name = ident.Cast()->Name;
            if (ambiguous.contains(name)) {
                return std::unexpected(TError(key.Expression->Location,
                    "GROUP BY alias is ambiguous: " + name));
            }
            auto alias = aliases.find(name);
            if (alias == aliases.end()) {
                return std::unexpected(TError(key.Expression->Location,
                    "unknown input column: " + name));
            }
            key.Expression = alias->second;
        }
        if (HasOpaqueExpression(key.Expression) || HasGroupingCall(key.Expression)) {
            return std::unexpected(TError(key.Expression->Location,
                "GROUP BY key must refer to a scalar input expression"));
        }
        if (HasAggregateCall(key.Expression)) {
            return std::unexpected(TError(key.Expression->Location,
                "GROUP BY key cannot refer to an aggregate"));
        }
        key.Expression = CloneExpr(key.Expression);
        if (auto ident = NAst::TMaybeNode<NAst::TIdentExpr>(key.Expression)) {
            key.Name = ident.Cast()->Name;
            key.Computed = false;
        } else {
            key.Computed = true;
            if (!reserved.insert(key.Name).second) {
                do {
                    key.Name = "gb_" + std::to_string(nextGroupKey++);
                } while (!reserved.insert(key.Name).second);
            }
        }
        const auto canonical = NAst::NCore::PrintAst(key.Expression);
        auto [it, inserted] = expressions.emplace(canonical, resolved.size());
        remap.push_back(it->second);
        if (inserted) {
            resolved.push_back(std::move(key));
        }
    }
    for (auto& set : sets) {
        std::vector<size_t> mapped;
        for (const auto index : set) {
            if (std::ranges::find(mapped, remap[index]) == mapped.end()) {
                mapped.push_back(remap[index]);
            }
        }
        set = std::move(mapped);
    }
    NarrowPlainGroupKeys(resolved, sets, hasGroupingSyntax);
    auto result = std::make_shared<TGrouping>(TGrouping{.Input = input, .Aggregates = collector,
        .Rewrite = MakeGroupingRewriteInfo(resolved, sets, hasGroupingSyntax)});
    std::vector<std::string> names;
    std::unordered_map<std::string, std::string> labels;
    for (const auto& key : resolved) {
        names.push_back(key.Name);
        if (key.Computed) {
            result->Computed.emplace(NAst::NCore::PrintAst(key.Expression), key.Name);
        } else {
            labels.emplace(key.Name, key.Name);
        }
    }
    for (size_t i = 0; i < input.Labels.size(); ++i) {
        if (auto label = labels.find(schema->Fields[i].first); label != labels.end()) {
            label->second = input.Labels[i];
        }
    }
    const bool multiSet = sets.size() > 1;
    TOperatorPtr plan = input.Plan;
    // count(distinct col): dedup with an inner aggregate on (group keys + col),
    // then count over the deduped rows above.
    if (collector->DistinctColumn()) {
        if (multiSet) {
            return std::unexpected(TError("GROUPING SETS with COUNT(DISTINCT) is not supported yet"));
        }
        if (!result->Computed.empty()) {
            return std::unexpected(TError(
                "GROUP BY on an expression with COUNT(DISTINCT) is not supported yet"));
        }
        const auto& distinct = *collector->DistinctColumn();
        auto dedup = names;
        if (std::ranges::find(dedup, distinct) == dedup.end()) {
            dedup.push_back(distinct);
        }
        // Mixed DISTINCT + non-DISTINCT: the inner aggregate computes the
        // non-distinct aggregates per (group, distinct value) as partials and the
        // outer one merges them - sum->sum, count->sum, min/max->min/max.
        std::vector<TAggregateSpec> partials;
        if (collector->HasNonDistinct()) {
            std::vector<TAggregateSpec> combined;
            for (auto& spec : specs) {
                if (collector->IsDistinctSpec(spec.Name)) {
                    combined.push_back(std::move(spec));
                    continue;
                }
                if (spec.Arg && !NAst::TMaybeNode<NAst::TIdentExpr>(spec.Arg)) {
                    return std::unexpected(TError(
                        "mixing DISTINCT with a computed aggregate argument is not supported"));
                }
                const auto partial = "partial_" + spec.Name;
                partials.push_back({.Name = partial, .Func = spec.Func, .Arg = spec.Arg});
                combined.push_back({.Name = spec.Name,
                    .Func = spec.Func == "count" ? "sum" : spec.Func, .Arg = Ident({}, partial)});
            }
            specs = std::move(combined);
        }
        plan = std::make_shared<TAggregateOperator>(std::move(plan), std::move(dedup), std::move(partials));
    }
    // The aggregate executor requires column-reference arguments, so materialize
    // computed arguments (and pass the group keys through) in a project below.
    // Grouping sets also force it so the input is exactly [keys..., args...], which
    // the masked-batch driver relies on.
    const bool global = names.empty();
    bool materialize = global || multiSet || !result->Computed.empty();
    for (const auto& spec : specs) {
        materialize |= spec.Arg && !NAst::TMaybeNode<NAst::TIdentExpr>(spec.Arg);
    }
    if (materialize) {
        std::vector<TProjectionSpec> arguments;
        std::unordered_set<std::string> used;
        for (const auto& key : resolved) {
            if (used.insert(key.Name).second) {
                arguments.push_back({.Name = key.Name, .Expression = key.Expression});
            }
        }
        std::unordered_set<std::string> occupied = used;
        for (const auto& name : ColumnNames(plan)) {
            occupied.insert(name);
        }
        size_t next = 0;
        for (auto& spec : specs) {
            if (!spec.Arg) {
                continue;
            }
            if (auto ident = NAst::TMaybeNode<NAst::TIdentExpr>(spec.Arg);
                ident && !(global && ident.Cast()->Name == "__group__"))
            {
                if (used.insert(ident.Cast()->Name).second) {
                    arguments.push_back({.Name = ident.Cast()->Name, .Expression = spec.Arg});
                }
            } else {
                std::string name;
                do {
                    name = "arg_" + std::to_string(next++);
                } while (!occupied.insert(name).second);
                used.insert(name);
                arguments.push_back({.Name = name, .Expression = spec.Arg});
                spec.Arg = Ident(spec.Arg->Location, name);
            }
        }
        if (global) {
            // A global aggregate still needs a grouping key: a constant one, which
            // the scheduler and estimates recognize. A colliding input argument
            // was materialized under another name.
            arguments.push_back({
                .Name = "__group__",
                .Expression = std::make_shared<NAst::TNumberExpr>(TLocation{}, static_cast<int64_t>(1)),
            });
            names.push_back("__group__");
        }
        plan = std::make_shared<TProjectOperator>(std::move(plan), std::move(arguments));
        if (auto annotated = Annotate(plan, context); !annotated) {
            return std::unexpected(annotated.error());
        }
    }
    auto aggregate = std::make_shared<TAggregateOperator>(
        std::move(plan), std::move(names), std::move(specs));
    if (multiSet) {
        // A single set over all keys is a plain aggregate; more need the kernel's
        // grouping-sets machinery (keys become nullable, masked per set).
        aggregate->MutableGroupingSets() = std::move(sets);
        if (auto annotated = Annotate(aggregate, context); !annotated) {
            return std::unexpected(annotated.error());
        }
    }
    // Plain group keys keep their input labels: correlated subqueries above the
    // aggregate reference them by SQL name.
    TRelation relation{.Plan = aggregate};
    for (const auto& name : ColumnNames(aggregate)) {
        auto label = labels.find(name);
        relation.Labels.push_back(label == labels.end() ? name : label->second);
    }
    grouping = std::move(result);
    return relation;
}

// What ORDER BY may reference besides output labels: SELECT expressions and the
// names they were bound against.
struct TSelectOutput {
    std::shared_ptr<TProjectOperator> Project;
    TRelation Input;
    std::shared_ptr<const TGrouping> Grouping;
    // Bound SELECT expressions by canonical text -> first output column.
    std::unordered_map<std::string, size_t> Expressions;
    // Per output column: equal identities are the same expression field.
    std::vector<std::optional<std::string>> Identities;
    bool Distinct = false;
};

std::expected<TRelation, TError> BuildSelect(
    const NSql::TSqlSelect& select,
    TBuildContext& context,
    TSelectOutput* output)
{
    if (!select.From) {
        return std::unexpected(TError("SELECT without FROM is not supported yet"));
    }
    if (!select.SelectList) {
        return std::unexpected(TError("empty select list"));
    }
    // Window functions are only valid in the SELECT list and ORDER BY. Use the
    // local check so a window inside an independent subquery in WHERE/HAVING (its
    // own scope) is not mistaken for one here.
    if (HasWindowExprLocal(select.Where)) {
        return std::unexpected(TError("window functions are not allowed in WHERE"));
    }
    if (HasWindowExprLocal(select.Having)) {
        return std::unexpected(TError("window functions are not allowed in HAVING"));
    }
    // Shared by WHERE, HAVING and SELECT so the __scalar_N__ names stay unique.
    int scalarCounter = 0;
    auto from = BuildFrom(*select.From, context);
    if (!from) {
        return from;
    }
    TRelation node = *from;
    if (select.Where) {
        // WHERE binds as one expression: BETWEEN shares its operand between the
        // two conjuncts it becomes.
        NAst::TExprPtr where = select.Where;
        if (auto bound = BindColumns(where, {node}); !bound) {
            return std::unexpected(bound.error());
        }
        std::vector<NAst::TExprPtr> conjuncts;
        FlattenConjuncts(where, conjuncts);

        std::vector<NAst::TExprPtr> residual;
        for (auto& conjunct : conjuncts) {
            if (auto decorrelation = AsDecorrelation(conjunct)) {
                auto joined = decorrelation->IsIn
                    ? DecorrelateIn(
                        std::move(node), *decorrelation->Subquery, decorrelation->Type, context)
                    : DecorrelateExists(
                        std::move(node), *decorrelation->Subquery, decorrelation->Type, context);
                if (!joined) {
                    return joined;
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
    std::optional<TGroupingParse> parsed;
    if (select.GroupBy) {
        auto grouping = ParseGrouping(*select.GroupBy);
        if (!grouping) {
            return std::unexpected(grouping.error());
        }
        parsed = std::move(*grouping);
    }
    std::vector<TSelectItem> items;
    for (size_t i = 0; i < select.SelectList->Items.size(); ++i) {
        const auto& item = select.SelectList->Items[i];
        if (item->Star) {
            std::string prefix;
            for (const auto& part : item->StarPrefix) {
                prefix += part + ".";
            }
            const auto& fields = from->Plan->OutputColumns()->Fields;
            for (size_t column = 0; column < from->Labels.size(); ++column) {
                if (from->Labels[column].starts_with(prefix)) {
                    items.push_back({.Name = BareName(from->Labels[column]),
                        .Expression = Ident({}, fields[column].first), .Star = true});
                }
            }
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
        items.push_back({.Name = ItemName(*item, i), .Expression = item->Expr,
            .ColumnAliases = item->ColumnAliases,
            .ImplicitName = !item->Alias && item->ColumnAliases.empty()
                && !NAst::TMaybeNode<NAst::TIdentExpr>(item->Expr),
            .Opaque = HasWindowExpr(item->Expr) || HasSubquery(item->Expr),
            .Hidden = item->Hidden});
    }
    const bool aggregated = parsed.has_value() || HasAggregateCall(select.Having)
        || std::ranges::any_of(items, [](const auto& item) { return HasAggregateCall(item.Expression); });
    if (!aggregated && select.Having) {
        return std::unexpected(TError("HAVING requires aggregation"));
    }
    std::shared_ptr<const TGrouping> grouping;
    if (aggregated) {
        auto aggregate = BuildAggregate(node, items, select.Having, std::move(parsed), grouping, context);
        if (!aggregate) {
            return aggregate;
        }
        node = std::move(*aggregate);
    }
    if (select.Having) {
        NAst::TExprPtr having = select.Having;
        if (auto bound = BindColumns(having, {grouping->Input}); !bound) {
            return std::unexpected(bound.error());
        }
        auto rewritten = RewriteGroupedExpression(std::move(having), *grouping);
        if (!rewritten) {
            return std::unexpected(rewritten.error());
        }
        auto extracted = ExtractScalarSubqueries(std::move(*rewritten), node, context, scalarCounter);
        if (!extracted) {
            return std::unexpected(extracted.error());
        }
        node.Plan = std::make_shared<TFilterOperator>(std::move(node.Plan), std::move(*extracted));
    }

    // SELECT expressions bind against the grouping input or the filtered FROM;
    // their scalar subqueries then join above any aggregate and HAVING. Window
    // functions run last.
    const TRelation input = grouping ? grouping->Input : node;
    std::vector<TProjectionSpec> projections;
    for (const auto& item : items) {
        auto& spec = projections.emplace_back(TProjectionSpec{.Name = item.Name,
            .Expression = item.Star || item.Opaque ? item.Expression : CloneExpr(item.Expression),
            .ColumnAliases = item.ColumnAliases, .ImplicitName = item.ImplicitName});
        if (!item.Star) {
            if (auto bound = BindColumns(spec.Expression, {input}); !bound) {
                return std::unexpected(bound.error());
            }
        }
        if (grouping) {
            auto rewritten = RewriteGroupedExpression(std::move(spec.Expression), *grouping);
            if (!rewritten) {
                return std::unexpected(rewritten.error());
            }
            spec.Expression = std::move(*rewritten);
        } else if (HasGroupingCall(spec.Expression)) {
            return std::unexpected(TError(spec.Expression->Location,
                "GROUPING() requires GROUPING SETS, ROLLUP, or CUBE"));
        }
        auto extracted = ExtractScalarSubqueries(std::move(spec.Expression), node, context, scalarCounter);
        if (!extracted) {
            return std::unexpected(extracted.error());
        }
        spec.Expression = std::move(*extracted);
    }
    TWindowCollector windows;
    for (auto& spec : projections) {
        auto rewritten = windows.Rewrite(spec.Expression);
        if (!rewritten) {
            return std::unexpected(rewritten.error());
        }
        spec.Expression = std::move(*rewritten);
    }
    auto windowed = windows.Build(std::move(node.Plan));
    if (!windowed) {
        return std::unexpected(windowed.error());
    }
    auto project = std::make_shared<TProjectOperator>(std::move(*windowed), std::move(projections));
    if (auto annotated = Annotate(project, context); !annotated) {
        return std::unexpected(annotated.error());
    }

    // Repeated labels get physical names unique among the inputs: sibling SELECTs
    // meet in joins and set operations.
    auto labels = ColumnNames(project);
    auto names = labels;
    std::unordered_set<std::string> used(names.begin(), names.end());
    if (used.size() != names.size()) {
        for (const auto& name : ColumnNames(project->Input())) {
            used.insert(name);
        }
        for (auto& name : names) {
            name = NextColumnName("__output_", used, context.State);
        }
    }
    TSelectOutput selected{.Project = project, .Input = input, .Grouping = grouping,
        .Distinct = select.Quantifier == NSql::ESetQuantifier::Distinct};
    size_t offset = 0;
    for (size_t i = 0; i < items.size(); ++i) {
        const auto& item = items[i];
        auto& spec = project->MutableProjections()[i];
        const auto width = FlattenedProjectionArity(spec);
        if (!width) {
            return std::unexpected(TError(spec.Expression->Location,
                "a SELECT projection must return at least one column"));
        }
        std::optional<std::string> canonical;
        if (!item.Opaque) {
            auto expression = CloneExpr(item.Expression);
            if (!item.Star) {
                if (auto bound = BindColumns(expression, {input}); !bound) {
                    return std::unexpected(bound.error());
                }
            }
            canonical = NAst::NCore::PrintAst(expression);
            if (!item.Hidden) {
                selected.Expressions.emplace(*canonical, offset);
            }
        }
        for (size_t field = 0; field < width; ++field) {
            selected.Identities.push_back(canonical
                ? std::optional<std::string>{*canonical + ":" + std::to_string(field)}
                : std::nullopt);
        }
        spec.Name = names[offset];
        if (!spec.ColumnAliases.empty()) {
            spec.ColumnAliases.assign(names.begin() + offset, names.begin() + offset + width);
        }
        offset += width;
    }
    if (auto annotated = Annotate(project, context); !annotated) {
        return std::unexpected(annotated.error());
    }
    TRelation result{.Plan = project, .Labels = std::move(labels)};
    if (selected.Distinct) {
        result = ApplyDistinct(std::move(result));
    }
    if (output) {
        *output = std::move(selected);
    }
    return result;
}

// An ORDER BY item names an output label, matches a SELECT expression or, over a
// plain SELECT, sorts on a hidden column computed in the SELECT projection.
std::expected<TRelation, TError> ApplyOrderBy(
    const NSql::TSqlQuery& query,
    TRelation child,
    TSelectOutput* select,
    TBuildContext& context)
{
    if (!query.OrderBy || query.OrderBy->Items.empty()) {
        return child;
    }
    const auto fields = child.Plan->OutputColumns()->Fields;
    std::unordered_set<std::string> used;
    for (const auto& [name, _] : fields) {
        used.insert(name);
    }
    if (select) {
        for (const auto& name : ColumnNames(select->Project->Input())) {
            used.insert(name);
        }
    }
    std::vector<TSortKey> keys;
    std::vector<TProjectionSpec> hidden;
    size_t next = 0;
    for (const auto& item : query.OrderBy->Items) {
        std::optional<size_t> index;
        if (auto ident = NAst::TMaybeNode<NAst::TIdentExpr>(item->Expr)) {
            for (size_t i = 0; i < child.Labels.size(); ++i) {
                if (child.Labels[i] != ident.Cast()->Name) {
                    continue;
                }
                if (index && (!select || !select->Identities[i]
                    || select->Identities[*index] != select->Identities[i]))
                {
                    return std::unexpected(TError(item->Expr->Location,
                        "ORDER BY output column is ambiguous: " + ident.Cast()->Name));
                }
                index = i;
            }
        }
        NAst::TExprPtr expression;
        if (!index && select && !HasOpaqueExpression(item->Expr)) {
            expression = CloneExpr(item->Expr);
            if (auto bound = BindColumns(expression, {select->Input}); !bound) {
                return std::unexpected(bound.error());
            }
            if (auto found = select->Expressions.find(NAst::NCore::PrintAst(expression));
                found != select->Expressions.end())
            {
                index = found->second;
            }
        }
        std::string name;
        if (index) {
            name = fields[*index].first;
        } else {
            if (HasOpaqueExpression(item->Expr)) {
                return std::unexpected(TError(item->Expr->Location,
                    "ORDER BY on a window or subquery must reference its select alias"));
            }
            if (!select) {
                return std::unexpected(TError(item->Expr->Location,
                    "ORDER BY on an expression is not supported here"));
            }
            if (select->Distinct) {
                return std::unexpected(TError(item->Expr->Location,
                    "ORDER BY expressions must appear in the SELECT DISTINCT output"));
            }
            if (HasAggregateCall(expression) || HasGroupingCall(expression)) {
                return std::unexpected(TError(item->Expr->Location,
                    "ORDER BY on an aggregate not in the select list is not supported"));
            }
            if (select->Grouping) {
                auto rewritten = RewriteGroupedExpression(std::move(expression), *select->Grouping);
                if (!rewritten) {
                    return std::unexpected(rewritten.error());
                }
                expression = std::move(*rewritten);
            }
            do {
                name = "__sort_" + std::to_string(next++);
            } while (!used.insert(name).second);
            hidden.push_back({.Name = name, .Expression = std::move(expression)});
        }
        keys.push_back({
            .Column = name,
            .Direction = item->Desc ? ESortDirection::Desc : ESortDirection::Asc,
            .Nulls = ConvertSortNulls(item->NullOrder),
        });
    }
    if (hidden.empty()) {
        child.Plan = std::make_shared<TSortOperator>(std::move(child.Plan), std::move(keys));
        return child;
    }
    // The hidden columns extend the SELECT projection; a final projection strips them.
    std::ranges::move(hidden, std::back_inserter(select->Project->MutableProjections()));
    if (auto annotated = Annotate(select->Project, context); !annotated) {
        return std::unexpected(annotated.error());
    }
    std::vector<TProjectionSpec> visible;
    for (const auto& [name, _] : fields) {
        visible.push_back({.Name = name, .Expression = Ident({}, name)});
    }
    child.Plan = std::make_shared<TProjectOperator>(
        std::make_shared<TSortOperator>(std::move(child.Plan), std::move(keys)), std::move(visible));
    if (auto annotated = Annotate(child.Plan, context); !annotated) {
        return std::unexpected(annotated.error());
    }
    return child;
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
    TBuildContext& context,
    TSelectOutput* output = nullptr)
{
    if (auto select = NSql::TMaybeNode<NSql::TSqlSelect>(body)) {
        return BuildSelect(*select.Cast(), context, output);
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
        std::vector<std::string> labels;
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
            if (branches.empty()) {
                labels = std::move(branch->Labels);
            }
            branches.push_back(std::move(branch->Plan));
            return {};
        };
        if (auto l = collect(setOp.Left); !l) return std::unexpected(l.error());
        if (auto r = collect(setOp.Right); !r) return std::unexpected(r.error());

        // The union's output takes the column names and labels of its first branch.
        TRelation unionAll{
            .Plan = std::make_shared<TUnionAllOperator>(std::move(branches)), .Labels = std::move(labels)};
        if (auto annotated = Annotate(unionAll.Plan, context); !annotated) {
            return std::unexpected(annotated.error());
        }
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

        auto left = BuildQueryBody(setOp.Left, context);
        if (!left) {
            return std::unexpected(left.error());
        }
        auto right = BuildQueryBody(setOp.Right, context);
        if (!right) {
            return std::unexpected(right.error());
        }
        const auto leftNames = ColumnNames(left->Plan);
        const auto rightNames = ColumnNames(right->Plan);
        if (leftNames.size() != rightNames.size()) {
            return std::unexpected(TError(
                opName + " branches must have the same number of columns"));
        }
        // Set membership uses IS NOT DISTINCT FROM semantics, including NULLs.
        std::vector<TJoinKey> keys;
        for (size_t i = 0; i < leftNames.size(); ++i) {
            keys.push_back({.Left = leftNames[i], .Right = rightNames[i], .NullsEqual = true});
        }
        return JoinRelations(ApplyDistinct(std::move(*left)), ApplyDistinct(std::move(*right)),
            std::move(keys), isIntersect ? EJoinType::LeftSemi : EJoinType::LeftAnti, nullptr);
    }

    return std::unexpected(TError("unsupported set operation"));
}

std::expected<TRelation, TError> BuildQuery(
    const NSql::TSqlQuery& query,
    TBuildContext& context)
{
    if (!query.WithClause) {
        TSelectOutput select;
        auto plan = BuildQueryBody(query.Body, context, &select);
        if (!plan) {
            return plan;
        }
        auto ordered = ApplyOrderBy(query, std::move(*plan), select.Project ? &select : nullptr, context);
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
    TBuildContext inner{.Annotation = context.Annotation, .State = context.State};
    inner.Sources = [&](std::string_view name) -> std::expected<TOperatorPtr, TError> {
        const std::string lower = ToLower(std::string(name));
        auto it = ctes.find(lower);
        if (it == ctes.end()) {
            return context.Sources(name);
        }
        auto cached = builtDefs.find(lower);
        if (cached == builtDefs.end()) {
            auto aliases = std::exchange(context.State.SourceAliases, {});
            auto plan = BuildQuery(*it->second->Query, inner);
            context.State.SourceAliases = std::move(aliases);
            if (!plan) {
                return std::unexpected(plan.error());
            }
            if (it->second->Columns) {
                plan = ApplyColumnAliases(std::move(*plan), it->second->Columns->Items, inner);
                if (!plan) {
                    return std::unexpected(plan.error());
                }
            }
            auto definition = std::make_shared<TCteDefinition>();
            definition->OutputType = plan->Plan->OutputColumns();
            definition->Plan = std::move(plan->Plan);
            context.State.CteLabels.emplace(definition.get(), std::move(plan->Labels));
            cached = builtDefs.emplace(lower, std::move(definition)).first;
        }
        return std::make_shared<TCteRef>(cached->second);
    };

    TSelectOutput select;
    auto plan = BuildQueryBody(query.Body, inner, &select);
    if (!plan) {
        return plan;
    }
    auto ordered = ApplyOrderBy(query, std::move(*plan), select.Project ? &select : nullptr, inner);
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
    TBuildState state;
    TBuildContext context{.Sources = sources, .Annotation = annotation, .State = state};
    auto relation = BuildQuery(*root.Cast(), context);
    if (!relation) {
        return std::unexpected(relation.error());
    }
    // The query's output columns carry their SQL names, repeated ones included.
    auto plan = relation->Plan;
    if (ColumnNames(plan) != relation->Labels) {
        auto renamed = RenameOutputs(std::move(plan), relation->Labels, context);
        if (!renamed) {
            return renamed;
        }
        plan = std::move(*renamed);
    }
    AssignCteIds(plan);
    return plan;
}

} // namespace NQdb
