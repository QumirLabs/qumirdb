#include "regtest_lib.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
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

    std::vector<NRegTest::TQueryCase> Collect() const {
        return NRegTest::CollectCases(CasesDir, GoldensDir);
    }

    NRegTest::TComparison Run(bool canonize = false) const {
        auto tests = Collect();
        NRegTest::PrepareCase(tests.at(0));
        return NRegTest::CheckCase(tests.at(0), canonize).Result.value();
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
        "schema":"<struct (v i64)>",
        "rows":[["2"],["1"]]
    })";
};

std::string FloatResult(std::string_view rows, std::string_view schema = "<struct (v f64)>") {
    return "{\"schema\":\"" + std::string(schema) + "\",\"rows\":" + std::string(rows) + '}';
}

TEST(RegResultComparison, DefaultToleranceAcceptsAggregateRoundingWithoutChangingGolden) {
    const auto actual = FloatResult("[[306804.95999999996]]");
    const auto expected = FloatResult("[[306804.96000000002]]");
    const auto result = NRegTest::CompareResults(actual, expected);
    EXPECT_TRUE(result.Matches);
    EXPECT_NE(result.Actual, result.Expected);
    EXPECT_FALSE(NRegTest::CompareResults(actual, expected, true, {0, 0}).Matches);
}

TEST(RegResultComparison, AbsoluteAndRelativeToleranceIncludeTheirBoundary) {
    const auto zero = FloatResult("[[0]]");
    EXPECT_TRUE(NRegTest::CompareResults(zero, FloatResult("[[1]]"), true, {1, 0}).Matches);
    EXPECT_FALSE(NRegTest::CompareResults(zero, FloatResult("[[1.000001]]"), true, {1, 0}).Matches);
    EXPECT_TRUE(NRegTest::CompareResults(zero, FloatResult("[[1e-13]]")).Matches);
    EXPECT_FALSE(NRegTest::CompareResults(zero, FloatResult("[[1e-6]]")).Matches);
    const auto a = FloatResult("[[-100]]");
    const auto b = FloatResult("[[-200]]");
    EXPECT_TRUE(NRegTest::CompareResults(a, b, true, {0, 0.5}).Matches);
    EXPECT_TRUE(NRegTest::CompareResults(b, a, true, {0, 0.5}).Matches);
    EXPECT_FALSE(NRegTest::CompareResults(a, b, true, {0, 0.499}).Matches);
    EXPECT_TRUE(NRegTest::CompareResults(zero, zero, true, {0, 0}).Matches);
}

TEST(RegResultComparison, ToleranceCombinesAbsoluteAndRelativeTerms) {
    EXPECT_TRUE(NRegTest::CompareResults(FloatResult("[[100]]"), FloatResult("[[102]]"),
        true, {1, 0.01}).Matches);
    EXPECT_FALSE(NRegTest::CompareResults(FloatResult("[[100]]"), FloatResult("[[102]]"),
        true, {0.5, 0.01}).Matches);
}

TEST(RegResultComparison, OverflowDoesNotMakeDifferentFiniteValuesEqual) {
    const auto positive = FloatResult("[[1.7976931348623157e308]]");
    const auto negative = FloatResult("[[-1.7976931348623157e308]]");
    EXPECT_FALSE(NRegTest::CompareResults(positive, negative, true, {0, 1}).Matches);
    EXPECT_TRUE(NRegTest::CompareResults(positive, negative, true, {0, 2}).Matches);
    // Both the naive difference and abs + rel * scale overflow here.
    EXPECT_FALSE(NRegTest::CompareResults(positive, negative, true,
        {1.7976931348623157e308, 0.5}).Matches);
}

