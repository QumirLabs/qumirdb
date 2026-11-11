#include <qdb/scheduler/instrumentation/task.h>
#include <qdb/scheduler/impl/single_threaded_scheduler.h>

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

TSingleThreadedScheduler::TSingleThreadedScheduler(TSealedTaskGraph graph)
    : Graph_(graph)
    , Execution_(graph.TaskCount())
{}

bool TSingleThreadedScheduler::Run(std::string* error) {
    if (!Graph_) {
        SetError(error, "task graph is not sealed");
        return false;
    }
    if (HasRun_) {
        SetError(error, "single-threaded scheduler cannot run twice");
        return false;
    }
    HasRun_ = true;

    Ready_.clear();
    Scheduled_.clear();
    Stats_ = {};
    TExecutionContext context{.State = &Execution_};
    // Every root pulls its own inputs; the run ends when all of them finish.
    size_t unfinishedRoots = 0;
    for (const auto& graph : Graph_.Graphs()) {
        unfinishedRoots += graph.size();
        for (auto* root : graph) {
            Schedule(root);
        }
    }

    while (unfinishedRoots != 0 && !Ready_.empty()) {
        auto* node = Ready_.front();
        Ready_.pop_front();
        Scheduled_.erase(node);
        ++Stats_.Popped;
        // Abandoned while queued: no consumer needs its output.
        if (node->Retired.load(std::memory_order_acquire)) {
            continue;
        }

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
        node->OutputBlocked.store(
            state == ETaskResult::BLOCKED_OUTPUT && HasBlockedOutput(*node),
            std::memory_order_relaxed);
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
                Retire(*node, [this](TTaskNode& producer) {
                    Schedule(&producer);
                });
                if (IsRoot(*node)) {
                    --unfinishedRoots;
                }
            }
            ScheduleOutput(node);
        } else if (state == ETaskResult::WAITING) {
            SetError(error, "single-threaded scheduler does not support external waits");
            return false;
        } else if (state == ETaskResult::OK) {
            ++Stats_.Ok;
            Schedule(node);
            ScheduleOutput(node);
        }
    }

    return true;
}

void TSingleThreadedScheduler::Schedule(TTaskNode* node) {
    // A retired node is finished or abandoned.
    if (!node || Scheduled_.contains(node)
        || node->Retired.load(std::memory_order_acquire)) {
        return;
    }
    Ready_.push_back(node);
    Scheduled_.insert(node);
    ++Stats_.Scheduled;
}

void TSingleThreadedScheduler::ScheduleInput(TTaskNode* node) {
    bool hasWantedInput = false;
    bool allFinished = true;
    bool rerun = false;
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
        const auto demand = Demand(*edge);
        if (demand == EParkedDemand::WakeProducer) {
            Schedule(edge->Src);
        }
        rerun = rerun || demand == EParkedDemand::RerunConsumer;
    }
    if ((hasWantedInput && allFinished) || rerun) {
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
