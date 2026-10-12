#include <qdb/plan/passes/runtime_filters.h>

#include <qdb/exec/join_exec.h>
#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/ops/stats.h>
#include <qdb/plan/ops/union.h>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace NQdb {

namespace {

// Work units are relative to one join hash-table probe. These are deliberately
// conservative: the logical pass cannot yet know whether lowering will need an
// extra one-lane hash shuffle or whether the NDV estimate is accurate.
constexpr double SavedRowWork = 2.0; // shuffle transfer + join probe
constexpr double FilterBuildWork = 1.0; // collect hashes + possible Bloom merge
constexpr double FilterProbeWork = 0.75; // lookup + possible one-lane shuffle
constexpr double UpstreamOperatorWork = 0.5; // filter/project work per input row
constexpr double AggregateRowWork = 1.0; // group lookup/update per input row
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

struct TConsumerTarget {
    TOperatorPtr Operator;
    std::vector<std::string> ProbeKeys;
    struct TUpstreamStage {
        TOperatorPtr Operator;
        TOperatorPtr Input;
        std::vector<std::string> ProbeKeys;
        double WorkPerRow = UpstreamOperatorWork;
    };
    std::vector<TUpstreamStage> UpstreamStages;
};

using TConsumerPlacement = std::vector<TConsumerTarget>;

struct TScoredPlacement {
    double Benefit = 0.0;
    TConsumerPlacement Targets;
};

struct TFilterDecision {
    EJoinFilterSide BuildSide;
    TConsumerPlacement Targets;
    double Benefit = 0.0;
};

std::vector<TConsumerPlacement> PlacementCandidates(
    TOperatorPtr target, std::vector<std::string> probeKeys,
    std::optional<TKeyEstimate> buildKey = std::nullopt);
void AttachPlacement(const TConsumerPlacement& placement, uint32_t id);
void RemoveFilterUse(const TOperatorPtr& root, uint32_t id);
void ReuseRuntimeFilters(const TOperatorPtr& root);

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
    const TStatsPtr& stats, const std::vector<std::string>& keys)
{
    if (!stats || keys.empty()) {
        return std::nullopt;
    }
    const double rows = static_cast<double>(stats->RowCount);
    double ndv = 1.0;
    // Exact per-column NDVs do not describe correlations in a composite key.
    bool exact = keys.size() == 1;
    for (const auto& key : keys) {
        auto it = stats->ColumnStats.find(key);
        if (it == stats->ColumnStats.end() || !it->second->Ndv) {
            // Guessing here could introduce an unprofitable build barrier.
            return std::nullopt;
        }
        ndv *= std::max(1.0, static_cast<double>(*it->second->Ndv));
        exact &= it->second->NdvIsExact;
        // A row-count cap on inherited column stats is only an upper bound.
        exact &= ndv <= rows;
        ndv = std::min(ndv, rows);
    }
    return TKeyEstimate{ndv, exact};
}

std::vector<std::string> JoinSideKeys(
    const TJoinOperator& join, EJoinFilterSide side)
{
    std::vector<std::string> names;
    names.reserve(join.Keys().size());
    for (const auto& key : join.Keys()) {
        names.push_back(side == EJoinFilterSide::Left ? key.Left : key.Right);
    }
    return names;
}

double PassFraction(TKeyEstimate buildKey, TKeyEstimate probeKey) {
    const double uncertainty = buildKey.Exact && probeKey.Exact
        ? 1.0 : InexactNdvSafetyFactor;
    return std::min(1.0,
        uncertainty * buildKey.Ndv / std::max(1.0, probeKey.Ndv));
}

std::optional<double> EstimatedPassFraction(double buildRows, double probeRows,
    TKeyEstimate buildKey, TKeyEstimate probeKey)
{
    if (buildRows > NdvCostPathMaxBuildRows || buildRows > probeRows) {
        return std::nullopt;
    }

    // With unknown overlap, the NDV ratio estimates the share of probe rows
    // that may match. Widen it when either NDV is approximate so optimism does
    // not force a join into a build-first mode for a weak filter.
    return PassFraction(buildKey, probeKey);
}

