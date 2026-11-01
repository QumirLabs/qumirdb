#include <qdb/exec/plan_builder.h>

#include <qdb/exec/planner_helpers.h>
#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/cte_consumer.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/late_materialize.h>
#include <qdb/plan/ops/limit.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/sort.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/ops/window.h>

#include <algorithm>
#include <ranges>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace NQdb {
namespace {

using TNode = NScheduler::TTaskNode;
using TEdge = NScheduler::TTaskEdge;

struct TBuildResult {
    TExecPlanNodeId NodeId = InvalidExecPlanNodeId;
    NQumir::NAst::TTypePtr OutputType;
};

class TExecPlanBuilder {
public:
    explicit TExecPlanBuilder(const NScheduler::TLoweredPlan& lowered)
        : Lowered_(lowered)
        , Graph_(lowered.Graph.get())
    {}

    std::expected<TExecPlan, std::string> Build() {
        try {
            Initialize();
            RootStageIds_ = RootStages();
            auto root = BuildUnion(RootStageIds_);
            Plan_.Root = root.NodeId;
            return std::move(Plan_);
        } catch (const NQumir::TError& e) {
            return std::unexpected(e.ToString());
        } catch (const std::exception& e) {
            return std::unexpected(e.what());
        }
    }

private:
    void Initialize() {
        if (!Lowered_.Graph) {
            throw std::runtime_error("exec plan builder: missing task graph");
        }
        for (const auto& stage : Lowered_.ExecStages) {
            if (stage.Id == InvalidExecStageId ||
                !Stages_.emplace(stage.Id, &stage).second)
            {
                throw std::runtime_error(
                    "exec plan builder: invalid or duplicate stage id");
            }
        }
        for (const auto& node : Graph_->Nodes()) {
            if (node->ExecStageId == InvalidExecStageId) {
                continue;
            }
            if (!Stages_.contains(node->ExecStageId)) {
                throw std::runtime_error(
                    "exec plan builder: task references an unknown stage id");
            }
            StageNodes_[node->ExecStageId].push_back(node.get());
        }
        for (const auto& [stageId, _] : Stages_) {
            if (!StageNodes_.contains(stageId)) {
                throw std::runtime_error(
                    "exec plan builder: stage has no physical tasks");
            }
        }
        for (const auto& edge : Graph_->Edges()) {
            Inbound_[edge->Dst].push_back(edge.get());
        }
        for (size_t i = 0; i < Lowered_.Kernels.size(); ++i) {
            const auto stageId = Lowered_.Kernels[i].ExecStageId;
            if (stageId == InvalidExecStageId || !Stages_.contains(stageId)) {
                throw std::runtime_error(
                    "exec plan builder: kernel references an unknown stage id");
            }
            KernelIndexes_[stageId].push_back(i);
        }
    }

    void CollectUpstreamStages(
        const TNode* node,
        TExecStageId current,
        std::vector<TExecStageId>& out,
        std::unordered_set<const TNode*>& visited) const
    {
        if (!node || !visited.insert(node).second) {
            return;
        }
        if (node->ExecStageId != InvalidExecStageId &&
            node->ExecStageId != current)
        {
            out.push_back(node->ExecStageId);
            return;
        }
        if (auto it = Inbound_.find(node); it != Inbound_.end()) {
            for (const auto* edge : it->second) {
                if (edge->Kind == NScheduler::ETaskEdgeKind::Dependency) {
                    continue;
                }
                CollectUpstreamStages(edge->Src, current, out, visited);
            }
        }
    }

    static void SortUnique(std::vector<TExecStageId>& ids) {
        std::ranges::sort(ids);
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    }

    // Union branches must line up positionally by column type; SQL takes the
    // first branch's column names, so names may differ. Compare each field's
    // TypeKey (not the whole-struct key, which pins field names).
    static bool SameUnionColumns(
        const NQumir::NAst::TTypePtr& a, const NQumir::NAst::TTypePtr& b)
    {
        using namespace NQumir::NAst;
        auto* sa = a ? static_cast<TStructType*>(a.get()) : nullptr;
        auto* sb = b ? static_cast<TStructType*>(b.get()) : nullptr;
        if (!sa || !sb || sa->Fields.size() != sb->Fields.size()) {
            return false;
        }
        for (size_t i = 0; i < sa->Fields.size(); ++i) {
            if (TypeKey(sa->Fields[i].second) != TypeKey(sb->Fields[i].second)) {
                return false;
            }
        }
        return true;
    }

