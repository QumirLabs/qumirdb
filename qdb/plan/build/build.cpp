#include <qdb/plan/build/build.h>

#include <qdb/plan/build/aggregate.h>
#include <qdb/plan/build/binder.h>
#include <qdb/plan/build/context.h>
#include <qdb/plan/build/expr.h>
#include <qdb/plan/build/relation.h>
#include <qdb/plan/clone_expr.h>
#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/cte_ref.h>
#include <qdb/plan/ops/dependent_join.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/limit.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/sort.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/plan_print.h>
#include <qdb/plan/sql_lowering.h>

#include <qumir/parser/ast.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace NQdb {
namespace NBuild {

namespace NAst = NQumir::NAst;
using NQumir::TError;
using NQumir::TLocation;
using namespace NAst::NLiterals;

namespace {

std::string TableName(const std::vector<std::string>& parts) {
    std::string name;
    for (const auto& part : parts) {
        name += (name.empty() ? "" : ".") + part;
    }
    return name;
}

// Where a SELECT starts, for errors about SQL nodes that carry no location: its
// first expression.
TLocation SelectLocation(const NSql::TSqlSelect& select) {
    if (select.SelectList) {
        for (const auto& item : select.SelectList->Items) {
            if (item->Expr) {
                return item->Expr->Location;
            }
        }
    }
    return {};
}

TLocation BodyLocation(const NSql::TSqlNodePtr& body) {
    if (auto setOp = NSql::TMaybeNode<NSql::TSqlSetOp>(body)) {
        return BodyLocation(setOp.Cast()->Left);
    }
    if (auto query = NSql::TMaybeNode<NSql::TSqlQuery>(body)) {
        return BodyLocation(query.Cast()->Body);
    }
    if (auto select = NSql::TMaybeNode<NSql::TSqlSelect>(body)) {
        return SelectLocation(*select.Cast());
    }
    return {};
}

// Where a ROLLUP, CUBE or GROUPING SETS element starts: its first expression.
TLocation GroupingLocation(const NSql::TSqlNodePtr& element) {
    NAst::TExprPtr exprs;
    if (auto rollup = NSql::TMaybeNode<NSql::TSqlRollUp>(element)) {
        exprs = rollup.Cast()->Exprs;
    } else if (auto cube = NSql::TMaybeNode<NSql::TSqlCube>(element)) {
        exprs = cube.Cast()->Exprs;
    } else if (auto list = NSql::TMaybeNode<NSql::TSqlGroupingExprOrList>(element)) {
        exprs = list.Cast()->Exprs;
    } else if (auto maybeSet = NSql::TMaybeNode<NSql::TSqlGroupingSet>(element)) {
        const auto& items = maybeSet.Cast()->Items;
        return items.empty()
            ? TLocation{}
            : GroupingLocation(items.front());
    }
    return exprs
        ? exprs->Location
        : TLocation{};
}

// The output name of a SELECT item: its alias, a column's own name, or colN.
std::string ItemName(const NSql::TSqlSelectItem& item, size_t index) {
    if (!item.ColumnAliases.empty()) {
        return item.ColumnAliases.front();
    }
    if (item.Alias) {
        return *item.Alias;
    }
    if (auto ident = NAst::TMaybeNode<NAst::TIdentExpr>(item.Expr)) {
        return BareColumnName(ident.Cast()->Name);
    }
    return "col" + std::to_string(index);
}

// A bare integer in GROUP BY or ORDER BY is the 1-based position of a SELECT
// output.
std::optional<int64_t> SelectPosition(const NAst::TExprPtr& expr) {
    auto operand = expr;
    int64_t sign = 1;
    if (auto maybeUnary = NAst::TMaybeNode<NAst::TUnaryExpr>(expr)) {
        auto unary = maybeUnary.Cast();
        if (unary->Operator != "-"_op) {
            return std::nullopt;
        }
        operand = unary->Operand;
        sign = -1;
    }
    auto maybeNumber = NAst::TMaybeNode<NAst::TNumberExpr>(operand);
    if (!maybeNumber) {
        return std::nullopt;
    }
    auto number = maybeNumber.Cast();
    if (!NAst::TMaybeType<NAst::TIntegerType>(number->Type)) {
        return std::nullopt;
    }
    return sign * number->IntValue;
}

// The CTE a table name refers to, defined by the nearest WITH clause.
std::expected<std::optional<TRelation>, TError> FindCte(const std::string& name, TContext& context) {
    const auto key = ToLower(name);
    auto* scope = FindCteScope(context.Ctes, key);
    if (!scope) {
        return std::nullopt;
    }
    const auto& definition = scope->Definitions.at(key);
    auto [built, first] = scope->Built.try_emplace(key);
    if (first) {
        TBlock block{.Id = context.NextScope++, .Parent = scope->Enclosing};
        // A definition is optimized as a plan of its own and numbers its sources apart.
        auto aliases = std::exchange(context.SourceAliases, {});
        auto ctes = std::exchange(context.Ctes, scope);
        auto relation = BuildQuery(*definition->Query, block, context);
        context.Ctes = ctes;
        context.SourceAliases = std::move(aliases);
        if (relation && !block.OuterScopes.empty()) {
            relation = std::unexpected(TError(block.OuterScopes.begin()->second,
                "CTE " + name + " reads columns of an enclosing query; this is not supported yet"));
        }
        if (relation && definition->Columns) {
            relation = Relabel(std::move(*relation), definition->Columns->Items, context.Annotation);
        }
        if (!relation) {
            scope->Built.erase(key);
            return std::unexpected(relation.error());
        }
        auto cte = std::make_shared<TCteDefinition>();
        cte->OutputType = relation->Plan->OutputColumns();
        cte->Plan = std::move(relation->Plan);
        built->second = TBuiltCte{.Definition = std::move(cte), .Labels = std::move(relation->Labels)};
    } else if (!built->second.Definition) {
        return std::unexpected(TError("CTE " + name + " refers to itself; WITH RECURSIVE is not supported yet"));
    }
    return TRelation{
        .Plan = std::make_shared<TCteRef>(built->second.Definition), .Labels = built->second.Labels};
}

// A FROM item, and where it first reads the relation to its left, if it does
// (an implicit LATERAL item, as HyPer accepts it).
struct TFromItem {
    TRelation Relation;
    std::optional<TLocation> Correlation;
};

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

// The column names both inputs expose to SQL, in the order of `left`: the USING
// columns of a NATURAL JOIN.
std::vector<std::string> CommonColumns(const TRelation& left, const TRelation& right) {
    auto visibleNames = [](const TRelation& relation) {
        std::vector<std::string> names;
        for (const auto& label : relation.Labels) {
            if (!label.empty() && !relation.QualifiedOnly.contains(label)) {
                names.push_back(BareColumnName(label));
            }
        }
        return names;
    };
    const auto rightNames = visibleNames(right);
    std::vector<std::string> common;
    for (auto& name : visibleNames(left)) {
        if (std::ranges::find(rightNames, name) != rightNames.end() && std::ranges::find(common, name) == common.end()) {
            common.push_back(std::move(name));
        }
    }
    return common;
}

// A USING column is one column of the join: the left input of an inner or left
// join, the right input of a right join, or the COALESCE of both for a full
// join, in place of the left input. The inputs stay reachable qualified only.
std::expected<TRelation, TError> ShareUsingColumns(
    TRelation joined,
    const std::vector<TJoinKey>& keys,
    EJoinType type,
    const TContext& context)
{
    const auto columns = ColumnNames(joined.Plan);
    auto labelOf = [&](const std::string& column) -> std::string& {
        return joined.Labels[std::ranges::find(columns, column) - columns.begin()];
    };
    // An unqualified input, such as the shared column of a nested USING join,
    // has no other name, so it is hidden.
    auto qualifyOnly = [&](std::string& label) {
        if (label.find('.') == std::string::npos) {
            label.clear();
        } else {
            joined.QualifiedOnly.insert(label);
        }
    };
    switch (type) {
        case EJoinType::Inner:
        case EJoinType::Left:
            for (const auto& key : keys) {
                qualifyOnly(labelOf(key.Right));
            }
            return joined;
        case EJoinType::Right:
        case EJoinType::Full:
            break;
        default:
            // A semi join outputs one input only.
            return joined;
    }
    std::unordered_map<std::string, std::string> shared; // left input -> SQL name
    for (const auto& key : keys) {
        shared.emplace(key.Left, BareColumnName(labelOf(key.Left)));
        qualifyOnly(labelOf(key.Left));
        qualifyOnly(labelOf(key.Right));
    }
    TFreshNames fresh;
    for (const auto& column : columns) {
        fresh.Reserve(column);
    }
    std::vector<TProjectionSpec> projections;
    std::vector<std::string> labels;
    for (size_t i = 0; i < columns.size(); ++i) {
        if (auto name = shared.find(columns[i]); name != shared.end()) {
            const auto& right = std::ranges::find(keys, columns[i], &TJoinKey::Left)->Right;
            auto value = type == EJoinType::Right
                ? Ident({}, right)
                : Call({}, "coalesce", {Ident({}, columns[i]), Ident({}, right)});
            projections.push_back({.Name = fresh.Next("__using"), .Expression = std::move(value)});
            labels.push_back(name->second);
        }
        projections.push_back({.Name = columns[i], .Expression = Ident({}, columns[i])});
        labels.push_back(joined.Labels[i]);
    }
    joined.Plan = std::make_shared<TProjectOperator>(std::move(joined.Plan), std::move(projections));
    joined.Labels = std::move(labels);
    if (auto annotated = Annotate(joined.Plan, context.Annotation); !annotated) {
        return std::unexpected(annotated.error());
    }
    return joined;
}

std::expected<TFromItem, TError> BuildTableRef(
    const NSql::TSqlPtr<NSql::TSqlTableRef>& ref,
    TBlock& block,
    TContext& context);

// L JOIN R: R may read L, which makes the join dependent (section 3.4, step 1).
std::expected<TFromItem, TError> BuildJoin(const NSql::TSqlJoin& join, TBlock& block, TContext& context) {
    auto left = BuildTableRef(join.Left, block, context);
    if (!left) {
        return left;
    }
    auto saved = std::exchange(block.Tree, left->Relation);
    auto right = BuildTableRef(join.Right, block, context);
    block.Tree = std::move(saved);
    if (!right) {
        return right;
    }
    auto separated = SeparateRight(left->Relation, std::move(right->Relation), context.Annotation);
    if (!separated) {
        return std::unexpected(separated.error());
    }

    auto condition = join.Condition;
    if (join.Natural) {
        condition = std::make_shared<NSql::TJoinCondition>();
        condition->UsingColumns = std::make_shared<NSql::TIdentList>();
        condition->UsingColumns->Items = CommonColumns(left->Relation, *separated);
        if (condition->UsingColumns->Items.empty()) {
            return std::unexpected(TError(join.NaturalLocation, "No columns found to join on in NATURAL JOIN"));
        }
    }
    const auto type = MapJoinType(join.Type);
    NAst::TExprPtr residual;
    std::vector<TJoinKey> keys;
    if (condition && condition->On) {
        if (auto rejected = RejectSubqueries(condition->On, "JOIN conditions"); !rejected) {
            return std::unexpected(rejected.error());
        }
        auto both = JoinRelations(left->Relation, *separated, {}, EJoinType::Inner, nullptr);
        auto bound = BindOver(block, std::move(both), condition->On, context);
        if (!bound) {
            return std::unexpected(bound.error());
        }
        residual = std::move(*bound);
    } else if (condition && condition->UsingColumns) {
        for (const auto& column : condition->UsingColumns->Items) {
            auto leftKey = FindColumn(left->Relation, column, {});
            auto rightKey = FindColumn(*separated, column, {});
            if (!leftKey || !rightKey) {
                return std::unexpected(!leftKey ? leftKey.error() : rightKey.error());
            }
            if (!*leftKey || !*rightKey) {
                return std::unexpected(TError("USING column " + column + " is not in both join inputs"));
            }
            keys.push_back({
                .Left = ColumnNames(left->Relation.Plan)[**leftKey],
                .Right = ColumnNames(separated->Plan)[**rightKey],
            });
        }
    }

    if (!right->Correlation) {
        auto joined = JoinRelations(std::move(left->Relation), std::move(*separated), keys, type, std::move(residual));
        if (!condition || !condition->UsingColumns) {
            return TFromItem{.Relation = std::move(joined), .Correlation = left->Correlation};
        }
        auto shared = ShareUsingColumns(std::move(joined), keys, type, context);
        if (!shared) {
            return std::unexpected(shared.error());
        }
        return TFromItem{.Relation = std::move(*shared), .Correlation = left->Correlation};
    }
    if (!keys.empty()) {
        return std::unexpected(TError(*right->Correlation, "USING with a correlated join input is not supported yet"));
    }
    std::optional<EDependentJoinKind> kind;
    switch (type) {
        case EJoinType::Inner: kind = EDependentJoinKind::Inner; break;
        case EJoinType::Left: kind = EDependentJoinKind::Left; break;
        case EJoinType::LeftSemi: kind = EDependentJoinKind::Semi; break;
        default: break;
    }
    if (!kind) {
        return std::unexpected(TError(*right->Correlation, "a join input that reads the other input "
            "must be the right input of an inner, left or semi join"));
    }
    // The right input resolved its reads of the left one against this block.
    auto attached = AttachDependent(left->Relation, block.Id, std::move(*separated), *kind, std::move(residual), "",
        {}, context.Annotation);
    if (!attached) {
        return std::unexpected(attached.error());
    }
    return TFromItem{.Relation = std::move(left->Relation), .Correlation = left->Correlation};
}

std::expected<TFromItem, TError> BuildTableRef(
    const NSql::TSqlPtr<NSql::TSqlTableRef>& ref,
    TBlock& block,
    TContext& context)
{
    if (auto maybeTable = NSql::TMaybeNode<NSql::TSqlTableName>(ref)) {
        auto table = maybeTable.Cast();
        const auto qualifier = table->Alias ? *table->Alias : table->Name.back();
        if (table->Name.size() == 1) {
            auto cte = FindCte(table->Name.front(), context);
            if (!cte) {
                return std::unexpected(cte.error());
            }
            if (*cte) {
                auto aliased = AliasRelation(std::move(**cte), qualifier, context.Annotation);
                if (!aliased) {
                    return std::unexpected(aliased.error());
                }
                return TFromItem{.Relation = std::move(*aliased)};
            }
        }
        auto source = context.Sources(TableName(table->Name));
        if (!source) {
            return std::unexpected(source.error());
        }
        auto maybeSource = TMaybeOp<TSourceOperator>(*source);
        if (!maybeSource) {
            return std::unexpected(TError("table " + TableName(table->Name) + " is not a scan"));
        }
        auto sourceOp = maybeSource.Cast();
        sourceOp->SetAlias(SourceAlias(*table, context.SourceAliases));
        return TFromItem{.Relation = TableRelation(sourceOp, qualifier)};
    }

    if (auto maybeSubquery = NSql::TMaybeNode<NSql::TSqlSubqueryTable>(ref)) {
        auto subquery = maybeSubquery.Cast();
        TBlock inner{.Id = context.NextScope++, .Parent = &block};
        auto relation = BuildQuery(*subquery->Query, inner, context);
        if (relation && subquery->ColumnAliases) {
            relation = Relabel(std::move(*relation), subquery->ColumnAliases->Items, context.Annotation);
        }
        if (relation && subquery->Alias) {
            relation = AliasRelation(std::move(*relation), *subquery->Alias, context.Annotation);
        }
        if (!relation) {
            return std::unexpected(relation.error());
        }
        auto correlation = inner.OuterScopes.find(block.Id);
        return TFromItem{
            .Relation = std::move(*relation),
            .Correlation = correlation == inner.OuterScopes.end()
                ? std::nullopt
                : std::optional(correlation->second),
        };
    }

    if (auto maybeJoin = NSql::TMaybeNode<NSql::TSqlJoin>(ref)) {
        return BuildJoin(*maybeJoin.Cast(), block, context);
    }
    return std::unexpected(TError("unsupported table reference"));
}

// Translates FROM left to right into the block's tree: a cross product, or a
// dependent join for an item that reads the items before it.
std::expected<void, TError> BuildFrom(const NSql::TSqlFrom& from, TBlock& block, TContext& context) {
    if (from.Items.empty()) {
        return std::unexpected(TError("empty FROM"));
    }
    block.Tree = {};
    for (const auto& ref : from.Items) {
        auto item = BuildTableRef(ref, block, context);
        if (!item) {
            return std::unexpected(item.error());
        }
        if (!block.Tree.Plan) {
            block.Tree = std::move(item->Relation);
            continue;
        }
        auto right = SeparateRight(block.Tree, std::move(item->Relation), context.Annotation);
        if (!right) {
            return std::unexpected(right.error());
        }
        if (!item->Correlation) {
            block.Tree = JoinRelations(std::move(block.Tree), std::move(*right), {}, EJoinType::Inner, nullptr);
            continue;
        }
        auto attached = AttachDependent(block.Tree, block.Id, std::move(*right), EDependentJoinKind::Inner, nullptr, "",
            {}, context.Annotation);
        if (!attached) {
            return attached;
        }
    }
    return {};
}

// The expression a GROUP BY element groups by: a position or a SELECT alias
// names an output, whose expression is the key; an input column is found before
// an alias, as in PostgreSQL.
std::expected<NAst::TExprPtr, TError> GroupKeyExpression(
    const NAst::TExprPtr& element,
    const std::vector<NSql::TSqlPtr<NSql::TSqlSelectItem>>& items,
    const TBlock& block)
{
    auto selected = [&](const NSql::TSqlSelectItem& item) -> std::expected<NAst::TExprPtr, TError> {
        try {
            return CloneExpr(item.Expr);
        } catch (const TError&) {
            return std::unexpected(TError(element->Location, "GROUP BY on this output is not supported yet"));
        }
    };
    if (auto position = SelectPosition(element)) {
        if (std::ranges::any_of(items, [](const auto& item) { return item->Star; })) {
            return std::unexpected(TError(element->Location,
                "GROUP BY position with '*' in the select list is not supported yet"));
        }
        if (*position < 1 || *position > static_cast<int64_t>(items.size())) {
            return std::unexpected(TError(element->Location,
                "GROUP BY position " + std::to_string(*position) + " is not in the select list"));
        }
        return selected(*items[*position - 1]);
    }
    auto maybeIdent = NAst::TMaybeNode<NAst::TIdentExpr>(element);
    if (!maybeIdent) {
        return element;
    }
    const auto name = maybeIdent.Cast()->Name;
    auto column = FindColumn(block.Tree, name, element->Location);
    if (!column) {
        return std::unexpected(column.error());
    }
    auto alias = std::ranges::find_if(items, [&](const auto& item) { return item->Alias == name; });
    if (*column || alias == items.end()) {
        return element;
    }
    return selected(**alias);
}

std::expected<TGrouping, TError> BuildGrouping(
    const NSql::TSqlSelect& select,
    TBlock& block,
    TFreshNames& names,
    TContext& context)
{
    TGrouping grouping{.Present = select.GroupBy != nullptr};
    if (!select.GroupBy) {
        return grouping;
    }
    std::set<std::string> seen;
    for (const auto& element : select.GroupBy->Items) {
        auto maybeList = NSql::TMaybeNode<NSql::TSqlGroupingExprOrList>(element);
        if (!maybeList) {
            return std::unexpected(TError(GroupingLocation(element),
                "ROLLUP, CUBE and GROUPING SETS are not supported by the new builder yet"));
        }
        auto list = maybeList.Cast();
        std::vector<NAst::TExprPtr> exprs{list->Exprs};
        if (auto maybeBlock = NAst::TMaybeNode<NAst::TBlockExpr>(list->Exprs)) {
            exprs = maybeBlock.Cast()->Stmts;
        }
        for (const auto& expr : exprs) {
            auto key = GroupKeyExpression(expr, select.SelectList->Items, block);
            if (!key) {
                return std::unexpected(key.error());
            }
            if (auto rejected = RejectSubqueries(*key, "GROUP BY"); !rejected) {
                return std::unexpected(rejected.error());
            }
            auto bound = TExprTranslator(block, context, true, false).Translate(*key);
            if (!bound) {
                return std::unexpected(bound.error());
            }
            if (auto outer = FindNode(*bound, IsOuterRef)) {
                return std::unexpected(TError(outer->Location,
                    "GROUP BY on a column of an enclosing query is not supported yet"));
            }
            const auto printed = ExprLine(*bound);
            if (!seen.insert(printed).second) {
                continue;
            }
            if (auto ident = NAst::TMaybeNode<NAst::TIdentExpr>(*bound)) {
                grouping.Keys.push_back({.Name = ident.Cast()->Name, .Expression = *bound});
                continue;
            }
            auto name = names.Next("gb");
            grouping.Computed.emplace(printed, name);
            grouping.Keys.push_back({.Name = std::move(name), .Expression = *bound, .Computed = true});
        }
    }
    return grouping;
}

// The SELECT items bound against the block's tree, `*` expanded. Subqueries
// in aggregate arguments attach to that tree now; the others stay in place.
std::expected<std::vector<TProjectionSpec>, TError> BindSelectList(
    const NSql::TSqlSelect& select,
    TBlock& block,
    TContext& context)
{
    std::vector<TProjectionSpec> projections;
    for (size_t i = 0; i < select.SelectList->Items.size(); ++i) {
        const auto& item = select.SelectList->Items[i];
        if (!item->Star) {
            auto bound = TExprTranslator(block, context, true, false).Translate(item->Expr);
            if (!bound) {
                return std::unexpected(bound.error());
            }
            projections.push_back({
                .Name = ItemName(*item, i),
                .Expression = std::move(*bound),
                .ColumnAliases = item->ColumnAliases,
                .ImplicitName = !item->Alias && item->ColumnAliases.empty()
                    && !NAst::TMaybeNode<NAst::TIdentExpr>(item->Expr),
            });
            continue;
        }
        std::string prefix;
        for (const auto& part : item->StarPrefix) {
            prefix += part + ".";
        }
        const auto columns = ColumnNames(block.Tree.Plan);
        const auto expanded = projections.size();
        for (size_t column = 0; column < columns.size(); ++column) {
            const auto& label = block.Tree.Labels[column];
            if (label.empty() || (!prefix.empty() && !label.starts_with(prefix))
                || (prefix.empty() && block.Tree.QualifiedOnly.contains(label)))
            {
                continue;
            }
            projections.push_back({.Name = BareColumnName(label), .Expression = Ident({}, columns[column])});
        }
        if (!prefix.empty() && projections.size() == expanded) {
            return std::unexpected(TError(SelectLocation(select), "'" + prefix + "*' does not name a FROM item"));
        }
    }
    return projections;
}

// One ORDER BY item: a SELECT output, or an expression computed as a hidden
// column of the final projection.
struct TOrderItem {
    std::optional<size_t> Output;
    NAst::TExprPtr Hidden;
    const NSql::TSqlOrderItem* Item = nullptr;
};

// Resolves ORDER BY against the bound SELECT outputs: a position, an output
// name (first, as in PostgreSQL) or an expression written as in the select list;
// any other expression binds like the select list.
std::expected<std::vector<TOrderItem>, TError> BindOrderBy(
    const NSql::TSqlOrder* orderBy,
    const std::vector<TProjectionSpec>& projections,
    TBlock& block,
    TContext& context)
{
    std::vector<TOrderItem> items;
    if (!orderBy) {
        return items;
    }
    for (const auto& item : orderBy->Items) {
        TOrderItem order{.Item = item.get()};
        if (auto position = SelectPosition(item->Expr)) {
            if (*position < 1 || *position > static_cast<int64_t>(projections.size())) {
                return std::unexpected(TError(item->Expr->Location,
                    "ORDER BY position " + std::to_string(*position) + " is not in the select list"));
            }
            order.Output = *position - 1;
        } else if (auto ident = NAst::TMaybeNode<NAst::TIdentExpr>(item->Expr)) {
            const auto name = ident.Cast()->Name;
            for (size_t i = 0; i < projections.size(); ++i) {
                if (projections[i].Name != name) {
                    continue;
                }
                if (order.Output) {
                    return std::unexpected(TError(item->Expr->Location, "ORDER BY output column is ambiguous: " + name));
                }
                order.Output = i;
            }
        }
        if (!order.Output) {
            if (auto rejected = RejectSubqueries(item->Expr, "ORDER BY"); !rejected) {
                return std::unexpected(rejected.error());
            }
            auto bound = TExprTranslator(block, context, true, false).Translate(item->Expr);
            if (!bound) {
                return std::unexpected(bound.error());
            }
            const auto printed = ExprLine(*bound);
            auto same = std::ranges::find_if(projections, [&](const TProjectionSpec& projection) {
                return !FindNode(projection.Expression, IsSubquery) && ExprLine(projection.Expression) == printed;
            });
            if (same != projections.end()) {
                order.Output = same - projections.begin();
            } else {
                order.Hidden = std::move(*bound);
            }
        }
        items.push_back(std::move(order));
    }
    return items;
}

// The final projection: the SELECT outputs and then the hidden sort columns,
// deduplicated for DISTINCT, sorted, and stripped of the sort columns.
std::expected<TRelation, TError> ProjectOutputs(
    TOperatorPtr input,
    std::vector<TProjectionSpec> projections,
    std::vector<TOrderItem> order,
    bool distinct,
    TFreshNames& names,
    const TContext& context)
{
    const auto selected = projections.size();
    for (auto& item : order) {
        if (item.Hidden) {
            item.Output = projections.size();
            projections.push_back({.Name = names.Next("__sort"), .Expression = item.Hidden});
        }
    }
    auto project = std::make_shared<TProjectOperator>(std::move(input), std::move(projections));
    if (auto annotated = Annotate(project, context.Annotation); !annotated) {
        return std::unexpected(annotated.error());
    }
    // Labels are the typed output names, struct fields included; sort columns
    // are hidden. Physical names stay unique where labels repeat.
    TRelation output{.Plan = project, .Labels = ColumnNames(project)};
    std::vector<size_t> firstColumn(project->Projections().size());
    for (size_t i = 0, offset = 0; i < firstColumn.size(); ++i) {
        firstColumn[i] = offset;
        offset += FlattenedProjectionArity(project->Projections()[i]);
        if (i >= selected) {
            output.Labels[firstColumn[i]].clear();
        }
    }
    auto columns = ColumnNames(project);
    std::set<std::string> taken;
    for (size_t i = 0; i < columns.size(); ++i) {
        if (!output.Labels[i].empty() && !taken.insert(output.Labels[i]).second) {
            columns[i] = names.Next("__scope");
        }
    }
    if (auto renamed = RenameOutputs(project, columns, context.Annotation); !renamed) {
        return std::unexpected(renamed.error());
    }
    const bool hidden = selected != firstColumn.size();
    if (distinct) {
        if (hidden) {
            const auto& item = *std::ranges::find_if(order, [](const TOrderItem& item) { return item.Hidden != nullptr; });
            return std::unexpected(TError(item.Hidden->Location,
                "for SELECT DISTINCT, ORDER BY expressions must appear in the select list"));
        }
        // Grouping on every output column deduplicates the rows.
        output.Plan = std::make_shared<TAggregateOperator>(std::move(output.Plan), columns,
            std::vector<TAggregateSpec>{});
    }
    if (order.empty()) {
        return output;
    }
    std::vector<TSortKey> keys;
    for (const auto& item : order) {
        keys.push_back({
            .Column = columns[firstColumn[*item.Output]],
            .Direction = item.Item->Desc ? ESortDirection::Desc : ESortDirection::Asc,
            .Nulls = ConvertSortNulls(item.Item->NullOrder),
        });
    }
    output.Plan = std::make_shared<TSortOperator>(std::move(output.Plan), std::move(keys));
    if (!hidden) {
        return output;
    }
    std::vector<TProjectionSpec> visible;
    std::vector<std::string> labels;
    for (size_t i = 0; i < output.Labels.size(); ++i) {
        if (!output.Labels[i].empty()) {
            visible.push_back({.Name = columns[i], .Expression = Ident({}, columns[i])});
            labels.push_back(output.Labels[i]);
        }
    }
    output.Plan = std::make_shared<TProjectOperator>(std::move(output.Plan), std::move(visible));
    output.Labels = std::move(labels);
    if (auto annotated = Annotate(output.Plan, context.Annotation); !annotated) {
        return std::unexpected(annotated.error());
    }
    return output;
}

// Translates one SELECT block in the order of the canonical translation
// (section 3.4): FROM, WHERE, GROUP BY, HAVING, SELECT, ORDER BY. Each clause
// applies to the block's tree, and its subqueries attach to the tree it sees.
std::expected<TRelation, TError> BuildSelect(
    const NSql::TSqlSelect& select,
    const NSql::TSqlOrder* orderBy,
    TBlock& block,
    TContext& context)
{
    if (!select.From) {
        return std::unexpected(TError(SelectLocation(select), "SELECT without FROM is not supported yet"));
    }
    if (!select.SelectList || select.SelectList->Items.empty()) {
        return std::unexpected(TError("empty select list"));
    }
    if (auto from = BuildFrom(*select.From, block, context); !from) {
        return std::unexpected(from.error());
    }
    if (select.Where) {
        auto predicate = TExprTranslator(block, context, true, true).Translate(select.Where);
        if (!predicate) {
            return std::unexpected(predicate.error());
        }
        if (auto aggregate = FindNode(*predicate, IsAggregateCall)) {
            return std::unexpected(TError(aggregate->Location, "aggregate functions are not allowed in WHERE"));
        }
        block.Tree.Plan = std::make_shared<TFilterOperator>(std::move(block.Tree.Plan), std::move(*predicate));
    }

    // Generated names avoid the input columns and the SELECT aliases.
    TFreshNames names;
    for (const auto& column : ColumnNames(block.Tree.Plan)) {
        names.Reserve(column);
        names.Reserve(BareColumnName(column));
    }
    for (const auto& item : select.SelectList->Items) {
        if (item->Alias) {
            names.Reserve(*item->Alias);
        }
    }
    auto grouping = BuildGrouping(select, block, names, context);
    if (!grouping) {
        return std::unexpected(grouping.error());
    }
    // SELECT, HAVING and ORDER BY bind against FROM and WHERE.
    auto projections = BindSelectList(select, block, context);
    if (!projections) {
        return std::unexpected(projections.error());
    }
    NAst::TExprPtr having = select.Having;
    if (having) {
        auto bound = TExprTranslator(block, context, true, false).Translate(having);
        if (!bound) {
            return std::unexpected(bound.error());
        }
        having = std::move(*bound);
    }
    auto order = BindOrderBy(orderBy, *projections, block, context);
    if (!order) {
        return std::unexpected(order.error());
    }

    std::vector<NAst::TExprPtr*> clauses;
    for (auto& projection : *projections) {
        clauses.push_back(&projection.Expression);
    }
    if (having) {
        clauses.push_back(&having);
    }
    for (auto& item : *order) {
        if (item.Hidden) {
            clauses.push_back(&item.Hidden);
        }
    }
    if (auto aggregate = Aggregate(block, *grouping, clauses, names, context.Annotation); !aggregate) {
        return std::unexpected(aggregate.error());
    }
    if (having && !block.Ungrouped) {
        return std::unexpected(TError(having->Location, "HAVING requires aggregation"));
    }

    // The remaining subqueries of HAVING and SELECT attach above the aggregate.
    TExprTranslator above(block, context, false, true);
    if (having) {
        auto predicate = above.Translate(having);
        if (!predicate) {
            return std::unexpected(predicate.error());
        }
        block.Tree.Plan = std::make_shared<TFilterOperator>(std::move(block.Tree.Plan), std::move(*predicate));
    }
    for (auto& projection : *projections) {
        auto translated = above.Translate(projection.Expression);
        if (!translated) {
            return std::unexpected(translated.error());
        }
        projection.Expression = std::move(*translated);
    }
    return ProjectOutputs(block.Tree.Plan, std::move(*projections), std::move(*order),
        select.Quantifier == NSql::ESetQuantifier::Distinct, names, context);
}

std::expected<int64_t, TError> ConstNonNegativeI64(const NAst::TExprPtr& expr, std::string_view clause) {
    auto maybeNumber = NAst::TMaybeNode<NAst::TNumberExpr>(expr);
    if (!maybeNumber) {
        return std::unexpected(TError(expr->Location, std::string(clause) + " currently requires an integer literal"));
    }
    auto number = maybeNumber.Cast();
    if (number->IsFloat()) {
        return std::unexpected(TError(expr->Location, std::string(clause) + " currently requires an integer literal"));
    }
    if (number->IntValue < 0) {
        return std::unexpected(TError(expr->Location, std::string(clause) + " must be non-negative"));
    }
    return number->IntValue;
}

std::expected<TRelation, TError> ApplyLimit(const NSql::TSqlQuery& query, TRelation relation) {
    if (!query.Limit && !query.Offset) {
        return relation;
    }
    if (!query.Limit) {
        return std::unexpected(TError(query.Offset->Location, "OFFSET without LIMIT is not supported yet"));
    }
    auto limit = ConstNonNegativeI64(query.Limit, "LIMIT");
    if (!limit) {
        return std::unexpected(limit.error());
    }
    int64_t offset = 0;
    if (query.Offset) {
        auto value = ConstNonNegativeI64(query.Offset, "OFFSET");
        if (!value) {
            return std::unexpected(value.error());
        }
        offset = *value;
    }
    relation.Plan = std::make_shared<TLimitOperator>(std::move(relation.Plan), *limit, offset);
    return relation;
}

// ORDER BY over a parenthesized query reads its output columns.
std::expected<TRelation, TError> OrderOutputs(const NSql::TSqlOrder& orderBy, TRelation relation) {
    const auto columns = ColumnNames(relation.Plan);
    std::vector<TSortKey> keys;
    for (const auto& item : orderBy.Items) {
        std::optional<size_t> index;
        if (auto position = SelectPosition(item->Expr); position && *position >= 1
            && *position <= static_cast<int64_t>(columns.size()))
        {
            index = *position - 1;
        } else if (auto ident = NAst::TMaybeNode<NAst::TIdentExpr>(item->Expr)) {
            auto found = FindColumn(relation, ident.Cast()->Name, item->Expr->Location);
            if (!found) {
                return std::unexpected(found.error());
            }
            index = *found;
        }
        if (!index) {
            return std::unexpected(TError(item->Expr->Location,
                "ORDER BY over a parenthesized query must name one of its output columns"));
        }
        keys.push_back({
            .Column = columns[*index],
            .Direction = item->Desc ? ESortDirection::Desc : ESortDirection::Asc,
            .Nulls = ConvertSortNulls(item->NullOrder),
        });
    }
    relation.Plan = std::make_shared<TSortOperator>(std::move(relation.Plan), std::move(keys));
    return relation;
}

} // namespace

std::expected<TRelation, TError> BuildQuery(const NSql::TSqlQuery& query, TBlock& block, TContext& context) {
    TCteScope ctes{.Parent = context.Ctes, .Enclosing = block.Parent};
    if (query.WithClause) {
        if (query.WithClause->Recursive) {
            return std::unexpected(TError("WITH RECURSIVE is not supported yet"));
        }
        for (const auto& cte : query.WithClause->Ctes) {
            ctes.Definitions[ToLower(cte->Name)] = cte;
        }
        context.Ctes = &ctes;
    }
    std::expected<TRelation, TError> relation = std::unexpected(TError("unsupported query body"));
    if (auto select = NSql::TMaybeNode<NSql::TSqlSelect>(query.Body)) {
        relation = BuildSelect(*select.Cast(), query.OrderBy.get(), block, context);
    } else if (auto inner = NSql::TMaybeNode<NSql::TSqlQuery>(query.Body)) {
        relation = BuildQuery(*inner.Cast(), block, context);
        if (relation && query.OrderBy) {
            relation = OrderOutputs(*query.OrderBy, std::move(*relation));
        }
    } else if (NSql::TMaybeNode<NSql::TSqlSetOp>(query.Body)) {
        relation = std::unexpected(TError(BodyLocation(query.Body),
            "UNION, INTERSECT and EXCEPT are not supported by the new builder yet"));
    }
    context.Ctes = ctes.Parent;
    if (!relation) {
        return relation;
    }
    return ApplyLimit(query, std::move(*relation));
}

std::expected<TOperatorPtr, TError> BuildCanonicalPlan(
    const NQdb::NSql::TSqlNodePtr& query,
    const TTableSourceFactory& sources,
    const NKernel::TAnnotationContext& annotation)
{
    auto root = NSql::TMaybeNode<NSql::TSqlQuery>(query);
    if (!root) {
        return std::unexpected(TError("expected a query"));
    }
    TContext context{.Sources = sources, .Annotation = annotation};
    TBlock block{.Id = context.NextScope++};
    auto relation = BuildQuery(*root.Cast(), block, context);
    if (!relation) {
        return std::unexpected(relation.error());
    }
    // The query's output columns carry their SQL names, repeated ones included.
    auto plan = RenameOutputs(relation->Plan, relation->Labels, context.Annotation);
    if (!plan) {
        return plan;
    }
    AssignCteIds(*plan);
    return plan;
}

} // namespace NBuild
} // namespace NQdb