std::vector<std::string> ProbeKeys(
    const TJoinOperator& join, EJoinFilterSide buildSide)
{
    return JoinSideKeys(join, buildSide == EJoinFilterSide::Left
        ? EJoinFilterSide::Right : EJoinFilterSide::Left);
}

TOperatorPtr ProbeInput(
    const TJoinOperator& join, EJoinFilterSide buildSide)
{
    return buildSide == EJoinFilterSide::Left
        ? join.Right() : join.Left();
}

std::optional<double> PlacementRows(const TConsumerPlacement& placement) {
    double rows = 0.0;
    for (const auto& target : placement) {
        if (!target.Operator->Stats_) {
            return std::nullopt;
        }
        rows += static_cast<double>(target.Operator->Stats_->RowCount);
    }
    return rows;
}

std::optional<double> PlacementVariableBenefit(
    const TConsumerPlacement& placement,
    std::optional<TKeyEstimate> buildKey)
{
    double benefit = 0.0;
    std::unordered_set<const IOperator*> creditedStages;
    for (const auto& target : placement) {
        if (!target.Operator->Stats_) {
            return std::nullopt;
        }
        benefit -= static_cast<double>(target.Operator->Stats_->RowCount)
            * FilterProbeWork;
        if (!buildKey) {
            continue;
        }
        // A consumer below a filter or project avoids that operator's work
        // for rows rejected at its input. Use the key NDV at each input: the
        // join input can have a different key distribution after filtering.
        for (const auto& stage : target.UpstreamStages) {
            if (!creditedStages.insert(stage.Operator.get()).second
                || !stage.Input->Stats_) {
                continue;
            }
            auto key = KeyNdv(stage.Input->Stats_, stage.ProbeKeys);
            if (!key) {
                continue;
            }
            const double rows = static_cast<double>(stage.Input->Stats_->RowCount);
            benefit += rows * (1.0 - PassFraction(*buildKey, *key))
                * stage.WorkPerRow;
        }
    }
    return benefit;
}

std::optional<TScoredPlacement> BestScoredPlacement(
    const TOperatorPtr& probe, const std::vector<std::string>& probeKeys,
    std::optional<TKeyEstimate> buildKey, double baseBenefit)
{
    std::optional<TScoredPlacement> best;
    for (auto& placement : PlacementCandidates(probe, probeKeys, buildKey)) {
        const auto variableBenefit = PlacementVariableBenefit(placement, buildKey);
        if (!variableBenefit) {
            continue;
        }
        const double benefit = baseBenefit + *variableBenefit;
        // Prefer the earlier consumer when its estimated cost is equal.
        if (benefit > 0.0 && (!best || benefit >= best->Benefit)) {
            best = TScoredPlacement{
                .Benefit = benefit, .Targets = std::move(placement)};
        }
    }
    return best;
}

std::optional<TScoredPlacement> BestPlacement(
    const TJoinOperator& join, EJoinFilterSide buildSide,
    double passFraction, std::optional<TKeyEstimate> buildKey,
    bool includeBuildPenalty = true)
{
    const auto probe = ProbeInput(join, buildSide);
    const auto build = buildSide == EJoinFilterSide::Left
        ? join.Left() : join.Right();
    const double buildRows = static_cast<double>(build->Stats_->RowCount);
    const double joinProbeRows = static_cast<double>(probe->Stats_->RowCount);
    const double saved = joinProbeRows * (1.0 - passFraction) * SavedRowWork;
    const double buildCost = buildRows * FilterBuildWork
        + (!includeBuildPenalty || joinProbeRows >= buildRows * JoinAsymmetryRatio
            ? 0.0 : buildRows * ForcedBuildWork);
    return BestScoredPlacement(probe, ProbeKeys(join, buildSide), buildKey,
        saved - buildCost);
}

