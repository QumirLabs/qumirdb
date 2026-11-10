#include "partitioner.h"

#include <qdb/scheduler/connections/local.h>
#include <qdb/scheduler/port.h>
#include <qdb/scheduler/tasks/runtime_adapter.h>

#include <algorithm>
#include <cassert>
#include <limits>
#include <memory>
#include <stack>
#include <stdexcept>
#include <utility>

namespace NQdb {
namespace NScheduler {

TPartitioner::TPartitioner(TTaskNode* root, int partitions)
    : Root_(root)
    , Partitions_(partitions)
{ }

void TPartitioner::Do()
{
    Colors_.clear();
    SplitEdges_.clear();
    ConnectionSplits_.clear();

    Color();

    for (const auto& [node, color] : Colors_) {
        for (auto* edge : node->Outbound) {
            if (color != Colors_.at(edge->Dst)) {
                SplitEdges_.insert(edge);
            }
        }
    }

    Partition();
}

void TPartitioner::Partition()
{
    for (auto* edge : SplitEdges_) {
        if (edge->Kind == ETaskEdgeKind::Dependency) {
            // TODO: dependency transport is separate from rowset streams.
            continue;
        }
        if (auto* oneToOne = dynamic_cast<TOneToOneConnection*>(edge->Connection)) {
            assert(edge->SrcLane == edge->DstLane);
            PlanSplit(*edge, EReboundPort::DestinationInput, oneToOne->Capacity());
        } else if (auto* gather = dynamic_cast<TGatherConnection*>(edge->Connection)) {
            assert(edge->DstLane == 0);
            PlanSplit(*edge, EReboundPort::SourceOutput, gather->Capacity());
        } else if (auto* broadcast = dynamic_cast<TBroadcastConnection*>(edge->Connection)) {
            assert(edge->SrcLane == 0);
            PlanSplit(*edge, EReboundPort::DestinationInput, broadcast->Capacity());
        } else {
            throw std::logic_error("unsupported connection for partitioning");
        }
    }
}

void TPartitioner::PlanSplit(TTaskEdge& edge, EReboundPort reboundPort, size_t capacity) {
    auto split = std::make_unique<TConnectionSplitPlan>();
    split->OriginalEdge = &edge;
    split->ReboundPort = reboundPort;
    split->SinkPartition = Colors_.at(edge.Src);
    split->SourcePartition = Colors_.at(edge.Dst);
    split->NewConnection = std::make_unique<TOneToOneConnection>(capacity);
    const auto layouts = edge.Connection->RowLayouts();
    split->NewConnection->SetRowLayouts(
        std::vector<TValueLayout>(layouts.begin(), layouts.end()));

    TInputPort sinkInput{.Connection = edge.Connection, .Lane = edge.DstLane};
    TOutputPort sourceOutput{.Connection = split->NewConnection.get(), .Lane = 0};
    if (reboundPort == EReboundPort::SourceOutput) {
        sinkInput = {.Connection = split->NewConnection.get(), .Lane = 0};
        sourceOutput = {.Connection = edge.Connection, .Lane = edge.SrcLane};
    }

    split->SinkTask = std::make_unique<TSinkTask>(nullptr, nullptr, sinkInput);
    split->SourceTask = std::make_unique<TSourceTask>(nullptr, nullptr, sourceOutput);
    split->SinkNode.Task = split->SinkTask.get();
    split->SinkNode.DebugKind = "remote-sink";
    split->SourceNode.Task = split->SourceTask.get();
    split->SourceNode.DebugKind = "remote-source";
    split->SinkEdge = {
        .Src = edge.Src,
        .Dst = &split->SinkNode,
        .Connection = sinkInput.Connection,
        .SrcLane = reboundPort == EReboundPort::SourceOutput ? 0 : edge.SrcLane,
        .DstLane = sinkInput.Lane,
    };
    split->SourceEdge = {
        .Src = &split->SourceNode,
        .Dst = edge.Dst,
        .Connection = sourceOutput.Connection,
        .SrcLane = sourceOutput.Lane,
        .DstLane = reboundPort == EReboundPort::DestinationInput ? 0 : edge.DstLane,
        .ExecInput = edge.ExecInput,
    };

    // Applying the plan must also rebind the selected original task port.
    ConnectionSplits_.push_back(std::move(split));
}

void TPartitioner::Color()
{
    std::unordered_map<TTaskNode*, int> inDegree;
    std::unordered_set<TTaskNode*> visited;
    std::stack<TTaskNode*> st;
    st.push(Root_);
    visited.insert(Root_);

    std::vector<TTaskNode*> currentLevel;
    while (!st.empty()) {
        auto node = st.top(); st.pop();
        int count = 0;
        for (auto& edge : node->Inbound) {
            if (edge->Kind == ETaskEdgeKind::Rowset) {
                auto next = edge->Src;
                if (!visited.contains(next)) {
                    visited.insert(next);
                    st.push(next);
                }

                count ++;
            }
        }
        inDegree[node] = count;
        if (count == 0) {
            currentLevel.push_back(node);
        }
    }

    std::vector<std::vector<TTaskNode*>> levels;

    while (!currentLevel.empty()) {
        levels.push_back(currentLevel);
        std::vector<TTaskNode*> nextLevel;

        for (auto* node : currentLevel) {
            for (auto& edge : node->Outbound) {
                if (edge->Kind == ETaskEdgeKind::Rowset) {
                    auto* next = edge->Dst;
                    if (--inDegree[next] == 0) {
                        nextLevel.push_back(next);
                    }
                }
            }
        }

        currentLevel.swap(nextLevel);
    }

    static constexpr int invalidColor = std::numeric_limits<int>::max();

    for (auto levelIt = levels.rbegin(); levelIt != levels.rend(); ++levelIt) {
        auto& level = *levelIt;
        std::vector<int> budget(Partitions_);

        auto place = [&](TTaskNode* node) {
            int color = invalidColor;
            int colorUsage = std::numeric_limits<int>::max();

            // parents
            for (auto* edge : node->Outbound) {
                if (edge->Kind == ETaskEdgeKind::Rowset) {
                    auto* parent = edge->Dst;
                    auto it = Colors_.find(parent);
                    if (it != Colors_.end() && colorUsage > budget[it->second]) {
                        color = it->second;
                        colorUsage = budget[it->second];
                    }
                }
            }

            auto it = std::min_element(budget.begin(), budget.end());
            int globalMin = it - budget.begin();

            if (color == invalidColor || budget[color] != budget[globalMin]) {
                color = globalMin;
            }

            Colors_[node] = color;
            budget[color] ++;
        };

        for (auto* node : level) {
            place(node);
        }
    }
}

} // namespace NScheduler
} // namespace NQdb
