#include <qdb/plan/passes/runtime_filters.h>

#include <qdb/exec/join_exec.h>
#include <qdb/plan/ops/join.h>
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
constexpr double RuntimeFilterMaxBuildRows = 2'000'000.0;
constexpr double RuntimeFilterMaxBuildKeys = 2'000'000.0;

struct TKeyEstimate {
    double Ndv;
    bool Exact;
};

std::optional<TKeyEstimate> KeyNdv(
    const TStatsPtr& stats,
    const std::vector<TJoinKey>& keys,
    bool leftSide)
{
    const double rows = static_cast<double>(stats->RowCount);
    double ndv = 1.0;
    bool exact = true;
    for (const auto& key : keys) {
        auto it = stats->ColumnStats.find(leftSide ? key.Left : key.Right);
        if (it == stats->ColumnStats.end() || !it->second->Ndv) {
            // Guessing here could introduce an unprofitable build barrier.
            return std::nullopt;
        }
        ndv *= std::max(1.0, static_cast<double>(*it->second->Ndv));
        exact &= it->second->NdvIsExact;
        // Filtering can leave the original key domain intact in stats while
        // reducing rows. Capping that domain to rows is an estimate, not an
        // exact distinct count for the filtered side.
        if (ndv > rows) {
            exact = false;
        }
        ndv = std::min(ndv, rows);
    }
    return TKeyEstimate{ndv, exact};
}

std::optional<double> EstimatedBenefit(double buildRows, double probeRows,
    TKeyEstimate buildKey, TKeyEstimate probeKey)
{
    if (buildRows > RuntimeFilterMaxBuildRows
        || buildKey.Ndv > RuntimeFilterMaxBuildKeys
        || buildRows > probeRows) {
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
    if (!leftNdv || !rightNdv) {
        return std::nullopt;
    }

    std::optional<EJoinFilterSide> side;
    double bestBenefit = 0.0;
    if (auto benefit = EstimatedBenefit(
            static_cast<double>(rightStats->RowCount),
            static_cast<double>(leftStats->RowCount),
            *rightNdv, *leftNdv)) {
        side = EJoinFilterSide::Right;
        bestBenefit = *benefit;
    }
    // SEMI can publish only from its key-only right table.
    if (!semi) {
        if (auto benefit = EstimatedBenefit(
                static_cast<double>(leftStats->RowCount),
                static_cast<double>(rightStats->RowCount),
                *leftNdv, *rightNdv);
            benefit && *benefit > bestBenefit) {
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

} // namespace NQdb
