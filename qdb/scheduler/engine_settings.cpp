#include "engine_settings.h"

#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

namespace NQdb {

namespace {

using NUtils::TConfig;
using NUtils::TConfigSchema;

using TApply = std::function<void(const TConfig&, TEngineSettings&)>;

// The schema and, for each key, the code that moves its value into the settings.
// Both come from the same call, so a key cannot exist without its field.
class TEngineConfigBuilder {
public:
    TConfigSchema Schema;
    std::vector<TApply> Appliers;

    void Section(std::string name) {
        Section_ = std::move(name);
        Schema.Section(Section_);
    }

    template <class TAccess>
    void Bool(std::string key, TAccess field, std::string description) {
        Schema.Bool(key, field(Defaults_), std::move(description));
        Appliers.push_back([section = Section_, key, field](const TConfig& config, TEngineSettings& settings) {
            if (config.IsSet(section, key)) {
                field(settings) = config.GetBool(section, key);
            }
        });
    }

    // A flag whose field is not a bool, as an output stream pointer is.
    void Flag(
        std::string key,
        bool def,
        std::function<void(TEngineSettings&, bool)> set,
        std::string description)
    {
        Schema.Bool(key, def, std::move(description));
        Appliers.push_back([section = Section_, key, set = std::move(set)](
            const TConfig& config, TEngineSettings& settings)
        {
            if (config.IsSet(section, key)) {
                set(settings, config.GetBool(section, key));
            }
        });
    }

    // An unsigned field; the bounds are the schema's.
    template <class TAccess>
    void Size(std::string key, TAccess field, std::string description, int64_t min = 0) {
        Schema.Int(key, static_cast<int64_t>(field(Defaults_)), std::move(description), min);
        Appliers.push_back([section = Section_, key, field](const TConfig& config, TEngineSettings& settings) {
            if (config.IsSet(section, key)) {
                field(settings) = static_cast<std::remove_reference_t<decltype(field(settings))>>(
                    config.GetInt(section, key));
            }
        });
    }

    template <class TAccess>
    void Double(std::string key, TAccess field, std::string description, double min) {
        Schema.Double(key, field(Defaults_), std::move(description), min);
        Appliers.push_back([section = Section_, key, field](const TConfig& config, TEngineSettings& settings) {
            if (config.IsSet(section, key)) {
                field(settings) = config.GetDouble(section, key);
            }
        });
    }

    template <class TAccess>
    void String(std::string key, TAccess field, std::string description) {
        Schema.String(key, field(Defaults_), std::move(description));
        Appliers.push_back([section = Section_, key, field](const TConfig& config, TEngineSettings& settings) {
            if (config.IsSet(section, key)) {
                field(settings) = config.GetString(section, key);
            }
        });
    }

