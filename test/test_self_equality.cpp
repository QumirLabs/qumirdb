#include "mock_source.h"
#include "plan_runner.h"

#include <qdb/plan/build.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/passes/column_pruning.h>
#include <qdb/plan/passes/equijoin.h>
#include <qdb/plan/passes/qualify_columns.h>
#include <qdb/plan/passes/sql_binding.h>
#include <qdb/plan/passes/typing.h>
#include <qdb/plan/pipeline.h>
#include <qdb/plan/types/nullable.h>
#include <qdb/sql/parser.h>

#include <qumir/codegen/llvm/llvm_initializer.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <expected>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace NQdb;
using namespace NQumir::NAst;

namespace {

// Table (key, value). A non-empty key mask makes the key nullable; values are
// i64 unless another 8-byte type is given.
struct TTable {
    std::vector<int64_t> Keys;
    std::vector<int64_t> Values;
    std::vector<uint8_t> KeyMask;
    std::vector<TColumn> Columns;
    TMockSource Source;

    TTable(
        std::vector<int64_t> keys,
        std::vector<int64_t> values,
        std::vector<uint8_t> keyMask = {},
        TTypePtr valueType = std::make_shared<TIntegerType>(TIntegerType::I64))
        : Keys(std::move(keys))
        , Values(std::move(values))
        , KeyMask(std::move(keyMask))
    {
        Columns = {
            TColumn{.Data = reinterpret_cast<char*>(Keys.data()),
                .Mask = KeyMask.empty() ? nullptr : KeyMask.data()},
            TColumn{.Data = reinterpret_cast<char*>(Values.data())},
        };
        TTypePtr keyType = std::make_shared<TIntegerType>(TIntegerType::I64);
        if (!KeyMask.empty()) {
            keyType = std::make_shared<TNullable>(keyType);
        }
        std::vector<TRowSet> batches;
        if (!Keys.empty()) {
            batches.push_back(TRowSet{
                .Columns = Columns.data(), .ColumnCount = 2,
                .RowCount = static_cast<int64_t>(Keys.size()), .RefCount = 1});
        }
        Source = TMockSource({"key", "value"}, std::move(batches),
            std::vector<TTypePtr>{keyType, std::move(valueType)});
    }
};

bool IsSelfEquality(const TExprPtr& expr) {
    auto binary = TMaybeNode<TBinaryExpr>(expr);
    if (!binary || binary.Cast()->Operator != "==") {
        return false;
    }
    auto left = TMaybeNode<TIdentExpr>(binary.Cast()->Left);
    auto right = TMaybeNode<TIdentExpr>(binary.Cast()->Right);
    return left && right && left.Cast()->Name == right.Cast()->Name;
}

bool HasSelfEquality(const TExprPtr& expr) {
    return expr && (IsSelfEquality(expr)
        || std::ranges::any_of(expr->Children(), HasSelfEquality));
}

class SelfEquality : public ::testing::Test {
protected:
    // Plans `sql` over tables t and u. Without `optimize` only the passes
    // execution needs are run, which leaves predicates as written.
    TOperatorPtr Plan(std::string_view sql, TTable& t, TTable& u, bool optimize = true) {
        std::istringstream input{std::string(sql)};
        NSql::TTokenStream tokens(input);
        NSql::TParser parser;
        auto parsed = parser.Parse(tokens);
        if (!parsed) {
            throw std::runtime_error(parsed.error().ToString());
        }
        t.Source.Index = 0;
        u.Source.Index = 0;
        const std::map<std::string, ISource*> tables = {{"t", &t.Source}, {"u", &u.Source}};
        auto plan = BuildPlan(*parsed, [&](std::string_view name)
            -> std::expected<TOperatorPtr, NQumir::TError>
        {
            return std::make_shared<TSourceOperator>(
                *tables.at(std::string(name)), std::string(name));
        });
        if (!plan) {
            throw std::runtime_error(plan.error().ToString());
        }
        auto root = *plan;
        if (optimize) {
            ApplyPlanPasses(root);
        } else {
            ResolveSqlBindings(root);
            AssignSourceAliases(root);
            QualifyColumns(root);
            AnnotateTypes(root);
            root = ExtractEquiJoins(root);
            AnnotateTypes(root);
            ApplyColumnPruning(root);
        }
        return root;
    }

    // Sorted values of the single i64 output column.
    std::vector<int64_t> Run(const TOperatorPtr& plan) {
        auto runtime = RunPlan(plan);
        std::vector<int64_t> result;
        TRowSet output{};
        while (runtime->Next(output)) {
            EXPECT_EQ(output.ColumnCount, 1);
            for (int64_t row = 0; row < output.RowCount; ++row) {
                if (!output.Selection || output.Selection[row]) {
                    result.push_back(
                        reinterpret_cast<const int64_t*>(output.Columns[0].Data)[row]);
                }
            }
            Release(&output);
        }
        std::ranges::sort(result);
        return result;
    }

    // Filter predicates of the plan that still compare a column with itself.
    size_t SelfEqualities(const TOperatorPtr& plan) {
        size_t count = 0;
        if (auto filter = TMaybeOp<TFilterOperator>(plan)) {
            count += HasSelfEquality(filter.Cast()->Predicate());
        }
        for (const auto& input : plan->Inputs()) {
            count += SelfEqualities(input);
        }
        return count;
    }

