#pragma once

#include <qdb/kernel/backend.h>

#include <algorithm>
#include <cstddef>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace NQdb {
namespace NScheduler {

enum class EExecutionMode {
    SingleThreadedScheduler = 0,
    ThreadedScheduler = 1,
};

// The names the command lines and the config accept, next to each enum.
inline constexpr std::string_view ExecutionModeNames = "single, threaded";

inline std::optional<EExecutionMode> ParseExecutionMode(std::string_view name) {
    if (name == "single") return EExecutionMode::SingleThreadedScheduler;
    if (name == "threaded") return EExecutionMode::ThreadedScheduler;
    return std::nullopt;
}

enum class EScanSplitStrategy {
    Auto = 0,
    RowGroupRange = 1,
    SerialRead = 2,
};

inline constexpr std::string_view ScanSplitStrategyNames = "auto, row_group_range, serial_read";

inline std::optional<EScanSplitStrategy> ParseScanSplitStrategy(std::string_view name) {
    if (name == "auto") return EScanSplitStrategy::Auto;
    if (name == "row_group_range") return EScanSplitStrategy::RowGroupRange;
    if (name == "serial_read") return EScanSplitStrategy::SerialRead;
    return std::nullopt;
}

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
    // workers) with negligible memory cost. Override with [queue] depth.
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

// Which hash table an inner equi-join with a fixed build side uses. Row is the
// arena of build rows behind a salted pointer table (kernel/join/join_rows.oz);
// Legacy is the symmetric Robin Hood table with per-key row-id buckets.
enum class EJoinTableKind {
    Row = 0,
    Legacy = 1,
};

// The names the config accepts.
inline constexpr std::string_view JoinTableKindNames = "row, legacy";

inline std::optional<EJoinTableKind> ParseJoinTableKind(std::string_view name) {
    if (name == "row") return EJoinTableKind::Row;
    if (name == "legacy") return EJoinTableKind::Legacy;
    return std::nullopt;
}

struct TJoinSettings {
    EJoinTableKind Table = EJoinTableKind::Row;
    // Probe bytes one row join task may hold while its table is being built.
    // At or above it the task waits for the build input, as it did before
    // probe batches were held; below it, it takes probe batches again. Zero
    // disables the hold. The bound is per task, so a join over P partitions
    // holds about P times this. A batch is taken while the held bytes are
    // below the budget, so the one that crosses it is still accepted (a single
    // batch larger than the budget too, when nothing is held): the peak can
    // exceed the budget by one batch per task.
    // 16 MiB: measured on TPC-DS SF10 q74 and TPC-H SF100 Q15/Q18, where the
    // hold peaks at 40, 62 and 37 MB per task. Budgets of 4 to 64 MiB gave the
    // same time and peak RSS there, so the default is the one that keeps the
    // worst case small: 16 partitions hold about 256 MiB.
    size_t RowJoinProbeHoldBytes = 16 * 1024 * 1024;
};

struct THashShuffleSettings {
    size_t PartitionCount = 0;
    size_t MaxPartitionCount = 0;
    size_t MaxQueuedRowsetsPerLane = 1;
    size_t TargetOutputBatchRows = 16 * 1024;
    size_t MaxOutputBatchRows = 64 * 1024;
    size_t TargetOutputBatchBytes = 1024 * 1024;
};

// Which layout the hash table of a grouped aggregate task has. Swiss is the
// swiss table, with one array per state word. Row keeps the key and the
// states of a group in one arena row behind salted entries
// (kernel/aggregation/aggregation_rows.oz). Auto starts every task swiss and
// moves its table to rows when it must grow past the size from which rows are
// faster. Aggregates with string keys or states, and those with groups over
// an empty input, have only the swiss table.
enum class EAggregateTableKind {
    Auto = 0,
    Swiss = 1,
    Row = 2,
};

// The names the config accepts.
inline constexpr std::string_view AggregateTableKindNames = "auto, swiss, row";

inline std::optional<EAggregateTableKind> ParseAggregateTableKind(std::string_view name) {
    if (name == "auto") {
        return EAggregateTableKind::Auto;
    }
    if (name == "swiss") {
        return EAggregateTableKind::Swiss;
    }
    if (name == "row") {
        return EAggregateTableKind::Row;
    }
    return std::nullopt;
}

inline std::string_view AggregateTableKindName(EAggregateTableKind kind) {
    switch (kind) {
        case EAggregateTableKind::Auto:
            return "auto";
        case EAggregateTableKind::Swiss:
            return "swiss";
        case EAggregateTableKind::Row:
            return "row";
    }
    return "unknown";
}

// Lowering of an ungrouped or global aggregate over a parallel input: the
// partial->gather->combine cascade, one gathered aggregate, or a choice per
// aggregate from the estimated input size (ShouldCascadeGlobalAggregate).
enum class EGlobalCascadeMode {
    Auto = 0,
    On = 1,
    Off = 2,
};

// `true` and `false` keep the spelling of the former boolean key.
inline constexpr std::string_view GlobalCascadeModeNames = "auto, true, false";

inline std::optional<EGlobalCascadeMode> ParseGlobalCascadeMode(std::string_view name) {
    if (name == "auto") {
        return EGlobalCascadeMode::Auto;
    }
    if (name == "true") {
        return EGlobalCascadeMode::On;
    }
    if (name == "false") {
        return EGlobalCascadeMode::Off;
    }
    return std::nullopt;
}

inline std::string_view GlobalCascadeModeName(EGlobalCascadeMode mode) {
    switch (mode) {
        case EGlobalCascadeMode::Auto:
            return "auto";
        case EGlobalCascadeMode::On:
            return "true";
        case EGlobalCascadeMode::Off:
            return "false";
    }
    return "unknown";
}

struct TAggregateSettings {
    // Auto cascades when the estimated input is at least PartialMinInputRows.
    EGlobalCascadeMode CascadeGlobal = EGlobalCascadeMode::Auto;

    // Insert a partition-local partial aggregate before a repartition exchange
    // when statistics predict enough row reduction. Partial and final stages
    // are explicit so the same plan can later be placed on different workers.
    bool EnablePartial = true;
    size_t PartialMinInputRows = 1024 * 1024;
    size_t PartialMinReductionFactor = 8;

    // Every partial aggregate judges its own reduction at run time, per lane,
    // whatever statistics the planner had: once a lane has hashed
    // PartialSampleRows rows and holds more than one group per
    // PartialMinReductionFactor rows, it stops hashing and hands its rows
    // through as single-row partial states. Statistics only decide whether the
    // planner inserts the partial stage. Its table emits its groups and
    // restarts empty at PartialMaxGroups groups, so one lane's table stays
    // bounded whatever the input.
    size_t PartialSampleRows = 1 << 20;
    size_t PartialMaxGroups = 1 << 20;

    EAggregateTableKind Table = EAggregateTableKind::Auto;
};

struct TJitSettings {
    // Persistent cache of compiled kernel dependencies; empty disables it.
    std::string CacheDir;
};

struct TKernelSettings {
    EKernelBackend Backend = EKernelBackend::Llvm;
};

struct TInstrumentationSettings {
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
    TJoinSettings Join;
    TAggregateSettings Aggregate;
    TJitSettings Jit;
    TKernelSettings Kernel;
    TInstrumentationSettings Instrumentation;
};

} // namespace NScheduler
} // namespace NQdb
