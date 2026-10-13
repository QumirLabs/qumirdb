#include <gtest/gtest.h>
#include "mock_source.h"
#include "reg_suite.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <qdb/io/schema.h>
#include <qdb/plan/build.h>
#include <qdb/plan/ops/source.h>
#include <qdb/sexp/printer.h>
#include <qdb/sql/parser.h>
#include <qdb/sql/printer.h>

#include <qumir/parser/core/printer.h>
#include <qumir/codegen/llvm/llvm_initializer.h>

namespace fs = std::filesystem;

using namespace NQdb;

namespace {

fs::path RootDir = "regtest";
fs::path CasesDir = RootDir / "cases";
fs::path GoldensDir = RootDir / "goldens";

bool updateGoldens = false;
bool printOutput = false;

std::string ReadAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), {});
}

void WriteAll(const fs::path& p, const std::string& s) {
    std::cerr << "Updating golden file: " << p << "\n";
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out << s;
}

struct ProgCase { fs::path base; }; // base without extension

std::vector<ProgCase> Collect(const fs::path& root, std::string_view extension = ".sql") {
    std::vector<ProgCase> v;
    if (!fs::exists(root)) {
        return v;
    }
    auto casePath = [&](const fs::path& p) {
        auto rel = fs::relative(p, root);
        return rel.replace_extension();
    };
    for (auto& e : fs::recursive_directory_iterator(root)) {
        if (e.is_regular_file() && e.path().extension() == extension) {
            if (fs::exists(e.path().parent_path() / "suite.json")) {
                continue;
            }
            v.push_back({ casePath(e.path()) });
        }
    }
    std::sort(v.begin(), v.end(),
        [](auto& a, auto& b){ return a.base.string() < b.base.string(); });
    return v;
}

std::string NameFromPath(const fs::path& p) {
    std::string s = p.string();
    for (auto& c : s) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') {
            c = '_';
        }
    }
    return s;
}

std::string BuildAst(std::istream& in) {
    NSql::TTokenStream ts(in);
    NSql::TParser parser;

    auto parsed = parser.Parse(ts);
    if (!parsed) {
        return parsed.error().ToString() + "\n";
    }
    return NSql::PrintAst(parsed.value());
}

std::string BuildPlan(std::istream& in) {
    NSql::TTokenStream ts(in);
    NSql::TParser parser;

    auto parsed = parser.Parse(ts);
    if (!parsed) {
        return parsed.error().ToString() + "\n";
    }

    std::vector<std::unique_ptr<NQdb::TMockSource>> sources;
    auto factory = [&](std::string_view table)
        -> std::expected<NQdb::TOperatorPtr, NQumir::TError>
    {
        auto& source = sources.emplace_back(std::make_unique<NQdb::TMockSource>());
        return std::make_shared<NQdb::TSourceOperator>(*source, std::string(table));
    };

    auto plan = NQdb::BuildPlan(parsed.value(), factory);
    if (!plan) {
        return plan.error().ToString() + "\n";
    }

    std::ostringstream out;
    NQdb::NSexp::PrintRelPlan(out, plan.value());
    out << "\n";
    return out.str();
}

void CheckGolden(const fs::path& base, std::string_view extension, const std::string& got) {
    const fs::path src = fs::path(CasesDir / base).replace_extension(".sql");
    const fs::path golden = fs::path(GoldensDir / base).replace_extension(extension);

    if (printOutput) {
        std::cout << "=== " << src << " (" << extension << ") ===\n" << got << "\n";
    }
    if (updateGoldens) {
        WriteAll(golden, got);
    }
    if (!fs::exists(golden)) {
        std::cerr << "Missing golden file: " << golden << "\n";
        FAIL();
    }
    EXPECT_EQ(got, ReadAll(golden));
}

} // namespace

class RegAst : public ::testing::TestWithParam<ProgCase> {};
class RegPlan : public ::testing::TestWithParam<ProgCase> {};

class RegExec : public ::testing::Test {
public:
    explicit RegExec(NRegTest::TExecCase test) : Case(std::move(test)) {}

private:
    void TestBody() override {
        const auto result = NRegTest::ExecuteCase(Case, updateGoldens);
        if (printOutput) {
            std::cout << "=== " << Case.Sql << " (" << Case.Variant << ") ===\n"
                << result.Actual;
        }
        EXPECT_TRUE(result.Matches) << "SQL: " << Case.Sql << "\nGolden: " << Case.Golden
            << "\nExpected:\n" << result.Expected << "Actual:\n" << result.Actual;
    }

    NRegTest::TExecCase Case;
};

class RegExecEnvironment : public ::testing::Environment {
public:
    std::vector<std::pair<NRegTest::TExecCase, const ::testing::TestInfo*>> Cases;

    void SetUp() override {
        for (const auto& [test, info] : Cases) {
            if (info->should_run()) NRegTest::PrepareExecCase(test);
        }
    }
};

TEST_P(RegAst, Ast) {
    std::istringstream input(ReadAll(fs::path(CasesDir / GetParam().base).replace_extension(".sql")));
    CheckGolden(GetParam().base, ".ast", BuildAst(input));
}

TEST_P(RegPlan, Plan) {
    std::istringstream input(ReadAll(fs::path(CasesDir / GetParam().base).replace_extension(".sql")));
    CheckGolden(GetParam().base, ".plan", BuildPlan(input));
}

INSTANTIATE_TEST_SUITE_P(
    RegProgAst,
    RegAst,
    ::testing::ValuesIn(Collect(CasesDir)),
    [](const ::testing::TestParamInfo<ProgCase>& i){ return "AST_" + NameFromPath(i.param.base); });

INSTANTIATE_TEST_SUITE_P(
    RegProgPlan,
    RegPlan,
    ::testing::ValuesIn(Collect(CasesDir)),
    [](const ::testing::TestParamInfo<ProgCase>& i){ return "PLAN_" + NameFromPath(i.param.base); });

int main(int argc, char** argv) {
    if (argc > 1 && argv[1][0] != '-') {
        RootDir = argv[1];
        CasesDir = RootDir / "cases";
        GoldensDir = RootDir / "goldens";
    }
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--canonize")) {
            updateGoldens = true;
        } else if (!strcmp(argv[i], "--print")) {
            printOutput = true;
        }
    }
    try {
        const auto execCases = NRegTest::CollectExecCases(CasesDir, GoldensDir);
        auto environment = std::make_unique<RegExecEnvironment>();
        std::set<std::string> names;
        for (const auto& test : execCases) {
            const auto name = NameFromPath(test.Base) + "_" + test.Variant;
            if (!names.insert(name).second) {
                throw std::runtime_error("duplicate execution test name: " + name);
            }
            const auto* info = ::testing::RegisterTest("RegExec", name.c_str(),
                nullptr, nullptr, __FILE__, __LINE__, [test]() { return new RegExec(test); });
            environment->Cases.emplace_back(test, info);
        }
        ::testing::AddGlobalTestEnvironment(environment.release());
        ::testing::InitGoogleTest(&argc, argv);
        NQumir::NCodeGen::TLLVMInitializer llvmInit;
        const auto result = RUN_ALL_TESTS();
        if (result == 0 && updateGoldens) NRegTest::CommitExecGoldens(execCases);
        return result;
    } catch (const std::exception& error) {
        std::cerr << "Regression test setup failed: " << error.what() << '\n';
        return 1;
    }
}
