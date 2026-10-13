#include "regtest_lib.h"

#include <gtest/gtest.h>
#include <qumir/codegen/llvm/llvm_initializer.h>

#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

fs::path RootDir = "regtest";
fs::path CasesDir = RootDir / "cases";
fs::path GoldensDir = RootDir / "goldens";
std::vector<NRegTest::TQueryCase> AstCases;
bool updateGoldens = false;
bool printOutput = false;

} // namespace

class RegAst : public ::testing::TestWithParam<NRegTest::TQueryCase> {};

TEST_P(RegAst, Ast) {
    const auto result = NRegTest::CheckAst(GetParam(), updateGoldens);
    if (printOutput) {
        std::cout << "=== " << GetParam().Sql << " (.ast) ===\n" << result.Actual;
    }
    EXPECT_EQ(result.Expected, result.Actual);
}

INSTANTIATE_TEST_SUITE_P(
    RegProgAst,
    RegAst,
    ::testing::ValuesIn(AstCases),
    [](const ::testing::TestParamInfo<NRegTest::TQueryCase>& test) {
        return "AST_" + NRegTest::TestName(test.param.Base);
    });

class RegQuery : public ::testing::Test {
public:
    explicit RegQuery(NRegTest::TQueryCase test) : Case(std::move(test)) {}

private:
    void TestBody() override {
        const auto result = NRegTest::CheckCase(Case, updateGoldens);
        if (printOutput) {
            std::cout << "=== " << Case.Sql << " (.plan, " << Case.Variant << ") ===\n"
                << result.Plan.Actual;
            if (result.Result) {
                std::cout << "=== " << Case.Sql << " (.result.json, " << Case.Variant << ") ===\n"
                    << result.Result->Actual;
            }
        }
        EXPECT_TRUE(result.Plan.Matches) << "SQL: " << Case.Sql << "\nGolden: " << Case.PlanGolden
            << "\nExpected:\n" << result.Plan.Expected << "Actual:\n" << result.Plan.Actual;
        if (result.Result) {
            EXPECT_TRUE(result.Result->Matches) << "SQL: " << Case.Sql << "\nGolden: " << Case.Golden
                << "\nExpected:\n" << result.Result->Expected << "Actual:\n" << result.Result->Actual;
        }
    }

    NRegTest::TQueryCase Case;
};

class RegQueryEnvironment : public ::testing::Environment {
public:
    std::vector<std::pair<NRegTest::TQueryCase, const ::testing::TestInfo*>> Cases;

    void SetUp() override {
        for (const auto& [test, info] : Cases) {
            if (info->should_run()) NRegTest::PrepareCase(test);
        }
    }
};

int main(int argc, char** argv) {
    if (argc > 1 && argv[1][0] != '-') {
        RootDir = argv[1];
        CasesDir = RootDir / "cases";
        GoldensDir = RootDir / "goldens";
    }
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--canonize")) updateGoldens = true;
        else if (!strcmp(argv[i], "--print")) printOutput = true;
    }
    try {
        const auto cases = NRegTest::CollectCases(CasesDir, GoldensDir);
        auto environment = std::make_unique<RegQueryEnvironment>();
        std::set<std::string> names;
        for (const auto& test : cases) {
            if (!test.Suite) AstCases.push_back(test);
            const auto name = NRegTest::TestName(test.Base) + "_" + test.Variant;
            if (!names.insert(name).second) throw std::runtime_error("duplicate query test name: " + name);
            const auto* info = ::testing::RegisterTest("RegQuery", name.c_str(),
                nullptr, nullptr, __FILE__, __LINE__, [test]() { return new RegQuery(test); });
            environment->Cases.emplace_back(test, info);
        }
        ::testing::AddGlobalTestEnvironment(environment.release());
        ::testing::InitGoogleTest(&argc, argv);
        NQumir::NCodeGen::TLLVMInitializer llvmInit;
        const auto result = RUN_ALL_TESTS();
        if (result == 0 && updateGoldens) NRegTest::CommitGoldens(cases);
        return result;
    } catch (const std::exception& error) {
        std::cerr << "Regression test setup failed: " << error.what() << '\n';
        return 1;
    }
}
