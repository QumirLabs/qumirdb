#include "partitioner.h"
#include "qdb/scheduler/tasks/runtime_adapter.h"

#include <memory>
#include <qdb/scheduler/connections/local.h>

#include <algorithm>
#include <limits>
#include <queue>
#include <stack>
#include <set>
#include <cassert>

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
    std::unordered_map<NScheduler::TOneToOneConnection*, std::vector<TTaskEdge*>> oneToOneConnections;
    for (auto* edge : SplitEdges_) {
        if (edge->Kind == ETaskEdgeKind::Dependency) {
            // src -> [removeSinkProducer] :: [remoteSourceConsumer] -> dst
            auto* dst = edge->Dst;
            edge->Dst = nullptr; // TODO: should be removeSinkProducer

            auto newEdge = std::make_unique<TTaskEdge>();
            newEdge->Src = nullptr; // TODO: should be removeSinkConsumer
            newEdge->Dst = dst;
            // TODO: add newEdge to graph
        } else if (auto oneToOne = dynamic_cast<NScheduler::TOneToOneConnection*>(edge->Connection)) {
            // simple split
            assert(edge->SrcLane == edge->DstLane);
            oneToOneConnections[oneToOne].push_back(edge);
        }
    }

    for (auto& [oneToOne, edges] : oneToOneConnections) {
        // create new capacity=1 connection per split
        for (auto* edge : edges) {
            auto newConnection = std::make_unique<TOneToOneConnection>(1);
            // 0 -> 0
            // dst -> edge -> [remoteSink] :: [remoteSource] -> edge -> src
            auto newEdge = std::make_unique<TTaskEdge>();
            newEdge->SrcLane = 0;
            newEdge->DstLane = 0;
            newEdge->Dst = edge->Dst;
            auto src = std::make_unique<TSourceTask>(nullptr, nullptr, TOutputPort {
                .Connection = newConnection.get(),
                .Lane = 0,
                .Instrumentation = nullptr,
            });
            TTaskNode srcTask = TTaskNode {
                .Task = src.get()
            };
            newEdge->Src = &srcTask;

            auto dst = std::make_unique<TSinkTask>(nullptr, nullptr, TInputPort {
                .Connection = oneToOne,
                .Lane = edge->SrcLane,
                .Instrumentation = nullptr
            });
            TTaskNode dstTask = TTaskNode {
                .Task = dst.get()
            };
            edge->Dst = &dstTask;
        }
    }
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
