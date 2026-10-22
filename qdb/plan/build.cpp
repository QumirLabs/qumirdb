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

// The qualifier a FROM item answers to: its alias, or a table's own name.
std::optional<std::string> ItemQualifier(const NSql::TSqlPtr<NSql::TSqlTableRef>& ref) {
    if (auto table = NSql::TMaybeNode<NSql::TSqlTableName>(ref); table && !table.Cast()->Alias) {
        return table.Cast()->Name.back();
    }
    return ref->Alias;
}

// A WITH clause's CTEs by lower-cased name, the key table references look them up by.
std::map<std::string, NSql::TSqlPtr<NSql::TSqlCte>> CteMap(const NSql::TSqlWithClause& with) {
    std::map<std::string, NSql::TSqlPtr<NSql::TSqlCte>> ctes;
    for (const auto& cte : with.Ctes) {
        ctes[ToLower(cte->Name)] = cte;
    }
    return ctes;
}

// A bare integer in GROUP BY or ORDER BY is the 1-based position of a SELECT
// output. A negated one is a position too, which is out of range. TRUE and FALSE
// are numbers of another type and stay constants, as does any other expression,
// such as 1 + 0.
std::optional<int64_t> SelectPosition(const NAst::TExprPtr& expr) {
    auto operand = expr;
    int64_t sign = 1;
    if (auto unary = NAst::TMaybeNode<NAst::TUnaryExpr>(expr);
        unary && unary.Cast()->Operator == NAst::TOperator("-"))
    {
        operand = unary.Cast()->Operand;
        sign = -1;
    }
    auto number = NAst::TMaybeNode<NAst::TNumberExpr>(operand);
    if (!number || !NAst::TMaybeType<NAst::TIntegerType>(number.Cast()->Type)) {
        return std::nullopt;
    }
    return sign * number.Cast()->IntValue;
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

// Bound GROUP BY keys that read the same input column, e.g. a position and the
// column's name, are one key, listed once by each grouping set, as ParseGrouping
// does for keys of equal text. Computed keys stay apart: HAVING and GROUPING()
// may refer to each by its own name.
void MergeColumnKeys(std::vector<TGroupKey>& keys, std::vector<std::vector<size_t>>& sets) {
    std::vector<TGroupKey> merged;
    std::vector<size_t> index;
    std::unordered_map<std::string, size_t> columns;
    for (auto& key : keys) {
        if (!key.Computed) {
            if (auto [column, inserted] = columns.emplace(key.Name, merged.size()); !inserted) {
                index.push_back(column->second);
                continue;
            }
        }
        index.push_back(merged.size());
        merged.push_back(std::move(key));
    }
    for (auto& set : sets) {
        std::vector<size_t> remapped;
        for (const auto key : set) {
            if (std::ranges::find(remapped, index[key]) == remapped.end()) {
                remapped.push_back(index[key]);
            }
        }
        set = std::move(remapped);
    }
    keys = std::move(merged);
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

// The SELECT outputs that GROUP BY positions count: the projections after the
// leading helper ones, and the output columns each of them has.
struct TPositionedOutputs {
    size_t Helpers = 0;
    std::vector<size_t> Widths;
};

// GROUP BY resolves its positions before the aggregate that the SELECT
// projections read exists, so it types them over the input columns and the
// aggregate columns. A projection that also reads a window, subquery, GROUPING()
// or GROUP BY alias result is typed only above the aggregate. Those results are
// single values, so it counts as one column, which BuildSelect checks once it is
// typed.
std::expected<TPositionedOutputs, TError> PositionedOutputs(
    const std::vector<TProjectionSpec>& projections,
    size_t helpers,
    const TAggCollector& collector,
    const NAst::TStructType* inputSchema,
    const NKernel::TAnnotationContext& annotation)
{
    auto slots = collector.SlotTypes();
    if (!slots) {
        return std::unexpected(slots.error());
    }
    std::vector<std::pair<std::string, NAst::TTypePtr>> fields;
    if (inputSchema) {
        fields = inputSchema->Fields;
    }
    fields.insert(fields.end(), slots->begin(), slots->end());
    std::unordered_set<std::string> columns;
    for (const auto& [name, _] : fields) {
        columns.insert(name);
    }
    const NAst::TStructType schema(std::move(fields));

    TPositionedOutputs outputs{.Helpers = helpers};
    for (size_t i = helpers; i < projections.size(); ++i) {
        const auto& projection = projections[i];
        const auto& expression = projection.Expression;
        const bool typedAbove = HasWindowExprLocal(expression) || HasSubquery(expression)
            || HasGroupingCall(expression)
            || !std::ranges::all_of(FindUnboundVars(expression), [&](const auto& name) {
                return columns.contains(name);
            });
        size_t width = std::max<size_t>(projection.ColumnAliases.size(), 1);
        if (projection.ColumnAliases.empty() && !typedAbove) {
            try {
                width = FlattenedArity(NKernel::AnnotateExprType(expression, schema, &annotation));
            } catch (const TError& error) {
                return std::unexpected(error);
            }
        }
        outputs.Widths.push_back(width);
    }
    return outputs;
}

// The SELECT projection whose output a GROUP BY position names.
std::expected<const TProjectionSpec*, TError> PositionedProjection(
    const std::vector<TProjectionSpec>& projections,
    const TPositionedOutputs& outputs,
    int64_t position,
    const TLocation& location)
{
    const auto reference = "GROUP BY position " + std::to_string(position);
    int64_t first = 1;
    for (size_t i = 0; i < outputs.Widths.size(); ++i) {
        const auto width = static_cast<int64_t>(outputs.Widths[i]);
        if (position >= first && position < first + width) {
            if (width > 1) {
                return std::unexpected(TError(location, reference + " refers to a field of a struct item"));
            }
            return &projections[outputs.Helpers + i];
        }
        first += width;
    }
    return std::unexpected(TError(location, reference + " is not in the select list"));
}

// A built relation: a typed plan and the SQL label of each output column. Labels
// are the names SQL references see, qualified by a FROM alias where one applies;
// an empty label hides a helper column from SQL. Physical names only need to be
// unique where relations meet.
struct TRelation {
    TOperatorPtr Plan;
    std::vector<std::string> Labels;
};

// Tables by name: base tables, and the CTEs in scope with their labels.
using TRelationSources = std::function<std::expected<TRelation, TError>(std::string_view table)>;

struct TBuildContext {
    TRelationSources Sources;
    const NKernel::TAnnotationContext& Annotation;
    // Source aliases taken in the CTE definition or main query being built.
    std::unordered_set<std::string>& SourceAliases;
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

// The SQL names an expression sees: the labels of its inputs, each by its
// qualified name and by its bare column part.
class TNameScope {
public:
    // Resolves a reference that no input matches, or returns false to report it.
    using TFallback = std::function<std::expected<bool, TError>(NAst::TExprPtr& reference)>;

    TNameScope(std::initializer_list<std::reference_wrapper<const TRelation>> inputs) {
        for (const TRelation& input : inputs) {
            const auto columns = ColumnNames(input.Plan);
            for (size_t i = 0; i < input.Labels.size(); ++i) {
                const auto& label = input.Labels[i];
                if (label.empty()) {
                    continue;
                }
                Add(label, columns[i]);
                if (label.find('.') != std::string::npos) {
                    Add(BareColumnName(label), columns[i]);
                }
            }
        }
    }

    // The physical column of `name`, or nullopt when no input has it.
    std::expected<std::optional<std::string>, TError> Find(
        const std::string& name,
        const TLocation& location) const
    {
        auto found = Columns_.find(name);
        if (found == Columns_.end()) {
            return std::nullopt;
        }
        if (!found->second) {
            return std::unexpected(TError(location, "ambiguous input column: " + name));
        }
        return found->second;
    }

    // Rewrites an identifier to its physical column; false when no input has it.
    std::expected<bool, TError> Resolve(NAst::TExprPtr& reference) const {
        auto ident = NAst::TMaybeNode<NAst::TIdentExpr>(reference).Cast();
        auto column = Find(ident->Name, reference->Location);
        if (!column) {
            return std::unexpected(column.error());
        }
        if (*column) {
            ident->Name = **column;
        }
        return column->has_value();
    }

    // Rewrites the column references of `expression` to physical columns, in
    // place. A name no input has is an error unless `fallback` resolves it.
    std::expected<void, TError> Bind(NAst::TExprPtr& expression, const TFallback& fallback = {}) const {
        // The parser shares subtrees, e.g. the operand of BETWEEN; each binds once.
        std::unordered_set<const NAst::TExpr*> visited;
        // Names declared by enclosing blocks: the operand of a CASE with a computed operand.
        std::vector<std::string> locals;
        auto bind = [&](const auto& self, NAst::TExprPtr& node) -> std::expected<void, TError> {
            if (!node || !visited.insert(node.get()).second) {
                return {};
            }
            if (auto ident = NAst::TMaybeNode<NAst::TIdentExpr>(node)) {
                const auto name = ident.Cast()->Name;
                // count(*) takes a synthetic `*` argument.
                if (name == "*" || std::ranges::find(locals, name) != locals.end()) {
                    return {};
                }
                auto resolved = Resolve(node);
                if (resolved && !*resolved && fallback) {
                    resolved = fallback(node);
                }
                if (!resolved) {
                    return std::unexpected(resolved.error());
                }
                if (!*resolved) {
                    return std::unexpected(TError(node->Location, "unknown input column: " + name));
                }
                return {};
            }
            // A call's callee names a function, not a column.
            if (auto call = NAst::TMaybeNode<NAst::TCallExpr>(node)) {
                for (auto& argument : call.Cast()->Args) {
                    if (auto bound = self(self, argument); !bound) {
                        return bound;
                    }
                }
                return {};
            }
            if (auto block = NAst::TMaybeNode<NAst::TBlockExpr>(node)) {
                const auto depth = locals.size();
                for (auto& statement : block.Cast()->Stmts) {
                    auto var = NAst::TMaybeNode<NAst::TVarStmt>(statement);
                    auto bound = var
                        ? self(self, var.Cast()->Init)
                        : self(self, statement);
                    if (!bound) {
                        return bound;
                    }
                    if (var) {
                        locals.push_back(var.Cast()->Name);
                    }
                }
                locals.resize(depth);
                return {};
            }
            for (auto* child : node->MutableChildren()) {
                if (auto bound = self(self, *child); !bound) {
                    return bound;
                }
            }
            return {};
        };
        return bind(bind, expression);
    }

private:
    void Add(const std::string& name, const std::string& column) {
        if (auto [it, inserted] = Columns_.emplace(name, column); !inserted) {
            it->second.reset();
        }
    }

    // nullopt marks a name that several input columns share.
    std::unordered_map<std::string, std::optional<std::string>> Columns_;
};

// The physical column of a name that must exist in `input`.
std::expected<std::string, TError> BindColumn(const std::string& name, const TRelation& input) {
    auto column = TNameScope({input}).Find(name, {});
    if (!column) {
        return std::unexpected(column.error());
    }
    if (!*column) {
        return std::unexpected(TError("unknown input column: " + name));
    }
    return **column;
}

// Renames the output columns of a plan. The top projection belongs to the plan
// being built alone, so it is renamed in place; another plan gets a renaming
// projection.
std::expected<TOperatorPtr, TError> RenameOutputs(
    TOperatorPtr plan,
    const std::vector<std::string>& names,
    const TBuildContext& context)
{
    const auto columns = ColumnNames(plan);
    if (columns == names) {
        return plan;
    }
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
        std::vector<TProjectionSpec> renamed;
        for (size_t i = 0; i < columns.size(); ++i) {
            renamed.push_back({.Name = names[i], .Expression = Ident({}, columns[i])});
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
    const TBuildContext& context)
{
    if (labels.size() != ColumnNames(relation.Plan).size()) {
        return std::unexpected(TError("column alias count does not match output columns"));
    }
    auto names = labels;
    if (std::unordered_set<std::string>(names.begin(), names.end()).size() != names.size()) {
        TFreshNames fresh;
        for (auto& name : names) {
            name = fresh.Next("__scope");
        }
    }
    auto plan = RenameOutputs(std::move(relation.Plan), names, context);
    if (!plan) {
        return std::unexpected(plan.error());
    }
    return TRelation{.Plan = std::move(*plan), .Labels = std::move(labels)};
}

// Renames a built sub-plan's output columns to the given list (CTE column lists
// and derived-table column aliases).
std::expected<TRelation, TError> ApplyColumnAliases(
    TRelation relation,
    const std::vector<std::string>& aliases,
    const TBuildContext& context)
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
    const TBuildContext& context)
{
    std::vector<std::string> labels;
    for (const auto& label : relation.Labels) {
        labels.push_back(alias + "." + BareColumnName(label));
    }
    return Relabel(std::move(relation), std::move(labels), context);
}

// FROM items and subqueries may expose equal physical names, for example two
// unaliased derived tables. The right side gets other names; labels stay.
std::expected<TRelation, TError> SeparateRight(
    const TRelation& left,
    TRelation right,
    const TBuildContext& context)
{
    const auto leftNames = ColumnNames(left.Plan);
    const std::unordered_set<std::string> taken(leftNames.begin(), leftNames.end());
    auto names = ColumnNames(right.Plan);
    if (std::ranges::none_of(names, [&](const auto& name) { return taken.contains(name); })) {
        return right;
    }
    TFreshNames fresh;
    for (const auto& name : leftNames) {
        fresh.Reserve(name);
    }
    for (const auto& name : names) {
        fresh.Reserve(name);
    }
    for (auto& name : names) {
        if (taken.contains(name)) {
            name = fresh.Next("__scope");
        }
    }
    auto plan = RenameOutputs(std::move(right.Plan), names, context);
    if (!plan) {
        return std::unexpected(plan.error());
    }
    right.Plan = std::move(*plan);
    return right;
}

// A subquery's outputs under internal names, hidden from SQL, ready to join onto
// `node`.
std::expected<TRelation, TError> SubqueryOutputs(
    const TRelation& node,
    TRelation subquery,
    std::vector<std::string> names,
    const TBuildContext& context)
{
    auto renamed = Relabel(std::move(subquery), std::move(names), context);
    if (!renamed) {
        return renamed;
    }
    std::ranges::fill(renamed->Labels, std::string());
    return SeparateRight(node, std::move(*renamed), context);
}

// The labels of a plan built over `input`: a column that keeps the name of an
// input column keeps its label, any other column is a hidden helper.
TRelation CarryLabels(const TRelation& input, TOperatorPtr plan) {
    std::unordered_map<std::string, std::string> labels;
    const auto columns = ColumnNames(input.Plan);
    for (size_t i = 0; i < columns.size(); ++i) {
        labels.emplace(columns[i], input.Labels[i]);
    }
    TRelation relation{.Plan = std::move(plan)};
    for (const auto& name : ColumnNames(relation.Plan)) {
        auto label = labels.find(name);
        relation.Labels.push_back(label == labels.end() ? std::string() : label->second);
    }
    return relation;
}

// SELECT DISTINCT: dedup by grouping on every output column. ORDER BY extends the
// pass-through project on top with hidden sort columns over the distinct rows.
std::expected<TRelation, TError> ApplyDistinct(
    TRelation projected,
    const std::vector<std::string>& columns,
    const TBuildContext& context)
{
    auto agg = std::make_shared<TAggregateOperator>(
        std::move(projected.Plan), columns, std::vector<TAggregateSpec>{});
    std::vector<TProjectionSpec> passthrough;
    passthrough.reserve(columns.size());
    for (const auto& col : columns) {
        passthrough.push_back({ .Name = col, .Expression = Ident({}, col) });
    }
    projected.Plan = std::make_shared<TProjectOperator>(std::move(agg), std::move(passthrough));
    if (auto annotated = Annotate(projected.Plan, context); !annotated) {
        return std::unexpected(annotated.error());
    }
    return projected;
}

// Unaliased sources are named by their table, numbered when the name repeats
// within one CTE definition or the main query.
std::string SourceAlias(const NSql::TSqlTableName& table, std::unordered_set<std::string>& taken) {
    if (table.Alias) {
        taken.insert(*table.Alias);
        return *table.Alias;
    }
    const auto stem = ToLower(table.Name.back());
    auto alias = stem;
    for (size_t i = 1; !taken.insert(alias).second; ++i) {
        alias = stem + "_" + std::to_string(i);
    }
    return alias;
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

struct TSelectOutput;

std::expected<TRelation, TError> BuildQuery(
    const NSql::TSqlQuery& query,
    TBuildContext& context,
    std::optional<TSelectOutput>* output = nullptr);

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
        const auto qualifier = *ItemQualifier(ref);
        if (auto sourceOp = TMaybeOp<TSourceOperator>(source->Plan)) {
            sourceOp.Cast()->SetAlias(SourceAlias(*node, context.SourceAliases));
            source->Labels.clear();
            for (const auto& column : sourceOp.Cast()->GetSource().Schema().Columns) {
                source->Labels.push_back(qualifier + "." + std::string(column.Name));
            }
            return source;
        }
        // A CTE inlined as a subplan: qualify its columns by the reference alias, or -
        // when referenced bare - by the CTE name, so `<cte>.col` references resolve
        // (otherwise the inlined columns stay bare and equi-joins over them drop their
        // keys, producing keyless cross joins).
        return AliasSubplan(std::move(*source), qualifier, context);
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
                if (auto bound = TNameScope({*left, *right}).Bind(residual); !bound) {
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
    if (residual) {
        // The subquery's own columns shadow those of the outer query.
        const TNameScope outer({left});
        auto bound = TNameScope({*right}).Bind(residual, [&](NAst::TExprPtr& reference) {
            return outer.Resolve(reference);
        });
        if (!bound) {
            return std::unexpected(bound.error());
        }
    }
    return JoinRelations(std::move(left), std::move(*right), {}, type, std::move(residual));
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
    const auto columns = ColumnNames(right->Plan);
    if (columns.size() != 1) {
        return std::unexpected(TError("IN subquery must return exactly one column"));
    }
    // Rename the subquery's output to a synthetic name so the residual's two sides
    // never share a name: otherwise `x IN (SELECT x ...)` yields `x == x`, which
    // key extraction drops entirely, leaving a keyless (unschedulable,
    // condition-less) semi-join. The name is internal - a semi/anti join exposes
    // only its left side.
    right = SubqueryOutputs(left, std::move(*right), {"__qdb_in__" + columns.front()}, context);
    if (!right) {
        return std::unexpected(right.error());
    }
    // The operand was bound with the rest of the WHERE clause.
    auto column = Ident(subquery.Operand->Location, ColumnNames(right->Plan).front());
    auto residual = std::make_shared<NAst::TBinaryExpr>(
        subquery.Operand->Location, NAst::TOperator("=="), subquery.Operand, std::move(column));
    return JoinRelations(std::move(left), std::move(*right), {}, type, std::move(residual));
}

// The names a subquery's FROM makes visible. A qualified reference is local iff its
// qualifier names a FROM item, whatever the item's columns are; a bare one iff a
// FROM item has a column of that name, so inner names shadow outer ones. Every
// other reference is outer.
struct TLocalSqlScope {
    std::unordered_set<std::string> Qualifiers;
    std::unordered_set<std::string> BareColumns;

    bool IsLocal(const std::string& name) const {
        const auto dot = name.rfind('.');
        return dot != std::string::npos
            ? Qualifiers.contains(name.substr(0, dot))
            : BareColumns.contains(name);
    }
};

// Table names visible to a FROM, scoped as BuildQuery scopes them: the CTEs of a
// query's own WITH shadow those of enclosing queries, which shadow `Sources`.
struct TSqlTableScope {
    const TRelationSources& Sources;
    const TSqlTableScope* Parent = nullptr;
    std::map<std::string, NSql::TSqlPtr<NSql::TSqlCte>> Ctes; // by lower-cased name
};

TSqlTableScope WithCtes(const TSqlTableScope& parent, const NSql::TSqlWithClause& with) {
    return {.Sources = parent.Sources, .Parent = &parent, .Ctes = CteMap(with)};
}

std::expected<void, TError> CollectLocalColumns(
    const NSql::TSqlPtr<NSql::TSqlTableRef>& ref,
    const TSqlTableScope& tables,
    TLocalSqlScope& out);

// The columns `qualifier.*` expands to: those of the FROM items it names.
std::expected<void, TError> CollectQualifiedColumns(
    const NSql::TSqlPtr<NSql::TSqlTableRef>& ref,
    const std::string& qualifier,
    const TSqlTableScope& tables,
    TLocalSqlScope& out)
{
    if (auto join = NSql::TMaybeNode<NSql::TSqlJoin>(ref)) {
        auto left = CollectQualifiedColumns(join.Cast()->Left, qualifier, tables, out);
        if (!left) {
            return left;
        }
        return CollectQualifiedColumns(join.Cast()->Right, qualifier, tables, out);
    }
    if (ItemQualifier(ref) == qualifier) {
        return CollectLocalColumns(ref, tables, out);
    }
    return {};
}

// Output names of a FROM subquery as the builder assigns them (ItemName). A star
// exposes the bare columns of the FROM items it expands.
std::expected<void, TError> CollectOutputNames(
    const NSql::TSqlNodePtr& body,
    const TSqlTableScope& tables,
    TLocalSqlScope& out)
{
    if (auto query = NSql::TMaybeNode<NSql::TSqlQuery>(body)) {
        if (const auto& with = query.Cast()->WithClause) {
            return CollectOutputNames(query.Cast()->Body, WithCtes(tables, *with), out);
        }
        return CollectOutputNames(query.Cast()->Body, tables, out);
    }
    if (auto set = NSql::TMaybeNode<NSql::TSqlSetOp>(body)) {
        // A set operation is named by its first input.
        return CollectOutputNames(set.Cast()->Left, tables, out);
    }
    if (auto select = NSql::TMaybeNode<NSql::TSqlSelect>(body)) {
        const auto& items = select.Cast()->SelectList->Items;
        for (size_t i = 0; i < items.size(); ++i) {
            const auto& item = *items[i];
            if (!item.Star) {
                if (item.ColumnAliases.empty()) {
                    out.BareColumns.insert(ItemName(item, i));
                } else {
                    out.BareColumns.insert(item.ColumnAliases.begin(), item.ColumnAliases.end());
                }
                continue;
            }
            if (!select.Cast()->From) {
                continue; // BuildSelect rejects a SELECT without FROM.
            }
            TLocalSqlScope expanded;
            for (const auto& ref : select.Cast()->From->Items) {
                auto collected = item.StarPrefix.empty()
                    ? CollectLocalColumns(ref, tables, expanded)
                    : CollectQualifiedColumns(ref, TableName(item.StarPrefix), tables, expanded);
                if (!collected) {
                    return collected;
                }
            }
            out.BareColumns.insert(expanded.BareColumns.begin(), expanded.BareColumns.end());
        }
    }
    return {};
}

// The scope of a subquery's FROM item: base tables, CTEs and derived tables,
// recursing into joins. Used to tell a subquery's local columns from outer
// (correlated) ones.
std::expected<void, TError> CollectLocalColumns(
    const NSql::TSqlPtr<NSql::TSqlTableRef>& ref,
    const TSqlTableScope& tables,
    TLocalSqlScope& out)
{
    if (auto join = NSql::TMaybeNode<NSql::TSqlJoin>(ref)) {
        auto left = CollectLocalColumns(join.Cast()->Left, tables, out);
        if (!left) {
            return left;
        }
        return CollectLocalColumns(join.Cast()->Right, tables, out);
    }
    // The alias (or the name) distinguishes a local column of `item j`
    // (`j.i_category`) from an outer column of another alias of the same table.
    if (auto qualifier = ItemQualifier(ref)) {
        out.Qualifiers.insert(*qualifier);
    }
    if (auto table = NSql::TMaybeNode<NSql::TSqlTableName>(ref)) {
        const std::string name = TableName(table.Cast()->Name);
        const std::string key = ToLower(name);
        for (const auto* scope = &tables; scope; scope = scope->Parent) {
            if (auto cte = scope->Ctes.find(key); cte != scope->Ctes.end()) {
                if (const auto& columns = cte->second->Columns) {
                    out.BareColumns.insert(columns->Items.begin(), columns->Items.end());
                    return {};
                }
                // As in BuildQuery, a CTE body sees the WITH that declares it.
                return CollectOutputNames(cte->second->Query, *scope, out);
            }
        }
        auto src = tables.Sources(name);
        if (!src) {
            return std::unexpected(src.error());
        }
        if (!src->Plan->OutputColumns()) {
            return std::unexpected(TError("subquery source '" + name + "' has unresolved columns"));
        }
        // A CTE of an enclosing query exposes its labels, not its physical names.
        for (const auto& label : src->Labels) {
            out.BareColumns.insert(BareColumnName(label));
        }
    } else if (auto derived = NSql::TMaybeNode<NSql::TSqlSubqueryTable>(ref)) {
        if (derived.Cast()->ColumnAliases) {
            const auto& columns = derived.Cast()->ColumnAliases->Items;
            out.BareColumns.insert(columns.begin(), columns.end());
        } else {
            return CollectOutputNames(derived.Cast()->Query, tables, out);
        }
    }
    return {};
}

// The scope of the FROM of a subquery, with the subquery's own WITH in effect.
std::expected<TLocalSqlScope, TError> CollectSubqueryScope(
    const NSql::TSqlQuery& query,
    const NSql::TSqlFrom& from,
    const TRelationSources& sources)
{
    const TSqlTableScope enclosing{.Sources = sources};
    const auto tables = query.WithClause
        ? WithCtes(enclosing, *query.WithClause)
        : enclosing;
    TLocalSqlScope local;
    for (const auto& item : from.Items) {
        if (auto collected = CollectLocalColumns(item, tables, local); !collected) {
            return std::unexpected(collected.error());
        }
    }
    return local;
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
            bool lLocal = local.IsLocal(ln);
            bool rLocal = local.IsLocal(rn);
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

// Replaces identifier leaves named in `replacements` with a copy of their
// expression, placed at the identifier's location. A call's callee names a
// function, not a column, so it is left alone.
void ReplaceIdents(
    NAst::TExprPtr& expr,
    const std::unordered_map<std::string, NAst::TExprPtr>& replacements)
{
    if (!expr) {
        return;
    }
    if (auto ident = NAst::TMaybeNode<NAst::TIdentExpr>(expr)) {
        if (auto it = replacements.find(ident.Cast()->Name); it != replacements.end()) {
            const auto location = expr->Location;
            expr = CloneExpr(it->second);
            expr->Location = location;
        }
        return;
    }
    if (auto call = NAst::TMaybeNode<NAst::TCallExpr>(expr)) {
        for (auto& arg : call.Cast()->Args) {
            ReplaceIdents(arg, replacements);
        }
        return;
    }
    for (auto* child : expr->MutableChildren()) {
        ReplaceIdents(*child, replacements);
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
        // A branch is built from its FROM alone, as in DecorrelateExists, so it cannot
        // carry its own WITH.
        if (sub->Query->WithClause || select->GroupBy || select->Having || !select->From
            || !select->Where || HasSubquery(select->Where))
        {
            return std::optional<TRelation>{};
        }
        auto local = CollectSubqueryScope(*sub->Query, *select->From, context.Sources);
        if (!local) {
            return std::unexpected(local.error());
        }
        std::vector<std::pair<std::string, std::string>> corrs;
        CollectCorrelations(select->Where, *local, corrs);
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
        ReplaceIdents(where, {{outer, Ident({}, inner)}}); // correlation -> inner=inner (tautology)
        NAst::TExprPtr key = Ident(sub->Location, inner);
        const TNameScope scope({*body});
        for (auto* expression : {&where, &key}) {
            if (auto bound = scope.Bind(*expression); !bound) {
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
        if (!local.IsLocal(name)) {
            return true;
        }
    }
    return false;
}

// The scalar and IN subqueries extracted from the expressions of one SELECT, whose
// columns are numbered apart. The parser shares the operand of BETWEEN between two
// comparisons, so each subquery is extracted once and referenced by both.
struct TExtractedSubqueries {
    int Counter = 0;
    std::unordered_map<const NSql::TSubqueryExpr*, NAst::TExprPtr> Columns;
};

// Replaces each scalar subquery in a bound expression with a reference to a fresh
// column. Uncorrelated subqueries broadcast via a cross join (a single row).
// Correlated ones are decorrelated: the subquery is grouped by its correlation
// columns and LEFT-joined onto `node` (Apply elimination).
std::expected<NAst::TExprPtr, TError> ExtractScalarSubqueries(
    NAst::TExprPtr expr,
    TRelation& node,
    TBuildContext& context,
    TExtractedSubqueries& extracted)
{
    if (!expr) {
        return expr;
    }
    if (auto sub = NAst::TMaybeNode<NSql::TSubqueryExpr>(expr)) {
        auto scalar = sub.Cast();
        if (auto found = extracted.Columns.find(scalar.get()); found != extracted.Columns.end()) {
            return CloneExpr(found->second);
        }
        auto extract = [&](NAst::TExprPtr replacement) {
            extracted.Columns.emplace(scalar.get(), CloneExpr(replacement));
            return replacement;
        };
        if (scalar->Kind == NSql::TSubqueryExpr::EKind::In) {
            // An uncorrelated IN-subquery nested inside a boolean expression (e.g. one
            // side of an OR) cannot be a semi-join - that would drop rows the OR keeps.
            // Rewrite it as a mark join: LEFT-join `node` to the DISTINCT subquery on
            // the IN key, then replace `x IN (subquery)` with `marker IS NOT NULL`.
            if (!scalar->Operand || HasSubquery(scalar->Operand)) {
                return std::unexpected(TError("unsupported IN operand"));
            }
            // Only uncorrelated IN is supported here: a correlated body would leave
            // outer columns unbound in the standalone subquery plan.
            if (auto sel = NSql::TMaybeNode<NSql::TSqlSelect>(scalar->Query->Body);
                sel && sel.Cast()->From && sel.Cast()->Where)
            {
                auto local = CollectSubqueryScope(*scalar->Query, *sel.Cast()->From, context.Sources);
                if (!local) {
                    return std::unexpected(local.error());
                }
                if (HasUnmappedOuter(sel.Cast()->Where, *local)) {
                    return std::unexpected(TError(
                        "correlated IN subquery in a boolean expression is not supported"));
                }
            }
            auto plan = BuildQuery(*scalar->Query, context);
            if (!plan) {
                return std::unexpected(plan.error());
            }
            if (ColumnNames(plan->Plan).size() != 1) {
                return std::unexpected(TError("IN subquery must return exactly one column"));
            }
            auto right = SubqueryOutputs(
                node, std::move(*plan), {"__in_mark_" + std::to_string(extracted.Counter++) + "__"}, context);
            if (!right) {
                return std::unexpected(right.error());
            }
            const auto markName = ColumnNames(right->Plan).front();
            // Dedup so each outer row matches at most one right row (no duplication).
            auto distinct = ApplyDistinct(std::move(*right), { markName }, context);
            if (!distinct) {
                return std::unexpected(distinct.error());
            }
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
            node = JoinRelations(std::move(node), std::move(*distinct), std::move(keys),
                EJoinType::Left, std::move(residual));
            // Replace `x IN (subquery)` with `!(qdb_is_null(marker))`.
            return extract(std::make_shared<NAst::TUnaryExpr>(
                expr->Location, NAst::TOperator("!"),
                Call(expr->Location, "qdb_is_null", { Ident(expr->Location, markName) })));
        }
        if (scalar->Kind != NSql::TSubqueryExpr::EKind::Scalar) {
            return std::unexpected(TError("subquery is not supported in this position"));
        }

        auto maybeSelect = NSql::TMaybeNode<NSql::TSqlSelect>(scalar->Query->Body);
        if (maybeSelect && maybeSelect.Cast()->From && maybeSelect.Cast()->Where) {
            auto sel = maybeSelect.Cast();
            auto scope = CollectSubqueryScope(*scalar->Query, *sel->From, context.Sources);
            if (!scope) {
                return std::unexpected(scope.error());
            }
            const auto& local = *scope;

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
                int id = extracted.Counter++;
                if (!sel->GroupBy) {
                    sel->GroupBy = std::make_shared<NSql::TSqlGroupBy>();
                }
                std::vector<NSql::TSqlPtr<NSql::TSqlSelectItem>> prepend;
                std::vector<std::string> outputs;
                std::vector<std::string> outerColumns;
                std::unordered_map<std::string, NAst::TExprPtr> outerToLocal;
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
                    auto outer = BindColumn(outerCol, node);
                    if (!outer) {
                        return std::unexpected(outer.error());
                    }
                    outerColumns.push_back(std::move(*outer));
                    outputs.push_back(std::move(corrName));
                    outerToLocal[outerCol] = Ident({}, localCol);
                }
                sel->SelectList->Items.insert(
                    sel->SelectList->Items.begin(), prepend.begin(), prepend.end());
                sel->SelectList->HelperItems += prepend.size();
                // Rewrite outer refs to their correlated local columns: a correlation
                // buried in a disjunction (`a = out AND P1) OR (a = out AND P2)`)
                // becomes fully local; the correlation itself is enforced by the
                // group-by + join keys above.
                ReplaceIdents(sel->Where, outerToLocal);
                if (HasUnmappedOuter(sel->Where, local)) {
                    return std::unexpected(
                        TError("unsupported correlated subquery (outer column outside an equality)"));
                }

                auto plan = BuildQuery(*scalar->Query, context);
                if (!plan) {
                    return std::unexpected(plan.error());
                }
                if (ColumnNames(plan->Plan).size() != correlation.size() + 1) {
                    return std::unexpected(
                        TError("correlated scalar subquery must return one value"));
                }
                outputs.push_back("__scalar_" + std::to_string(id) + "__");
                auto right = SubqueryOutputs(node, std::move(*plan), std::move(outputs), context);
                if (!right) {
                    return std::unexpected(right.error());
                }
                const auto columns = ColumnNames(right->Plan);
                std::vector<TJoinKey> joinKeys;
                for (size_t k = 0; k < outerColumns.size(); ++k) {
                    joinKeys.push_back({ .Left = outerColumns[k], .Right = columns[k] });
                }
                // LEFT join preserves outer rows; a missing match yields NULL, so
                // the surrounding comparison filters the row out (SQL semantics).
                node = JoinRelations(
                    std::move(node), std::move(*right), std::move(joinKeys), EJoinType::Left, nullptr);
                return extract(Ident(expr->Location, columns.back()));
            }

            // Correlation present but not liftable to an equi-key (a non-equality
            // correlation, or an equality that isn't ident==ident): the outer column
            // would be left unbound in the subquery. Fail loudly rather than silently
            // cross-joining and dropping rows.
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
        if (ColumnNames(plan->Plan).size() != 1) {
            return std::unexpected(TError("scalar subquery must return exactly one column"));
        }
        auto right = SubqueryOutputs(
            node, std::move(*plan), {"__scalar_" + std::to_string(extracted.Counter++) + "__"}, context);
        if (!right) {
            return std::unexpected(right.error());
        }
        const auto column = ColumnNames(right->Plan).front();
        node = JoinRelations(std::move(node), std::move(*right), {}, EJoinType::Inner, nullptr);
        return extract(Ident(expr->Location, column));
    }
    for (NAst::TExprPtr* child : expr->MutableChildren()) {
        auto rewritten = ExtractScalarSubqueries(*child, node, context, extracted);
        if (!rewritten) {
            return std::unexpected(rewritten.error());
        }
        *child = std::move(*rewritten);
    }
    return expr;
}

// Builds the aggregate of an aggregated SELECT over its input `node`: binds the
// GROUP BY keys (a name missing from the input falls back to a SELECT alias) and
// realizes the specs taken from `collector`; with no group keys the aggregate is
// global. `collector` must be built over the output schema of `node`, with affine
// rewriting disabled when `grouping` has several sets, and take its slot names from
// `names`, which also names the materialized arguments and partial aggregates.
// `projections` and `having` are bound against `node`, already reference the
// collector's aggregate columns and are rewritten onto computed group keys and
// GROUPING() values. A GROUP BY position names one of the `positioned` outputs
// of `projections`.
std::expected<TRelation, TError> BuildAggregate(
    TRelation node,
    TAggCollector& collector,
    TFreshNames& names,
    std::optional<TGroupingParse> grouping,
    const std::unordered_map<std::string, NAst::TExprPtr>& selectAliases,
    const std::unordered_set<std::string>& ambiguousSelectAliases,
    std::vector<TProjectionSpec>& projections,
    const TPositionedOutputs& positioned,
    NAst::TExprPtr& having,
    const TBuildContext& context)
{
    std::vector<TGroupKey> groupKeys;
    std::vector<std::vector<size_t>> groupingSets;
    bool hasGroupingSyntax = false;
    if (grouping) {
        groupKeys = std::move(grouping->Keys);
        groupingSets = std::move(grouping->Sets);
        hasGroupingSyntax = grouping->HasGroupingSyntax;
    }
    // A single set over all keys is a plain aggregate; >1 set needs the kernel's
    // grouping-sets machinery (keys become nullable, masked per set).
    const bool multiSet = groupingSets.size() > 1;
    const TRelation input = node;
    const TNameScope inputNames({input});
    const auto inputColumns = ColumnNames(input.Plan);
    const std::unordered_set<std::string> physicalColumns(inputColumns.begin(), inputColumns.end());
    // A GROUP BY alias or position names a SELECT output, whose bound expression
    // becomes the key. It must be a scalar over input columns: any other name in
    // it is an aggregate slot.
    auto selectedKey = [&](TGroupKey& key, const NAst::TExprPtr& selected, const std::string& reference)
        -> std::expected<void, TError>
    {
        const bool scalar = !HasSubquery(selected) && !HasWindowExprLocal(selected) && !HasGroupingCall(selected)
            && std::ranges::all_of(FindUnboundVars(selected), [&](const auto& ref) {
                return physicalColumns.contains(ref);
            });
        if (!scalar) {
            return std::unexpected(TError(
                key.Expression->Location, reference + " must refer to a scalar input expression"));
        }
        auto ident = NAst::TMaybeNode<NAst::TIdentExpr>(selected);
        if (ident) {
            key.Name = ident.Cast()->Name;
        }
        key.Expression = CloneExpr(selected);
        key.Computed = !ident;
        return {};
    };
    for (auto& key : groupKeys) {
        if (auto position = SelectPosition(key.Expression)) {
            auto projection = PositionedProjection(projections, positioned, *position, key.Expression->Location);
            if (!projection) {
                return std::unexpected(projection.error());
            }
            auto selected = selectedKey(
                key, (*projection)->Expression, "GROUP BY position " + std::to_string(*position));
            if (!selected) {
                return std::unexpected(selected.error());
            }
            continue;
        }
        if (key.Computed) {
            if (auto bound = inputNames.Bind(key.Expression); !bound) {
                return std::unexpected(bound.error());
            }
            continue;
        }
        // PostgreSQL GROUP BY resolves an input-column name before a SELECT alias.
        auto column = inputNames.Find(key.Name, key.Expression->Location);
        if (!column) {
            return std::unexpected(column.error());
        }
        if (*column) {
            key.Name = **column;
            key.Expression = Ident(key.Expression->Location, key.Name);
            continue;
        }
        if (ambiguousSelectAliases.contains(key.Name)) {
            return std::unexpected(TError(
                key.Expression->Location,
                "GROUP BY alias '" + key.Name + "' is ambiguous"));
        }
        auto alias = selectAliases.find(key.Name);
        if (alias == selectAliases.end()) {
            return std::unexpected(TError(
                key.Expression->Location, "unknown input column: " + key.Name));
        }
        if (auto selected = selectedKey(key, alias->second, "GROUP BY alias '" + key.Name + "'"); !selected) {
            return std::unexpected(selected.error());
        }
    }
    MergeColumnKeys(groupKeys, groupingSets);
    NarrowPlainGroupKeys(groupKeys, groupingSets, hasGroupingSyntax);
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
        if (auto annotated = Annotate(node.Plan, context); !annotated) {
            return std::unexpected(annotated.error());
        }
    }

    auto aggregate = std::make_shared<TAggregateOperator>(
        std::move(node.Plan), std::move(keys), std::move(specs));
    if (multiSet) {
        aggregate->MutableGroupingSets() = std::move(groupingSets);
        auto aggregateInput = aggregate->Input()->OutputColumns();
        aggregate->Type = std::make_shared<NAst::TFunctionType>(
            std::vector<NAst::TTypePtr>{aggregateInput},
            ComputeAggregateOutputType(aggregateInput, aggregate->GroupKeys(), aggregate->Aggs(),
                !aggregate->GroupingSets().empty()));
    }
    // Plain group keys keep their input labels: correlated subqueries above the
    // aggregate refer to them by SQL name.
    return CarryLabels(input, aggregate);
}

// Above the aggregate, SELECT and HAVING read group keys and aggregate results
// only. A FROM column they still reference is neither grouped nor aggregated.
std::expected<void, TError> RequireGroupedColumns(
    const TRelation& from,
    const TRelation& aggregate,
    const std::vector<TProjectionSpec>& projections,
    const NAst::TExprPtr& having)
{
    const auto fromColumns = ColumnNames(from.Plan);
    const auto aggregateColumns = ColumnNames(aggregate.Plan);
    const std::unordered_set<std::string> available(aggregateColumns.begin(), aggregateColumns.end());
    auto check = [&](const NAst::TExprPtr& expression) -> std::expected<void, TError> {
        for (const auto& ident : FindUnboundIdents(expression)) {
            auto column = std::ranges::find(fromColumns, ident->Name);
            if (available.contains(ident->Name) || column == fromColumns.end()) {
                continue;
            }
            const auto& label = from.Labels[column - fromColumns.begin()];
            return std::unexpected(TError(ident->Location, "column " + (label.empty() ? ident->Name : label)
                + " must appear in GROUP BY or be used in an aggregate function"));
        }
        return {};
    };
    for (const auto& projection : projections) {
        if (auto checked = check(projection.Expression); !checked) {
            return checked;
        }
    }
    return having ? check(having) : std::expected<void, TError>{};
}

// What ORDER BY may refer to besides the output labels of a SELECT, or of a
// UNION whose top projection it can extend.
struct TSelectOutput {
    // The FROM relation SELECT expressions are bound against, and the GROUP BY
    // aliases they may name; ORDER BY expressions bind the same way to match them.
    TRelation From;
    std::unordered_set<std::string> GroupAliases;
    // Bound SELECT expressions, by printed text -> first output column.
    std::unordered_map<std::string, size_t> Expressions;
    // Per output column: equal identities are the same field of the same bound
    // expression; nullopt for an expression that cannot be compared.
    std::vector<std::optional<std::string>> Identities;
    // Output columns with an explicit name (an alias or a plain column item), by
    // that name -> projection; nullopt for a name given to several of them.
    std::unordered_map<std::string, std::optional<size_t>> Aliases;
    // Labels of the input of the top projection, which hidden sort columns extend.
    std::vector<std::string> InputLabels;
    // Over SELECT DISTINCT, hidden sort columns read only the distinct outputs.
    bool Distinct = false;
};

std::expected<TRelation, TError> BuildSelect(
    const NSql::TSqlSelect& select,
    TBuildContext& context,
    std::optional<TSelectOutput>* output)
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
    TExtractedSubqueries subqueries;
    // Extracts scalar subqueries out of a finished projection list, broadcasting
    // each onto `n` (uncorrelated -> keyless cross join; correlated -> decorrelated
    // left join). Shares `subqueries` with the WHERE/HAVING extraction so the
    // __scalar_N__ names stay unique. Called just before the final project so the
    // broadcast joins attach above any aggregate (matching HAVING).
    auto extractProjectionSubqueries =
        [&](std::vector<TProjectionSpec>& projs, TRelation& n)
        -> std::expected<void, TError> {
        for (auto& p : projs) {
            auto rewritten = ExtractScalarSubqueries(p.Expression, n, context, subqueries);
            if (!rewritten) {
                return std::unexpected(rewritten.error());
            }
            p.Expression = std::move(*rewritten);
        }
        return {};
    };
    // Builds the pre-aggregate input (FROM + joins + WHERE).
    auto buildInput = [&]() -> std::expected<TRelation, TError> {
        auto base = BuildFrom(*select.From, context);
        if (!base) {
            return std::unexpected(base.error());
        }
        TRelation node = std::move(*base);
        if (select.Where) {
            // WHERE binds as one expression: BETWEEN shares its operand between the
            // two comparisons it becomes.
            NAst::TExprPtr where = select.Where;
            if (auto bound = TNameScope({node}).Bind(where); !bound) {
                return std::unexpected(bound.error());
            }
            std::vector<NAst::TExprPtr> conjuncts;
            FlattenConjuncts(where, conjuncts);

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
                auto rewritten = ExtractScalarSubqueries(conjunct, node, context, subqueries);
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
    // SELECT and HAVING bind against the aggregate input. A GROUP BY name that is
    // no input column is a SELECT alias naming its key, which HAVING and GROUPING()
    // refer to by that name (see BuildAggregate).
    const TRelation from = node;
    const TNameScope input({from});
    std::unordered_set<std::string> groupAliases;
    if (parsedGrouping) {
        for (const auto& key : parsedGrouping->Keys) {
            if (auto column = input.Find(key.Name, {}); !key.Computed && column && !*column) {
                groupAliases.insert(key.Name);
            }
        }
    }
    auto groupAlias = [&](NAst::TExprPtr& reference) -> std::expected<bool, TError> {
        return groupAliases.contains(NAst::TMaybeNode<NAst::TIdentExpr>(reference).Cast()->Name);
    };
    std::vector<TProjectionSpec> projections;
    // The printed text of each projection's bound SELECT expression, for ORDER BY.
    std::vector<std::optional<std::string>> texts;
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
            const auto expanded = projections.size();
            for (size_t column = 0; column < schema->Fields.size(); ++column) {
                const auto& label = node.Labels[column];
                if (label.empty() || (!prefix.empty() && label.rfind(prefix, 0) != 0)) {
                    continue;
                }
                projections.push_back({
                    .Name = BareColumnName(label), .Expression = Ident({}, schema->Fields[column].first) });
                texts.push_back(NAst::NCore::PrintAst(projections.back().Expression));
            }
            if (!prefix.empty() && projections.size() == expanded) {
                return std::unexpected(TError(
                    "'" + prefix + "*' does not name a FROM item"));
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
        NAst::TExprPtr bound = item->Expr;
        if (auto ok = input.Bind(bound, groupAlias); !ok) {
            return std::unexpected(ok.error());
        }
        // Window and subquery items cannot be core-printed; ORDER BY refers to them
        // by their aliases.
        texts.push_back(HasWindowExpr(item->Expr) || HasSubquery(item->Expr)
            ? std::nullopt
            : std::optional<std::string>(NAst::NCore::PrintAst(bound)));
        auto expr = collector.Rewrite(std::move(bound));
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

    // A scalar subquery in HAVING is left intact by the aggregate collector
    // (it is opaque) and decorrelated below, once the aggregate node exists.
    NAst::TExprPtr having;
    if (select.Having) {
        having = select.Having;
        if (auto bound = input.Bind(having, groupAlias); !bound) {
            return std::unexpected(bound.error());
        }
        auto rewritten = collector.Rewrite(std::move(having));
        if (!rewritten) {
            return std::unexpected(rewritten.error());
        }
        having = std::move(*rewritten);
    }

    // Window functions run after aggregation/HAVING and before the final project.
    // Shared by both the aggregated and non-aggregated paths. Their outputs are
    // helper columns.
    auto applyWindows = [&](const TRelation& n) -> std::expected<TRelation, TError> {
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
        auto windowed = windows.Build(n.Plan);
        if (!windowed) {
            return std::unexpected(windowed.error());
        }
        return CarryLabels(n, std::move(*windowed));
    };

    // SQL positions count the projections after those of helper items, which
    // are plain columns. GROUP BY counts them before the projection is typed, up
    // to the greatest position it names.
    const size_t helpers = select.SelectList->HelperItems;
    std::optional<int64_t> groupPosition;
    if (parsedGrouping) {
        for (const auto& key : parsedGrouping->Keys) {
            if (auto position = SelectPosition(key.Expression)) {
                groupPosition = std::max(*position, groupPosition.value_or(*position));
            }
        }
    }
    TPositionedOutputs positioned{.Helpers = helpers};
    if (groupPosition) {
        auto outputs = PositionedOutputs(
            projections,
            helpers,
            collector,
            inputSchema ? inputSchema.Cast().get() : nullptr,
            context.Annotation);
        if (!outputs) {
            return std::unexpected(outputs.error());
        }
        positioned = std::move(*outputs);
    }

    // The projection is typed as it is built, so its output names, struct fields
    // included, are the labels of the SELECT. Repeated labels get physical names
    // that stay unique where sibling SELECTs meet.
    auto project = [&](const TRelation& n) -> std::expected<TRelation, TError> {
        std::vector<bool> named;
        for (const auto& projection : projections) {
            named.push_back(!projection.ImplicitName && projection.ColumnAliases.empty());
        }
        auto plan = std::make_shared<TProjectOperator>(n.Plan, std::move(projections));
        if (auto annotated = Annotate(plan, context); !annotated) {
            return std::unexpected(annotated.error());
        }
        // GROUP BY counted a projection typed only above the aggregate as one
        // column; a struct there would move the positions after it.
        if (groupPosition) {
            int64_t first = 1;
            for (size_t i = 0; i < positioned.Widths.size() && first <= *groupPosition; ++i) {
                const auto& projection = plan->Projections()[positioned.Helpers + i];
                if (FlattenedProjectionArity(projection) != positioned.Widths[i]) {
                    return std::unexpected(TError(projection.Expression->Location,
                        "GROUP BY position " + std::to_string(*groupPosition)
                        + " requires AS (...) on a struct-return projection typed above the aggregate"));
                }
                first += static_cast<int64_t>(positioned.Widths[i]);
            }
        }
        auto labels = ColumnNames(plan);
        auto projected = Relabel(TRelation{.Plan = plan}, labels, context);
        if (!projected) {
            return projected;
        }
        std::fill_n(projected->Labels.begin(), helpers, std::string());
        if (output) {
            auto& selected = output->emplace();
            selected.From = from;
            selected.GroupAliases = groupAliases;
            size_t offset = 0;
            for (size_t i = 0; i < texts.size(); ++i) {
                if (texts[i]) {
                    selected.Expressions.emplace(*texts[i], offset);
                }
                const auto arity = FlattenedProjectionArity(plan->Projections()[i]);
                for (size_t field = 0; field < arity; ++field) {
                    selected.Identities.push_back(texts[i]
                        ? std::optional<std::string>(*texts[i] + "#" + std::to_string(field))
                        : std::nullopt);
                }
                // Over DISTINCT, ORDER BY reads the output labels themselves.
                if (named[i] && !distinct) {
                    if (auto [alias, inserted] = selected.Aliases.emplace(labels[offset], i); !inserted) {
                        alias->second.reset();
                    }
                }
                offset += arity;
            }
            selected.InputLabels = distinct ? labels : n.Labels;
            selected.Distinct = distinct;
        }
        if (!distinct) {
            return projected;
        }
        const auto columns = ColumnNames(projected->Plan);
        return ApplyDistinct(std::move(*projected), columns, context);
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
        auto windowed = applyWindows(node);
        if (!windowed) {
            return windowed;
        }
        node = std::move(*windowed);
        if (auto ok = extractProjectionSubqueries(projections, node); !ok) {
            return std::unexpected(ok.error());
        }
        return project(node);
    }

    auto aggregate = BuildAggregate(
        std::move(node),
        collector,
        names,
        std::move(parsedGrouping),
        selectAliases,
        ambiguousSelectAliases,
        projections,
        positioned,
        having,
        context);
    if (!aggregate) {
        return std::unexpected(aggregate.error());
    }
    node = std::move(*aggregate);
    if (auto grouped = RequireGroupedColumns(from, node, projections, having); !grouped) {
        return std::unexpected(grouped.error());
    }
    if (having) {
        auto rewritten = ExtractScalarSubqueries(having, node, context, subqueries);
        if (!rewritten) {
            return std::unexpected(rewritten.error());
        }
        node.Plan = std::make_shared<TFilterOperator>(std::move(node.Plan), std::move(*rewritten));
    }
    {
        auto windowed = applyWindows(node);
        if (!windowed) {
            return windowed;
        }
        node = std::move(*windowed);
    }
    if (auto ok = extractProjectionSubqueries(projections, node); !ok) {
        return std::unexpected(ok.error());
    }
    return project(node);
}

std::expected<TRelation, TError> ApplyOrderBy(
    const NSql::TSqlQuery& query,
    TRelation relation,
    const TSelectOutput* select,
    const TBuildContext& context)
{
    if (!query.OrderBy || query.OrderBy->Items.empty()) {
        return relation;
    }

    // BuildSelect always tops the plan with a projection. An ORDER BY item is
    // resolved in order of preference:
    //   1. a SELECT position or a plain output-column identifier -> sort on that
    //      output column directly;
    //   2. an expression already computed in the select list (matched
    //      structurally) -> sort on that output column, so aggregates and other
    //      computed columns are reused rather than re-evaluated;
    //   3. a pure-scalar expression over the projection's input, where a SELECT
    //      alias may stand for a missing input column -> materialized as a hidden
    //      column, sorted on, then stripped by a final projection so the output
    //      schema is unchanged.
    // An aggregate not present in the select list cannot be sorted on (it would
    // be re-applied as a scalar over already-grouped rows), so it is rejected.
    auto topProject = TMaybeOp<TProjectOperator>(relation.Plan);
    const auto columns = ColumnNames(relation.Plan);
    std::optional<TRelation> projectionInput;
    std::optional<TNameScope> input;
    TFreshNames sortNames;
    for (const auto& column : columns) {
        sortNames.Reserve(column);
    }

    // SQL positions count the outputs that have a label; the hidden sort columns
    // are added only below.
    std::vector<size_t> positioned;
    for (size_t i = 0; i < relation.Labels.size(); ++i) {
        if (!relation.Labels[i].empty()) {
            positioned.push_back(i);
        }
    }

    std::vector<TSortKey> keys;
    keys.reserve(query.OrderBy->Items.size());
    std::vector<TProjectionSpec> hidden;

    for (const auto& item : query.OrderBy->Items) {
        std::optional<size_t> index;
        if (auto position = SelectPosition(item->Expr)) {
            if (*position < 1 || *position > static_cast<int64_t>(positioned.size())) {
                return std::unexpected(TError(item->Expr->Location,
                    "ORDER BY position " + std::to_string(*position) + " is not in the select list"));
            }
            index = positioned[*position - 1];
        } else if (auto ident = NAst::TMaybeNode<NAst::TIdentExpr>(item->Expr)) {
            for (size_t i = 0; i < relation.Labels.size(); ++i) {
                if (relation.Labels[i] != ident.Cast()->Name) {
                    continue;
                }
                // Output columns sharing a label are one column only when they are
                // the same field of the same expression.
                if (index && (!select || i >= select->Identities.size() || !select->Identities[i]
                    || select->Identities[*index] != select->Identities[i]))
                {
                    return std::unexpected(TError(item->Expr->Location,
                        "ORDER BY output column is ambiguous: " + ident.Cast()->Name));
                }
                index = i;
            }
        }
        if (!index && HasWindowExprLocal(item->Expr)) {
            return std::unexpected(TError(item->Expr->Location,
                "ORDER BY on a window expression must reference its select alias"));
        }
        if (!index && select && select->From.Plan && !HasSubquery(item->Expr)) {
            // An expression bound as the SELECT list was matches a SELECT expression
            // written differently, e.g. with a qualified column.
            auto expression = CloneExpr(item->Expr);
            auto bound = TNameScope({select->From}).Bind(expression, [&](NAst::TExprPtr& reference) {
                return std::expected<bool, TError>(select->GroupAliases.contains(
                    NAst::TMaybeNode<NAst::TIdentExpr>(reference).Cast()->Name));
            });
            if (bound) {
                if (auto it = select->Expressions.find(NAst::NCore::PrintAst(expression));
                    it != select->Expressions.end())
                {
                    index = it->second;
                }
            }
        }
        std::string column;
        if (index) {
            column = columns[*index];
        } else {
            if (!topProject || !select) {
                return std::unexpected(TError(item->Expr->Location,
                    "ORDER BY on an expression is not supported here"));
            }
            if (HasGroupingCall(item->Expr)) {
                return std::unexpected(TError(item->Expr->Location,
                    "ORDER BY GROUPING() expression must appear in the select list"));
            }
            // An aggregate missing from the select list cannot be materialized in
            // the scalar project above the aggregate.
            if (HasAggregateCall(item->Expr)) {
                return std::unexpected(TError(item->Expr->Location,
                    "ORDER BY on an aggregate not in the select list is not supported"));
            }
            if (!input) {
                projectionInput = TRelation{.Plan = topProject.Cast()->Input(), .Labels = select->InputLabels};
                input.emplace(std::initializer_list<std::reference_wrapper<const TRelation>>{*projectionInput});
            }
            // The expression is materialized in the final projection, so it reads
            // that projection's input: the FROM columns of a plain SELECT, or the
            // group keys and the aggregate and window results of an aggregated one.
            // A name that is not such a column falls back to an output column with
            // an explicit name (an alias or a plain column item); it is replaced by
            // that column's expression, which already reads the aggregate,
            // GROUPING() and window results. An ungrouped FROM column is not
            // readable here, so in an aggregated SELECT its name may resolve to an
            // alias of the same name, where PostgreSQL and DuckDB reject the query.
            // A name given to several output columns is ambiguous. Over SELECT
            // DISTINCT the hidden columns read the distinct outputs alone.
            auto alias = [&](NAst::TExprPtr& reference) -> std::expected<bool, TError> {
                const auto name = NAst::TMaybeNode<NAst::TIdentExpr>(reference).Cast()->Name;
                if (select->Distinct) {
                    return std::unexpected(TError(reference->Location,
                        "ORDER BY expressions must appear in the SELECT DISTINCT output: " + name));
                }
                auto found = select->Aliases.find(name);
                if (found == select->Aliases.end()) {
                    return false;
                }
                if (!found->second) {
                    return std::unexpected(TError(reference->Location,
                        "ORDER BY alias '" + name + "' is ambiguous"));
                }
                const auto location = reference->Location;
                reference = CloneExpr(topProject.Cast()->Projections()[*found->second].Expression);
                reference->Location = location;
                return true;
            };
            NAst::TExprPtr expression = item->Expr;
            if (auto bound = input->Bind(expression, alias); !bound) {
                return std::unexpected(bound.error());
            }
            column = sortNames.Next("__sort");
            hidden.push_back({ .Name = column, .Expression = std::move(expression) });
        }

        keys.push_back({
            .Column = column,
            .Direction = item->Desc ? ESortDirection::Desc : ESortDirection::Asc,
            .Nulls = ConvertSortNulls(item->NullOrder),
        });
    }

    if (hidden.empty()) {
        relation.Plan = std::make_shared<TSortOperator>(std::move(relation.Plan), std::move(keys));
        return relation;
    }

    // Pass-through projection over the original outputs; it becomes the strip
    // projection above the sort.
    std::vector<TProjectionSpec> outputProjections;
    outputProjections.reserve(columns.size());
    for (const auto& name : columns) {
        outputProjections.push_back({ .Name = name, .Expression = Ident({}, name) });
    }

    auto& projections = topProject.Cast()->MutableProjections();
    for (auto& h : hidden) {
        projections.push_back(std::move(h));
    }
    if (auto annotated = Annotate(topProject.Cast(), context); !annotated) {
        return std::unexpected(annotated.error());
    }

    TOperatorPtr sorted = std::make_shared<TSortOperator>(std::move(relation.Plan), std::move(keys));
    relation.Plan = std::make_shared<TProjectOperator>(std::move(sorted), std::move(outputProjections));
    return relation;
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
    TBuildContext& context,
    std::optional<TSelectOutput>* output);

// Builds a query body (a SELECT, a set operation, or a parenthesized query).
std::expected<TRelation, TError> BuildQueryBody(
    const NSql::TSqlNodePtr& body,
    TBuildContext& context,
    std::optional<TSelectOutput>* output = nullptr)
{
    if (auto select = NSql::TMaybeNode<NSql::TSqlSelect>(body)) {
        return BuildSelect(*select.Cast(), context, output);
    }
    if (auto setOp = NSql::TMaybeNode<NSql::TSqlSetOp>(body)) {
        return BuildSetOp(*setOp.Cast(), context, output);
    }
    if (auto sub = NSql::TMaybeNode<NSql::TSqlQuery>(body)) {
        return BuildQuery(*sub.Cast(), context, output);
    }
    return std::unexpected(TError("unsupported query body"));
}

std::expected<TRelation, TError> BuildSetOp(
    const NSql::TSqlSetOp& setOp,
    TBuildContext& context,
    std::optional<TSelectOutput>* output)
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

        // The union takes the column names and labels of its first branch.
        TRelation unionAll{
            .Plan = std::make_shared<TUnionAllOperator>(std::move(branches)), .Labels = std::move(labels)};
        if (auto annotated = Annotate(unionAll.Plan, context); !annotated) {
            return std::unexpected(annotated.error());
        }
        if (setOp.Quantifier == NSql::ESetQuantifier::All) {
            return unionAll;
        }

        auto outputNames = OutputColumnNames(unionAll.Plan, "UNION");
        if (!outputNames) {
            return std::unexpected(outputNames.error());
        }
        if (output) {
            output->emplace().InputLabels = unionAll.Labels;
        }
        return ApplyDistinct(std::move(unionAll), *outputNames, context);
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

        auto leftDistinct = ApplyDistinct(std::move(*left), *leftNames, context);
        if (!leftDistinct) {
            return leftDistinct;
        }
        auto rightDistinct = ApplyDistinct(std::move(*right), *rightNames, context);
        if (!rightDistinct) {
            return rightDistinct;
        }

        std::vector<std::pair<std::string, std::string>> keys;
        keys.reserve(leftNames->size());
        for (size_t i = 0; i < leftNames->size(); ++i) {
            keys.emplace_back((*leftNames)[i], (*rightNames)[i]);
        }

        auto join = MakeJoin(
            leftDistinct->Plan, std::move(rightDistinct->Plan),
            std::move(keys),
            isIntersect ? EJoinType::LeftSemi : EJoinType::LeftAnti);
        if (!join) {
            return std::unexpected(join.error());
        }
        // Set membership uses IS NOT DISTINCT FROM semantics, including NULLs.
        for (auto& key : TMaybeOp<TJoinOperator>(*join).Cast()->MutableKeys()) {
            key.NullsEqual = true;
        }
        return TRelation{.Plan = std::move(*join), .Labels = std::move(leftDistinct->Labels)};
    }

    return std::unexpected(TError("unsupported set operation"));
}

std::expected<TRelation, TError> BuildQuery(
    const NSql::TSqlQuery& query,
    TBuildContext& context,
    std::optional<TSelectOutput>* output)
{
    // A query without ORDER BY or LIMIT ends in the projection of its body, so an
    // enclosing query, e.g. `(SELECT ...) ORDER BY ...`, may extend it.
    auto finish = [&](std::expected<TRelation, TError> plan, std::optional<TSelectOutput>& select,
        const TBuildContext& scope) -> std::expected<TRelation, TError>
    {
        if (!plan) {
            return plan;
        }
        if (output && !query.OrderBy && !query.Limit && !query.Offset) {
            *output = std::move(select);
            return plan;
        }
        auto ordered = ApplyOrderBy(query, std::move(*plan), select ? &*select : nullptr, scope);
        if (!ordered) {
            return ordered;
        }
        return ApplyLimit(query, std::move(*ordered));
    };
    if (!query.WithClause) {
        std::optional<TSelectOutput> select;
        return finish(BuildQueryBody(query.Body, context, &select), select, context);
    }
    if (query.WithClause->Recursive) {
        return std::unexpected(TError("WITH RECURSIVE is not supported yet"));
    }

    const auto ctes = CteMap(*query.WithClause);

    // CTE-aware factory: a CTE name is built once as a canonical definition
    // (shared by every reference via TCteRef); other names fall back to the base
    // sources. Nested CTEs memoize into a DAG. Stack-local - definitions survive
    // via the shared_ptr the TCteRef holds.
    struct TBuiltCte {
        TCteDefinitionPtr Definition;
        std::vector<std::string> Labels;
    };
    std::map<std::string, TBuiltCte> builtDefs;
    TBuildContext inner{.Annotation = context.Annotation, .SourceAliases = context.SourceAliases};
    inner.Sources = [&](std::string_view name) -> std::expected<TRelation, TError> {
        const std::string lower = ToLower(std::string(name));
        auto it = ctes.find(lower);
        if (it == ctes.end()) {
            return context.Sources(name);
        }
        auto cached = builtDefs.find(lower);
        if (cached == builtDefs.end()) {
            // A definition is optimized as a plan of its own and numbers its sources apart.
            auto aliases = std::exchange(inner.SourceAliases, {});
            auto plan = BuildQuery(*it->second->Query, inner);
            inner.SourceAliases = std::move(aliases);
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
            cached = builtDefs.emplace(lower, TBuiltCte{std::move(definition), std::move(plan->Labels)}).first;
        }
        return TRelation{
            .Plan = std::make_shared<TCteRef>(cached->second.Definition), .Labels = cached->second.Labels};
    };

    std::optional<TSelectOutput> select;
    return finish(BuildQueryBody(query.Body, inner, &select), select, inner);
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
    std::unordered_set<std::string> sourceAliases;
    TBuildContext context{
        .Sources = [&](std::string_view table) -> std::expected<TRelation, TError> {
            auto plan = sources(table);
            if (!plan) {
                return std::unexpected(plan.error());
            }
            return Relation(std::move(*plan));
        },
        .Annotation = annotation,
        .SourceAliases = sourceAliases,
    };
    auto relation = BuildQuery(*root.Cast(), context);
    if (!relation) {
        return std::unexpected(relation.error());
    }
    // The query's output columns carry their SQL names, repeated ones included.
    auto plan = RenameOutputs(relation->Plan, relation->Labels, context);
    if (!plan) {
        return plan;
    }
    AssignCteIds(*plan);
    return plan;
}

} // namespace NQdb
