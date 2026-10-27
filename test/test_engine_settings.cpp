#include <gtest/gtest.h>

#include <qdb/scheduler/engine_settings.h>

#include <filesystem>
#include <fstream>
#include <iostream>

using namespace NQdb;
using NUtils::TConfig;

namespace {

TConfig MakeConfig() {
    return TConfig(EngineConfigSchema());
}

} // namespace

TEST(EngineSettings, UnsetKeysLeaveSettingsAlone) {
    auto config = MakeConfig();
    TEngineSettings settings;
    settings.Scheduler.Scheduler.WorkerCount = 3;
    settings.Scheduler.Join.Table = NScheduler::EJoinTableKind::Legacy;
    settings.Optimizer.EnableCbo = false;
    ApplyEngineConfig(config, settings);
    EXPECT_EQ(settings.Scheduler.Scheduler.WorkerCount, 3u);
    EXPECT_EQ(settings.Scheduler.Join.Table, NScheduler::EJoinTableKind::Legacy);
    EXPECT_FALSE(settings.Optimizer.EnableCbo);
}

TEST(EngineSettings, SchemaDefaultsAreTheDefaultsOfTheSettings) {
    const TEngineSettings defaults;
    auto config = MakeConfig();
    EXPECT_EQ(config.GetString("scheduler", "mode"), "threaded");
    EXPECT_EQ(static_cast<size_t>(config.GetInt("scheduler", "workers")), defaults.Scheduler.Scheduler.WorkerCount);
    EXPECT_EQ(config.GetInt("queue", "depth"), 4);
    EXPECT_EQ(config.GetString("scan", "strategy"), "auto");
    EXPECT_EQ(config.GetString("join", "table"), "row");
    EXPECT_TRUE(config.GetBool("join", "runtime_filters"));
    EXPECT_FALSE(config.GetBool("aggregate", "cascade_global"));
    EXPECT_TRUE(config.GetBool("aggregate", "partial"));
    EXPECT_EQ(config.GetString("aggregate", "table"), "auto");
    EXPECT_TRUE(config.GetBool("optimizer", "cbo"));
    EXPECT_EQ(config.GetDouble("late_materialization", "min_savings"), 2.0);
    EXPECT_EQ(config.GetString("io", "arrow_memory_pool"), "auto");
    EXPECT_EQ(config.GetString("jit", "cache_dir"), "");
}

TEST(EngineSettings, EveryDefaultSurvivesApplyingTheWholeDump) {
    // A dumped default config sets every key; applying it must change nothing.
    auto dumped = MakeConfig();
    auto config = MakeConfig();
    ASSERT_TRUE(config.LoadString(dumped.Dump()).has_value());
    TEngineSettings settings;
    ApplyEngineConfig(config, settings);
    const TEngineSettings defaults;
    EXPECT_EQ(settings.Scheduler.Scheduler.WorkerCount, defaults.Scheduler.Scheduler.WorkerCount);
    EXPECT_EQ(settings.Scheduler.Scheduler.Mode, defaults.Scheduler.Scheduler.Mode);
    EXPECT_EQ(settings.Scheduler.ScanSplit.TargetBytesPerTask, defaults.Scheduler.ScanSplit.TargetBytesPerTask);
    EXPECT_EQ(settings.Scheduler.HashShuffle.TargetOutputBatchRows, defaults.Scheduler.HashShuffle.TargetOutputBatchRows);
    EXPECT_EQ(settings.Scheduler.Join.RowJoinProbeHoldBytes, defaults.Scheduler.Join.RowJoinProbeHoldBytes);
    EXPECT_EQ(settings.Scheduler.Aggregate.PartialMinInputRows, defaults.Scheduler.Aggregate.PartialMinInputRows);
    EXPECT_EQ(settings.Scheduler.Aggregate.Table, defaults.Scheduler.Aggregate.Table);
    EXPECT_EQ(settings.Optimizer.LateMaterialization.MaxOutputRows, defaults.Optimizer.LateMaterialization.MaxOutputRows);
    EXPECT_EQ(settings.Optimizer.DumpPasses, nullptr);
    EXPECT_EQ(settings.Scheduler.Instrumentation.Output, nullptr);
}

