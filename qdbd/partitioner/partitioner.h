#pragma once

#include <qdb/scheduler/graph.h>

#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace NQdb {
namespace NScheduler {

enum class EReboundPort { SourceOutput, DestinationInput };

struct TConnectionSplitPlan {
    TTaskEdge* OriginalEdge = nullptr;
    EReboundPort ReboundPort = EReboundPort::DestinationInput;
    int SinkPartition = 0;
    int SourcePartition = 0;

    std::unique_ptr<IConnection> NewConnection;
    TTaskNode SinkNode;
    TTaskNode SourceNode;
    std::unique_ptr<ITask> SinkTask;
    std::unique_ptr<ITask> SourceTask;
    TTaskEdge SinkEdge;
    TTaskEdge SourceEdge;
};

class TPartitioner {
public:
    TPartitioner(TTaskNode* root, int partitions);
    void Do();

    const std::unordered_map<TTaskNode*, int>& Colors() const {
        return Colors_;
    }

    const std::unordered_set<TTaskEdge*>& SplitEdges() const {
        return SplitEdges_;
    }

    const std::vector<std::unique_ptr<TConnectionSplitPlan>>& ConnectionSplits() const {
        return ConnectionSplits_;
    }

private:
    void Color();
    void Partition();
    void PlanSplit(TTaskEdge& edge, EReboundPort reboundPort, size_t capacity);

    TTaskNode* Root_;
    int Partitions_;

    std::unordered_map<TTaskNode*, int> Colors_;
    std::unordered_set<TTaskEdge*> SplitEdges_;
    std::vector<std::unique_ptr<TConnectionSplitPlan>> ConnectionSplits_;
};

} // namespace NScheduler
} // namespace NQdb
