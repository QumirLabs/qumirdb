#include <qdb/scheduler/impl/executor.h>

#include <qdb/scheduler/impl/single_threaded_scheduler.h>
#include <qdb/scheduler/impl/threaded_scheduler.h>

#include <algorithm>
#include <utility>

namespace NQdb {
namespace NScheduler {
namespace {

void SetError(std::string* error, std::string message) {
    if (error) {
        *error = std::move(message);
    }
}

} // namespace

TSchedulerExecutor::TSchedulerExecutor(TSealedTaskGraph graph, TSettings settings)
    : Graph_(graph)
    , Settings_(settings)
{}

bool TSchedulerExecutor::Run(std::string* error) {
    if (Settings_.Scheduler.Mode == EExecutionMode::SingleThreadedScheduler) {
        TSingleThreadedScheduler scheduler(Graph_);
        const bool ok = scheduler.Run(error);
        Stats_ = scheduler.Stats();
        return ok;
    }

    if (Settings_.Scheduler.Mode == EExecutionMode::ThreadedScheduler) {
        TThreadedScheduler scheduler(
            Graph_,
            std::max<size_t>(Settings_.Scheduler.WorkerCount, 1),
            Settings_.Scheduler.ReadyQueueCapacity);
        const bool ok = scheduler.Run(error);
        Stats_ = scheduler.Stats();
        return ok;
    }

    SetError(error, "unknown scheduler execution mode");
    return false;
}

TSchedulerRunStats TSchedulerExecutor::Stats() const {
    return Stats_;
}

} // namespace NScheduler
} // namespace NQdb
