#include "join_order.h"

#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/passes/cbo/dpccp.h>

#include "factor_conjuncts.h"
#include "unbound_vars.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace NQdb {

using namespace NQumir::NAst;
using namespace NQumir::NAst::NLiterals;

namespace {

struct TEdge {
    std::string Left;
    std::string Right;
};

// The conjuncts of the filters above a join chain: key equalities, which form
// the join graph, and the other conjuncts, which only scale its joins.
struct TJoinPredicates {
    std::vector<TEdge> Edges;
    std::vector<TExprPtr> Residuals;
};

struct TOwnedEdge {
    NCbo::TJoinEdge Edge;
    size_t LeftLeaf;
    size_t RightLeaf;
};

struct TContext {
    bool EnableCbo = true;
    TJoinReorderDiagnostics* Diagnostics = nullptr;
    TConstFolder& Folder;
};

void CollectJoinPredicates(const TExprPtr& predicate, TJoinPredicates& out) {
    std::vector<TExprPtr> conjucts;
    FactorConjuncts(predicate, conjucts);
    for (const auto& conj : conjucts) {
        auto maybeBinary = TMaybeNode<TBinaryExpr>(conj);
        if (maybeBinary) {
            auto binary = maybeBinary.Cast();
            auto maybeLeft = TMaybeNode<TIdentExpr>(binary->Left);
            auto maybeRight = TMaybeNode<TIdentExpr>(binary->Right);
            if (binary->Operator == "=="_op && maybeLeft && maybeRight) {
                auto left = maybeLeft.Cast();
                auto right = maybeRight.Cast();
                out.Edges.push_back({left->Name, right->Name});
                continue;
            }
        }
        out.Residuals.push_back(conj);
    }
}

// Does this join input carry a filter (a selective dimension), rather than being
// a raw fact scan? Used by the reorder seed heuristic (put filtered relations
// first) and the semi-pushdown guard (keep selective joins in the build). Stops
// at subquery boundaries (aggregate/project) so an inner correlated filter does
// not count.
bool ContainsFilter(const TOperatorPtr& node) {
    if (!node) {
        return false;
    }
    if (TMaybeOp<TFilterOperator>(node)) {
        return true;
    }
    if (auto join = TMaybeOp<TJoinOperator>(node)) {
        return ContainsFilter(join.Cast()->Left())
            || ContainsFilter(join.Cast()->Right());
    }
    return false;
}

// A reorderable join is an inner join we are free to reassociate: no extracted
// keys yet and no residual of its own (the comma-join shape the builder emits).
bool IsReorderableJoin(const TOperatorPtr& node) {
    auto join = TMaybeOp<TJoinOperator>(node);
    return join
        && join.Cast()->JoinType() == EJoinType::Inner
        && join.Cast()->Keys().empty()
        && join.Cast()->Filter() == nullptr;
}

void CollectChainLeaves(const TOperatorPtr& node, std::vector<TOperatorPtr>& out) {
    if (IsReorderableJoin(node)) {
        auto join = TMaybeOp<TJoinOperator>(node).Cast();
        CollectChainLeaves(join->Left(), out);
        CollectChainLeaves(join->Right(), out);
    } else {
        out.push_back(node);
    }
}

// Greedy connected order: seed a component with the first unplaced leaf, then
// repeatedly attach any leaf sharing an edge with an already-placed one. When
// nothing connects, the next leaf seeds a new component (a deliberate cross).
std::vector<size_t> ConnectedOrder(
    size_t count,
    const std::vector<std::unordered_set<size_t>>& adjacency)
{
    std::vector<size_t> order;
    order.reserve(count);
    std::vector<bool> placed(count, false);

    while (order.size() < count) {
        size_t seed = 0;
        while (seed < count && placed[seed]) {
            ++seed;
        }
        order.push_back(seed);
        placed[seed] = true;

        bool grew = true;
        while (grew) {
            grew = false;
            for (size_t i = 0; i < count; ++i) {
                if (placed[i]) {
                    continue;
                }
                bool connected = false;
                for (size_t p : order) {
                    if (adjacency[i].count(p)) {
                        connected = true;
                        break;
                    }
                }
                if (connected) {
                    order.push_back(i);
                    placed[i] = true;
                    grew = true;
                }
            }
        }
    }
    return order;
}

TOperatorPtr MakeInnerJoin(TOperatorPtr left, TOperatorPtr right) {
    return std::make_shared<TJoinOperator>(
        std::move(left), std::move(right), std::vector<TJoinKey>{}, EJoinType::Inner, nullptr);
}

// Left-deep heuristic order - fallback when a component exceeds MaxRelations.
TOperatorPtr HeuristicChain(
    const std::vector<TOperatorPtr>& leaves,
    const std::vector<std::unordered_set<size_t>>& adjacency)
{
    auto order = ConnectedOrder(leaves.size(), adjacency);
    TOperatorPtr result = leaves[order.front()];
    for (size_t i = 1; i < order.size(); ++i) {
        result = MakeInnerJoin(std::move(result), leaves[order[i]]);
    }
    return result;
}

bool HasRowStats(const TOperatorPtr& leaf) {
    return leaf && leaf->Stats_ && leaf->Stats_->RowCount > 0;
}

bool HasColumnNdv(const TOperatorPtr& leaf, const std::string& col) {
    if (!HasRowStats(leaf)) {
        return false;
    }
    auto it = leaf->Stats_->ColumnStats.find(col);
    return it != leaf->Stats_->ColumnStats.end()
        && it->second
        && it->second->Ndv
        && *it->second->Ndv > 0;
}

std::optional<std::string> CboStatsFallbackReason(
    const std::vector<TOperatorPtr>& leaves,
    const std::vector<TOwnedEdge>& edges)
{
    if (edges.empty()) {
        return "no equi-join edges";
    }
    for (const auto& leaf : leaves) {
        if (!HasRowStats(leaf)) {
            return "missing row stats";
        }
    }
    for (const auto& edge : edges) {
        if (!HasColumnNdv(leaves[edge.LeftLeaf], edge.Edge.LeftCol) ||
            !HasColumnNdv(leaves[edge.RightLeaf], edge.Edge.RightCol)) {
            return "missing column NDV stats";
        }
    }
    return std::nullopt;
}

void RecordJoinReorderChain(
    TContext& ctx,
    size_t leafCount,
    size_t edgeCount,
    bool usedCbo,
    std::string strategy,
    std::string reason = {})
{
    if (!ctx.Diagnostics) {
        return;
    }
    ctx.Diagnostics->UsedCbo = ctx.Diagnostics->UsedCbo || usedCbo;
    ctx.Diagnostics->Chains.push_back({
        .LeafCount = leafCount,
        .EdgeCount = edgeCount,
        .EnableCbo = ctx.EnableCbo,
        .UsedCbo = usedCbo,
        .Strategy = std::move(strategy),
        .Reason = std::move(reason),
    });
}

TOperatorPtr BuildChain(
    const std::vector<TOperatorPtr>& leaves,
    const TJoinPredicates& predicates,
    TContext& ctx)
{
    const size_t n = leaves.size();

    // Map every output column to the leaf that produces it.
    std::unordered_map<std::string, size_t> owner;
    for (size_t i = 0; i < n; ++i) {
        auto&& schema = leaves[i]->OutputColumns();
        if (!schema) {
            continue;
        }
        for (const auto& [name, _] : schema->Fields) {
            owner.emplace(name, i);
        }
    }

    std::vector<std::unordered_set<size_t>> adjacency(n);
    std::vector<TOwnedEdge> graphEdges;
    for (const auto& edge : predicates.Edges) {
        auto left = owner.find(edge.Left);
        auto right = owner.find(edge.Right);
        if (left == owner.end() || right == owner.end() || left->second == right->second) {
            continue;
        }
        adjacency[left->second].insert(right->second);
        adjacency[right->second].insert(left->second);
        graphEdges.push_back({
            .Edge = {edge.Left, edge.Right},
            .LeftLeaf = left->second,
            .RightLeaf = right->second,
        });
    }
    if (!ctx.EnableCbo) {
        RecordJoinReorderChain(
            ctx, leaves.size(), graphEdges.size(), false, "heuristic", "cbo disabled");
        return HeuristicChain(leaves, adjacency);
    }
    if (auto reason = CboStatsFallbackReason(leaves, graphEdges)) {
        RecordJoinReorderChain(
            ctx, leaves.size(), graphEdges.size(), false, "heuristic", *reason);
        return HeuristicChain(leaves, adjacency);
    }

    // Connected components (a disconnected graph means a deliberate cross).
    std::vector<int> comp(n, -1);
    int components = 0;
    for (size_t s = 0; s < n; ++s) {
        if (comp[s] != -1) {
            continue;
        }
        std::vector<size_t> stack{s};
        comp[s] = components;
        while (!stack.empty()) {
            size_t u = stack.back();
            stack.pop_back();
            for (size_t v : adjacency[u]) {
                if (comp[v] == -1) {
                    comp[v] = components;
                    stack.push_back(v);
                }
            }
        }
        ++components;
    }

    std::vector<std::vector<TOperatorPtr>> compLeaves(components);
    for (size_t i = 0; i < n; ++i) {
        compLeaves[comp[i]].push_back(leaves[i]);
    }
    for (const auto& group : compLeaves) {
        if (group.size() > NCbo::MaxRelations) {
            RecordJoinReorderChain(
                ctx, leaves.size(), graphEdges.size(), false, "heuristic",
                "too many relations for cbo");
            return HeuristicChain(leaves, adjacency);
        }
    }
    std::vector<std::vector<NCbo::TJoinEdge>> compEdges(components);
    for (const auto& edge : graphEdges) {
        compEdges[comp[edge.LeftLeaf]].push_back(edge.Edge);
    }

    // Optimal subtree per component; cross-join components smallest-first.
    std::vector<std::pair<size_t, TOperatorPtr>> subtrees;
    for (int c = 0; c < components; ++c) {
        TOperatorPtr tree = compLeaves[c].size() == 1
            ? compLeaves[c].front()
            : NCbo::DpccpJoinOrder(compLeaves[c], compEdges[c], predicates.Residuals, ctx.Folder);
        if (!tree) {
            RecordJoinReorderChain(
                ctx, leaves.size(), graphEdges.size(), false, "heuristic",
                "cbo returned no plan");
            return HeuristicChain(leaves, adjacency); // defensive
        }
        subtrees.push_back({compLeaves[c].size(), std::move(tree)});
    }
    RecordJoinReorderChain(
        ctx, leaves.size(), graphEdges.size(), true, "cbo");
    std::stable_sort(subtrees.begin(), subtrees.end(),
        [](const auto& a, const auto& b) { return a.first < b.first; });

    TOperatorPtr result = std::move(subtrees.front().second);
    for (size_t i = 1; i < subtrees.size(); ++i) {
        result = MakeInnerJoin(std::move(result), std::move(subtrees[i].second));
    }
    return result;
}

// `predicates` are threaded down from enclosing filters, so an inner-join
// chain can be reordered even when a filter is separated from it by other
// operators (e.g. a decorrelation LEFT JOIN). They are reset across
// project/aggregate, which redefine columns; BuildChain ignores the edges and
// DPccp the residuals that read columns the chain's leaves do not own
// (qualified names disambiguate).
TOperatorPtr Reorder(
    TOperatorPtr node,
    TJoinPredicates predicates,
    TContext& ctx)
{
    if (!node) {
        return node;
    }
    if (auto maybeFilter = TMaybeOp<TFilterOperator>(node)) {
        auto filter = maybeFilter.Cast();
        CollectJoinPredicates(filter->Predicate(), predicates);
        filter->MutableInput() = Reorder(
            filter->Input(), std::move(predicates), ctx);
        return filter;
    }
    if (IsReorderableJoin(node)) {
        std::vector<TOperatorPtr> leaves;
        CollectChainLeaves(node, leaves);
        for (auto& leaf : leaves) {
            leaf = Reorder(std::move(leaf), {}, ctx);
        }
        return BuildChain(leaves, predicates, ctx);
    }
    if (auto maybeJoin = TMaybeOp<TJoinOperator>(node)) {
        auto join = maybeJoin.Cast();
        join->MutableLeft() = Reorder(join->Left(), predicates, ctx);
        join->MutableRight() = Reorder(join->Right(), predicates, ctx);
        return join;
    }
    for (auto& input : node->MutableInputs()) {
        input = Reorder(input, {}, ctx);
    }
    return node;
}

} // namespace