    std::unordered_map<size_t, std::vector<TExecStageId>> Inputs(
        TExecStageId stageId) const
    {
        std::unordered_map<size_t, std::vector<TExecStageId>> result;
        const auto nodesIt = StageNodes_.find(stageId);
        if (nodesIt == StageNodes_.end()) {
            return result;
        }
        for (const auto* node : nodesIt->second) {
            const auto inboundIt = Inbound_.find(node);
            if (inboundIt == Inbound_.end()) {
                continue;
            }
            for (const auto* edge : inboundIt->second) {
                if (edge->Kind == NScheduler::ETaskEdgeKind::Dependency) {
                    continue;
                }
                std::unordered_set<const TNode*> visited;
                CollectUpstreamStages(
                    edge->Src, stageId, result[edge->ExecInput], visited);
            }
        }
        for (auto& [_, stages] : result) {
            SortUnique(stages);
        }
        std::erase_if(result, [](const auto& item) {
            return item.second.empty();
        });
        return result;
    }

    static size_t Arity(EExecPlanNodeKind kind) {
        switch (kind) {
            case EExecPlanNodeKind::Source:
            case EExecPlanNodeKind::StagedScan:
                return 0;
            case EExecPlanNodeKind::Join:
            case EExecPlanNodeKind::CrossJoin:
                return 2;
            case EExecPlanNodeKind::UnionAll:
                throw std::runtime_error(
                    "exec plan builder: union is not a lowered stage");
            default:
                return 1;
        }
    }

    std::vector<TBuildResult> BuildInputs(const TLoweredExecStage& stage) {
        auto resolved = Inputs(stage.Id);
        const size_t arity = Arity(stage.Kind);
        if (resolved.size() != arity) {
            throw std::runtime_error(
                "exec plan builder: '" +
                std::string(ExecPlanNodeKindName(stage.Kind)) +
                "' expected " + std::to_string(arity) + " inputs, got " +
                std::to_string(resolved.size()));
        }
        std::vector<TBuildResult> inputs;
        inputs.reserve(arity);
        for (size_t input = 0; input < arity; ++input) {
            auto it = resolved.find(input);
            if (it == resolved.end()) {
                throw std::runtime_error(
                    "exec plan builder: missing ordered input " +
                    std::to_string(input));
            }
            inputs.push_back(BuildUnion(it->second));
        }
        return inputs;
    }

    TExecPlanNodeId AddNode(
        const TLoweredExecStage* stage,
        EExecPlanNodeKind kind,
        std::vector<TBuildResult> inputs,
        NQumir::NAst::TTypePtr outputType,
        std::optional<size_t> materialization = std::nullopt,
        std::optional<std::vector<int32_t>> keptColumns = std::nullopt)
    {
        TExecPlanNode node{
            .Id = Plan_.Nodes.size(),
            .StageId = stage ? stage->Id : InvalidExecStageId,
            .Kind = kind,
            .Operator = stage ? stage->Operator : nullptr,
            .OperatorOwner = stage ? stage->OperatorOwner : TOperatorPtr{},
            .AggregatePhase = stage
                ? stage->AggregatePhase
                : EAggregatePhase::Single,
            .RuntimeFilterUse = stage ? stage->RuntimeFilterUse
                : std::nullopt,
            .OutputType = std::move(outputType),
            .MaterializationId = materialization,
            .KeptInputColumns = std::move(keptColumns),
        };
        node.Inputs.reserve(inputs.size());
        for (const auto& input : inputs) {
            node.Inputs.push_back(input.NodeId);
        }
        if (stage) {
            if (auto it = KernelIndexes_.find(stage->Id);
                it != KernelIndexes_.end())
            {
                node.KernelIndexes = it->second;
            }
        }
        const auto id = node.Id;
        Plan_.Nodes.push_back(std::move(node));
        return id;
    }