std::optional<TScoredPlacement> BestReusePlacement(
    const TOperatorPtr& probe, const std::vector<std::string>& probeKeys,
    double buildRows, double passFraction, TKeyEstimate buildKey)
{
    const double probeRows = static_cast<double>(probe->Stats_->RowCount);
    const double saved = probeRows * (1.0 - passFraction) * SavedRowWork;
    // The Bloom is already built for another join. A new consumer can still
    // delay this branch while it waits for the producer to publish.
    const double waitCost = probeRows >= buildRows * JoinAsymmetryRatio
        ? 0.0 : buildRows * ForcedBuildWork;
    return BestScoredPlacement(probe, probeKeys, buildKey, saved - waitCost);
}

std::optional<TFilterDecision> ChooseRuntimeFilter(
    const TJoinOperator& join, bool force)
{
    if (join.Keys().empty()) {
        return std::nullopt;
    }
    const auto type = join.JoinType();
    const bool semi = type == EJoinType::LeftSemi;
    const bool leftOuter = type == EJoinType::Left;
    // A left outer join may filter only its nullable right input, using keys
    // from the preserved left input. Anti joins need a different rule.
    if (type != EJoinType::Inner && !semi && !leftOuter) {
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
        const auto side = leftOuter
            || (!semi && leftStats->RowCount < rightStats->RowCount)
            ? EJoinFilterSide::Left : EJoinFilterSide::Right;
        if (semi && rightStats->RowCount > leftStats->RowCount) {
            return std::nullopt;
        }
        auto placements = PlacementCandidates(
            ProbeInput(join, side), ProbeKeys(join, side));
        return TFilterDecision{
            .BuildSide = side, .Targets = std::move(placements.back())};
    }

    const auto leftNdv = KeyNdv(leftStats,
        JoinSideKeys(join, EJoinFilterSide::Left));
    const auto rightNdv = KeyNdv(rightStats,
        JoinSideKeys(join, EJoinFilterSide::Right));

    std::optional<TFilterDecision> decision;
    auto consider = [&](EJoinFilterSide side,
        std::optional<double> passFraction,
        std::optional<TKeyEstimate> buildKey,
        bool includeBuildPenalty = true) {
        if (!passFraction) {
            return;
        }
        auto placement = BestPlacement(join, side, *passFraction, buildKey,
            includeBuildPenalty);
        if (placement && (!decision || placement->Benefit > decision->Benefit)) {
            decision = TFilterDecision{
                .BuildSide = side,
                .Targets = std::move(placement->Targets),
                .Benefit = placement->Benefit};
        }
    };
    if (!leftOuter && leftNdv && rightNdv) {
        consider(EJoinFilterSide::Right, EstimatedPassFraction(
            static_cast<double>(rightStats->RowCount),
            static_cast<double>(leftStats->RowCount),
            *rightNdv, *leftNdv), rightNdv);
    }
    // SEMI can publish only from its key-only right table.
    if (!semi && leftNdv && rightNdv) {
        consider(EJoinFilterSide::Left, EstimatedPassFraction(
            static_cast<double>(leftStats->RowCount),
            static_cast<double>(rightStats->RowCount),
            *leftNdv, *rightNdv), leftNdv);
    }
    if (!decision) {
        const double leftRows = static_cast<double>(leftStats->RowCount);
        const double rightRows = static_cast<double>(rightStats->RowCount);
        if (!leftOuter && FilteredBuildCandidate(
                join.Right(), rightRows, leftRows)) {
            // Filtered-build row estimates can be pessimistic, so the
            // fallback keeps its existing barrier assumption. Placement
            // still rejects a probe site whose cost exceeds even the most
            // optimistic join-side savings.
            consider(EJoinFilterSide::Right, /*passFraction=*/0.0,
                std::nullopt, /*includeBuildPenalty=*/false);
        }
        if (!semi && FilteredBuildCandidate(join.Left(), leftRows, rightRows)) {
            consider(EJoinFilterSide::Left, /*passFraction=*/0.0,
                std::nullopt, /*includeBuildPenalty=*/false);
        }
    }
    return decision;
}

