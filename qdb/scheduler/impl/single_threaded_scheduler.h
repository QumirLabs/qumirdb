#pragma once

#include <qdb/scheduler/graph.h>

#include <list>
#include <string>
#include <unordered_set>

namespace NQdb {
namespace NScheduler {

class TSingleThreadedScheduler {
public:
    explicit TSingleThreadedScheduler(TTaskGraph& graph);

    bool Run(std::string* error = nullptr);
    TSchedulerRunStats Stats() const;

private:
    void Schedule(TTaskNode* node);
    void ScheduleInput(TTaskNode* node);
    void ScheduleOutput(TTaskNode* node);

    TTaskGraph& Graph_;
    std::list<TTaskNode*> Ready_;
    std::unordered_set<TTaskNode*> Scheduled_;
    TSchedulerRunStats Stats_;
    bool HasRun_ = false;
};

} // namespace NScheduler
} // namespace NQdb