    // `names` is the comma-separated list kept next to the enum, `parse` its
    // parser, so the schema and the engine accept the same spellings.
    template <class TAccess, class TParse>
    void Enum(std::string key, std::string_view names, TParse parse, TAccess field, std::string description) {
        std::vector<std::string> values;
        std::string defaultName;
        const auto defaultValue = field(Defaults_);
        while (!names.empty()) {
            const auto comma = names.find(',');
            std::string name(names.substr(0, comma));
            names = (comma == std::string_view::npos) ? std::string_view() : names.substr(comma + 2);
            if (parse(name) == defaultValue) {
                defaultName = name;
            }
            values.push_back(std::move(name));
        }
        Schema.Enum(key, std::move(values), std::move(defaultName), std::move(description));
        Appliers.push_back([section = Section_, key, field, parse](const TConfig& config, TEngineSettings& settings) {
            if (config.IsSet(section, key)) {
                field(settings) = *parse(config.GetString(section, key));
            }
        });
    }

private:
    const TEngineSettings Defaults_;
    std::string Section_;
};

TEngineConfigBuilder BuildEngineConfig() {
    TEngineConfigBuilder b;

    b.Section("scheduler");
    b.Enum("mode", NScheduler::ExecutionModeNames, NScheduler::ParseExecutionMode,
        [](auto& s) -> auto& { return s.Scheduler.Scheduler.Mode; },
        "Execution mode.");
    b.Size("workers", [](auto& s) -> auto& { return s.Scheduler.Scheduler.WorkerCount; },
        "Worker threads; the default is the logical CPU count.", 1);
    b.Size("ready_queue_capacity", [](auto& s) -> auto& { return s.Scheduler.Scheduler.ReadyQueueCapacity; },
        "Capacity of the queue of ready tasks.", 1);

    b.Section("queue");
    b.Size("depth", [](auto& s) -> auto& { return s.Scheduler.Queue.RowsetCapacityPerLane; },
        "Rowset slots per connection lane.", 1);
    b.Bool("debug_counters", [](auto& s) -> auto& { return s.Scheduler.Queue.EnableDebugCounters; },
        "Collect scheduler and connection counters; --verbose prints them.");

    b.Section("scan");
    b.Enum("strategy", NScheduler::ScanSplitStrategyNames, NScheduler::ParseScanSplitStrategy,
        [](auto& s) -> auto& { return s.Scheduler.ScanSplit.Strategy; },
        "How a parquet scan is split into tasks.");
    b.Size("max_tasks", [](auto& s) -> auto& { return s.Scheduler.ScanSplit.MaxScanTasks; },
        "Cap on scan tasks per source; 0 chooses from the scan size, workers and consumers.");
    b.Size("target_rows_per_task", [](auto& s) -> auto& { return s.Scheduler.ScanSplit.TargetRowsPerTask; },
        "Rows a scan task aims to read.", 1);
    b.Size("target_bytes_per_task", [](auto& s) -> auto& { return s.Scheduler.ScanSplit.TargetBytesPerTask; },
        "Bytes a scan task aims to read.", 1);
    b.Size("tiny_input_rows", [](auto& s) -> auto& { return s.Scheduler.ScanSplit.TinyInputRowsThreshold; },
        "Scans of at most this many rows are not split; 0 splits every scan.");

    b.Section("join");
    b.Enum("table", NScheduler::JoinTableKindNames, NScheduler::ParseJoinTableKind,
        [](auto& s) -> auto& { return s.Scheduler.Join.Table; },
        "Hash table of an inner equi-join with a fixed build side.");
    b.Size("row_probe_hold_bytes", [](auto& s) -> auto& { return s.Scheduler.Join.RowJoinProbeHoldBytes; },
        "Probe bytes a row join task holds while its build runs; 0 disables the hold.");
    b.Bool("runtime_filters", [](auto& s) -> auto& { return s.Optimizer.EnableRuntimeFilters; },
        "Push join runtime filters to the probe side scans.");
    b.Bool("force_runtime_filters", [](auto& s) -> auto& { return s.Optimizer.ForceRuntimeFilters; },
        "Emit a runtime filter wherever a build side exists, ignoring selectivity estimates.");

    b.Section("shuffle");
    b.Size("partitions", [](auto& s) -> auto& { return s.Scheduler.HashShuffle.PartitionCount; },
        "Partition count of a hash shuffle; 0 chooses from the input lanes and workers.");
    b.Size("max_partitions", [](auto& s) -> auto& { return s.Scheduler.HashShuffle.MaxPartitionCount; },
        "Upper bound of the partition count; 0 means no bound beyond partitions.");
    b.Size("queue", [](auto& s) -> auto& { return s.Scheduler.HashShuffle.MaxQueuedRowsetsPerLane; },
        "Rowset queue capacity per shuffle lane.", 1);
    b.Size("target_rows", [](auto& s) -> auto& { return s.Scheduler.HashShuffle.TargetOutputBatchRows; },
        "Target rows per materialized shuffle batch.", 1);
    b.Size("max_rows", [](auto& s) -> auto& { return s.Scheduler.HashShuffle.MaxOutputBatchRows; },
        "Maximum rows per materialized shuffle batch.", 1);
    b.Size("target_bytes", [](auto& s) -> auto& { return s.Scheduler.HashShuffle.TargetOutputBatchBytes; },
        "Byte target for shuffle output and retained inputs.", 1);

    b.Section("aggregate");
    b.Bool("cascade_global", [](auto& s) -> auto& { return s.Scheduler.Aggregate.CascadeGlobal; },
        "Parallelize global aggregates as partial, gather, combine.");
    b.Bool("partial", [](auto& s) -> auto& { return s.Scheduler.Aggregate.EnablePartial; },
        "Insert a partition-local partial aggregate before a repartition unless statistics predict too little reduction.");
    b.Size("partial_min_input_rows", [](auto& s) -> auto& { return s.Scheduler.Aggregate.PartialMinInputRows; },
        "Estimated input rows from which a partial aggregate is considered.");
    b.Size("partial_min_reduction_factor", [](auto& s) -> auto& { return s.Scheduler.Aggregate.PartialMinReductionFactor; },
        "Row reduction a partial aggregate must reach: estimated by the planner, and observed by each lane at run time.", 1);
    b.Size("partial_sample_rows", [](auto& s) -> auto& { return s.Scheduler.Aggregate.PartialSampleRows; },
        "Rows each lane of a partial aggregate hashes before it judges its observed reduction and may switch to pass-through.", 1);
    b.Size("partial_max_groups", [](auto& s) -> auto& { return s.Scheduler.Aggregate.PartialMaxGroups; },
        "Groups at which a partial aggregate emits its table and restarts it empty.", 1);
    b.Enum("table", NScheduler::AggregateTableKindNames, NScheduler::ParseAggregateTableKind,
        [](auto& s) -> auto& { return s.Scheduler.Aggregate.Table; },
        "Hash table of a grouped aggregate: swiss, row, or auto, which moves a swiss table to rows once it is large.");

    b.Section("instrumentation");
    b.Bool("operators", [](auto& s) -> auto& { return s.Scheduler.Instrumentation.Operators; },
        "Report per-operator statistics to stderr.");
    b.Bool("row_group_predicate", [](auto& s) -> auto& { return s.Scheduler.Instrumentation.RowGroupPredicate; },
        "Report row group predicate decisions to stderr.");

    b.Section("late_materialization");
    b.Bool("enabled", [](auto& s) -> auto& { return s.Optimizer.LateMaterialization.Enabled; },
        "Fetch wide columns after the rows are chosen when the cost model says it pays.");
    b.Size("max_rows", [](auto& s) -> auto& { return s.Optimizer.LateMaterialization.MaxOutputRows; },
        "Maximum statically known output rows.", 1);
    b.Double("min_savings", [](auto& s) -> auto& { return s.Optimizer.LateMaterialization.MinSavingsFactor; },
        "Factor by which the fetched bytes must shrink.", 1.0);

    b.Section("optimizer");
    b.Bool("cbo", [](auto& s) -> auto& { return s.Optimizer.EnableCbo; },
        "Reorder joins by cost.");
    b.Flag("dump_passes", false,
        [](TEngineSettings& s, bool on) { s.Optimizer.DumpPasses = on ? &std::cerr : nullptr; },
        "Dump the plan to stderr after each optimizer pass.");

    b.Section("jit");
    b.String("cache_dir", [](auto& s) -> auto& { return s.Scheduler.Jit.CacheDir; },
        "Persistent cache of compiled kernel dependencies; empty disables it.");

    b.Section("io");
    b.Enum("arrow_memory_pool", ArrowMemoryPoolNames, ParseArrowMemoryPool,
        [](auto& s) -> auto& { return s.ArrowMemoryPool; },
        "Allocator behind parquet reads.");

    return b;
}

const TEngineConfigBuilder& EngineConfig() {
    static const TEngineConfigBuilder config = BuildEngineConfig();
    return config;
}

} // namespace

const NUtils::TConfigSchema& EngineConfigSchema() {
    return EngineConfig().Schema;
}

void ApplyEngineConfig(const NUtils::TConfig& config, TEngineSettings& settings) {
    for (const auto& apply : EngineConfig().Appliers) {
        apply(config, settings);
    }
    auto& instrumentation = settings.Scheduler.Instrumentation;
    instrumentation.Output = (instrumentation.Operators || instrumentation.RowGroupPredicate)
        ? &std::cerr
        : nullptr;
}

NUtils::TConfig::TResult LoadEngineConfig(
    NUtils::TConfig& config,
    const std::filesystem::path& defaultPath,
    const std::optional<std::string>& configPath,
    std::span<const std::string> overrides)
{
    std::error_code error;
    if (!defaultPath.empty() && std::filesystem::exists(defaultPath, error)) {
        if (!std::filesystem::is_regular_file(defaultPath, error)) {
            return std::unexpected("default config path '" + defaultPath.string() + "' is not a regular file");
        }
        if (auto loaded = config.LoadFile(defaultPath.string()); !loaded) {
            return loaded;
        }
    }
    if (configPath) {
        if (configPath->empty()) {
            return std::unexpected("the config path is empty");
        }
        if (auto loaded = config.LoadFile(*configPath); !loaded) {
            return loaded;
        }
    }
    for (const auto& spec : overrides) {
        if (auto applied = config.Override(spec); !applied) {
            return applied;
        }
    }
    return {};
}

} // namespace NQdb
