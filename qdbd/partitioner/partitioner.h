#pragma once

#include <qdb/scheduler/graph.h>

#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace NQdb {
namespace NScheduler {

class TPartitioner {
public:
    TPartitioner(const TSealedTaskGraph& graph, int partitions);
    void Do();

    const std::unordered_map<TTaskNode*, int>& Colors() const {
        return Colors_;
    }

    const std::unordered_set<TTaskEdge*>& SplitEdges() const {
        return SplitEdges_;
    }

    // The rowset cuts of SplitEdges() for TTaskGraph::Apply.
    TSplitPlan TakePlan() {
        return std::exchange(Plan_, {});
    }

private:
    void Color();
    void Partition(const std::vector<TTaskEdge*>& cuts);
    void PlanSplit(TTaskEdge& edge, EReboundPort reboundPort, size_t capacity);

    std::vector<TTaskNode*> Roots_;
    int Partitions_;

    std::unordered_map<TTaskNode*, int> Colors_;
    std::unordered_set<TTaskEdge*> SplitEdges_;
    TSplitPlan Plan_;
};

} // namespace NScheduler
} // namespace NQdb