void Attach(const TOperatorPtr& node, uint32_t& nextId, bool force) {
    if (!node) {
        return;
    }
    if (auto join = TMaybeOp<TJoinOperator>(node)) {
        auto decision = ChooseRuntimeFilter(*join.Cast(), force);
        if (decision) {
            const uint32_t id = nextId++;
            join.Cast()->MutableRuntimeFilter() = TRuntimeFilterSpec{
                .Id = id, .BuildSide = decision->BuildSide};
            AttachPlacement(decision->Targets, id);
        } else {
            join.Cast()->MutableRuntimeFilter() = std::nullopt;
        }
    }
    for (const auto& input : node->Inputs()) {
        Attach(input, nextId, force);
    }
}

void ClearFilterUses(const TOperatorPtr& node) {
    node->RuntimeFilterUses.clear();
    for (const auto& input : node->Inputs()) {
        ClearFilterUses(input);
    }
}

} // namespace

void AttachRuntimeFilters(
    const TOperatorPtr& root, uint32_t& nextId, bool force)
{
    ClearFilterUses(root);
    Attach(root, nextId, force);
    if (!force) {
        ReuseRuntimeFilters(root);
    }
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

bool PreservesKeys(const TOperatorPtr& outer, const TOperatorPtr& input,
    const std::vector<std::string>& keys)
{
    return outer->Type && input->Type
        && std::all_of(keys.begin(), keys.end(), [&](const auto& key) {
            return SameKeyType(*outer->OutputColumns(), key,
                *input->OutputColumns(), key);
        });
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

std::optional<std::vector<std::vector<std::string>>> UnionBranchKeys(
    const TUnionAllOperator& un, const std::vector<std::string>& probeKeys)
{
    if (!un.Type || un.Inputs().empty()) {
        return std::nullopt;
    }
    const auto output = un.OutputColumns();
    if (!output) {
        return std::nullopt;
    }
    std::vector<size_t> keyPositions;
    keyPositions.reserve(probeKeys.size());
    for (const auto& key : probeKeys) {
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

    std::vector<std::vector<std::string>> branches;
    branches.reserve(un.Inputs().size());
    for (const auto& branch : un.Inputs()) {
        if (!branch->Type) {
            return std::nullopt;
        }
        const auto schema = branch->OutputColumns();
        if (!schema || schema->Fields.size() != output->Fields.size()) {
            return std::nullopt;
        }
        std::vector<std::string> translated;
        translated.reserve(keyPositions.size());
        for (size_t position : keyPositions) {
            const auto& outputField = output->Fields[position];
            const auto& branchField = schema->Fields[position];
            if (!outputField.second || !branchField.second
                || outputField.second->ToString()
                    != branchField.second->ToString()) {
                return std::nullopt;
            }
            translated.push_back(branchField.first);
        }
        branches.push_back(std::move(translated));
    }
    return branches;
}

TOperatorPtr InnerJoinKeyInput(const TJoinOperator& join,
    const std::vector<std::string>& keys)
{
    if (join.JoinType() != EJoinType::Inner || !join.Type
        || !join.Left()->Type || !join.Right()->Type) {
        return nullptr;
    }
    const auto output = join.OutputColumns();
    const auto left = join.Left()->OutputColumns();
    const auto right = join.Right()->OutputColumns();
    if (!output || !left || !right) {
        return nullptr;
    }

    TOperatorPtr input;
    for (const auto& key : keys) {
        const bool fromLeft = SameKeyType(*output, key, *left, key);
        const bool fromRight = SameKeyType(*output, key, *right, key);
        if (fromLeft == fromRight) {
            return nullptr;
        }
        auto side = fromLeft ? join.Left() : join.Right();
        if (input && input != side) {
            return nullptr;
        }
        input = std::move(side);
    }
    return input;
}

void AppendUpstreamCandidates(std::vector<TConsumerPlacement>& candidates,
    const TOperatorPtr& target, const TOperatorPtr& input,
    const std::vector<std::string>& inputKeys,
    std::optional<TKeyEstimate> buildKey,
    double workPerRow = UpstreamOperatorWork)
{
    auto deeper = PlacementCandidates(input, inputKeys, buildKey);
    for (auto& placement : deeper) {
        for (auto& consumer : placement) {
            consumer.UpstreamStages.push_back({
                .Operator = target, .Input = input,
                .ProbeKeys = inputKeys, .WorkPerRow = workPerRow});
        }
    }
    candidates.insert(candidates.end(),
        std::make_move_iterator(deeper.begin()),
        std::make_move_iterator(deeper.end()));
}

std::vector<TConsumerPlacement> PlacementCandidates(
    TOperatorPtr target, std::vector<std::string> probeKeys,
    std::optional<TKeyEstimate> buildKey)
{
    std::vector<TConsumerPlacement> candidates{
        TConsumerPlacement{{.Operator = target, .ProbeKeys = probeKeys}}};
    if (auto predicate = TMaybeOp<TFilterOperator>(target)) {
        auto input = predicate.Cast()->Input();
        if (PreservesKeys(target, input, probeKeys)) {
            AppendUpstreamCandidates(candidates, target, input, probeKeys,
                buildKey);
        }
    } else if (auto project = TMaybeOp<TProjectOperator>(target)) {
        auto inputKeys = ProjectInputKeys(*project.Cast(), probeKeys);
        if (inputKeys) {
            AppendUpstreamCandidates(candidates, target,
                project.Cast()->Input(), *inputKeys, buildKey);
        }
    } else if (auto aggregate = TMaybeOp<TAggregateOperator>(target)) {
        const auto& groupKeys = aggregate.Cast()->GroupKeys();
        auto input = aggregate.Cast()->Input();
        // Removing an entire group is safe only when the membership test uses
        // group keys. Grouping sets can emit a key as NULL even for input rows
        // with a non-NULL value, so keep their consumer above the aggregate.
        if (aggregate.Cast()->GroupingSets().empty()
            && PreservesKeys(target, input, probeKeys)
            && std::all_of(probeKeys.begin(), probeKeys.end(),
                [&](const auto& key) {
                    return std::find(groupKeys.begin(), groupKeys.end(), key)
                        != groupKeys.end();
                })) {
            AppendUpstreamCandidates(candidates, target, input, probeKeys,
                buildKey, AggregateRowWork);
        }
    } else if (auto un = TMaybeOp<TUnionAllOperator>(target)) {
        auto branches = UnionBranchKeys(*un.Cast(), probeKeys);
        if (branches) {
            TConsumerPlacement bestBranches;
            TConsumerPlacement fanout;
            for (size_t i = 0; i < branches->size(); ++i) {
                auto branch = PlacementCandidates(
                    un.Cast()->Inputs()[i],
                    std::move((*branches)[i]), buildKey);
                size_t bestIndex = 0;
                std::optional<double> bestScore;
                for (size_t candidate = 0; candidate < branch.size(); ++candidate) {
                    const auto score = buildKey
                        ? PlacementVariableBenefit(branch[candidate], buildKey)
                        : PlacementRows(branch[candidate]).transform(
                            [](double rows) { return -rows; });
                    if (score && (!bestScore || *score >= *bestScore)) {
                        bestScore = score;
                        bestIndex = candidate;
                    }
                }
                const auto& branchBest = branch[bestIndex];
                bestBranches.insert(bestBranches.end(),
                    branchBest.begin(), branchBest.end());
                auto& deepest = branch.back();
                fanout.insert(fanout.end(),
                    std::make_move_iterator(deepest.begin()),
                    std::make_move_iterator(deepest.end()));
            }
            candidates.push_back(std::move(bestBranches));
            candidates.push_back(std::move(fanout));
        }
    } else if (auto join = TMaybeOp<TJoinOperator>(target)) {
        auto input = InnerJoinKeyInput(*join.Cast(), probeKeys);
        if (input) {
            auto deeper = PlacementCandidates(
                std::move(input), std::move(probeKeys), buildKey);
            candidates.insert(candidates.end(),
                std::make_move_iterator(deeper.begin()),
                std::make_move_iterator(deeper.end()));
        }
    }
    return candidates;
}

void AttachPlacement(const TConsumerPlacement& placement, uint32_t id) {
    for (const auto& target : placement) {
        if (!std::any_of(target.Operator->RuntimeFilterUses.begin(),
                target.Operator->RuntimeFilterUses.end(),
                [id](const auto& existing) { return existing.Id == id; })) {
            target.Operator->RuntimeFilterUses.push_back(
                TRuntimeFilterUseSpec{
                    .Id = id, .ProbeKeys = target.ProbeKeys});
        }
    }
}

void RemoveFilterUse(const TOperatorPtr& root, uint32_t id) {
    std::erase_if(root->RuntimeFilterUses,
        [id](const auto& use) { return use.Id == id; });
    for (const auto& input : root->Inputs()) {
        RemoveFilterUse(input, id);
    }
}

struct TReusablePlacement {
    uint32_t Id;
    TScoredPlacement Placement;
};

void FindReusableFilter(const TOperatorPtr& node,
    const std::vector<std::string>& inheritedKeys,
    const TJoinOperator& consumerJoin,
    std::optional<TReusablePlacement>& best)
{
    if (auto filter = TMaybeOp<TFilterOperator>(node)) {
        auto input = filter.Cast()->Input();
        if (PreservesKeys(node, input, inheritedKeys)) {
            FindReusableFilter(input, inheritedKeys, consumerJoin, best);
        }
        return;
    }
    if (auto project = TMaybeOp<TProjectOperator>(node)) {
        if (auto inputKeys = ProjectInputKeys(*project.Cast(), inheritedKeys)) {
            FindReusableFilter(
                project.Cast()->Input(), *inputKeys, consumerJoin, best);
        }
        return;
    }
    auto ancestor = TMaybeOp<TJoinOperator>(node);
    if (!ancestor || ancestor.Cast()->JoinType() != EJoinType::Left
        || !node->Type || !ancestor.Cast()->Left()->Type
        || !ancestor.Cast()->Right()->Type
        || !consumerJoin.Right()->Type) {
        return;
    }
    const auto& left = ancestor.Cast()->Left();
    // The nullable right input cannot supply membership information for a
    // later left join. Continue only along keys preserved from the left.
    if (!std::all_of(inheritedKeys.begin(), inheritedKeys.end(),
            [&](const auto& key) {
                return SameKeyType(*node->OutputColumns(), key,
                        *left->OutputColumns(), key)
                    && !FieldType(
                        ancestor.Cast()->Right()->OutputColumns().get(), key);
            })) {
        return;
    }

    const auto& producer = ancestor.Cast()->RuntimeFilter();
    if (producer && producer->BuildSide == EJoinFilterSide::Left
        && ancestor.Cast()->Keys().size() == inheritedKeys.size()
        && left->Stats_ && consumerJoin.Right()->Stats_
        && left->Stats_->RowCount && consumerJoin.Right()->Stats_->RowCount) {
        std::vector<std::string> probeKeys;
        std::vector<bool> matched(inheritedKeys.size(), false);
        bool compatible = true;
        for (const auto& key : ancestor.Cast()->Keys()) {
            auto pos = std::find(inheritedKeys.begin(), inheritedKeys.end(), key.Left);
            if (pos == inheritedKeys.end()
                || std::find(std::next(pos), inheritedKeys.end(), key.Left)
                    != inheritedKeys.end()) {
                compatible = false;
                break;
            }
            const size_t index = static_cast<size_t>(
                std::distance(inheritedKeys.begin(), pos));
            const auto& laterKey = consumerJoin.Keys()[index];
            if (matched[index] || key.NullsEqual != laterKey.NullsEqual
                || !SameKeyType(*ancestor.Cast()->Right()->OutputColumns(),
                    key.Right, *consumerJoin.Right()->OutputColumns(),
                    laterKey.Right)) {
                compatible = false;
                break;
            }
            matched[index] = true;
            probeKeys.push_back(laterKey.Right);
        }
        if (compatible) {
            auto buildNdv = KeyNdv(left->Stats_,
                JoinSideKeys(*ancestor.Cast(), EJoinFilterSide::Left));
            auto probeNdv = KeyNdv(consumerJoin.Right()->Stats_, probeKeys);
            if (buildNdv && probeNdv) {
                const double buildRows = static_cast<double>(left->Stats_->RowCount);
                const double probeRows = static_cast<double>(
                    consumerJoin.Right()->Stats_->RowCount);
                auto pass = EstimatedPassFraction(
                    buildRows, probeRows, *buildNdv, *probeNdv);
                if (pass) {
                    auto placement = BestReusePlacement(consumerJoin.Right(),
                        probeKeys, buildRows, *pass, *buildNdv);
                    if (placement && (!best
                            || placement->Benefit > best->Placement.Benefit)) {
                        best = TReusablePlacement{
                            .Id = producer->Id,
                            .Placement = std::move(*placement)};
                    }
                }
            }
        }
    }
    FindReusableFilter(left, inheritedKeys, consumerJoin, best);
}

void ReuseRuntimeFilters(const TOperatorPtr& root) {
    for (const auto& input : root->Inputs()) {
        ReuseRuntimeFilters(input);
    }
    auto join = TMaybeOp<TJoinOperator>(root);
    if (!join || join.Cast()->JoinType() != EJoinType::Left
        || join.Cast()->Keys().empty()) {
        return;
    }
    auto leftKeys = JoinSideKeys(*join.Cast(), EJoinFilterSide::Left);
    std::optional<TReusablePlacement> best;
    FindReusableFilter(join.Cast()->Left(), leftKeys, *join.Cast(), best);
    if (best) {
        if (const auto& own = join.Cast()->RuntimeFilter()) {
            auto ownDecision = ChooseRuntimeFilter(*join.Cast(), false);
            if (ownDecision && ownDecision->Benefit >= best->Placement.Benefit) {
                return;
            }
            RemoveFilterUse(root, own->Id);
            join.Cast()->MutableRuntimeFilter() = std::nullopt;
        }
        AttachPlacement(best->Placement.Targets, best->Id);
    }
}

} // namespace

void PlaceRuntimeFilterConsumers(const TOperatorPtr& root) {
    for (const auto& child : root->Inputs()) {
        PlaceRuntimeFilterConsumers(child);
    }
    auto join = TMaybeOp<TJoinOperator>(root);
    if (!join || !join.Cast()->RuntimeFilter()) {
        return;
    }
    const auto& filter = *join.Cast()->RuntimeFilter();
    const auto type = join.Cast()->JoinType();
    if (!((type == EJoinType::Inner)
            || (type == EJoinType::LeftSemi
                && !join.Cast()->Filter()
                && filter.BuildSide == EJoinFilterSide::Right)
            || (type == EJoinType::Left
                && filter.BuildSide == EJoinFilterSide::Left))) {
        throw std::runtime_error("runtime filter build side is unsafe for join type");
    }
    auto probe = ProbeInput(*join.Cast(), filter.BuildSide);
    if (HasFilterUse(probe, filter.Id)) {
        return;
    }

    auto candidates = PlacementCandidates(probe,
        ProbeKeys(*join.Cast(), filter.BuildSide));
    AttachPlacement(candidates.back(), filter.Id);
}

} // namespace NQdb