    // Runs `sql` optimized and as written, expects the same rows from both and
    // returns them.
    std::vector<int64_t> RunBoth(std::string_view sql, TTable& t, TTable& u) {
        auto written = Run(Plan(sql, t, u, false));
        EXPECT_EQ(Run(Plan(sql, t, u)), written) << sql;
        return written;
    }

    TTable Empty{{}, {}};
};

TEST_F(SelfEquality, NonNullableColumnKeepsEveryRow) {
    TTable t({1, 2, 2}, {10, 20, 21});
    const std::string sql = "SELECT value FROM t WHERE key = key";
    EXPECT_EQ(SelfEqualities(Plan(sql, t, Empty)), 0u);
    EXPECT_EQ(RunBoth(sql, t, Empty), (std::vector<int64_t>{10, 20, 21}));
}

TEST_F(SelfEquality, NullableColumnDropsOnlyNullRows) {
    TTable t({1, 0, 2, 0}, {10, 20, 30, 40}, {0b00000101});
    const std::string sql = "SELECT value FROM t WHERE key = key";
    EXPECT_EQ(SelfEqualities(Plan(sql, t, Empty)), 0u);
    EXPECT_EQ(RunBoth(sql, t, Empty), (std::vector<int64_t>{10, 30}));
}

TEST_F(SelfEquality, InsideOrNullRowsStillNeedTheOtherBranch) {
    TTable t({1, 0, 2, 0}, {10, 20, 30, 40}, {0b00000101});
    const std::string anyBranch = "SELECT value FROM t WHERE key = key OR value >= 40";
    EXPECT_EQ(SelfEqualities(Plan(anyBranch, t, Empty)), 0u);
    EXPECT_EQ(RunBoth(anyBranch, t, Empty), (std::vector<int64_t>{10, 30, 40}));

    const std::string everyBranch = R"sql(
SELECT value FROM t
WHERE (key = key AND value < 20) OR (key = key AND value >= 20)
)sql";
    EXPECT_EQ(SelfEqualities(Plan(everyBranch, t, Empty)), 0u);
    EXPECT_EQ(RunBoth(everyBranch, t, Empty), (std::vector<int64_t>{10, 30}));
}

TEST_F(SelfEquality, InsideOrNonNullableColumnKeepsEveryRow) {
    TTable t({1, 2, 3}, {10, 20, 30});
    const std::string sql = "SELECT value FROM t WHERE key = key OR value >= 30";
    EXPECT_EQ(SelfEqualities(Plan(sql, t, Empty)), 0u);
    EXPECT_EQ(RunBoth(sql, t, Empty), (std::vector<int64_t>{10, 20, 30}));
}

// NOT (NULL = NULL) is unknown, while NOT (x IS NOT NULL) would be true.
TEST_F(SelfEquality, NegatedComparisonStillRejectsNullRows) {
    TTable t({1, 0, 2}, {10, 20, 30}, {0b00000101});
    EXPECT_EQ(RunBoth("SELECT value FROM t WHERE NOT (key = key)", t, Empty),
        (std::vector<int64_t>{}));
}

TEST_F(SelfEquality, FloatColumnStillRejectsNaN) {
    const auto bits = [](double value) { return std::bit_cast<int64_t>(value); };
    TTable t({1, 2, 3},
        {bits(1.5), bits(std::numeric_limits<double>::quiet_NaN()), bits(2.5)},
        {}, std::make_shared<TFloatType>());
    EXPECT_EQ(RunBoth("SELECT key FROM t WHERE value = value", t, Empty),
        (std::vector<int64_t>{1, 3}));
}

// Above a LEFT join the right side is NULL on unmatched rows even when its
// source column is not nullable.
TEST_F(SelfEquality, NullExtendedColumnRejectsUnmatchedRows) {
    TTable t({1, 2}, {10, 20});
    TTable u({1}, {100});
    EXPECT_EQ(RunBoth(R"sql(
SELECT t.value FROM t LEFT JOIN u ON t.key = u.key WHERE u.key = u.key
)sql", t, u), (std::vector<int64_t>{10}));
}

// The correlation equality of a scalar subquery is renamed onto the subquery's
// own column; the join key enforces it afterwards.
TEST_F(SelfEquality, DecorrelatedSubqueryOverNonNullableKey) {
    TTable t({1, 2, 3}, {10, 20, 30});
    TTable u({1, 1, 2}, {5, 7, 25});
    const std::string sql = R"sql(
SELECT t.value FROM t
WHERE t.value > (SELECT min(u.value) FROM u WHERE u.key = t.key)
)sql";
    EXPECT_EQ(SelfEqualities(Plan(sql, t, u)), 0u);
    EXPECT_EQ(RunBoth(sql, t, u), (std::vector<int64_t>{10}));
}

TEST_F(SelfEquality, DecorrelatedSubqueryNullKeysNeverMatch) {
    // The NULL-key row of u would make 40 > 1 pass if NULL keys could match.
    TTable t({1, 2, 3, 0}, {10, 20, 30, 40}, {0b00000111});
    TTable u({1, 1, 2, 0}, {5, 7, 25, 1}, {0b00000111});
    const std::string sql = R"sql(
SELECT t.value FROM t
WHERE t.value > (SELECT min(u.value) FROM u WHERE u.key = t.key)
)sql";
    EXPECT_EQ(SelfEqualities(Plan(sql, t, u)), 0u);
    EXPECT_EQ(RunBoth(sql, t, u), (std::vector<int64_t>{10}));
}

} // namespace

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    NQumir::NCodeGen::TLLVMInitializer initializer;
    return RUN_ALL_TESTS();
}
