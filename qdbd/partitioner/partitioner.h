#pragma once

#include <qdb/scheduler/graph.h>
#include <unordered_map>

namespace NQdb {
namespace NScheduler {

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

private:
    void Color();

    TTaskNode* Root_;
    int Partitions_;

    std::unordered_map<TTaskNode*, int> Colors_;
    std::unordered_set<TTaskEdge*> SplitEdges_;
};

} // namespace NScheduler
} // namespace NQdb
