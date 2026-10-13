#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace NRegTest {

struct TSuite;

struct TExecCase {
    std::filesystem::path Base;
    std::filesystem::path Sql;
    std::filesystem::path Golden;
    std::shared_ptr<TSuite> Suite;
    std::map<std::string, std::string> Tables;
    std::string Variant;
    bool Ordered = true;
};

struct TComparison {
    bool Matches;
    std::string Actual;
    std::string Expected;
};

std::vector<TExecCase> CollectExecCases(
    const std::filesystem::path& casesDir,
    const std::filesystem::path& goldensDir);
// Called once by the preparation environment, for selected cases only.
void PrepareExecCase(const TExecCase& test);
TComparison ExecuteCase(const TExecCase& test, bool canonize);
// Called only after all selected tests passed, including cross-variant checks.
void CommitExecGoldens(const std::vector<TExecCase>& tests);

} // namespace NRegTest
