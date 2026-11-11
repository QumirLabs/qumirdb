#include <qdb/scheduler/instrumentation/task.h>
#include <qdb/scheduler/impl/threaded_scheduler.h>

#include <algorithm>
#include <cassert>
#include <exception>
#include <thread>
#include <utility>
#include <vector>

namespace NQdb {
namespace NScheduler {
namespace {

void SetError(std::string* error, std::string message) {
    if (error) {
        *error = std::move(message);
    }
}

} // namespace

TThreadedScheduler::TThreadedScheduler(
    TSealedTaskGraph graph,
    size_t workerCount,
    size_t readyQueueCapacity)
    : Graph_(graph)
    , Execution_(graph.TaskCount())
    , Ready_(std::max({
        readyQueueCapacity,
        graph.TaskCount(),
        size_t{2}}))
    , WorkerCount_(std::max<size_t>(workerCount, 1))
{}

bool TThreadedScheduler::Run(std::string* error) {
    if (!Graph_) {
        SetError(error, "task graph is not sealed");
        return false;
    }
    if (HasRun_) {
        SetError(error, "threaded scheduler cannot run twice");
        return false;
    }
    HasRun_ = true;

    ResetStats();
    // Every root pulls its own inputs; the run ends when all of them finish.
    size_t roots = 0;
    for (const auto& graph : Graph_.Graphs()) {
        roots += graph.size();
        for (auto* root : graph) {
            Schedule(root);
        }
    }
    UnfinishedRoots_.store(roots, std::memory_order_release);

    std::vector<std::thread> workers;
    workers.reserve(WorkerCount_);
    for (size_t i = 0; i < WorkerCount_; ++i) {
        workers.emplace_back([this]() {
            Work();
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    if (Failed_.load(std::memory_order_acquire)) {
        std::lock_guard lock(ErrorMutex_);
        SetError(error, Error_);
        return false;
    }
    return true;
}

void TThreadedScheduler::Cancel() {
    bool expected = false;
    if (Failed_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        std::lock_guard lock(ErrorMutex_);
        Error_ = "scheduler cancelled";
    }
    WorkAvailable_.release(static_cast<ptrdiff_t>(WorkerCount_));
}

void TThreadedScheduler::Schedule(TTaskNode* node) {
    // A retired node is finished or abandoned; external wakes for it are dropped too.
    if (!node || node->Retired.load(std::memory_order_acquire)) {
        return;
    }

    auto state = node->State.load(std::memory_order_acquire);
    for (;;) {
        if (state == ETaskState::Idle || state == ETaskState::Waiting) {
            if (node->State.compare_exchange_weak(
                    state,
                    ETaskState::Queued,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                break;
            }
        } else if (state == ETaskState::Running) {
            if (node->State.compare_exchange_weak(
                    state,
                    ETaskState::Reschedule,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return;
            }
        } else {
            return;
        }
    }

    while (!Ready_.TryPush(std::move(node))) {
        ReadyPushRetries_.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::yield();
    }
    Scheduled_.fetch_add(1, std::memory_order_relaxed);
    // Wake one idle worker (release synchronizes with the acquiring worker, so
    // the pushed node is visible before it pops).
    WorkAvailable_.release();
}

TTaskNode* TThreadedScheduler::PopReady() {
    TTaskNode* node = nullptr;
    if (Ready_.TryPop(node)) {
        Popped_.fetch_add(1, std::memory_order_relaxed);
    } else {
        EmptyReadyPolls_.fetch_add(1, std::memory_order_relaxed);
    }
    return node;
}

void TThreadedScheduler::ScheduleInput(TTaskNode* node) {
    bool hasWantedInput = false;
    bool allFinished = true;
    bool rerun = false;
    for (auto* edge : node->Inbound) {
        if (!node->Task->WantsInput(*edge)) {
            continue;
        }
        hasWantedInput = true;
        if (edge->Src->State.load(std::memory_order_acquire) ==
            ETaskState::Finished)
        {
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

void TThreadedScheduler::ScheduleOutput(TTaskNode* node) {
    for (auto* edge : node->Outbound) {
        if (edge->Kind == ETaskEdgeKind::Dependency
            && !edge->Dst->Task->TryNotifyDependency()) {
            continue;
        }
        Schedule(edge->Dst);
    }
}

void TThreadedScheduler::Work() {
    TExecutionContext context{.State = &Execution_};
    // Terminate once every root is Finished: all output the graph needs has
    // been produced. Nodes still unfinished then feed only finished roots and
    // were abandoned by early termination (a satisfied LIMIT stops pulling,
    // leaving its producers blocked on a Finished consumer) - waiting for them to
    // finish would deadlock. External waits on abandoned branches do not delay
    // completion.
    auto done = [&] {
        return Failed_.load(std::memory_order_acquire) ||
            UnfinishedRoots_.load(std::memory_order_acquire) == 0;
    };
    // Brief spin on the work permit before blocking, so bursts of scheduling
    // are picked up without an OS wakeup; then park on the semaphore.
    constexpr int acquireSpin = 64;
    for (;;) {
        if (done()) {
            return;
        }
        bool gotPermit = false;
        for (int i = 0; i < acquireSpin; ++i) {
            if (WorkAvailable_.try_acquire()) {
                gotPermit = true;
                break;
            }
        }
        if (!gotPermit) {
            WorkAvailable_.acquire();
        }
        if (done()) {
            return;
        }

        // A permit means a node was enqueued; release/acquire ordering makes it
        // visible, so this pops immediately (the loop only guards a transient
        // MPMC gap or the termination wakeup).
        auto* node = PopReady();
        while (!node) {
            if (done()) {
                return;
            }
            node = PopReady();
        }

        // The last root may have finished while this node sat queued. Leaving it
        // unrun is safe - the graph is being torn down.
        if (done()) {
            return;
        }
        // Abandoned while queued: no consumer needs its output, so it never runs,
        // and a satisfied LIMIT stops its upstream promptly.
        if (node->Retired.load(std::memory_order_acquire)) {
            continue;
        }

        auto expected = ETaskState::Queued;
        if (!node->State.compare_exchange_strong(
                expected,
                ETaskState::Running,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            continue;
        }

        ETaskResult result;
        try {
            result = ExecuteTask(*node, context);
        } catch (const std::exception& e) {
            bool expected = false;
            if (Failed_.compare_exchange_strong(
                    expected, true, std::memory_order_acq_rel))
            {
                std::lock_guard lock(ErrorMutex_);
                Error_ = e.what();
            }
            WorkAvailable_.release(static_cast<ptrdiff_t>(WorkerCount_));
            return;
        } catch (...) {
            bool expected = false;
            if (Failed_.compare_exchange_strong(
                    expected, true, std::memory_order_acq_rel))
            {
                std::lock_guard lock(ErrorMutex_);
                Error_ = "scheduler task failed with an unknown exception";
            }
            WorkAvailable_.release(static_cast<ptrdiff_t>(WorkerCount_));
            return;
        }
        Executed_.fetch_add(1, std::memory_order_relaxed);
        // Set before the state leaves Running, so a consumer that sees it parked sees why.
        node->OutputBlocked.store(
            result == ETaskResult::BLOCKED_OUTPUT && HasBlockedOutput(*node),
            std::memory_order_release);
        if (result == ETaskResult::NEED_DATA) {
            NeedData_.fetch_add(1, std::memory_order_relaxed);
            auto reschedule = FinishRun(node);
            ScheduleInput(node);
            if (reschedule) {
                Rescheduled_.fetch_add(1, std::memory_order_relaxed);
                Schedule(node);
            }
        } else if (result == ETaskResult::BLOCKED_OUTPUT) {
            BlockedOutput_.fetch_add(1, std::memory_order_relaxed);
            auto reschedule = FinishRun(node);
            ScheduleOutput(node);
            if (reschedule) {
                Rescheduled_.fetch_add(1, std::memory_order_relaxed);
                Schedule(node);
            }
        } else if (result == ETaskResult::WAITING) {
            Waiting_.fetch_add(1, std::memory_order_relaxed);
            if (FinishRun(node, ETaskState::Waiting)) {
                Rescheduled_.fetch_add(1, std::memory_order_relaxed);
                Schedule(node);
            }
        } else if (result == ETaskResult::FINISHED) {
            Finished_.fetch_add(1, std::memory_order_relaxed);
            FinishNode(node);
            ScheduleOutput(node);
        } else if (result == ETaskResult::OK) {
            Ok_.fetch_add(1, std::memory_order_relaxed);
            FinishRun(node);
            Schedule(node);
            ScheduleOutput(node);
        }
    }
}

bool TThreadedScheduler::FinishRun(TTaskNode* node, ETaskState nextState) {
    auto previous = node->State.exchange(nextState, std::memory_order_acq_rel);
    assert(previous == ETaskState::Running || previous == ETaskState::Reschedule);
    return previous == ETaskState::Reschedule;
}

void TThreadedScheduler::FinishNode(TTaskNode* node) {
    auto previous = node->State.exchange(ETaskState::Finished, std::memory_order_acq_rel);
    assert(previous == ETaskState::Running || previous == ETaskState::Reschedule);
    Retire(*node, [this](TTaskNode& producer) {
        Schedule(&producer);
    });
    if (IsRoot(*node) && UnfinishedRoots_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        // The last root is done: the graph's output is complete. Wake every worker
        // so they observe termination and exit instead of parking on the permit
        // while upstream branches sit unfinished (abandoned by early termination).
        WorkAvailable_.release(static_cast<ptrdiff_t>(WorkerCount_));
    }
}

void TThreadedScheduler::ResetStats() {
    Scheduled_.store(0, std::memory_order_relaxed);
    Popped_.store(0, std::memory_order_relaxed);
    Executed_.store(0, std::memory_order_relaxed);
    NeedData_.store(0, std::memory_order_relaxed);
    BlockedOutput_.store(0, std::memory_order_relaxed);
    Ok_.store(0, std::memory_order_relaxed);
    Finished_.store(0, std::memory_order_relaxed);
    Waiting_.store(0, std::memory_order_relaxed);
    Rescheduled_.store(0, std::memory_order_relaxed);
    ReadyPushRetries_.store(0, std::memory_order_relaxed);
    EmptyReadyPolls_.store(0, std::memory_order_relaxed);
}

TSchedulerRunStats TThreadedScheduler::Stats() const {
    return TSchedulerRunStats{
        .Scheduled = Scheduled_.load(std::memory_order_relaxed),
        .Popped = Popped_.load(std::memory_order_relaxed),
        .Executed = Executed_.load(std::memory_order_relaxed),
        .NeedData = NeedData_.load(std::memory_order_relaxed),
        .BlockedOutput = BlockedOutput_.load(std::memory_order_relaxed),
        .Ok = Ok_.load(std::memory_order_relaxed),
        .Finished = Finished_.load(std::memory_order_relaxed),
        .Waiting = Waiting_.load(std::memory_order_relaxed),
        .Rescheduled = Rescheduled_.load(std::memory_order_relaxed),
        .ReadyPushRetries = ReadyPushRetries_.load(std::memory_order_relaxed),
        .EmptyReadyPolls = EmptyReadyPolls_.load(std::memory_order_relaxed),
    };
}

} // namespace NScheduler
} // namespace NQdb
