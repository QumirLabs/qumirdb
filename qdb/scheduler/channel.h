#pragma once

#include <qdb/scheduler/impl/threaded_scheduler.h>

namespace NQdb {
namespace NScheduler {

class TSchedulerChannel {
public:
    explicit TSchedulerChannel(IThreadedScheduler& scheduler)
        : Scheduler_(scheduler)
    {}

    void Notify(TTaskNode* task) {
        Scheduler_.Schedule(task);
    }

private:
    IThreadedScheduler& Scheduler_;
};

} // namespace NScheduler
} // namespace NQdb