namespace {

std::unordered_set<std::string> ColumnNames(const TOperatorPtr& node) {
    std::unordered_set<std::string> names;
    if (auto st = TMaybeType<TStructType>(node->OutputColumns())) {
        for (const auto& [name, _] : st.Cast()->Fields) {
            names.insert(name);
        }
    }
    return names;
}

bool AllIn(const std::unordered_set<std::string>& cols,
    const std::unordered_set<std::string>& set)
{
    for (const auto& c : cols) {
        if (!set.count(c)) {
            return false;
        }
    }
    return true;
}

// Sinks a LeftSemi/LeftAnti join through the inner joins beneath its build side,
// as deep as its build-side dependencies allow. Reads output schemas of the
// current (typed) tree before mutating, so a caller must re-annotate afterwards.
TOperatorPtr SinkSemi(std::shared_ptr<TJoinOperator> semi) {
    auto childJoin = TMaybeOp<TJoinOperator>(semi->Left());
    if (!childJoin || childJoin.Cast()->JoinType() != EJoinType::Inner) {
        return semi;
    }
    auto inner = childJoin.Cast();

    // Everything the semi needs from its build side: its left key columns plus
    // any build-side columns its residual predicate reads. (Probe-side columns
    // travel with the probe input C and are unaffected by the push.)
    std::unordered_set<std::string> needBuild;
    for (const auto& key : semi->Keys()) {
        needBuild.insert(key.Left);
    }
    if (semi->Filter()) {
        const auto childCols = ColumnNames(semi->Left());
        for (const auto& v : FindUnboundVars(semi->Filter())) {
            if (childCols.count(v)) {
                needBuild.insert(v);
            }
        }
    }

    const auto leftCols = ColumnNames(inner->Left());
    const auto rightCols = ColumnNames(inner->Right());
    const bool toLeft = AllIn(needBuild, leftCols);
    const bool toRight = !toLeft && AllIn(needBuild, rightCols);
    if (!toLeft && !toRight) {
        return semi; // build dependencies span both sides - cannot push
    }

    // Don't push past a selective (filtered) side: keeping that join in the
    // build shrinks the blocking semi/anti far more than the push would.
    const auto& other = toLeft ? inner->Right() : inner->Left();
    if (ContainsFilter(other)) {
        return semi;
    }

    auto& target = toLeft ? inner->MutableLeft() : inner->MutableRight();
    auto pushed = std::make_shared<TJoinOperator>(
        target, semi->Right(), semi->Keys(), semi->JoinType(), semi->Filter());
    // The filters derived from the residual lie in `target` and the build side.
    pushed->MutableDerivedSideFilters() = semi->DerivedSideFilters();
    target = SinkSemi(std::move(pushed));
    return inner;
}

TOperatorPtr PushDown(TOperatorPtr node) {
    if (!node) {
        return node;
    }
    if (auto join = TMaybeOp<TJoinOperator>(node)) {
        auto j = join.Cast();
        const auto type = j->JoinType();
        // Sink first (top-down), while this subtree's types are still current;
        // SinkSemi preserves output column names, so recursing into the
        // restructured result stays valid before the pipeline re-annotates.
        if (type == EJoinType::LeftSemi || type == EJoinType::LeftAnti) {
            auto sunk = SinkSemi(j);
            if (sunk.get() != j.get()) {
                return PushDown(std::move(sunk));
            }
        }
    }
    for (auto& input : node->MutableInputs()) {
        input = PushDown(input);
    }
    return node;
}

} // namespace

TOperatorPtr ReorderJoins(
    TOperatorPtr root,
    TConstFolder& folder,
    bool enableCbo,
    TJoinReorderDiagnostics* diagnostics)
{
    if (diagnostics) {
        *diagnostics = {};
        diagnostics->EnableCbo = enableCbo;
    }
    TContext ctx{
        .EnableCbo = enableCbo,
        .Diagnostics = diagnostics,
        .Folder = folder,
    };
    return Reorder(std::move(root), {}, ctx);
}

TOperatorPtr PushDownSemiJoins(TOperatorPtr root) {
    return PushDown(std::move(root));
}

} // namespace NQdb
