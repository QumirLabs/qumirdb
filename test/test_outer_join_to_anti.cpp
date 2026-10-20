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
#include <qdb/plan/passes/sql_binding.h>

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

TEST_P(OuterJoinToAntiExecution, MatchesOuterJoinWithNullableKeysAndDuplicates) {
    const std::string sql = R"sql(
SELECT s.value FROM sales s
LEFT JOIN returns r ON s.key = r.key
WHERE r.key IS NULL AND s.value >= 20
)sql";
    TTable sales({1, 2, 2, 3, 0}, {10, 20, 21, 30, 40}, {0b00001111});
    TTable returns({1, 1, 0, 0}, {100, 101, 998, 999}, {0b00000011});
    auto original = PlanSql(sql, sales, returns, false);
    auto originalRows = RunValues(original, Settings());
    sales.Source.Index = 0;
    returns.Source.Index = 0;
    auto rewritten = PlanSql(sql, sales, returns);
    ASSERT_EQ(Joins(rewritten).size(), 1);
    EXPECT_EQ(Joins(rewritten).front()->JoinType(), EJoinType::LeftAnti);
    EXPECT_EQ(RunValues(rewritten, Settings()), originalRows);
    EXPECT_EQ(originalRows, (std::vector<int64_t>{20, 21, 30, 40}));
    EXPECT_FALSE(RewriteOuterJoinAsAnti(rewritten));
}

TEST_P(OuterJoinToAntiExecution, RightJoinSwapsInputsAndKeys) {
    TTable sales({1, 2, 2, 3}, {10, 20, 21, 30});
    TTable returns({1, 1}, {100, 101});
    auto plan = PlanSql(R"sql(
SELECT s.value FROM returns r
RIGHT JOIN sales s ON r.key = s.key
WHERE r.key IS NULL
)sql", sales, returns);
    auto joins = Joins(plan);
    ASSERT_EQ(joins.size(), 1);
    EXPECT_EQ(joins.front()->JoinType(), EJoinType::LeftAnti);
    ASSERT_EQ(joins.front()->Keys().size(), 1);
    EXPECT_EQ(joins.front()->Keys().front().Left, "s.key");
    EXPECT_EQ(joins.front()->Keys().front().Right, "r.key");
    EXPECT_EQ(RunValues(plan, Settings()), (std::vector<int64_t>{20, 21, 30}));
}

TEST_P(OuterJoinToAntiExecution, EmptyRightSidePreservesEveryLeftRow) {
    TTable sales({1, 2, 2}, {10, 20, 21});
    TTable returns({}, {});
    auto plan = PlanSql(R"sql(
SELECT s.value FROM sales s
LEFT JOIN returns r ON s.key = r.key WHERE r.key IS NULL
)sql", sales, returns);
    ASSERT_EQ(Joins(plan).front()->JoinType(), EJoinType::LeftAnti);
    EXPECT_EQ(RunValues(plan, Settings()), (std::vector<int64_t>{10, 20, 21}));
}

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

TEST_P(OuterJoinToAntiExecution, PreservesCompositeKeys) {
    TTable sales({1, 1, 2, 2}, {10, 11, 20, 21});
    TTable returns({1, 2}, {11, 20});
    auto plan = PlanSql(R"sql(
SELECT s.value FROM sales s
LEFT JOIN returns r ON s.key = r.key AND s.value = r.value
WHERE r.value IS NULL
)sql", sales, returns);
    auto join = Joins(plan).front();
    EXPECT_EQ(join->JoinType(), EJoinType::LeftAnti);
    EXPECT_EQ(join->Keys().size(), 2);
    EXPECT_EQ(RunValues(plan, Settings()), (std::vector<int64_t>{10, 21}));
}

TEST_P(OuterJoinToAntiExecution, NullCompositeKeyDoesNotMatch) {
    const std::string sql = R"sql(
SELECT s.key FROM sales s
LEFT JOIN returns r ON s.key = r.key AND s.value = r.value
WHERE r.value IS NULL
)sql";
    TTable sales({1, 1, 2}, {10, 0, 20}, {}, {0b00000101});
    TTable returns({1, 1, 1, 2}, {10, 0, 0, 20}, {}, {0b00001001});
    auto originalRows = RunValues(PlanSql(sql, sales, returns, false), Settings());
    sales.Source.Index = 0;
    returns.Source.Index = 0;
    auto plan = PlanSql(sql, sales, returns);
    EXPECT_EQ(Joins(plan).front()->JoinType(), EJoinType::LeftAnti);
    EXPECT_EQ(RunValues(plan, Settings()), originalRows);
    EXPECT_EQ(originalRows, (std::vector<int64_t>{1}));
}

