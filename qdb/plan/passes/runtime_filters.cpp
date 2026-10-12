#include <qdb/plan/passes/runtime_filters.h>

#include <qdb/exec/join_exec.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/ops/stats.h>
#include <qdb/plan/ops/union.h>

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

bool SameKeyType(const NQumir::NAst::TStructType& outer,
    const std::string& outerKey, const NQumir::NAst::TStructType& inner,
    const std::string& innerKey)
{
    const auto outerType = FieldType(&outer, outerKey);
    const auto innerType = FieldType(&inner, innerKey);
    return outerType && innerType
        && outerType->ToString() == innerType->ToString();
}

std::optional<std::vector<std::string>> ProjectInputKeys(
    const TProjectOperator& project, const std::vector<std::string>& outputKeys)
{
    if (!project.Type || !project.Input()->Type) {
        return std::nullopt;
    }
    const auto output = project.OutputColumns();
    const auto input = project.Input()->OutputColumns();
    std::vector<std::string> inputKeys;
    inputKeys.reserve(outputKeys.size());
    for (const auto& key : outputKeys) {
        size_t field = 0;
        bool found = false;
        for (const auto& projection : project.Projections()) {
            const size_t arity = FlattenedProjectionArity(projection);
            if (arity == 1 && field < output->Fields.size()
                && output->Fields[field].first == key) {
                auto ident = NQumir::NAst::TMaybeNode<
                    NQumir::NAst::TIdentExpr>(projection.Expression);
                if (!ident || !SameKeyType(*output, key, *input,
                        ident.Cast()->Name)) {
                    return std::nullopt;
                }
                inputKeys.push_back(ident.Cast()->Name);
                found = true;
                break;
            }
            field += arity;
        }
        if (!found) {
            return std::nullopt;
        }
    }
    return inputKeys;
}

std::optional<std::vector<TRuntimeFilterUseSpec>> UnionBranchUses(
    const TUnionAllOperator& un, const TRuntimeFilterUseSpec& use)
{
    if (!un.Type || un.Inputs().empty()) {
        return std::nullopt;
    }
    const auto output = un.OutputColumns();
    if (!output) {
        return std::nullopt;
    }
    std::vector<size_t> keyPositions;
    keyPositions.reserve(use.ProbeKeys.size());
    for (const auto& key : use.ProbeKeys) {
        const auto field = std::find_if(output->Fields.begin(),
            output->Fields.end(), [&](const auto& item) {
                return item.first == key;
            });
        if (field == output->Fields.end()) {
            return std::nullopt;
        }
        keyPositions.push_back(static_cast<size_t>(
            std::distance(output->Fields.begin(), field)));
    }

    std::vector<TRuntimeFilterUseSpec> branches;
    branches.reserve(un.Inputs().size());
    for (const auto& branch : un.Inputs()) {
        if (!branch->Type) {
            return std::nullopt;
        }
        const auto schema = branch->OutputColumns();
        if (!schema || schema->Fields.size() != output->Fields.size()) {
            return std::nullopt;
        }
        TRuntimeFilterUseSpec translated{.Id = use.Id};
        translated.ProbeKeys.reserve(keyPositions.size());
        for (size_t position : keyPositions) {
            const auto& outputField = output->Fields[position];
            const auto& branchField = schema->Fields[position];
            if (!outputField.second || !branchField.second
                || outputField.second->ToString()
                    != branchField.second->ToString()) {
                return std::nullopt;
            }
            translated.ProbeKeys.push_back(branchField.first);
        }
        branches.push_back(std::move(translated));
    }
    return branches;
}

void PlaceConsumer(TOperatorPtr target, TRuntimeFilterUseSpec use) {
    if (auto predicate = TMaybeOp<TFilterOperator>(target)) {
        auto input = predicate.Cast()->Input();
        if (target->Type && input->Type
            && std::all_of(use.ProbeKeys.begin(), use.ProbeKeys.end(),
                [&](const auto& key) {
                    return SameKeyType(*target->OutputColumns(), key,
                        *input->OutputColumns(), key);
                })) {
            PlaceConsumer(input, std::move(use));
            return;
        }
    } else if (auto project = TMaybeOp<TProjectOperator>(target)) {
        auto inputKeys = ProjectInputKeys(*project.Cast(), use.ProbeKeys);
        if (inputKeys) {
            use.ProbeKeys = std::move(*inputKeys);
            PlaceConsumer(project.Cast()->Input(), std::move(use));
            return;
        }
    } else if (auto un = TMaybeOp<TUnionAllOperator>(target)) {
        auto branches = UnionBranchUses(*un.Cast(), use);
        if (branches) {
            for (size_t i = 0; i < branches->size(); ++i) {
                PlaceConsumer(un.Cast()->Inputs()[i], std::move((*branches)[i]));
            }
            return;
        }
    }
    if (!std::any_of(target->RuntimeFilterUses.begin(),
            target->RuntimeFilterUses.end(),
            [&](const auto& existing) { return existing.Id == use.Id; })) {
        target->RuntimeFilterUses.push_back(std::move(use));
    }
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

    TRuntimeFilterUseSpec use{.Id = filter.Id};
    use.ProbeKeys.reserve(join.Cast()->Keys().size());
    for (const auto& key : join.Cast()->Keys()) {
        use.ProbeKeys.push_back(filter.BuildSide == EJoinFilterSide::Left
            ? key.Right : key.Left);
    }
    PlaceConsumer(probe, std::move(use));
}

} // namespace

void PlaceRuntimeFilterConsumers(const TOperatorPtr& root) {
    PlaceConsumers(root);
}

} // namespace NQdb