    TBuildResult BuildUnion(const std::vector<TExecStageId>& stageIds) {
        if (stageIds.empty()) {
            throw std::runtime_error("exec plan builder: union has no inputs");
        }
        if (stageIds.size() == 1) {
            return BuildStage(stageIds.front());
        }
        std::vector<TBuildResult> branches;
        branches.reserve(stageIds.size());
        for (const auto stageId : stageIds) {
            branches.push_back(BuildStage(stageId));
        }
        const auto& output = branches.front().OutputType;
        for (size_t i = 1; i < branches.size(); ++i) {
            if (!SameUnionColumns(output, branches[i].OutputType)) {
                throw std::runtime_error(
                    "exec plan builder: union input schemas differ");
            }
        }
        const auto id = AddNode(
            nullptr, EExecPlanNodeKind::UnionAll, branches, output);
        return {.NodeId = id, .OutputType = output};
    }

    TBuildResult BuildStage(TExecStageId stageId) {
        if (auto it = Memo_.find(stageId); it != Memo_.end()) {
            return it->second;
        }
        const auto stageIt = Stages_.find(stageId);
        if (stageIt == Stages_.end() || !stageIt->second->Operator) {
            throw std::runtime_error(
                "exec plan builder: missing stage metadata");
        }
        const auto& stage = *stageIt->second;
        auto inputs = BuildInputs(stage);
        auto result = BuildStageNode(stage, std::move(inputs));
        Memo_[stageId] = result;
        return result;
    }

    TBuildResult BuildStageNode(
        const TLoweredExecStage& stage,
        std::vector<TBuildResult> inputs)
    {
        using namespace NQumir::NAst;
        TTypePtr outputType;
        std::optional<std::vector<int32_t>> keptColumns;
        std::optional<size_t> materialization;

        switch (stage.Kind) {
            case EExecPlanNodeKind::Source:
                outputType = BuildSourceRuntimeType(
                    *const_cast<TSourceOperator*>(
                        static_cast<const TSourceOperator*>(stage.Operator)));
                break;
            case EExecPlanNodeKind::StagedScan: {
                // The source itself, or the chain of filters fused into it.
                auto* filter = dynamic_cast<const TFilterOperator*>(stage.Operator);
                if (!filter) {
                    outputType = BuildSourceRuntimeType(
                        *const_cast<TSourceOperator*>(
                            static_cast<const TSourceOperator*>(stage.Operator)));
                    break;
                }
                auto input = filter->Input();
                while (auto nested = TMaybeOp<TFilterOperator>(input)) {
                    input = nested.Cast()->Input();
                }
                auto columnPlan = BuildFilterColumnPlan(
                    *filter, BuildSourceRuntimeType(*TMaybeOp<TSourceOperator>(input).Cast()));
                outputType = std::move(columnPlan.OutputType);
                keptColumns = std::move(columnPlan.KeptInputColumns);
                break;
            }
            case EExecPlanNodeKind::Filter: {
                auto* filter = static_cast<const TFilterOperator*>(stage.Operator);
                auto columnPlan = BuildFilterColumnPlan(
                    *filter, inputs[0].OutputType);
                outputType = std::move(columnPlan.OutputType);
                keptColumns = std::move(columnPlan.KeptInputColumns);
                break;
            }
            case EExecPlanNodeKind::RuntimeFilter:
                if (!stage.RuntimeFilterUse) {
                    throw std::runtime_error(
                        "exec plan builder: runtime filter has no binding");
                }
                outputType = inputs[0].OutputType;
                break;
            case EExecPlanNodeKind::Project: {
                auto* project = const_cast<TProjectOperator*>(
                    static_cast<const TProjectOperator*>(stage.Operator));
                auto* input = static_cast<TStructType*>(inputs[0].OutputType.get());
                outputType = BuildProjectColumnPlan(*project, *input).OutputType;
                break;
            }
            case EExecPlanNodeKind::Aggregate: {
                auto* aggregate = static_cast<const TAggregateOperator*>(stage.Operator);
                outputType = ComputeAggregateOutputType(
                    inputs[0].OutputType,
                    aggregate->GroupKeys(),
                    aggregate->Aggs(),
                    !aggregate->GroupingSets().empty(), !aggregate->EmptyInputGroups().empty());
                break;
            }
            case EExecPlanNodeKind::Join:
            case EExecPlanNodeKind::CrossJoin: {
                auto* join = static_cast<const TJoinOperator*>(stage.Operator);
                auto result = ComputeJoinOutputType(
                    inputs[0].OutputType, inputs[1].OutputType, join->JoinType());
                if (!result) {
                    throw result.error();
                }
                outputType = *result;
                break;
            }
            case EExecPlanNodeKind::CrossResidualFilter:
                outputType = inputs[0].OutputType;
                break;
            case EExecPlanNodeKind::CteProducer:
                outputType = inputs[0].OutputType;
                materialization = NextMaterializationId_++;
                Materializations_[stage.Id] = *materialization;
                MaterializationOutputs_[stage.Id] = outputType;
                break;
            case EExecPlanNodeKind::CteConsumer: {
                const auto producerStage = InputStage(stage.Id, 0);
                const auto materialized = Materializations_.find(producerStage);
                if (materialized == Materializations_.end()) {
                    throw std::runtime_error(
                        "exec plan builder: CTE producer is not materialized");
                }
                materialization = materialized->second;
                outputType = MaterializationOutputs_.at(producerStage);
                break;
            }
            case EExecPlanNodeKind::Window: {
                auto* window = static_cast<const TWindowOperator*>(stage.Operator);
                outputType = ComputeWindowOutputType(
                    inputs[0].OutputType, window->Functions());
                break;
            }
            case EExecPlanNodeKind::Sort:
            case EExecPlanNodeKind::TopSort:
                outputType = inputs[0].OutputType;
                break;
            case EExecPlanNodeKind::Limit: {
                auto* limit = static_cast<const TLimitOperator*>(stage.Operator);
                const bool atRoot =
                    RootStageIds_.size() == 1 &&
                    RootStageIds_.front() == stage.Id && !Plan_.RootLimit;
                if (atRoot) {
                    // The root limit is applied by the terminal sink; the graph
                    // needs no exec node for it.
                    Plan_.RootLimit = TExecPlanLimit{
                        .StageId = stage.Id,
                        .Limit = limit->Limit(),
                        .Offset = limit->Offset(),
                    };
                    return inputs[0];
                }
                // A nested limit is a real streaming node applied in place.
                outputType = inputs[0].OutputType;
                break;
            }
            case EExecPlanNodeKind::LateMaterialize: {
                auto* late = static_cast<const TLateMaterializeOperator*>(stage.Operator);
                outputType = late->OutputColumns();
                break;
            }
            case EExecPlanNodeKind::UnionAll:
                throw std::runtime_error(
                    "exec plan builder: union cannot have lowered metadata");
        }

        const auto id = AddNode(
            &stage,
            stage.Kind,
            std::move(inputs),
            outputType,
            materialization,
            std::move(keptColumns));
        return {.NodeId = id, .OutputType = std::move(outputType)};
    }