TEST_P(OuterJoinToAntiExecution, NullKeysDoNotSatisfyOnResidual) {
    for (bool swapped : {false, true}) {
        TTable sales({1, 0}, {200, 300}, {0b00000001});
        TTable returns({1, 0}, {100, 100}, {0b00000001});
        const std::string from = swapped
            ? "returns r RIGHT JOIN sales s"
            : "sales s LEFT JOIN returns r";
        auto plan = PlanSql("SELECT s.value FROM " + from
            + " ON s.key = r.key AND s.value > r.value WHERE r.key IS NULL",
            sales, returns);
        EXPECT_EQ(Joins(plan).front()->JoinType(), EJoinType::LeftAnti);
        EXPECT_EQ(RunValues(plan, Settings()), (std::vector<int64_t>{300}));
    }
}

TEST_P(OuterJoinToAntiExecution, NullKeysNeverMatchInOrdinaryJoins) {
    for (const auto& [kind, count] :
        std::vector<std::pair<std::string, int64_t>>{
            {"INNER", 1}, {"LEFT", 2}, {"RIGHT", 3}, {"FULL", 4}})
    {
        TTable sales({1, 0}, {10, 20}, {0b00000001});
        TTable returns({1, 0, 0}, {100, 200, 201}, {0b00000001});
        // Read both payloads: the mock source does not implement column pruning.
        // COALESCE keeps the count expression non-NULL on unmatched outer rows.
        auto plan = PlanSql("SELECT count(coalesce(s.value, 0) + coalesce(r.value, 0)) FROM sales s " + kind
            + " JOIN returns r ON s.key = r.key", sales, returns);
        EXPECT_EQ(RunValues(plan, Settings()), (std::vector<int64_t>{count}))
            << kind;
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

TEST_P(OuterJoinToAntiExecution, PreservesOnResidualAfterSwapping) {
    for (bool swapped : {false, true}) {
        TTable sales({1, 1, 2}, {10, 200, 30});
        TTable returns({1}, {100});
        const std::string from = swapped
            ? "returns r RIGHT JOIN sales s"
            : "sales s LEFT JOIN returns r";
        auto plan = PlanSql("SELECT s.value FROM " + from
            + " ON s.key = r.key AND s.value > r.value WHERE r.key IS NULL",
            sales, returns);
        auto join = Joins(plan).front();
        EXPECT_EQ(join->JoinType(), EJoinType::LeftAnti);
        ASSERT_NE(join->Filter(), nullptr);
        EXPECT_EQ(RunValues(plan, Settings()), (std::vector<int64_t>{10, 30}));
    }
}

TEST_P(OuterJoinToAntiExecution, PreservesRightOnlyOnPredicate) {
    TTable sales({1, 2}, {10, 20});
    TTable returns({1}, {100});
    auto plan = PlanSql(R"sql(
SELECT s.value FROM sales s
LEFT JOIN returns r ON s.key = r.key AND r.value > 1000
WHERE r.key IS NULL
)sql", sales, returns);
    EXPECT_EQ(Joins(plan).front()->JoinType(), EJoinType::LeftAnti);
    EXPECT_EQ(RunValues(plan, Settings()), (std::vector<int64_t>{10, 20}));
}

TEST(OuterJoinToAnti, NullablePayloadCanBeNullOnMatchedRows) {
    TTable sales({1, 2}, {10, 20});
    TTable returns({1}, {100}, {}, {0});
    auto plan = PlanSql(R"sql(
SELECT s.value FROM sales s
LEFT JOIN returns r ON s.key = r.key WHERE r.value IS NULL
)sql", sales, returns);
    EXPECT_EQ(Joins(plan).front()->JoinType(), EJoinType::Left);
    EXPECT_EQ(RunValues(plan), (std::vector<int64_t>{10, 20}));
}

TEST(OuterJoinToAnti, KeepsJoinWhenNullExtendedColumnsAreSelected) {
    TTable sales({}, {});
    TTable returns({}, {});
    for (const std::string sql : {
        "SELECT s.value, r.value FROM sales s LEFT JOIN returns r ON s.key = r.key WHERE r.key IS NULL",
        "SELECT * FROM sales s LEFT JOIN returns r ON s.key = r.key WHERE r.key IS NULL",
        "SELECT count(r.key) FROM sales s LEFT JOIN returns r ON s.key = r.key WHERE r.key IS NULL",
        "SELECT r.value FROM returns r RIGHT JOIN sales s ON s.key = r.key WHERE r.key IS NULL"})
    {
        auto plan = PlanSql(sql, sales, returns);
        EXPECT_NE(Joins(plan).front()->JoinType(), EJoinType::LeftAnti) << sql;
    }
}

TEST(OuterJoinToAnti, KeepsJoinWhenAnotherConjunctReadsRightSide) {
    TTable sales({}, {});
    TTable returns({}, {});
    auto plan = PlanSql(R"sql(
SELECT s.value FROM sales s
LEFT JOIN returns r ON s.key = r.key
WHERE r.key IS NULL AND coalesce(r.value, 0) = 0
)sql", sales, returns);
    EXPECT_EQ(Joins(plan).front()->JoinType(), EJoinType::Left);
}

TEST(OuterJoinToAnti, NullTestInsideOrDoesNotRejectEveryMatch) {
    TTable sales({1, 2}, {10, 20});
    TTable returns({1}, {100});
    auto plan = PlanSql(R"sql(
SELECT s.value FROM sales s LEFT JOIN returns r ON s.key = r.key
WHERE r.key IS NULL OR s.value = 10
)sql", sales, returns);
    EXPECT_EQ(Joins(plan).front()->JoinType(), EJoinType::Left);
    EXPECT_EQ(RunValues(plan), (std::vector<int64_t>{10, 20}));
}

TEST(OuterJoinToAnti, FactorsCommonNullTestFromEveryDisjunct) {
    TTable sales({1, 2, 3}, {10, 20, 30});
    TTable returns({1}, {100});
    auto plan = PlanSql(R"sql(
SELECT s.value FROM sales s LEFT JOIN returns r ON s.key = r.key
WHERE (r.key IS NULL AND s.value > 20)
   OR (r.key IS NULL AND s.value = 20)
)sql", sales, returns);
    EXPECT_EQ(Joins(plan).front()->JoinType(), EJoinType::LeftAnti);
    EXPECT_EQ(RunValues(plan), (std::vector<int64_t>{20, 30}));
}

TEST(OuterJoinToAnti, DoesNotRewriteFullJoinOrNullTestInOn) {
    TTable sales({}, {});
    TTable returns({}, {});
    for (const std::string sql : {
        "SELECT s.value FROM sales s FULL JOIN returns r ON s.key = r.key WHERE r.key IS NULL",
        "SELECT s.value FROM sales s LEFT JOIN returns r ON s.key = r.key AND r.key IS NULL",
        "SELECT s.value FROM sales s LEFT JOIN returns r ON s.key = r.key WHERE s.key IS NULL",
        "SELECT s.value FROM sales s LEFT JOIN returns r ON s.key = r.key WHERE r.key IS NOT NULL",
        "SELECT s.value FROM sales s LEFT JOIN returns r ON coalesce(s.key, 0) = coalesce(r.key, 0) WHERE r.key IS NULL"})
    {
        auto plan = PlanSql(sql, sales, returns);
        EXPECT_NE(Joins(plan).front()->JoinType(), EJoinType::LeftAnti) << sql;
    }
}

TEST(OuterJoinToAnti, CountStarDoesNotDemandAMarkerColumn) {
    TTable sales({1, 2, 2}, {10, 20, 21});
    TTable returns({1}, {100});
    auto plan = PlanSql(R"sql(
SELECT count(*) FROM sales s LEFT JOIN returns r ON s.key = r.key
WHERE r.key IS NULL
)sql", sales, returns);
    EXPECT_EQ(Joins(plan).front()->JoinType(), EJoinType::LeftAnti);
    EXPECT_EQ(RunValues(plan), (std::vector<int64_t>{2}));
}

TEST(OuterJoinToAnti, RewritesInsideGroupedCte) {
    TTable sales({1, 2, 2}, {10, 20, 21});
    TTable returns({1}, {100});
    auto plan = PlanSql(R"sql(
WITH unreturned AS (
    SELECT s.key, sum(s.value) AS total FROM sales s
    LEFT JOIN returns r ON s.key = r.key WHERE r.key IS NULL
    GROUP BY s.key)
SELECT total FROM unreturned
)sql", sales, returns);
    EXPECT_EQ(Joins(plan).front()->JoinType(), EJoinType::LeftAnti);
    EXPECT_EQ(RunValues(plan), (std::vector<int64_t>{41}));
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
