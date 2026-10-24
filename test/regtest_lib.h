#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <qdb/plan/ops/stats.h>

namespace NRegTest {

struct TSuite;

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
// A persistent cache next to the executable, unless QDB_JIT_CACHE_DIR is set.
std::filesystem::path InitializeJitCache(const char* executable);
// Prepare each selected suite/fixture once, sharing files across cases and variants.
void PrepareCases(std::span<const TQueryCase> tests);
void PrepareCase(const TQueryCase& test);
TComparison CheckAst(const TQueryCase& test, bool canonize);
TCaseResult CheckCase(const TQueryCase& test, bool canonize);
NQdb::TStatsPtr TableStats(const TQueryCase& test, const std::string& table);
std::filesystem::path TableFile(const TQueryCase& test, const std::string& table);
// Called only after all selected tests passed, including cross-variant checks.
void CommitGoldens(const std::vector<TQueryCase>& tests);
// Removes the golden files under `goldensDir` that belong to none of `tests`: the
// goldens of cases that were deleted or renamed. Only a canonize run over every
// case may call it, since a filtered run registers fewer cases.
void RemoveOrphanGoldens(const std::vector<TQueryCase>& tests, const std::filesystem::path& goldensDir);

} // namespace NRegTest
