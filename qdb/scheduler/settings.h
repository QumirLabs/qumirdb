#pragma once

#include <algorithm>
#include <cstddef>
#include <iosfwd>
#include <thread>

namespace NQdb {
namespace NScheduler {

enum class EExecutionMode {
    SingleThreadedScheduler = 0,
    ThreadedScheduler = 1,
};

enum class EScanSplitStrategy {
    Auto = 0,
    RowGroupRange = 1,
    SerialRead = 2,
};

struct TSchedulerSettings {
    EExecutionMode Mode = EExecutionMode::ThreadedScheduler;
    // hardware_concurrency may be unknown (zero); always keep one worker.
    size_t WorkerCount = std::max(1u, std::thread::hardware_concurrency());
    size_t ReadyQueueCapacity = 1024;
};

struct TQueueSettings {
    // Rowset slots per connection lane. Depth 1 ping-pongs producer/consumer on
    // every batch; a small buffer (measured sweet spot ~4) lets a producer run
    // ahead, cutting handoff/scheduling overhead (~10-17% on TPC-H at 8-16
    // workers) with negligible memory cost. Override with --queue-depth.
    size_t RowsetCapacityPerLane = 4;
    bool EnableDebugCounters = false;
};

struct TScanSplitSettings {
    EScanSplitStrategy Strategy = EScanSplitStrategy::Auto;
    // Zero selects a per-source limit from scan size, workers and consumers.
    // A positive value is an explicit cap, including 1 for a serial scan.
    size_t MaxScanTasks = 0;
    size_t TargetRowsPerTask = 1024 * 1024;
    size_t TargetBytesPerTask = 64 * 1024 * 1024;
    size_t TinyInputRowsThreshold = 0;
};

struct THashShuffleSettings {
    size_t PartitionCount = 0;
    size_t MaxPartitionCount = 0;
    size_t MaxQueuedRowsetsPerLane = 1;
    size_t TargetOutputBatchRows = 16 * 1024;
    size_t MaxOutputBatchRows = 64 * 1024;
    size_t TargetOutputBatchBytes = 1024 * 1024;
};

struct TAggregateSettings {
    // Parallelize ungrouped/global aggregates with a partial->gather->combine
    // cascade instead of gathering every lane into one aggregate. Off by
    // default: the single-aggregate path is faster for cheap aggregates; the
    // cascade only pays off when the serial aggregate is the bottleneck (very
    // large inputs).
    bool CascadeGlobal = false;

    // Insert a partition-local partial aggregate before a repartition exchange
    // when statistics predict enough row reduction. Partial and final stages
    // are explicit so the same plan can later be placed on different workers.
    bool EnablePartial = true;
    size_t PartialMinInputRows = 1024 * 1024;
    size_t PartialMinReductionFactor = 8;
};

struct TDiagnosticsSettings {
    // Disabled by default. Applications may opt in and provide the destination
    // explicitly; the execution library never reads process environment state.
    bool RowGroupPredicate = false;
    bool Operators = false;
    std::ostream* Output = nullptr;
};

struct TSettings {
    TSchedulerSettings Scheduler;
    TQueueSettings Queue;
    TScanSplitSettings ScanSplit;
    THashShuffleSettings HashShuffle;
    TAggregateSettings Aggregate;
    TDiagnosticsSettings Diagnostics;
};

} // namespace NScheduler
} // namespace NQdb