TEST(RegResultComparison, NonFiniteTagsNullAndSignedZeroStayExact) {
    const auto nullable = "<struct (v <named Nullable [f64]>)>";
    EXPECT_TRUE(NRegTest::CompareResults(FloatResult("[[null],[1]]", nullable),
        FloatResult("[[null],[1.0000000000000002]]", nullable)).Matches);
    const auto special = FloatResult(
        R"([[{"$float":"nan"}],[{"$float":"+inf"}],[{"$float":"-inf"}],[{"$float":"-0"}],[null]])",
        "<struct (v <named Nullable [f64]>)>");
    EXPECT_TRUE(NRegTest::CompareResults(special, special).Matches);
    for (const auto value : {R"({"$float":"nan"})", R"({"$float":"+inf"})",
            R"({"$float":"-inf"})", R"({"$float":"-0"})", "null"})
    {
        const auto tagged = FloatResult("[[" + std::string(value) + "]]",
            "<struct (v <named Nullable [f64]>)>");
        EXPECT_FALSE(NRegTest::CompareResults(tagged,
            FloatResult("[[0]]", "<struct (v <named Nullable [f64]>)>"), true, {1e100, 1e100}).Matches);
    }
    EXPECT_FALSE(NRegTest::CompareResults(FloatResult(R"([[{"$float":"+inf"}]])"),
        FloatResult(R"([[{"$float":"-inf"}]])")).Matches);
    EXPECT_FALSE(NRegTest::CompareResults(FloatResult("[[1.7976931348623157e308]]"),
        FloatResult(R"([[{"$float":"+inf"}]])")).Matches);
}

TEST(RegResultComparison, OtherTypesAndSchemasStayExact) {
    const auto integer = R"({"schema":"<struct (v u64)>","rows":[["9007199254740993"]]})";
    const auto different = R"({"schema":"<struct (v u64)>","rows":[["9007199254740992"]]})";
    EXPECT_FALSE(NRegTest::CompareResults(integer, different, true, {1e100, 1e100}).Matches);
    EXPECT_FALSE(NRegTest::CompareResults(FloatResult("[[1]]"),
        R"({"schema":"<struct (v i32)>","rows":[[1]]})").Matches);
    EXPECT_FALSE(NRegTest::CompareResults(FloatResult("[[1]]"),
        FloatResult("[[1]]", "<struct (v <named Nullable [f64]>)>")).Matches);
    const auto schema = "<struct (v f64) (s string) (b bool)>";
    const auto base = FloatResult(R"([[1,"a",true]])", schema);
    EXPECT_TRUE(NRegTest::CompareResults(base, FloatResult(R"([[1.0000000000000002,"a",true]])", schema)).Matches);
    EXPECT_FALSE(NRegTest::CompareResults(base, FloatResult(R"([[1,"b",true]])", schema)).Matches);
    EXPECT_FALSE(NRegTest::CompareResults(base, FloatResult(R"([[1,"a",false]])", schema)).Matches);
}

TEST(RegResultComparison, UnorderedRowsCompareWithToleranceAndPreserveMultiplicity) {
    const auto actual = FloatResult("[[1],[2],[1]]");
    const auto expected = FloatResult("[[2.0000000000000004],[1],[1.0000000000000002]]");
    EXPECT_FALSE(NRegTest::CompareResults(actual, expected).Matches);
    EXPECT_TRUE(NRegTest::CompareResults(actual, expected, false).Matches);
    EXPECT_FALSE(NRegTest::CompareResults(actual, FloatResult("[[1],[2],[2]]"), false).Matches);
    EXPECT_FALSE(NRegTest::CompareResults(actual, FloatResult("[[1],[2]]"), false).Matches);
}

TEST(RegResultComparison, UnorderedMatchingReassignsAmbiguousPairs) {
    const auto schema = "<struct (a f64) (b f64)>";
    const auto actual = FloatResult("[[0,0],[0,1]]", schema);
    const auto expected = FloatResult("[[0,0.5],[1,-0.5]]", schema);
    // The first actual row matches both expected rows; the second only the
    // first. A sorted or greedy comparison would report a false difference.
    EXPECT_TRUE(NRegTest::CompareResults(actual, expected, false, {1, 0}).Matches);
    EXPECT_FALSE(NRegTest::CompareResults(actual, expected, true, {1, 0}).Matches);
    EXPECT_FALSE(NRegTest::CompareResults(actual,
        FloatResult("[[1,-0.5],[1,-0.5]]", schema), false, {1, 0}).Matches);
}

