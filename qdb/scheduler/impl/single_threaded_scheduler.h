#pragma once

#include <qdb/kernel/execution_context.h>
#include <qdb/scheduler/graph.h>

#include <list>
#include <string>
#include <unordered_set>

namespace NQdb {
namespace NScheduler {

class TSingleThreadedScheduler {
public:
    TSingleThreadedScheduler(TTaskNode& root, size_t taskCount);

    bool Run(std::string* error = nullptr);
    TSchedulerRunStats Stats() const;

private:
    void Schedule(TTaskNode* node);
    void ScheduleInput(TTaskNode* node);
    void ScheduleOutput(TTaskNode* node);

    TTaskNode& Root_;
    TExecutionState Execution_;
    std::list<TTaskNode*> Ready_;
    std::unordered_set<TTaskNode*> Scheduled_;
    TSchedulerRunStats Stats_;
    bool HasRun_ = false;
};

} // namespace NScheduler
} // namespace NQdb
