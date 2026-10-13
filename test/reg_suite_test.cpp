#include "reg_suite.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

class TCaseDirectory {
public:
    TCaseDirectory() {
        auto name = (fs::temp_directory_path() / "qdb-suite-test-XXXXXX").string();
        std::vector<char> pattern(name.begin(), name.end());
        pattern.push_back('\0');
        auto* created = ::mkdtemp(pattern.data());
        if (!created) throw std::runtime_error("cannot create suite test directory");
        Root = created;
        CasesDir = Root / "cases";
        SuiteDir = CasesDir / "group";
        GoldensDir = Root / "goldens";
        Golden = GoldensDir / "group/query.result.json";
        fs::create_directories(SuiteDir);
        fs::create_directories(Golden.parent_path());
        Write("suite.json", Manifest);
        Write("query.sql", "SELECT v AS v FROM t ORDER BY v DESC;");
        Write("table.json", Table);
        WriteGolden(Result);
    }

    ~TCaseDirectory() {
        std::error_code error;
        fs::remove_all(Root, error);
    }

    void Write(std::string_view file, std::string_view text) const {
        std::ofstream output(SuiteDir / file);
        if (!(output << text)) throw std::runtime_error("cannot write suite test file");
    }

    void WriteGolden(std::string_view text) const {
        std::ofstream output(Golden);
        if (!(output << text)) throw std::runtime_error("cannot write suite test golden");
    }

    std::string ReadGolden() const {
        std::ifstream input(Golden);
        return {std::istreambuf_iterator<char>(input), {}};
    }

    std::vector<NRegTest::TExecCase> Collect() const {
        return NRegTest::CollectExecCases(CasesDir, GoldensDir);
    }

    NRegTest::TComparison Run(bool canonize = false) const {
        auto tests = Collect();
        NRegTest::PrepareExecCase(tests.at(0));
        return NRegTest::ExecuteCase(tests.at(0), canonize);
    }

    fs::path Root;
    fs::path CasesDir;
    fs::path SuiteDir;
    fs::path GoldensDir;
    fs::path Golden;
    static constexpr auto Manifest = R"({
        "version":1,
        "fixtures":{"data":{"json":"table.json","parquet":{"row_group_rows":1}}},
        "tables":{"t":"data"},"variants":["single"],
        "cases":{"query.sql":{"order":"ordered"}}
    })";
    static constexpr auto Table = R"({
        "version":1,"schema":"<struct (v i64)>",
        "rows":[["1"],["2"]]
    })";
    static constexpr auto Result = R"({
        "version":1,"schema":"<struct (v i64)>",
        "rows":[["2"],["1"]]
    })";
};

TEST(RegSuiteInfrastructure, NoManifestHasNoExecutionCases) {
    TCaseDirectory dir;
    fs::rename(dir.SuiteDir / "suite.json", dir.SuiteDir / "disabled.json");
    EXPECT_TRUE(dir.Collect().empty());
}

TEST(RegSuiteInfrastructure, RejectsUnknownFixturesAndVariants) {
    TCaseDirectory dir;
    auto manifest = std::string(TCaseDirectory::Manifest);
    manifest.replace(manifest.find("\"t\":\"data\""), 10, "\"t\":\"missing\"");
    dir.Write("suite.json", manifest);
    EXPECT_THROW(dir.Collect(), std::runtime_error);
    manifest = TCaseDirectory::Manifest;
    manifest.replace(manifest.find("\"single\""), 8, "\"unknown\"");
    dir.Write("suite.json", manifest);
    EXPECT_THROW(dir.Collect(), std::runtime_error);
}

TEST(RegSuiteInfrastructure, RejectsUnlistedQueries) {
    TCaseDirectory dir;
    dir.Write("unlisted.sql", "SELECT v FROM t;");
    EXPECT_THROW(dir.Collect(), std::runtime_error);
}

TEST(RegSuiteInfrastructure, OrderedComparisonPreservesOrder) {
    TCaseDirectory dir;
    auto golden = std::string(TCaseDirectory::Result);
    golden.replace(golden.find("[[\"2\"],[\"1\"]]"), 13, "[[\"1\"],[\"2\"]]");
    dir.WriteGolden(golden);
    EXPECT_FALSE(dir.Run().Matches);
}

TEST(RegSuiteInfrastructure, UnorderedComparisonPreservesDuplicates) {
    TCaseDirectory dir;
    auto manifest = std::string(TCaseDirectory::Manifest);
    manifest.replace(manifest.find("\"ordered\""), 9, "\"unordered\"");
    dir.Write("suite.json", manifest);
    dir.Write("query.sql", "SELECT v AS v FROM t;");
    EXPECT_TRUE(dir.Run().Matches);
    dir.Write("table.json", R"({
        "version":1,"schema":"<struct (v i64)>",
        "rows":[["2"],["2"]]
    })");
    dir.WriteGolden(R"({
        "version":1,"schema":"<struct (v i64)>",
        "rows":[["2"]]
    })");
    EXPECT_FALSE(dir.Run().Matches);
}