TEST(RegResultComparison, RejectsMalformedResultsAndInvalidTolerances) {
    const auto valid = FloatResult("[[1]]");
    for (const auto invalid : {"null", R"({"schema":"<struct (v f64)>","rows":[[]]})",
        R"({"schema":"<struct (v f64)>","rows":[["1"]]})",
        R"({"schema":"<struct (v f64)>","rows":[[{"$float":"unknown"}]]})"})
    {
        EXPECT_THROW(NRegTest::CompareResults(valid, invalid), std::runtime_error);
    }
    EXPECT_THROW(NRegTest::CompareResults(valid, valid, true, (NRegTest::TFloatTolerance{-1, 0})),
        std::runtime_error);
    EXPECT_THROW(NRegTest::CompareResults(valid, valid, true,
        (NRegTest::TFloatTolerance{0, std::numeric_limits<double>::infinity()})), std::runtime_error);
}

TEST(RegSuiteInfrastructure, ToleranceDefaultsAndPartialCaseOverrides) {
    TCaseDirectory dir;
    const auto defaults = dir.Collect();
    ASSERT_EQ(defaults.size(), 1);
    EXPECT_DOUBLE_EQ(defaults[0].Tol.Absolute, 1e-12);
    EXPECT_DOUBLE_EQ(defaults[0].Tol.Relative, 1e-12);
    dir.Write("other.sql", "SELECT v AS v FROM t;");
    dir.Write("suite.json", R"({
        "version":1,"tol":{"abs":1e-4,"rel":1e-6},
        "fixtures":{"data":{"json":"table.json"}},"tables":{"t":"data"},
        "variants":["single","threaded"],
        "cases":{"query.sql":{"tol":{"abs":0}},"other.sql":{}}
    })");
    const auto tests = dir.Collect();
    ASSERT_EQ(tests.size(), 4);
    for (const auto& test : tests) {
        EXPECT_DOUBLE_EQ(test.Tol.Absolute, test.Base.filename() == "query" ? 0 : 1e-4);
        EXPECT_DOUBLE_EQ(test.Tol.Relative, 1e-6);
    }
}

TEST(RegSuiteInfrastructure, RejectsInvalidToleranceOptions) {
    TCaseDirectory dir;
    for (const std::string value : {"0", "null", R"({"abs":-1})", R"({"rel":-1})",
        R"({"abs":"1e-6"})", R"({"rel":true})", R"({"epsilon":0.1})"})
    {
        auto manifest = std::string(TCaseDirectory::Manifest);
        manifest.insert(1, "\"tol\":" + value + ',');
        dir.Write("suite.json", manifest);
        EXPECT_THROW(dir.Collect(), std::runtime_error);
        manifest = TCaseDirectory::Manifest;
        manifest.insert(manifest.find("\"order\":\"ordered\""), "\"tol\":" + value + ',');
        dir.Write("suite.json", manifest);
        EXPECT_THROW(dir.Collect(), std::runtime_error);
    }
}

TEST(RegSuiteInfrastructure, GoldenCheckingUsesCaseTolerance) {
    TCaseDirectory dir;
    dir.Write("table.json", R"({"version":1,"schema":"<struct (v f64)>","rows":[[1],[2]]})");
    dir.WriteGolden(FloatResult("[[2.0000000000000004],[1.0000000000000002]]"));
    EXPECT_TRUE(dir.Run().Matches);
    auto manifest = std::string(TCaseDirectory::Manifest);
    manifest.insert(manifest.find("\"order\":\"ordered\""), R"("tol":{"abs":0,"rel":0},)");
    dir.Write("suite.json", manifest);
    EXPECT_FALSE(dir.Run().Matches);
}

TEST(RegSuiteInfrastructure, CanonizedVariantsUseToleranceWithoutRoundingResults) {
    TCaseDirectory dir;
    dir.Write("table.json", R"({"version":1,"schema":"<struct (v f64)>","rows":[[1],[2]]})");
    const auto tests = dir.Collect();
    NRegTest::PrepareCases(tests);
    const auto first = NRegTest::CheckCase(tests[0], true);
    ASSERT_TRUE(first.Result);
    EXPECT_TRUE(first.Result->Matches);
    dir.Write("query.sql", "SELECT v + 0.0000000000005 AS v FROM t ORDER BY v DESC;");
    const auto nearby = NRegTest::CheckCase(tests[0], true);
    ASSERT_TRUE(nearby.Result);
    EXPECT_TRUE(nearby.Result->Matches);
    EXPECT_NE(nearby.Result->Actual, nearby.Result->Expected);
    dir.Write("query.sql", "SELECT v + 0.01 AS v FROM t ORDER BY v DESC;");
    const auto different = NRegTest::CheckCase(tests[0], true);
    ASSERT_TRUE(different.Result);
    EXPECT_FALSE(different.Result->Matches);
    EXPECT_EQ(nearby.Result->Expected, first.Result->Actual);
}