    TExecStageId InputStage(TExecStageId stageId, size_t input) const {
        auto resolved = Inputs(stageId);
        auto it = resolved.find(input);
        if (it == resolved.end() || it->second.size() != 1) {
            throw std::runtime_error(
                "exec plan builder: expected one upstream stage");
        }
        return it->second.front();
    }

    std::vector<TExecStageId> RootStages() const {
        std::vector<TExecStageId> result;
        for (const auto* producer : Lowered_.Producers) {
            std::unordered_set<const TNode*> visited;
            CollectUpstreamStages(
                producer, InvalidExecStageId, result, visited);
        }
        SortUnique(result);
        if (result.empty()) {
            throw std::runtime_error("exec plan builder: graph has no root stage");
        }
        return result;
    }

    const NScheduler::TLoweredPlan& Lowered_;
    const NScheduler::TTaskGraph* Graph_ = nullptr;
    TExecPlan Plan_;
    std::unordered_map<TExecStageId, const TLoweredExecStage*> Stages_;
    std::unordered_map<TExecStageId, std::vector<const TNode*>> StageNodes_;
    std::unordered_map<const TNode*, std::vector<const TEdge*>> Inbound_;
    std::unordered_map<TExecStageId, std::vector<size_t>> KernelIndexes_;
    std::unordered_map<TExecStageId, TBuildResult> Memo_;
    std::vector<TExecStageId> RootStageIds_;
    size_t NextMaterializationId_ = 0;
    std::unordered_map<TExecStageId, size_t> Materializations_;
    std::unordered_map<TExecStageId, NQumir::NAst::TTypePtr>
        MaterializationOutputs_;
};

} // namespace

std::expected<TExecPlan, std::string> BuildExecPlan(
    const NScheduler::TLoweredPlan& lowered)
{
    return TExecPlanBuilder(lowered).Build();
}

} // namespace NQdb
