#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <qdb/plan/ops/stats.h>
#include <qdb/scheduler/settings.h>

namespace NRegTest {

struct TSuite;

// Engine settings a run overrides on top of what each case variant sets. Every
// field is optional: an unset one keeps the engine default. A new engine option
// adds a field here, its flag in ParseRegTestOptions and a line in Apply.
struct TEngineOverrides {
    std::optional<NQdb::NScheduler::EJoinTableKind> JoinTable;

    void Apply(NQdb::NScheduler::TSettings& settings) const;
};

// Every setting of a regression run. Parsed once by ParseRegTestOptions and passed
// down explicitly: nothing below main reads the environment.
struct TRegTestOptions {
    TEngineOverrides Engine;
    // Print the timing of each phase of each case to stderr.
    bool Timing = false;
    // Trace the loading of fixtures to stderr.
    bool TraceFixtures = false;
    // Dump the plan after each optimizer pass to stderr.
    bool DumpPasses = false;
    // Persistent JIT cache directory; empty runs without a cache.
    std::string JitCacheDir;
};

// Parses and removes the run flags from argv (`--flag value` and `--flag=value`):
// --join-table, --timing, --trace-fixtures, --dump-passes, --jit-cache-dir and
// --arrow-memory-pool (applied at once, it is process-wide). Other arguments are left
// for the caller and for gtest. Throws std::invalid_argument with the accepted values
// on a bad flag value.
TRegTestOptions ParseRegTestOptions(int& argc, char** argv);

struct TFloatTolerance {
    double Absolute = 1e-12;
    double Relative = 1e-12;
};

struct TQueryCase {
    std::filesystem::path Base;
    std::filesystem::path Sql;
    std::filesystem::path PlanGolden;
    std::filesystem::path Golden;
    // Holds the error of a case that fails while it runs (see README.md).
    std::filesystem::path ErrorGolden;
    std::shared_ptr<TSuite> Suite;
    std::map<std::string, std::string> Tables;
    std::string Variant;
    bool Ordered = true;
    bool Execute = true;
    TFloatTolerance Tol;
    // The case must fail: planning errors in the .plan golden, running errors in
    // the .error golden. A success is reported as a failure of the case.
    bool ExpectError = false;

    bool HasTables() const { return !Tables.empty(); }
};

struct TComparison {
    bool Matches;
    std::string Actual;
    std::string Expected;
};

// Typed result comparison, also used for agreement between canonized variants.
TComparison CompareResults(const std::string& actual, const std::string& expected,
    bool ordered = true, TFloatTolerance tol = {});

std::string TestName(const std::filesystem::path& path);

struct TCaseResult {
    TComparison Plan;
    std::optional<TComparison> Result;
    // The error raised after planning, compared with the .error golden.
    std::optional<TComparison> Error;
};

std::vector<TQueryCase> CollectCases(
    const std::filesystem::path& casesDir,
    const std::filesystem::path& goldensDir);
// Creates the JIT cache directory: options.JitCacheDir, or a persistent cache next to
// the executable when it is empty. Returns the directory; the caller stores it in
// the options the cases run with.
std::filesystem::path InitializeJitCache(const char* executable, const TRegTestOptions& options);
// Prepare each selected suite/fixture once, sharing files across cases and variants.
void PrepareCases(std::span<const TQueryCase> tests, const TRegTestOptions& options = {});
void PrepareCase(const TQueryCase& test, const TRegTestOptions& options = {});
TComparison CheckAst(const TQueryCase& test, bool canonize);
TCaseResult CheckCase(const TQueryCase& test, bool canonize, const TRegTestOptions& options = {});
NQdb::TStatsPtr TableStats(const TQueryCase& test, const std::string& table);
std::filesystem::path TableFile(const TQueryCase& test, const std::string& table);
// Called only after all selected tests passed, including cross-variant checks.
void CommitGoldens(const std::vector<TQueryCase>& tests);
// Removes the golden files under `goldensDir` that belong to none of `tests`: the
// goldens of cases that were deleted or renamed. Only a canonize run over every
// case may call it, since a filtered run registers fewer cases.
void RemoveOrphanGoldens(const std::vector<TQueryCase>& tests, const std::filesystem::path& goldensDir);

} // namespace NRegTest