TEST(RegSuiteInfrastructure, NoManifestRunsPlanOnly) {
    TCaseDirectory dir;
    fs::rename(dir.SuiteDir / "suite.json", dir.SuiteDir / "disabled.json");
    dir.Write("table.json", "not JSON");
    auto tests = dir.Collect();
    ASSERT_EQ(tests.size(), 1);
    EXPECT_FALSE(tests[0].HasTables());
    const auto result = NRegTest::CheckCase(tests[0], true);
    EXPECT_TRUE(result.Plan.Matches);
    EXPECT_FALSE(result.Result.has_value());
    EXPECT_TRUE(NRegTest::CheckCase(tests[0], false).Plan.Matches);
}

TEST(RegSuiteInfrastructure, NoManifestRejectsUnknownFixtureTable) {
    TCaseDirectory dir;
    fs::rename(dir.SuiteDir / "suite.json", dir.SuiteDir / "disabled.json");
    dir.Write("query.sql", "SELECT v FROM missing;");
    const auto tests = dir.Collect();
    ASSERT_EQ(tests.size(), 1);
    EXPECT_THROW(NRegTest::CheckCase(tests[0], false), std::runtime_error);
}

TEST(RegSuiteInfrastructure, PlanOnlyKeepsRealSchemasAndDoesNotReadOrCanonizeResults) {
    TCaseDirectory dir;
    dir.Write("suite.json", R"({
        "version":1,"execute":false,
        "fixtures":{"data":{"json":"table.json"}},
        "tables":{"t":"data"},"variants":["single","threaded"],
        "cases":{"query.sql":{}}
    })");
    fs::remove(dir.Golden);
    const auto tests = dir.Collect();
    ASSERT_EQ(tests.size(), 1);
    EXPECT_TRUE(tests[0].HasTables());
    EXPECT_FALSE(tests[0].Execute);
    EXPECT_EQ(tests[0].Variant, "plan");
    NRegTest::PrepareCases(tests);
    EXPECT_EQ(NRegTest::TableStats(tests[0], "t")->RowCount, 2);
    const auto result = NRegTest::CheckCase(tests[0], true);
    EXPECT_TRUE(result.Plan.Matches);
    EXPECT_NE(result.Plan.Actual.find("source t"), std::string::npos);
    EXPECT_NE(result.Plan.Actual.find("rows≈2"), std::string::npos);
    EXPECT_FALSE(result.Result);
    NRegTest::CommitGoldens(tests);
    EXPECT_TRUE(fs::is_regular_file(tests[0].PlanGolden));
    EXPECT_FALSE(fs::exists(dir.Golden));
    const auto checked = NRegTest::CheckCase(tests[0], false);
    EXPECT_TRUE(checked.Plan.Matches);
    EXPECT_FALSE(checked.Result);
}

TEST(RegSuiteInfrastructure, CaseExecutionOverridesSuiteDefault) {
    TCaseDirectory dir;
    dir.Write("run.sql", "SELECT v AS v FROM t ORDER BY v DESC;");
    dir.Write("suite.json", R"({
        "version":1,"execute":false,
        "fixtures":{"data":{"json":"table.json"}},
        "tables":{"t":"data"},"variants":["single","threaded"],
        "cases":{"query.sql":{},"run.sql":{"execute":true}}
    })");
    const auto tests = dir.Collect();
    ASSERT_EQ(tests.size(), 3);
    EXPECT_FALSE(tests[0].Execute);
    EXPECT_EQ(tests[0].Variant, "plan");
    EXPECT_TRUE(tests[1].Execute);
    EXPECT_EQ(tests[1].Variant, "single");
    EXPECT_TRUE(tests[2].Execute);
    EXPECT_EQ(tests[2].Variant, "threaded");

    fs::remove(dir.SuiteDir / "run.sql");
    dir.Write("suite.json", R"({
        "version":1,
        "fixtures":{"data":{"json":"table.json"}},
        "tables":{"t":"data"},"variants":["single","threaded"],
        "cases":{"query.sql":{"execute":false}}
    })");
    const auto overridden = dir.Collect();
    ASSERT_EQ(overridden.size(), 1);
    EXPECT_FALSE(overridden[0].Execute);
    EXPECT_EQ(overridden[0].Variant, "plan");
}

