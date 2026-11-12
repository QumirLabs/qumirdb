#pragma once

#include <qdb/scheduler/graph.h>

#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace NQdb {
namespace NScheduler {

// Colors a sealed graph into `partitions` fragments and plans the cuts of the
// edges between colors: rowset edges one by one, the dependency edges into a
// runtime filter task by filter and pair of colors. Apply rejects any other
// dependency edge between colors.
class TPartitioner {
public:
    TPartitioner(const TSealedTaskGraph& graph, int partitions);
    void Do();

    const std::unordered_map<TTaskNode*, int>& Colors() const {
        return Colors_;
    }

    // Points into the graph's edges: invalid after TTaskGraph::Apply.
    const std::unordered_set<TTaskEdge*>& SplitEdges() const {
        return SplitEdges_;
    }

    // The cuts of SplitEdges() for TTaskGraph::Apply.
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
