#include "mock_source.h"
#include "plan_runner.h"

#include <qdb/plan/build.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/passes/column_pruning.h>
#include <qdb/plan/passes/equijoin.h>
#include <qdb/plan/passes/qualify_columns.h>
#include <qdb/plan/passes/typing.h>
#include <qdb/plan/pipeline.h>
#include <qdb/plan/types/nullable.h>
#include <qdb/sql/parser.h>

#include <qumir/codegen/llvm/llvm_initializer.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <expected>
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

struct TTable {
    std::vector<int64_t> Keys;
    std::vector<int64_t> Values;
    std::vector<uint8_t> KeyMask;
    std::vector<uint8_t> ValueMask;
    std::vector<TColumn> Columns;
    TMockSource Source;

    TTable(
        std::vector<int64_t> keys,
        std::vector<int64_t> values,
        std::vector<uint8_t> keyMask = {},
        std::vector<uint8_t> valueMask = {})
        : Keys(std::move(keys))
        , Values(std::move(values))
        , KeyMask(std::move(keyMask))
        , ValueMask(std::move(valueMask))
    {
        Columns = {
            TColumn{.Data = reinterpret_cast<char*>(Keys.data()),
                .Mask = KeyMask.empty() ? nullptr : KeyMask.data()},
            TColumn{.Data = reinterpret_cast<char*>(Values.data()),
                .Mask = ValueMask.empty() ? nullptr : ValueMask.data()},
        };
        TTypePtr keyType = std::make_shared<TIntegerType>(TIntegerType::I64);
        TTypePtr valueType = std::make_shared<TIntegerType>(TIntegerType::I64);
        if (!KeyMask.empty()) {
            keyType = std::make_shared<TNullable>(keyType);
        }
        if (!ValueMask.empty()) {
            valueType = std::make_shared<TNullable>(valueType);
        }
        std::vector<TRowSet> batches;
        if (!Keys.empty()) {
            batches.push_back(TRowSet{
                .Columns = Columns.data(), .ColumnCount = 2,
                .RowCount = static_cast<int64_t>(Keys.size()), .RefCount = 1});
        }
        Source = TMockSource({"key", "value"}, std::move(batches),
            std::vector<TTypePtr>{keyType, valueType});
    }
};

TOperatorPtr PlanSql(
    std::string_view sql,
    TTable& sales,
    TTable& returns,
    bool optimize = true)
{
    std::istringstream input{std::string(sql)};
    NSql::TTokenStream tokens(input);
    NSql::TParser parser;
    auto parsed = parser.Parse(tokens);
    if (!parsed) {
        throw std::runtime_error(parsed.error().ToString());
    }
    const std::map<std::string, ISource*> tables = {
        {"sales", &sales.Source}, {"returns", &returns.Source},
    };
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
        AssignSourceAliases(root);
        QualifyColumns(root);
        AnnotateTypes(root);
        root = ExtractEquiJoins(root);
        AnnotateTypes(root);
        ApplyColumnPruning(root);
    }
    return root;
}

std::vector<std::shared_ptr<TJoinOperator>> Joins(const TOperatorPtr& root) {
    std::vector<std::shared_ptr<TJoinOperator>> result;
    if (auto join = TMaybeOp<TJoinOperator>(root)) {
        result.push_back(join.Cast());
    }
    for (const auto& child : root->Children()) {
        if (auto op = TMaybeNode<IOperator>(child)) {
            auto joins = Joins(op.Cast());
            result.insert(result.end(), joins.begin(), joins.end());
        }
    }
    return result;
}

std::vector<int64_t> RunValues(
    const TOperatorPtr& plan,
    NScheduler::TSettings settings = {})
{
    auto runtime = RunPlan(plan, settings);
    std::vector<int64_t> result;
    TRowSet output{};
    while (runtime->Next(output)) {
        EXPECT_EQ(output.ColumnCount, 1);
        for (int64_t row = 0; row < output.RowCount; ++row) {
            if (!output.Selection || output.Selection[row]) {
                result.push_back(reinterpret_cast<const int64_t*>(output.Columns[0].Data)[row]);
            }
        }
        Release(&output);
    }
    std::ranges::sort(result);
    return result;
}