TEST(EngineSettings, OverridesReachTheirFields) {
    auto config = MakeConfig();
    for (const char* spec : {
             "scheduler:mode=single",
             "scheduler:workers=2",
             "queue:depth=8",
             "queue:debug_counters=true",
             "scan:strategy=serial_read",
             "scan:max_tasks=5",
             "join:table=legacy",
             "join:runtime_filters=false",
             "join:force_runtime_filters=true",
             "shuffle:partitions=6",
             "shuffle:max_partitions=7",
             "aggregate:cascade_global=true",
             "aggregate:partial=false",
             "aggregate:table=row",
             "instrumentation:operators=true",
             "late_materialization:enabled=false",
             "late_materialization:max_rows=9",
             "late_materialization:min_savings=3.5",
             "optimizer:cbo=false",
             "optimizer:dump_passes=true",
             "jit:cache_dir=/tmp/jit",
             "io:arrow_memory_pool=system",
         })
    {
        const auto result = config.Override(spec);
        ASSERT_TRUE(result.has_value()) << spec << ": " << result.error();
    }
    TEngineSettings settings;
    ApplyEngineConfig(config, settings);
    EXPECT_EQ(settings.Scheduler.Scheduler.Mode, NScheduler::EExecutionMode::SingleThreadedScheduler);
    EXPECT_EQ(settings.Scheduler.Scheduler.WorkerCount, 2u);
    EXPECT_EQ(settings.Scheduler.Queue.RowsetCapacityPerLane, 8u);
    EXPECT_TRUE(settings.Scheduler.Queue.EnableDebugCounters);
    EXPECT_EQ(settings.Scheduler.ScanSplit.Strategy, NScheduler::EScanSplitStrategy::SerialRead);
    EXPECT_EQ(settings.Scheduler.ScanSplit.MaxScanTasks, 5u);
    EXPECT_EQ(settings.Scheduler.Join.Table, NScheduler::EJoinTableKind::Legacy);
    EXPECT_FALSE(settings.Optimizer.EnableRuntimeFilters);
    EXPECT_TRUE(settings.Optimizer.ForceRuntimeFilters);
    EXPECT_EQ(settings.Scheduler.HashShuffle.PartitionCount, 6u);
    EXPECT_EQ(settings.Scheduler.HashShuffle.MaxPartitionCount, 7u);
    EXPECT_TRUE(settings.Scheduler.Aggregate.CascadeGlobal);
    EXPECT_FALSE(settings.Scheduler.Aggregate.EnablePartial);
    EXPECT_EQ(settings.Scheduler.Aggregate.Table, NScheduler::EAggregateTableKind::Row);
    EXPECT_TRUE(settings.Scheduler.Instrumentation.Operators);
    EXPECT_EQ(settings.Scheduler.Instrumentation.Output, &std::cerr);
    EXPECT_FALSE(settings.Optimizer.LateMaterialization.Enabled);
    EXPECT_EQ(settings.Optimizer.LateMaterialization.MaxOutputRows, 9u);
    EXPECT_EQ(settings.Optimizer.LateMaterialization.MinSavingsFactor, 3.5);
    EXPECT_FALSE(settings.Optimizer.EnableCbo);
    EXPECT_EQ(settings.Optimizer.DumpPasses, &std::cerr);
    EXPECT_EQ(settings.Scheduler.Jit.CacheDir, "/tmp/jit");
    EXPECT_EQ(settings.ArrowMemoryPool, EArrowMemoryPool::System);
}

