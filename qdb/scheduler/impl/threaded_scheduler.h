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
    TTaskNode* PopReady();
    void ScheduleInput(TTaskNode* node);
    void ScheduleOutput(TTaskNode* node);
    void Work();
    bool FinishRun(TTaskNode* node, ETaskState nextState = ETaskState::Idle);
    void FinishNode(TTaskNode* node);
    void ResetStats();

    TSealedTaskGraph Graph_;
    TTaskNode* Root_ = nullptr;
    TExecutionState Execution_;
    TMPMCQueue<TTaskNode*> Ready_;
    // One permit is released per task pushed to Ready_; idle workers block on
    // it (after a short spin) instead of busy-yielding until work arrives.
    std::counting_semaphore<> WorkAvailable_{0};
    std::atomic<size_t> FinishedCount_ = 0;
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