TEST(RegSuiteInfrastructure, RejectsInvalidExecutionOptions) {
    TCaseDirectory dir;
    for (const std::string value : {"0", "\"false\"", "null"}) {
        auto manifest = std::string(TCaseDirectory::Manifest);
        manifest.insert(1, "\"execute\":" + value + ',');
        dir.Write("suite.json", manifest);
        EXPECT_THROW(dir.Collect(), std::runtime_error);
        manifest = TCaseDirectory::Manifest;
        manifest.insert(manifest.find("\"order\":\"ordered\""), "\"execute\":" + value + ',');
        dir.Write("suite.json", manifest);
        EXPECT_THROW(dir.Collect(), std::runtime_error);
    }
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
        "schema":"<struct (v i64)>",
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
        "schema":"<struct (v u64)>",
        "rows":[["18446744073709551615"],["9007199254740993"]]
    })";
    auto fixture = std::string(data);
    fixture.insert(1, "\"version\":1,");
    dir.Write("table.json", fixture);
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
        "schema":"<struct   (v   i64) >",
        "rows":[["2"],["1"]]
    })");
    EXPECT_TRUE(dir.Run().Matches);
    dir.WriteGolden(R"({
        "schema":"<struct (v <named Nullable [i64]>)>",
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
    EXPECT_EQ(NRegTest::CollectCases(dir.CasesDir / ".", dir.GoldensDir / ".").size(), 1);
}

TEST(RegSuiteInfrastructure, SharesPreparedFixturesAcrossQueriesAliasesAndVariants) {
    TCaseDirectory dir;
    dir.Write("suite.json", R"({
        "version":1,
        "fixtures":{"data":{"json":"table.json"},"alias":{"json":"table.json"},
                    "split":{"json":"table.json","parquet":{"row_group_rows":1}}},
        "tables":{"t":"data"},"variants":["single","threaded"],
        "cases":{"query.sql":{"order":"ordered"},
                 "alias.sql":{"order":"ordered","tables":{"t":"alias"}},
                 "split.sql":{"order":"ordered","tables":{"t":"split"}}}
    })");
    dir.Write("alias.sql", "SELECT v AS v FROM t ORDER BY v DESC;");
    dir.Write("split.sql", "SELECT v AS v FROM t ORDER BY v DESC;");
    const auto tests = dir.Collect();
    ASSERT_EQ(tests.size(), 6);
    NRegTest::PrepareCase(tests[0]);
    const auto shared = NRegTest::TableFile(tests[0], "t");
    // Further cases must use the parsed table and generated file even after
    // the source disappears; a different row-group layout needs its own file.
    fs::remove(dir.SuiteDir / "table.json");
    NRegTest::PrepareCases(tests);
    NRegTest::PrepareCases(tests);
    const auto expected = NRegTest::CheckCase(tests[0], true).Result->Actual;
    for (const auto& test : tests) {
        const auto file = NRegTest::TableFile(test, "t");
        EXPECT_TRUE(fs::is_regular_file(file));
        if (test.Base.filename() == "split") EXPECT_NE(file, shared);
        else EXPECT_EQ(file, shared);
        const auto result = NRegTest::CheckCase(test, true);
        ASSERT_TRUE(result.Result);
        EXPECT_TRUE(result.Result->Matches);
        EXPECT_EQ(result.Result->Actual, expected);
    }
}

TEST(RegSuiteInfrastructure, ResultSchemaAllowsDuplicateColumnNames) {
    TCaseDirectory dir;
    dir.Write("query.sql", "SELECT v, v FROM t ORDER BY v DESC;");
    dir.WriteGolden(R"({
        "schema":"<struct (v i64) (v i64)>",
        "rows":[["2","2"],["1","1"]]
    })");
    EXPECT_TRUE(dir.Run().Matches);
}

