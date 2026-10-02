#include <qdb/plan/passes/runtime_filters.h>

#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/stats.h>

#include <algorithm>
#include <cstdint>
#include <optional>

namespace NQdb {

using NQumir::NAst::TMaybeNode;

namespace {

constexpr double RuntimeFilterEmitRatio = 3.0;
constexpr double RuntimeFilterMaxBuildKeys = 2'000'000.0;

std::optional<double> KeyNdv(
    const TStatsPtr& stats,
    const std::vector<TJoinKey>& keys,
    bool leftSide)
{
    const double rows = static_cast<double>(stats->RowCount);
    double ndv = 1.0;
    for (const auto& key : keys) {
        auto it = stats->ColumnStats.find(leftSide ? key.Left : key.Right);
        if (it == stats->ColumnStats.end() || !it->second->Ndv) {
            // Guessing here could introduce an unprofitable build barrier.
            return std::nullopt;
        }
        ndv *= std::max(1.0, static_cast<double>(*it->second->Ndv));
        if (ndv >= rows) {
            return rows;
        }
    }
    return std::min(ndv, rows);
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
    // Forced: fall back to row counts, which are exact in the file's stats,
    // and take the smaller side. Distinct keys would be the right measure, but
    // the NDV that would report them is saturated.
    const auto leftNdv = force
        ? std::optional<double>(static_cast<double>(leftStats->RowCount))
        : KeyNdv(leftStats, join.Keys(), /*leftSide=*/true);
    const auto rightNdv = force
        ? std::optional<double>(static_cast<double>(rightStats->RowCount))
        : KeyNdv(rightStats, join.Keys(), /*leftSide=*/false);
    if (!leftNdv || !rightNdv) {
        return std::nullopt;
    }

    // NDV ratio estimates how many probe keys the filter can reject.
    const double ratio = force ? 1.0 : RuntimeFilterEmitRatio;
    std::optional<EJoinFilterSide> side;
    if (*rightNdv * ratio <= *leftNdv) {
        side = EJoinFilterSide::Right;
    } else if (!semi && *leftNdv * ratio <= *rightNdv) {
        // SEMI can publish only from its key-only right table.
        side = EJoinFilterSide::Left;
    }
    if (!side) {
        return std::nullopt;
    }
    const double buildKeys =
        *side == EJoinFilterSide::Right ? *rightNdv : *leftNdv;
    if (!force && buildKeys > RuntimeFilterMaxBuildKeys) {
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