TEST(EngineSettings, BadSettingsAreErrorsThatListTheAccepted) {
    auto config = MakeConfig();
    auto message = [&](const char* spec) {
        const auto result = config.Override(spec);
        EXPECT_FALSE(result.has_value()) << spec;
        return result ? std::string() : result.error();
    };
    EXPECT_NE(message("join:table=hash").find("one of: row, legacy"), std::string::npos);
    EXPECT_NE(message("aggregate:table=rows").find("one of: auto, swiss, row"), std::string::npos);
    EXPECT_NE(message("join:tabel=row").find("table"), std::string::npos);
    EXPECT_NE(message("joins:table=row").find("accepted sections: scheduler, queue, scan, join"), std::string::npos);
    EXPECT_NE(message("scheduler:workers=0").find("an integer >= 1"), std::string::npos);
    EXPECT_NE(message("late_materialization:min_savings=0.5").find("a number >= 1"), std::string::npos);
    EXPECT_NE(message("io:arrow_memory_pool=tcmalloc").find("jemalloc"), std::string::npos);
}

namespace {

std::filesystem::path WriteIni(const std::string& name, const std::string& text) {
    const auto path = std::filesystem::temp_directory_path() / name;
    std::ofstream(path) << text;
    return path;
}

} // namespace

TEST(EngineSettings, SourcesApplyInOrder) {
    // defaults < default file < --config file < overrides
    const auto defaultFile = WriteIni(
        "qdb_test_default.ini", "[scan]\nmax_tasks = 1\n[queue]\ndepth = 2\n[shuffle]\nqueue = 3\n");
    const auto configFile = WriteIni("qdb_test_config.ini", "[scan]\nmax_tasks = 5\n[queue]\ndepth = 6\n");
    auto config = MakeConfig();
    const std::vector<std::string> overrides = { "scan:max_tasks=9" };
    const auto loaded = LoadEngineConfig(config, defaultFile, configFile.string(), overrides);
    ASSERT_TRUE(loaded.has_value()) << loaded.error();
    EXPECT_EQ(config.GetInt("scan", "max_tasks"), 9);
    EXPECT_EQ(config.GetInt("queue", "depth"), 6);
    EXPECT_EQ(config.GetInt("shuffle", "queue"), 3);
    EXPECT_EQ(config.GetInt("shuffle", "target_rows"), 16 * 1024);
    std::filesystem::remove(defaultFile);
    std::filesystem::remove(configFile);
}

TEST(EngineSettings, MissingDefaultFileIsSkipped) {
    auto config = MakeConfig();
    const auto missing = std::filesystem::temp_directory_path() / "qdb_test_no_such.ini";
    EXPECT_TRUE(LoadEngineConfig(config, missing, std::nullopt, {}).has_value());
    EXPECT_TRUE(LoadEngineConfig(config, {}, std::nullopt, {}).has_value());
}

TEST(EngineSettings, DefaultPathThatIsNotAFileIsAnError) {
    auto config = MakeConfig();
    const auto result = LoadEngineConfig(config, std::filesystem::temp_directory_path(), std::nullopt, {});
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("not a regular file"), std::string::npos);
}

TEST(EngineSettings, EmptyOrMissingConfigPathIsAnError) {
    auto config = MakeConfig();
    EXPECT_FALSE(LoadEngineConfig(config, {}, std::string(), {}).has_value());
    EXPECT_FALSE(LoadEngineConfig(config, {}, std::string("/no/such/qdb.ini"), {}).has_value());
}

TEST(EngineSettings, BadDefaultFileStopsBeforeLaterSources) {
    const auto bad = WriteIni("qdb_test_bad_default.ini", "[scan]\nnope = 1\n");
    auto config = MakeConfig();
    const std::vector<std::string> overrides = { "scan:max_tasks=9" };
    const auto result = LoadEngineConfig(config, bad, std::nullopt, overrides);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("unknown key 'nope'"), std::string::npos);
    EXPECT_FALSE(config.IsSet("scan", "max_tasks"));
    std::filesystem::remove(bad);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