class OuterJoinToAntiExecution
    : public ::testing::TestWithParam<NScheduler::EExecutionMode>
{
protected:
    NScheduler::TSettings Settings() const {
        NScheduler::TSettings settings;
        settings.Scheduler.Mode = GetParam();
        settings.Scheduler.WorkerCount = 2;
        settings.HashShuffle.PartitionCount = 2;
        return settings;
    }
};

TEST(OuterJoinKeys, CompositeKeysHaveStableOrder) {
    TTable sales({1}, {10});
    TTable returns({1}, {10});
    for (const auto* type : {"LEFT", "RIGHT", "FULL"}) {
        for (const auto* on : {"s.key = r.key AND s.value = r.value",
                              "s.value = r.value AND s.key = r.key",
                              "r.value = s.value AND r.key = s.key"}) {
            const auto sql = std::string("SELECT s.value FROM sales s ") + type
                + " JOIN returns r ON " + on;
            SCOPED_TRACE(sql);
            const auto joins = Joins(PlanSql(sql, sales, returns, false));
            ASSERT_EQ(joins.size(), 1);
            const auto& keys = joins[0]->Keys();
            ASSERT_EQ(keys.size(), 2);
            EXPECT_EQ(keys[0].Left, "s.key");
            EXPECT_EQ(keys[0].Right, "r.key");
            EXPECT_EQ(keys[1].Left, "s.value");
            EXPECT_EQ(keys[1].Right, "r.value");
        }
    }
}

TEST_P(OuterJoinToAntiExecution, PreservesMixedNullEqualityThroughOptimizer) {
    TTable sales({111, 222}, {10, 0}, {0}, {0b00000001});
    TTable returns({333, 444, 555}, {10, 0, 0}, {0}, {0b00000001});
    auto plan = PlanSql(R"sql(
SELECT s.value FROM sales s INNER JOIN returns r
ON s.key = r.key AND s.value = r.value
)sql", sales, returns, false);
    for (auto& key : Joins(plan).front()->MutableKeys()) {
        key.NullsEqual = key.Left == "s.key";
    }
    ApplyPlanPasses(plan);
    auto join = Joins(plan).front();
    ASSERT_EQ(join->Keys().size(), 2);
    EXPECT_EQ(std::ranges::count_if(join->Keys(), [](const TJoinKey& key) {
        return key.NullsEqual;
    }), 1);
    EXPECT_EQ(RunValues(plan, Settings()), (std::vector<int64_t>{10}));
}

TEST_P(OuterJoinToAntiExecution, DoesNotRewriteNullEqualMarker) {
    TTable sales({1, 111}, {10, 20}, {0b00000001});
    TTable returns({1, 222, 333}, {100, 200, 201}, {0b00000001});
    auto plan = PlanSql(R"sql(
SELECT s.value FROM sales s LEFT JOIN returns r ON s.key = r.key
WHERE r.key IS NULL
)sql", sales, returns, false);
    Joins(plan).front()->MutableKeys().front().NullsEqual = true;
    EXPECT_FALSE(RewriteOuterJoinAsAnti(plan));
    ApplyPlanPasses(plan);
    EXPECT_EQ(Joins(plan).front()->JoinType(), EJoinType::Left);
    EXPECT_EQ(RunValues(plan, Settings()), (std::vector<int64_t>{20, 20}));
}

INSTANTIATE_TEST_SUITE_P(Schedulers, OuterJoinToAntiExecution,
    ::testing::Values(NScheduler::EExecutionMode::SingleThreadedScheduler,
        NScheduler::EExecutionMode::ThreadedScheduler));

} // namespace

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    NQumir::NCodeGen::TLLVMInitializer initializer;
    return RUN_ALL_TESTS();
}
