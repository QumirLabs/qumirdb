#pragma once

#include <cstdint>

namespace NQdb {
namespace NScheduler {

class TTaskInstrumentationSetup;
struct TTaskEdge; // graph.h
struct TPortRebind; // port.h

enum class ETaskResult {
    OK = 0,
    NEED_DATA = 1,
    BLOCKED_OUTPUT = 2,
    FINISHED = 3,
    // Waits for an event: a wake from outside the graph or a producer's finish.
    WAITING = 4,
};

enum class ETaskState {
    Idle = 0,
    Queued = 1,
    Running = 2,
    // Woken by a neighbour while running.
    Reschedule = 3,
    Finished = 4,
    // Returned WAITING; neighbours' data and demand do not wake it.
    Waiting = 5,
    // Woken by an event while running.
    Notified = 6,
};

class ITask {
public:
    virtual ~ITask() = default;

    virtual ETaskResult Execute() = 0;
    virtual void ConfigureInstrumentation(TTaskInstrumentationSetup&) {}

    // Whether the demand path should schedule this inbound edge's producer.
    // Default: all. Lets a task that reads one input at a time (asymmetric join:
    // build side first, then probe) avoid waking a producer it won't yet read.
    virtual bool WantsInput(const TTaskEdge&) const noexcept { return true; }
    // Dependency edges signal publication, not every rowset produced upstream.
    virtual bool TryNotifyDependency() noexcept { return true; }
    // Moves the one port `rebind` matches, before the task runs; false if none
    // matches. TTaskGraph::Apply uses it to cut an edge.
    virtual bool RebindPort(const TPortRebind&) {
        return false;
    }
};

struct TSchedulerRunStats {
    uint64_t Scheduled = 0;
    uint64_t Popped = 0;
    uint64_t Executed = 0;
    uint64_t NeedData = 0;
    uint64_t BlockedOutput = 0;
    uint64_t Ok = 0;
    uint64_t Finished = 0;
    uint64_t Waiting = 0;
    uint64_t Rescheduled = 0;
    uint64_t ReadyPushRetries = 0;
    uint64_t EmptyReadyPolls = 0;
};

} // namespace NScheduler
} // namespace NQdb
