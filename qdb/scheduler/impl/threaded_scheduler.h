#pragma once

#include <qdb/kernel/execution_context.h>
#include <qdb/scheduler/graph.h>
#include <qdb/scheduler/utils/mpmc.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <semaphore>
#include <string>

namespace NQdb {
namespace NScheduler {

class IThreadedScheduler {
public:
    virtual ~IThreadedScheduler() = default;

    // An event from outside the graph: it also wakes a WAITING task.
    virtual void Schedule(TTaskNode* task) = 0;
};

class TThreadedScheduler : public IThreadedScheduler {
public:
    TThreadedScheduler(
        TSealedTaskGraph graph,
        size_t workerCount,
        size_t readyQueueCapacity = 0);

    bool Run(std::string* error = nullptr);
    // Makes a running Run() return false; safe from any thread.
    void Cancel();
    TSchedulerRunStats Stats() const;
    void Schedule(TTaskNode* node) override;

private:
    // Data or demand of a neighbour, or an event: an external wake or a producer's finish.
    enum class EWake { Neighbour, Event };

    void Wake(TTaskNode* node, EWake wake);
    TTaskNode* PopReady();
    void ScheduleInput(TTaskNode* node);
    // An event (a finish) wakes every consumer, a neighbour's wake those with something to fetch.
    void ScheduleOutput(TTaskNode* node, EWake wake);
    void Work();
    // Ends a run and returns the state it had: Reschedule or Notified asks for another run.
    ETaskState FinishRun(TTaskNode* node, ETaskState nextState = ETaskState::Idle);
    void FinishNode(TTaskNode* node);
    void ResetStats();

    TSealedTaskGraph Graph_;
    TExecutionState Execution_;
    TMPMCQueue<TTaskNode*> Ready_;
    // One permit is released per task pushed to Ready_; idle workers block on
    // it (after a short spin) instead of busy-yielding until work arrives.
    std::counting_semaphore<> WorkAvailable_{0};
    std::atomic<size_t> UnfinishedRoots_ = 0;
    std::atomic<bool> Failed_ = false;
    mutable std::mutex ErrorMutex_;
    std::string Error_;
    std::atomic<uint64_t> Scheduled_ = 0;
    std::atomic<uint64_t> Popped_ = 0;
    std::atomic<uint64_t> Executed_ = 0;
    std::atomic<uint64_t> NeedData_ = 0;
    std::atomic<uint64_t> BlockedOutput_ = 0;
    std::atomic<uint64_t> Ok_ = 0;
    std::atomic<uint64_t> Finished_ = 0;
    std::atomic<uint64_t> Waiting_ = 0;
    std::atomic<uint64_t> Rescheduled_ = 0;
    std::atomic<uint64_t> ReadyPushRetries_ = 0;
    std::atomic<uint64_t> EmptyReadyPolls_ = 0;
    size_t WorkerCount_;
    bool HasRun_ = false;
};

} // namespace NScheduler
} // namespace NQdb
