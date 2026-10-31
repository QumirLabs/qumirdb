#include <qdb/kernel/execution_context.h>
#include <qdb/scheduler/instrumentation/task.h>
#include <qdb/scheduler/single_threaded_scheduler.h>

#include <exception>
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

TSingleThreadedScheduler::TSingleThreadedScheduler(TTaskGraph& graph)
    : Graph_(graph)
{}

bool TSingleThreadedScheduler::Run(std::string* error) {
    if (HasRun_) {
        SetError(error, "single-threaded scheduler cannot run twice");
        return false;
    }
    HasRun_ = true;

    if (!Graph_.Validate(error)) {
        return false;
    }

    Ready_.clear();
    Scheduled_.clear();
    Stats_ = {};
    Schedule(Graph_.Root());

    TExecutionContext context;
    while (!Ready_.empty()) {
        auto* node = Ready_.front();
        Ready_.pop_front();
        Scheduled_.erase(node);
        ++Stats_.Popped;

        ETaskResult state;
        try {
            state = ExecuteTask(*node, context);
        } catch (const std::exception& e) {
            SetError(error, e.what());
            return false;
        } catch (...) {
            SetError(error, "scheduler task failed with an unknown exception");
            return false;
        }
        ++Stats_.Executed;
        if (state == ETaskResult::NEED_DATA) {
            ++Stats_.NeedData;
            ScheduleInput(node);
        } else if (
            state == ETaskResult::BLOCKED_OUTPUT ||
            state == ETaskResult::FINISHED)
        {
            if (state == ETaskResult::BLOCKED_OUTPUT) {
                ++Stats_.BlockedOutput;
            } else {
                ++Stats_.Finished;
                node->State.store(
                    ETaskState::Finished, std::memory_order_release);
            }
            ScheduleOutput(node);
        } else if (state == ETaskResult::OK) {
            ++Stats_.Ok;
            Schedule(node);
            ScheduleOutput(node);
        }
    }

    return true;
}

void TSingleThreadedScheduler::Schedule(TTaskNode* node) {
    if (!node || Scheduled_.contains(node)
        || node->State.load(std::memory_order_acquire) == ETaskState::Finished) {
        return;
    }
    Ready_.push_back(node);
    Scheduled_.insert(node);
    ++Stats_.Scheduled;
}

void TSingleThreadedScheduler::ScheduleInput(TTaskNode* node) {
    bool hasWantedInput = false;
    bool allFinished = true;
    for (auto* edge : node->Inbound) {
        if (!node->Task->WantsInput(*edge)) {
            continue;
        }
        hasWantedInput = true;
        if (edge->Src->State.load(std::memory_order_acquire)
            == ETaskState::Finished) {
            continue;
        }
        allFinished = false;
        Schedule(edge->Src);
    }
    if (hasWantedInput && allFinished) {
        Schedule(node);
    }
}

void TSingleThreadedScheduler::ScheduleOutput(TTaskNode* node) {
    for (auto* edge : node->Outbound) {
        if (edge->Kind == ETaskEdgeKind::Dependency
            && !edge->Dst->Task->TryNotifyDependency()) {
            continue;
        }
        Schedule(edge->Dst);
    }
}

TSchedulerRunStats TSingleThreadedScheduler::Stats() const {
    return Stats_;
}

} // namespace NScheduler
} // namespace NQdb