TEST(RegSuiteInfrastructure, GoldenPathMirrorsCasesOutsideCases) {
    TCaseDirectory dir;
    fs::copy(dir.SuiteDir, dir.CasesDir / "other", fs::copy_options::recursive);
    const auto tests = dir.Collect();
    ASSERT_EQ(tests.size(), 2);
    EXPECT_EQ(tests[0].Golden, dir.Golden);
    EXPECT_EQ(tests[0].PlanGolden, dir.GoldensDir / "group/query.plan");
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
    NRegTest::PrepareCase(tests.at(0));
    EXPECT_TRUE(NRegTest::CheckCase(tests.at(0), true).Result->Matches);
    EXPECT_EQ(dir.ReadGolden(), before);
    NRegTest::CommitGoldens(tests);
    EXPECT_TRUE(NRegTest::CheckCase(tests.at(0), false).Result->Matches);
}

TEST(RegSuiteInfrastructure, CanonizeRejectsDifferingVariantResults) {
    TCaseDirectory dir;
    auto tests = dir.Collect();
    const auto before = dir.ReadGolden();
    NRegTest::PrepareCase(tests.at(0));
    EXPECT_TRUE(NRegTest::CheckCase(tests.at(0), true).Result->Matches);
    dir.Write("query.sql", "SELECT v AS v FROM t ORDER BY v ASC;");
    const auto changed = NRegTest::CheckCase(tests.at(0), true);
    EXPECT_FALSE(changed.Plan.Matches);
    EXPECT_FALSE(changed.Result->Matches);
    EXPECT_EQ(dir.ReadGolden(), before);
}

TEST(RegSuiteInfrastructure, ChecksAndCanonizesPlanAlongsideResult) {
    TCaseDirectory dir;
    const auto tests = dir.Collect();
    NRegTest::PrepareCase(tests[0]);
    auto result = NRegTest::CheckCase(tests[0], false);
    EXPECT_FALSE(result.Plan.Matches);
    ASSERT_TRUE(result.Result);
    EXPECT_TRUE(result.Result->Matches);
    result = NRegTest::CheckCase(tests[0], true);
    EXPECT_TRUE(result.Plan.Matches);
    EXPECT_FALSE(fs::exists(tests[0].PlanGolden));
    NRegTest::CommitGoldens(tests);
    EXPECT_TRUE(fs::is_regular_file(tests[0].PlanGolden));
    EXPECT_TRUE(NRegTest::CheckCase(tests[0], false).Plan.Matches);
    std::ofstream(tests[0].PlanGolden) << "incorrect plan\n";
    result = NRegTest::CheckCase(tests[0], false);
    EXPECT_FALSE(result.Plan.Matches);
    EXPECT_TRUE(result.Result->Matches);
}

TEST(RegSuiteInfrastructure, GeneratedParquetCarriesCostMetadata) {
    TCaseDirectory dir;
    dir.Write("table.json", R"({
        "version":1,"schema":"<struct (v i64)>","rows":[["1"],["2"]],
        "stats":{"columns":[{"name":"v","ndv":99,"ndv_exact":false,"histogram":[0,100]}]}
    })");
    const auto tests = dir.Collect();
    NRegTest::PrepareCase(tests[0]);
    const auto stats = NRegTest::TableStats(tests[0], "t");
    ASSERT_TRUE(stats);
    EXPECT_EQ(stats->RowCount, 2);
    const auto column = stats->ColumnStats.at("v");
    EXPECT_EQ(column->NullCount, 0);
    EXPECT_EQ(column->GetMinValue<int64_t>(), 1);
    EXPECT_EQ(column->GetMaxValue<int64_t>(), 2);
    EXPECT_EQ(column->Ndv, 99);
    EXPECT_FALSE(column->NdvIsExact);
    EXPECT_EQ(column->Histogram, (std::vector<uint64_t>{0, 100}));
    fs::copy_file(NRegTest::TableFile(tests[0], "t"), dir.SuiteDir / "embedded.parquet");
    dir.Write("suite.json", R"({
        "version":1,"fixtures":{"data":{"parquet_file":"embedded.parquet"}},
        "tables":{"t":"data"},"variants":["single"],
        "cases":{"query.sql":{"order":"ordered"}}
    })");
    auto embedded = dir.Collect();
    NRegTest::PrepareCase(embedded[0]);
    EXPECT_EQ(NRegTest::TableStats(embedded[0], "t")->ColumnStats.at("v")->Ndv, 99);
    dir.Write("suite.json", TCaseDirectory::Manifest);
    dir.Write("table.json", R"({
        "version":1,"schema":"<struct (v i64)>","rows":[["1"]],
        "stats":{"columns":[{"name":"missing","ndv":1}]}
    })");
    EXPECT_THROW(dir.Run(), std::runtime_error);
}

