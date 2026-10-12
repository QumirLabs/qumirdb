#pragma once

#include <cstdint>

namespace NQdb {
namespace NScheduler {

struct TTaskEdge; // graph.h

enum class ETaskResult {
    OK = 0,
    NEED_DATA = 1,
    BLOCKED_OUTPUT = 2,
    FINISHED = 3,
    WAITING = 4,
};

enum class ETaskState {
    Idle = 0,
    Queued = 1,
    Running = 2,
    Reschedule = 3,
    Finished = 4,
};

class ITaskNode {
public:
    virtual ~ITaskNode() = default;

    virtual ETaskResult Execute() = 0;

    // Whether the demand path should schedule this inbound edge's producer.
    // Default: all. Lets a task that reads one input at a time (asymmetric join:
    // build side first, then probe) avoid waking a producer it won't yet read.
    virtual bool WantsInput(const TTaskEdge&) const { return true; }
    // Dependency edges signal publication, not every rowset produced upstream.
    virtual bool ShouldNotifyDependency() const { return true; }
};

struct TSchedulerRunStats {
    uint64_t Scheduled = 0;
    uint64_t Popped = 0;
    uint64_t Executed = 0;
    uint64_t NeedData = 0;
    uint64_t Waiting = 0;
    uint64_t BlockedOutput = 0;
    uint64_t Ok = 0;
    uint64_t Finished = 0;
    uint64_t Rescheduled = 0;
    uint64_t ReadyPushRetries = 0;
    uint64_t EmptyReadyPolls = 0;
};

} // namespace NScheduler
} // namespace NQdb
