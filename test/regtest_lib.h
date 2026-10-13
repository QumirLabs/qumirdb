#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <qdb/plan/ops/stats.h>

namespace NRegTest {

struct TSuite;

struct TQueryCase {
    std::filesystem::path Base;
    std::filesystem::path Sql;
    std::filesystem::path PlanGolden;
    std::filesystem::path Golden;
    std::shared_ptr<TSuite> Suite;
    std::map<std::string, std::string> Tables;
    std::string Variant;
    bool Ordered = true;

    bool HasTables() const { return !Tables.empty(); }
};

struct TComparison {
    bool Matches;
    std::string Actual;
    std::string Expected;
};

std::string TestName(const std::filesystem::path& path);

struct TCaseResult {
    TComparison Plan;
    std::optional<TComparison> Result;
};

std::vector<TQueryCase> CollectCases(
    const std::filesystem::path& casesDir,
    const std::filesystem::path& goldensDir);
// Called once by the preparation environment, for selected cases only.
void PrepareCase(const TQueryCase& test);
TComparison CheckAst(const TQueryCase& test, bool canonize);
TCaseResult CheckCase(const TQueryCase& test, bool canonize);
NQdb::TStatsPtr TableStats(const TQueryCase& test, const std::string& table);
std::filesystem::path TableFile(const TQueryCase& test, const std::string& table);
// Called only after all selected tests passed, including cross-variant checks.
void CommitGoldens(const std::vector<TQueryCase>& tests);

} // namespace NRegTest