TEST(RegSuiteInfrastructure, ComputesStatsOrUsesFooterOnly) {
    TCaseDirectory dir;
    auto tests = dir.Collect();
    NRegTest::PrepareCase(tests[0]);
    auto column = NRegTest::TableStats(tests[0], "t")->ColumnStats.at("v");
    EXPECT_EQ(column->Ndv, 2);
    EXPECT_TRUE(column->NdvIsExact);
    EXPECT_EQ(column->Histogram, (std::vector<uint64_t>{1, 2}));
    dir.Write("table.json", R"({
        "version":1,"schema":"<struct (v i64)>","rows":[["1"],["2"]],"stats":"footer"
    })");
    tests = dir.Collect();
    NRegTest::PrepareCase(tests[0]);
    column = NRegTest::TableStats(tests[0], "t")->ColumnStats.at("v");
    EXPECT_FALSE(column->Ndv.has_value());
    EXPECT_TRUE(column->Histogram.empty());
    EXPECT_EQ(column->GetMinValue<int64_t>(), 1);
    EXPECT_EQ(column->GetMaxValue<int64_t>(), 2);
}

TEST(RegSuiteInfrastructure, SuppliedStatsControlCboJoinOrder) {
    TCaseDirectory dir;
    dir.Write("suite.json", R"({
        "version":1,
        "fixtures":{"a":{"json":"a.json"},"b":{"json":"b.json"},"c":{"json":"c.json"}},
        "tables":{"a":"a","b":"b","c":"c"},"variants":["single"],
        "cases":{"query.sql":{"order":"ordered"}}
    })");
    dir.Write("query.sql", "SELECT COUNT(*) AS n FROM a JOIN b ON a.v=b.v JOIN c ON b.v=c.v;");
    dir.WriteGolden(R"({"schema":"<struct (n i64)>","rows":[["100"]]})");
    auto fixture = [&](const char* name, int ndv) {
        std::ostringstream data;
        data << R"({"version":1,"schema":"<struct (v i64)>","rows":[)";
        for (int v = 1; v <= 100; ++v) {
            if (v > 1) data << ',';
            data << "[\"" << v << "\"]";
        }
        data << R"(],"stats":{"columns":[{"name":"v","ndv":)" << ndv << ",\"ndv_exact\":false}]}}";
        dir.Write(std::string(name) + ".json", data.str());
    };
    auto run = [&]() {
        const auto tests = dir.Collect();
        NRegTest::PrepareCase(tests[0]);
        return NRegTest::CheckCase(tests[0], false);
    };
    fixture("a", 100); fixture("b", 1); fixture("c", 1);
    const auto first = run();
    fixture("a", 1); fixture("b", 1); fixture("c", 100);
    const auto second = run();
    ASSERT_TRUE(first.Result && second.Result);
    EXPECT_TRUE(first.Result->Matches) << first.Result->Actual;
    EXPECT_TRUE(second.Result->Matches) << second.Result->Actual;
    auto lastJoin = [](const std::string& plan) {
        std::istringstream lines(plan);
        std::string line, join;
        while (std::getline(lines, line)) {
            if (line.find("join inner") != std::string::npos) join = line.substr(0, line.find(" (rows"));
        }
        return join;
    };
    EXPECT_NE(lastJoin(first.Plan.Actual), lastJoin(second.Plan.Actual))
        << first.Plan.Actual << "\n" << second.Plan.Actual;
}

} // namespace
