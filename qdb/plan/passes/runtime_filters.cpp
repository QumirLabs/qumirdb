#include <qdb/plan/passes/runtime_filters.h>

#include <qdb/exec/join_exec.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/ops/stats.h>

#include <algorithm>
#include <cstdint>
#include <optional>

namespace NQdb {

using NQumir::NAst::TMaybeNode;

namespace {

// Work units are relative to one join hash-table probe. These are deliberately
// conservative: the logical pass cannot yet know whether lowering will need an
// extra one-lane hash shuffle or whether the NDV estimate is accurate.
constexpr double SavedRowWork = 2.0; // shuffle transfer + join probe
constexpr double FilterBuildWork = 1.0; // collect hashes + possible Bloom merge
constexpr double FilterProbeWork = 0.75; // lookup + possible one-lane shuffle
constexpr double ForcedBuildWork = 2.0; // blocking a join that would run Auto
constexpr double InexactNdvSafetyFactor = 2.0;
// The NDV cost path bounds unfiltered builds. Filtered builds have a separate
// fallback below, since their row estimates can be pessimistic.
constexpr double NdvCostPathMaxBuildRows = 2'000'000.0;
constexpr double FilteredBuildMaxProbeRatio = 0.6;
constexpr double FilteredBuildMinProbeRows = 1'000'000.0;

struct TKeyEstimate {
    double Ndv;
    bool Exact;
};

bool HasFilteredBuildInput(const TOperatorPtr& op) {
    if (TMaybeOp<TFilterOperator>(op)) {
        return true;
    }
    if (auto source = TMaybeOp<TSourceOperator>(op)) {
        return static_cast<bool>(source.Cast()->RowGroupPredicate());
    }
    if (op->Inputs().size() > 1) {
        auto join = TMaybeOp<TJoinOperator>(op);
        if (!join || (join.Cast()->JoinType() != EJoinType::Inner
                && join.Cast()->JoinType() != EJoinType::LeftSemi)) {
            return false;
        }
    }
    for (const auto& child : op->Inputs()) {
        if (HasFilteredBuildInput(child)) {
            return true;
        }
    }
    return false;
}

bool FilteredBuildCandidate(const TOperatorPtr& build,
    double buildRows, double probeRows)
{
    // A filter or HAVING below the build can make row-count estimates very
    // pessimistic. Admit a smaller, explicitly filtered build and let the
    // runtime probe pause if the published filter has weak selectivity.
    return probeRows >= FilteredBuildMinProbeRows
        && buildRows <= probeRows * FilteredBuildMaxProbeRatio
        && HasFilteredBuildInput(build);
}

std::optional<TKeyEstimate> KeyNdv(
    const TStatsPtr& stats,
    const std::vector<TJoinKey>& keys,
    bool leftSide)
{
    const double rows = static_cast<double>(stats->RowCount);
    double ndv = 1.0;
    // Exact per-column NDVs do not describe correlations in a composite key.
    bool exact = keys.size() == 1;
    for (const auto& key : keys) {
        auto it = stats->ColumnStats.find(leftSide ? key.Left : key.Right);
        if (it == stats->ColumnStats.end() || !it->second->Ndv) {
            // Guessing here could introduce an unprofitable build barrier.
            return std::nullopt;
        }
        ndv *= std::max(1.0, static_cast<double>(*it->second->Ndv));
        exact &= it->second->NdvIsExact;
        ndv = std::min(ndv, rows);
    }
    return TKeyEstimate{ndv, exact};
}

std::optional<double> EstimatedBenefit(double buildRows, double probeRows,
    TKeyEstimate buildKey, TKeyEstimate probeKey)
{
    if (buildRows > NdvCostPathMaxBuildRows || buildRows > probeRows) {
        return std::nullopt;
    }

    // With unknown overlap, the NDV ratio estimates the share of probe rows
    // that may match. Widen it when either NDV is approximate so optimism does
    // not force a join into a build-first mode for a weak filter.
    const double uncertainty = buildKey.Exact && probeKey.Exact
        ? 1.0 : InexactNdvSafetyFactor;
    const double passFraction = std::min(1.0,
        uncertainty * buildKey.Ndv / std::max(1.0, probeKey.Ndv));
    const double saved = probeRows * (1.0 - passFraction) * SavedRowWork;
    const double filterCost = buildRows * FilterBuildWork
        + probeRows * FilterProbeWork;
    const double buildPenalty = probeRows >= buildRows * JoinAsymmetryRatio
        ? 0.0 : buildRows * ForcedBuildWork;
    const double net = saved - filterCost - buildPenalty;
    return net > 0.0 ? std::optional(net) : std::nullopt;
}

std::optional<TRuntimeFilterSpec> ChooseRuntimeFilter(
    const TJoinOperator& join, uint32_t& nextId, bool force)
{
    if (join.Keys().empty()) {
        return std::nullopt;
    }
    const auto type = join.JoinType();
    const bool semi = type == EJoinType::LeftSemi;
    // Outer and anti joins preserve non-matches.
    if (type != EJoinType::Inner && !semi) {
        return std::nullopt;
    }
    // Residual SEMI builds the left, but filters are published from the right.
    if (semi && join.Filter()) {
        return std::nullopt;
    }

    const auto& leftStats = join.Left()->Stats_;
    const auto& rightStats = join.Right()->Stats_;
    if (!leftStats || !rightStats
        || leftStats->RowCount == 0 || rightStats->RowCount == 0) {
        return std::nullopt;
    }
    // Measurement mode retains the structural rules but bypasses estimates.
    if (force) {
        const auto side = !semi && leftStats->RowCount < rightStats->RowCount
            ? EJoinFilterSide::Left : EJoinFilterSide::Right;
        if (semi && rightStats->RowCount > leftStats->RowCount) {
            return std::nullopt;
        }
        return TRuntimeFilterSpec{.Id = nextId++, .BuildSide = side};
    }

    const auto leftNdv = KeyNdv(leftStats, join.Keys(), /*leftSide=*/true);
    const auto rightNdv = KeyNdv(rightStats, join.Keys(), /*leftSide=*/false);

    std::optional<EJoinFilterSide> side;
    double bestBenefit = 0.0;
    if (leftNdv && rightNdv) {
        if (auto benefit = EstimatedBenefit(
                static_cast<double>(rightStats->RowCount),
                static_cast<double>(leftStats->RowCount),
                *rightNdv, *leftNdv)) {
            side = EJoinFilterSide::Right;
            bestBenefit = *benefit;
        }
    }
    // SEMI can publish only from its key-only right table.
    if (!semi) {
        if (leftNdv && rightNdv) {
            if (auto benefit = EstimatedBenefit(
                    static_cast<double>(leftStats->RowCount),
                    static_cast<double>(rightStats->RowCount),
                    *leftNdv, *rightNdv);
                benefit && *benefit > bestBenefit) {
                side = EJoinFilterSide::Left;
            }
        }
    }
    if (!side) {
        const double leftRows = static_cast<double>(leftStats->RowCount);
        const double rightRows = static_cast<double>(rightStats->RowCount);
        if (FilteredBuildCandidate(join.Right(), rightRows, leftRows)) {
            side = EJoinFilterSide::Right;
        }
        if (!semi && FilteredBuildCandidate(join.Left(), leftRows, rightRows)
            && (!side || leftRows < rightRows)) {
            side = EJoinFilterSide::Left;
        }
    }
    if (!side) {
        return std::nullopt;
    }
    return TRuntimeFilterSpec{.Id = nextId++, .BuildSide = *side};
}

void Attach(const TOperatorPtr& node, uint32_t& nextId, bool force) {
    if (!node) {
        return;
    }
    if (auto join = TMaybeOp<TJoinOperator>(node)) {
        join.Cast()->MutableRuntimeFilter() =
            ChooseRuntimeFilter(*join.Cast(), nextId, force);
    }
    for (const auto& child : node->Children()) {
        if (auto op = TMaybeNode<IOperator>(child)) {
            Attach(op.Cast(), nextId, force);
        }
    }
}

} // namespace

void AttachRuntimeFilters(
    const TOperatorPtr& root, uint32_t& nextId, bool force)
{
    Attach(root, nextId, force);
}

namespace {

bool HasFilterUse(const TOperatorPtr& root, uint32_t id) {
    if (std::any_of(root->RuntimeFilterUses.begin(), root->RuntimeFilterUses.end(),
            [id](const auto& use) { return use.Id == id; })) {
        return true;
    }
    for (const auto& child : root->Inputs()) {
        if (HasFilterUse(child, id)) {
            return true;
        }
    }
    return false;
}

void PlaceConsumers(const TOperatorPtr& root) {
    for (const auto& child : root->Inputs()) {
        PlaceConsumers(child);
    }
    auto join = TMaybeOp<TJoinOperator>(root);
    if (!join || !join.Cast()->RuntimeFilter()) {
        return;
    }
    const auto& filter = *join.Cast()->RuntimeFilter();
    auto probe = filter.BuildSide == EJoinFilterSide::Left
        ? join.Cast()->Right() : join.Cast()->Left();
    if (HasFilterUse(probe, filter.Id)) {
        return;
    }

    auto target = probe;
    while (auto predicate = TMaybeOp<TFilterOperator>(target)) {
        target = predicate.Cast()->Input();
    }
    auto source = TMaybeOp<TSourceOperator>(target);
    const auto probeSchema = probe->Type ? probe->OutputColumns() : nullptr;
    const auto sourceSchema = source && source.Cast()->Type
        ? source.Cast()->OutputColumns() : nullptr;
    const bool sameSchema = sourceSchema && probeSchema
        && sourceSchema->Fields.size() == probeSchema->Fields.size()
        && std::equal(sourceSchema->Fields.begin(), sourceSchema->Fields.end(),
            probeSchema->Fields.begin(),
            [](const auto& a, const auto& b) { return a.first == b.first; });
    if (!sameSchema) {
        target = probe;
    }

    TRuntimeFilterUseSpec use{.Id = filter.Id};
    use.ProbeKeys.reserve(join.Cast()->Keys().size());
    for (const auto& key : join.Cast()->Keys()) {
        use.ProbeKeys.push_back(filter.BuildSide == EJoinFilterSide::Left
            ? key.Right : key.Left);
    }
    target->RuntimeFilterUses.push_back(std::move(use));
}

} // namespace

void PlaceRuntimeFilterConsumers(const TOperatorPtr& root) {
    PlaceConsumers(root);
}

} // namespace NQdb