TEST(RegSuiteInfrastructure, RejectsNullInNonNullableFixture) {
    TCaseDirectory dir;
    dir.Write("table.json", R"({
        "version":1,"schema":"<struct (v i64)>",
        "rows":[[null]]
    })");
    try {
        dir.Run();
        FAIL() << "invalid fixture accepted";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("row 0, column 'v': NULL"), std::string::npos);
    }
}

TEST(RegSuiteInfrastructure, IntegersNeverRoundThroughDouble) {
    TCaseDirectory dir;
    const auto data = R"({
        "version":1,"schema":"<struct (v u64)>",
        "rows":[["18446744073709551615"],["9007199254740993"]]
    })";
    dir.Write("table.json", data);
    dir.WriteGolden(data);
    EXPECT_TRUE(dir.Run().Matches);
    dir.Write("table.json", R"({
        "version":1,"schema":"<struct (v u64)>",
        "rows":[["18446744073709551616"]]
    })");
    EXPECT_THROW(dir.Run(), std::runtime_error);
}

TEST(RegSuiteInfrastructure, RejectsUnmappedSqlTable) {
    TCaseDirectory dir;
    dir.Write("query.sql", "SELECT v FROM missing;");
    EXPECT_THROW(dir.Run(), std::runtime_error);
}

TEST(RegSuiteInfrastructure, SchemaUsesCoreParserAndPrinter) {
    TCaseDirectory dir;
    dir.WriteGolden(R"({
        "version":1,"schema":"<struct   (v   i64) >",
        "rows":[["2"],["1"]]
    })");
    EXPECT_TRUE(dir.Run().Matches);
    dir.WriteGolden(R"({
        "version":1,"schema":"<struct (v <named Nullable [i64]>)>",
        "rows":[["2"],["1"]]
    })");
    EXPECT_FALSE(dir.Run().Matches);
    dir.Write("table.json", R"({
        "version":1,"schema":"<struct (v i64)> trailing", "rows":[]
    })");
    EXPECT_THROW(dir.Run(), std::runtime_error);
    dir.Write("table.json", R"({
        "version":1,"schema":"<struct (v i64) (v i64)>", "rows":[]
    })");
    EXPECT_THROW(dir.Run(), std::runtime_error);
}

TEST(RegSuiteInfrastructure, RelativeRootIsNormalized) {
    TCaseDirectory dir;
    EXPECT_EQ(NRegTest::CollectExecCases(dir.CasesDir / ".", dir.GoldensDir / ".").size(), 1);
}

TEST(RegSuiteInfrastructure, GoldenPathMirrorsCasesOutsideCases) {
    TCaseDirectory dir;
    fs::copy(dir.SuiteDir, dir.CasesDir / "other", fs::copy_options::recursive);
    const auto tests = dir.Collect();
    ASSERT_EQ(tests.size(), 2);
    EXPECT_EQ(tests[0].Golden, dir.Golden);
    EXPECT_EQ(tests[1].Golden, dir.GoldensDir / "other/query.result.json");

    auto manifest = std::string(TCaseDirectory::Manifest);
    manifest.replace(manifest.find("\"order\":\"ordered\""), 17,
        "\"order\":\"ordered\",\"golden\":\"result.json\"");
    dir.Write("suite.json", manifest);
    EXPECT_THROW(dir.Collect(), std::runtime_error);
}

TEST(RegSuiteInfrastructure, DoesNotReadGoldensFromCaseDirectory) {
    TCaseDirectory dir;
    fs::rename(dir.Golden, dir.SuiteDir / "query.result.json");
    EXPECT_THROW(dir.Run(), std::runtime_error);
}

TEST(RegSuiteInfrastructure, CanonizeWaitsForExplicitCommit) {
    TCaseDirectory dir;
    auto tests = dir.Collect();
    const auto before = dir.ReadGolden();
    NRegTest::PrepareExecCase(tests.at(0));
    EXPECT_TRUE(NRegTest::ExecuteCase(tests.at(0), true).Matches);
    EXPECT_EQ(dir.ReadGolden(), before);
    NRegTest::CommitExecGoldens(tests);
    EXPECT_TRUE(NRegTest::ExecuteCase(tests.at(0), false).Matches);
}

TEST(RegSuiteInfrastructure, CanonizeRejectsDifferingVariantResults) {
    TCaseDirectory dir;
    auto tests = dir.Collect();
    const auto before = dir.ReadGolden();
    NRegTest::PrepareExecCase(tests.at(0));
    EXPECT_TRUE(NRegTest::ExecuteCase(tests.at(0), true).Matches);
    dir.Write("query.sql", "SELECT v AS v FROM t ORDER BY v ASC;");
    EXPECT_FALSE(NRegTest::ExecuteCase(tests.at(0), true).Matches);
    EXPECT_EQ(dir.ReadGolden(), before);
}

} // namespace
