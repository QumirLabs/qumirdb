#include <gtest/gtest.h>
#include "mock_source.h"
#include "plan_runner.h"

#include <qdb/exec/join_exec.h>
#include <qdb/exec/planner_helpers.h>
#include <qdb/exec/row_join_exec.h>
#include <qdb/io/io.h>
#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/operator.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/ops/union.h>
#include <qdb/plan/ops/stats.h>
#include <qdb/plan/types/nullable.h>
#include <qdb/plan/passes/runtime_filters.h>
#include <qdb/plan/passes/typing.h>

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/parser/type.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

using namespace NQdb;
using namespace NQumir::NAst;
using namespace NQumir::NAst::NLiterals;

namespace {

TTypePtr I64Type() { return std::make_shared<TIntegerType>(TIntegerType::I64); }

TTypePtr KeyValSchema(const std::string& key, const std::string& val) {
    return std::make_shared<TStructType>(
        std::vector<std::pair<std::string, TTypePtr>>{{key, I64Type()}, {val, I64Type()}});
}

TRowSet KeyValBatch(int64_t* keys, int64_t* vals, int64_t rows, std::vector<TColumn>& cols) {
    cols = {TColumn{.Data = reinterpret_cast<char*>(keys)},
            TColumn{.Data = reinterpret_cast<char*>(vals)}};
    return TRowSet{.Columns = cols.data(), .ColumnCount = 2, .RowCount = rows, .RefCount = 1};
}

struct TOut4 {
    int64_t Lk, Lv, Rk, Rv;
    auto operator<=>(const TOut4&) const = default;
};

TJoinKernels CompileJoin(TKernelCompiler& compiler,
    const TTypePtr& leftType, const TTypePtr& rightType, EJoinType type,
    bool nullsEqual = false)
{
    auto spec = NKernel::BuildJoinKernelSpec(
        static_cast<TStructType&>(*leftType),
        static_cast<TStructType&>(*rightType),
        {{"lk", "rk", nullsEqual}}, type, nullptr);
    return compiler.CompileJoin(spec);
}

struct TOut2 {
    int64_t Lk, Lv;
    auto operator<=>(const TOut2&) const = default;
};

// Duplicate right keys must not duplicate SEMI output.
std::vector<TOut2> RunSemiAnti(
    EJoinType type, EJoinBuildSide buildSide, bool nullableKeys = false,
    bool nullsEqual = false)
{
    std::vector<int64_t> lk0 = {1, 2}, lv0 = {10, 20};
    std::vector<int64_t> lk1 = {1, 3}, lv1 = {30, 40};
    std::vector<int64_t> rk0 = {1, 1}, rv0 = {100, 200};
    std::vector<int64_t> rk1 = {2}, rv1 = {300};
    std::vector<TColumn> lcols0, lcols1, rcols0, rcols1;

    auto leftType = KeyValSchema("lk", "lv");
    auto rightType = KeyValSchema("rk", "rv");
    auto leftBatch0 = KeyValBatch(lk0.data(), lv0.data(), 2, lcols0);
    auto leftBatch1 = KeyValBatch(lk1.data(), lv1.data(), 2, lcols1);
    auto rightBatch0 = KeyValBatch(rk0.data(), rv0.data(), 2, rcols0);
    auto rightBatch1 = KeyValBatch(rk1.data(), rv1.data(), 1, rcols1);

    uint8_t valid = 0b00000001;
    uint8_t invalid = 0;
    if (nullableKeys) {
        static_cast<TStructType&>(*leftType).Fields[0].second =
            std::make_shared<TNullable>(I64Type());
        static_cast<TStructType&>(*rightType).Fields[0].second =
            std::make_shared<TNullable>(I64Type());
        lcols0[0].Mask = &valid;
        lcols1[0].Mask = &valid;
        rcols0[0].Mask = &valid;
        rcols1[0].Mask = &invalid;
    }

    TKernelCompiler compiler;
    auto kernels = CompileJoin(compiler, leftType, rightType, type, nullsEqual);
    TInnerJoinProcessor processor(std::move(kernels), type, buildSide);

    int leftFetches = 0;
    int rightIndex = 0;
    int leftIndex = 0;
    auto right = [&](TRowSet& rowSet) {
        if (rightIndex == 0) { rowSet = rightBatch0; ++rightIndex; return EJoinFetchResult::OK; }
        if (rightIndex == 1) { rowSet = rightBatch1; ++rightIndex; return EJoinFetchResult::OK; }
        return EJoinFetchResult::FINISHED;
    };
    auto left = [&](TRowSet& rowSet) {
        ++leftFetches;
        if (leftIndex == 0) { rowSet = leftBatch0; ++leftIndex; return EJoinFetchResult::OK; }
        if (leftIndex == 1) { rowSet = leftBatch1; ++leftIndex; return EJoinFetchResult::OK; }
        return EJoinFetchResult::FINISHED;
    };

    std::vector<TOut2> got;
    TRowSet out{};
    for (;;) {
        if (buildSide == EJoinBuildSide::Right &&
            processor.RequiredInputSide() == EJoinBuildSide::Right) {
            EXPECT_EQ(leftFetches, 0) << "left streamed before the right was built";
        }
        auto result = processor.Process(left, right, out);
        if (result == EJoinProcessorResult::NEED_DATA) continue;
        if (result == EJoinProcessorResult::FINISHED) break;
        EXPECT_EQ(out.ColumnCount, 2) << "semi/anti emits left columns only";
        const auto* c0 = reinterpret_cast<const int64_t*>(out.Columns[0].Data);
        const auto* c1 = reinterpret_cast<const int64_t*>(out.Columns[1].Data);
        for (int64_t i = 0; i < out.RowCount; ++i) {
            const bool validKey = !out.Columns[0].Mask ||
                ((out.Columns[0].Mask[i / 8] >> (i % 8)) & 1);
            if (nullableKeys) {
                EXPECT_EQ(validKey, c1[i] == 10 || c1[i] == 30);
            }
            got.push_back({validKey ? c0[i] : 0, c1[i]});
        }
        Release(&out);
    }
    std::sort(got.begin(), got.end());
    return got;
}

std::vector<TOut2> ExpectedSemiAnti(EJoinType type) {
    std::vector<TOut2> e = type == EJoinType::LeftAnti
        ? std::vector<TOut2>{{3, 40}}
        : std::vector<TOut2>{{1, 10}, {1, 30}, {2, 20}};
    std::sort(e.begin(), e.end());
    return e;
}

// Drives a processor with a one-batch build side and a two-batch probe side.
// Asserts the probe is never fetched while RequiredInputSide reports the build
// side, then returns the collected inner-join rows.
std::vector<TOut4> RunAsymmetric(EJoinBuildSide buildSide) {
    std::vector<int64_t> lk = {1, 2, 1}, lv = {10, 20, 30};
    std::vector<int64_t> rk0 = {1, 1}, rv0 = {100, 200};
    std::vector<int64_t> rk1 = {3}, rv1 = {300};
    std::vector<TColumn> lcols, rcols0, rcols1;

    auto leftType = KeyValSchema("lk", "lv");
    auto rightType = KeyValSchema("rk", "rv");
    auto leftBatch = KeyValBatch(lk.data(), lv.data(), 3, lcols);
    auto rightBatch0 = KeyValBatch(rk0.data(), rv0.data(), 2, rcols0);
    auto rightBatch1 = KeyValBatch(rk1.data(), rv1.data(), 1, rcols1);

    TKernelCompiler compiler;
    auto kernels = CompileJoin(compiler, leftType, rightType, EJoinType::Inner);
    TInnerJoinProcessor processor(std::move(kernels), EJoinType::Inner, buildSide);

    // Build side = single batch; probe side = two batches. Which physical input
    // is which depends on buildSide; the probe is the opposite of the build.
    const bool buildIsRight = buildSide == EJoinBuildSide::Right;
    int rightFetches = 0;
    int leftFetches = 0;
    int rightIndex = 0;
    int leftIndex = 0;

    auto right = [&](TRowSet& rowSet) {
        ++rightFetches;
        if (rightIndex == 0) { rowSet = rightBatch0; ++rightIndex; return EJoinFetchResult::OK; }
        if (rightIndex == 1) { rowSet = rightBatch1; ++rightIndex; return EJoinFetchResult::OK; }
        return EJoinFetchResult::FINISHED;
    };
    auto left = [&](TRowSet& rowSet) {
        ++leftFetches;
        if (leftIndex == 0) { rowSet = leftBatch; ++leftIndex; return EJoinFetchResult::OK; }
        return EJoinFetchResult::FINISHED;
    };

    std::vector<TOut4> got;
    TRowSet out{};
    for (;;) {
        // Before each step, the side not yet requested must not have been pulled.
        switch (processor.RequiredInputSide()) {
            case EJoinBuildSide::Left:
                if (!buildIsRight) EXPECT_EQ(rightFetches, 0) << "probe pulled while building";
                break;
            case EJoinBuildSide::Right:
                if (buildIsRight) EXPECT_EQ(leftFetches, 0) << "probe pulled while building";
                break;
            case EJoinBuildSide::Auto:
                break;
        }
        auto result = processor.Process(left, right, out);
        if (result == EJoinProcessorResult::NEED_DATA) continue;
        if (result == EJoinProcessorResult::FINISHED) break;
        const auto* c0 = reinterpret_cast<const int64_t*>(out.Columns[0].Data);
        const auto* c1 = reinterpret_cast<const int64_t*>(out.Columns[1].Data);
        const auto* c2 = reinterpret_cast<const int64_t*>(out.Columns[2].Data);
        const auto* c3 = reinterpret_cast<const int64_t*>(out.Columns[3].Data);
        for (int64_t i = 0; i < out.RowCount; ++i) {
            got.push_back({c0[i], c1[i], c2[i], c3[i]});
        }
        Release(&out);
    }
    std::sort(got.begin(), got.end());
    return got;
}

std::vector<TOut4> ExpectedInner() {
    // lk {1,2,1} x rk {1,1,3}: key 1 matches (l rows 10,30) x (r rows 100,200).
    std::vector<TOut4> e = {
        {1, 10, 1, 100}, {1, 10, 1, 200}, {1, 30, 1, 100}, {1, 30, 1, 200}};
    std::sort(e.begin(), e.end());
    return e;
}

// Minimal operator carrying a schema and Stats_, to exercise ChooseJoinBuildSide.
class TFakeSource : public IOperator {
public:
    static constexpr const char* OpId = "fake_source";
    explicit TFakeSource(std::vector<std::pair<std::string, TTypePtr>> fields) {
        auto schema = std::make_shared<TStructType>(std::move(fields));
        Type = std::make_shared<TFunctionType>(std::vector<TTypePtr>{}, schema);
    }
    std::string_view RelName() const override { return OpId; }
    std::vector<TColumnReference> ColumnReferences() const override { return {}; }
    std::vector<TExprPtr> Children() const override { return {}; }
    const std::string ToString() const override { return "(rel fake_source)"; }
};

TStatsPtr Rows(uint64_t n) {
    auto s = std::make_shared<TStats>();
    s->RowCount = n;
    return s;
}

TOperatorPtr JoinOfType(EJoinType type) {
    auto left = std::make_shared<TFakeSource>(
        std::vector<std::pair<std::string, TTypePtr>>{{"lk", I64Type()}, {"lv", I64Type()}});
    auto right = std::make_shared<TFakeSource>(
        std::vector<std::pair<std::string, TTypePtr>>{{"rk", I64Type()}, {"rv", I64Type()}});
    auto join = MakeJoin(left, right, {{"lk", "rk"}}, type);
    EXPECT_TRUE(join.has_value()) << (join ? "" : join.error().ToString());
    return std::static_pointer_cast<IOperator>(join.value_or(nullptr));
}

TOperatorPtr InnerJoin() {
    auto left = std::make_shared<TFakeSource>(
        std::vector<std::pair<std::string, TTypePtr>>{{"lk", I64Type()}, {"lv", I64Type()}});
    auto right = std::make_shared<TFakeSource>(
        std::vector<std::pair<std::string, TTypePtr>>{{"rk", I64Type()}, {"rv", I64Type()}});
    auto join = MakeJoin(left, right, {{"lk", "rk"}}, EJoinType::Inner);
    EXPECT_TRUE(join.has_value()) << (join ? "" : join.error().ToString());
    return std::static_pointer_cast<IOperator>(join.value_or(nullptr));
}

} // namespace

TEST(AsymmetricJoin, BuildRightMatchesSymmetric) {
    EXPECT_EQ(RunAsymmetric(EJoinBuildSide::Right), ExpectedInner());
}

TEST(AsymmetricJoin, BuildLeftMatchesSymmetric) {
    EXPECT_EQ(RunAsymmetric(EJoinBuildSide::Left), ExpectedInner());
}

TStatsPtr RowsWithKeyNdv(uint64_t rows, const std::string& column,
    uint64_t ndv, bool exact = false) {
    auto s = std::make_shared<TStats>();
    s->RowCount = rows;
    auto col = std::make_shared<TStats::TColumnStats>();
    col->Ndv = ndv;
    col->NdvIsExact = exact;
    s->ColumnStats[column] = std::move(col);
    return s;
}

std::optional<TRuntimeFilterSpec> AttachTo(const TOperatorPtr& join) {
    uint32_t nextId = 1;
    AttachRuntimeFilters(join, nextId);
    return static_cast<TJoinOperator*>(join.get())->RuntimeFilter();
}

TEST(AttachRuntimeFilters, EmitsFromTheSideWithFewerDistinctKeys) {
    auto join = JoinOfType(EJoinType::Inner);
    ASSERT_TRUE(join);
    auto* j = static_cast<TJoinOperator*>(join.get());

    j->Left()->Stats_ = RowsWithKeyNdv(400000, "lk", 400000);
    j->Right()->Stats_ = RowsWithKeyNdv(40000, "rk", 10000);
    auto emitted = AttachTo(join);
    ASSERT_TRUE(emitted.has_value());
    EXPECT_EQ(emitted->Id, 1u);
    EXPECT_EQ(emitted->BuildSide, EJoinFilterSide::Right);
    EXPECT_EQ(ChooseJoinBuildSide(*j), EJoinBuildSide::Right);

    j->Left()->Stats_ = RowsWithKeyNdv(40000, "lk", 10000);
    j->Right()->Stats_ = RowsWithKeyNdv(400000, "rk", 400000);
    emitted = AttachTo(join);
    ASSERT_TRUE(emitted.has_value());
    EXPECT_EQ(emitted->BuildSide, EJoinFilterSide::Left);
    EXPECT_EQ(ChooseJoinBuildSide(*j), EJoinBuildSide::Left);
}

TEST(AttachRuntimeFilters, JudgesDistinctKeysNotRows) {
    auto join = JoinOfType(EJoinType::Inner);
    ASSERT_TRUE(join);
    auto* j = static_cast<TJoinOperator*>(join.get());

    j->Left()->Stats_ = RowsWithKeyNdv(1000000, "lk", 1000000);
    j->Right()->Stats_ = RowsWithKeyNdv(50000000, "rk", 100);
    EXPECT_FALSE(AttachTo(join).has_value())
        << "a low-NDV side is too expensive to build when it has 50M rows";

    j->Left()->Stats_ = RowsWithKeyNdv(50000000, "lk", 1000);
    j->Right()->Stats_ = RowsWithKeyNdv(1000, "rk", 1000);
    EXPECT_FALSE(AttachTo(join).has_value());

    j->Left()->Stats_ = RowsWithKeyNdv(50000000, "lk", 50000000);
    j->Right()->Stats_ = RowsWithKeyNdv(1000000, "rk", 100);
    auto emitted = AttachTo(join);
    ASSERT_TRUE(emitted.has_value());
    EXPECT_EQ(emitted->BuildSide, EJoinFilterSide::Right);
}

TEST(AttachRuntimeFilters, AccountsForForcedBuildAndNdvUncertainty) {
    auto join = JoinOfType(EJoinType::Inner);
    ASSERT_TRUE(join);
    auto* j = static_cast<TJoinOperator*>(join.get());

    j->Left()->Stats_ = RowsWithKeyNdv(50000, "lk", 50000);
    j->Right()->Stats_ = RowsWithKeyNdv(10000, "rk", 10000);
    EXPECT_FALSE(AttachTo(join).has_value())
        << "weak filter does not pay for blocking a 5:1 join";

    j->Left()->Stats_ = RowsWithKeyNdv(300000, "lk", 300000);
    j->Right()->Stats_ = RowsWithKeyNdv(100000, "rk", 100);
    auto emitted = AttachTo(join);
    ASSERT_TRUE(emitted.has_value())
        << "high selectivity can pay for a 3:1 build barrier";
    EXPECT_EQ(emitted->BuildSide, EJoinFilterSide::Right);

    j->Left()->Stats_ = RowsWithKeyNdv(50000, "lk", 50000, true);
    j->Right()->Stats_ = RowsWithKeyNdv(10000, "rk", 10000, true);
    EXPECT_TRUE(AttachTo(join).has_value())
        << "exact NDV removes the uncertainty margin";

    j->MutableKeys().push_back({"lv", "rv"});
    j->Left()->Stats_ = RowsWithKeyNdv(50000, "lk", 250, true);
    j->Right()->Stats_ = RowsWithKeyNdv(10000, "rk", 100, true);
    j->Left()->Stats_->ColumnStats["lv"] =
        RowsWithKeyNdv(50000, "lv", 200, true)->ColumnStats.at("lv");
    j->Right()->Stats_->ColumnStats["rv"] =
        RowsWithKeyNdv(10000, "rv", 100, true)->ColumnStats.at("rv");
    EXPECT_FALSE(AttachTo(join).has_value())
        << "products of exact column NDVs still need an uncertainty margin";
}

TEST(AttachRuntimeFilters, PricesTheActualProbeBoundary) {
    auto join = JoinOfType(EJoinType::Inner);
    ASSERT_TRUE(join);
    auto* j = static_cast<TJoinOperator*>(join.get());
    auto source = j->Left();
    auto filtered = MakeFilter(source, "(> lk 0)");
    ASSERT_TRUE(filtered.has_value());
    j->MutableLeft() = *filtered;
    source->Stats_ = RowsWithKeyNdv(10'000'000, "lk", 1'000'000);
    (*filtered)->Stats_ = RowsWithKeyNdv(1'000'000, "lk", 1'000'000);
    j->Right()->Stats_ = RowsWithKeyNdv(100'000, "rk", 10'000);

    auto emitted = AttachTo(join);
    ASSERT_TRUE(emitted);
    EXPECT_EQ(emitted->BuildSide, EJoinFilterSide::Right);
    EXPECT_TRUE(source->RuntimeFilterUses.empty());
    ASSERT_EQ((*filtered)->RuntimeFilterUses.size(), 1u);
    EXPECT_EQ((*filtered)->RuntimeFilterUses[0].ProbeKeys,
        (std::vector<std::string>{"lk"}));

    // Forced measurement mode still chooses the deepest safe boundary.
    uint32_t nextId = 1;
    AttachRuntimeFilters(join, nextId, /*force=*/true);
    ASSERT_EQ(source->RuntimeFilterUses.size(), 1u);
    EXPECT_TRUE((*filtered)->RuntimeFilterUses.empty());

    // With equal row estimates, normal planning also chooses the source.
    source->Stats_ = RowsWithKeyNdv(1'000'000, "lk", 1'000'000);
    emitted = AttachTo(join);
    ASSERT_TRUE(emitted);
    ASSERT_EQ(source->RuntimeFilterUses.size(), 1u);
    EXPECT_TRUE((*filtered)->RuntimeFilterUses.empty());
}

TEST(AttachRuntimeFilters, UsesBoundaryNdvForWorkSavedBeforeJoin) {
    auto join = JoinOfType(EJoinType::Inner);
    ASSERT_TRUE(join);
    auto* j = static_cast<TJoinOperator*>(join.get());
    auto source = j->Left();
    auto filtered = MakeFilter(source, "(> lk 0)");
    ASSERT_TRUE(filtered.has_value());
    j->MutableLeft() = *filtered;
    (*filtered)->Stats_ = RowsWithKeyNdv(1'000'000, "lk", 100'000);
    j->Right()->Stats_ = RowsWithKeyNdv(100'000, "rk", 10'000);

    // The SQL filter retains many of the same keys. A Bloom probe on every
    // source row costs more than the predicate work it would avoid.
    source->Stats_ = RowsWithKeyNdv(2'500'000, "lk", 100'000);
    ASSERT_TRUE(AttachTo(join));
    EXPECT_TRUE(source->RuntimeFilterUses.empty());
    ASSERT_EQ((*filtered)->RuntimeFilterUses.size(), 1u);

    // When the source has a wider key domain, the runtime filter rejects
    // more rows before the SQL predicate, so the source boundary wins.
    source->Stats_ = RowsWithKeyNdv(2'500'000, "lk", 1'000'000);
    ASSERT_TRUE(AttachTo(join));
    ASSERT_EQ(source->RuntimeFilterUses.size(), 1u);
    EXPECT_TRUE((*filtered)->RuntimeFilterUses.empty());
}

TEST(AttachRuntimeFilters, RowCountCapDoesNotMakeNdvExact) {
    auto join = JoinOfType(EJoinType::Inner);
    ASSERT_TRUE(join);
    auto* j = static_cast<TJoinOperator*>(join.get());
    j->Left()->Stats_ = RowsWithKeyNdv(100'000, "lk", 1'000'000, true);
    j->Right()->Stats_ = RowsWithKeyNdv(40'000, "rk", 10'000, true);
    EXPECT_FALSE(AttachTo(join))
        << "capping an inherited exact NDV to row count is only an estimate";
}

TEST(AttachRuntimeFilters, StopsAtComputedProjectKey) {
    auto join = JoinOfType(EJoinType::Inner);
    ASSERT_TRUE(join);
    auto* j = static_cast<TJoinOperator*>(join.get());
    auto source = j->Left();
    auto projected = MakeProject(source,
        {{"alias_key", "(+ lk 0)"}, {"lv", "lv"}});
    ASSERT_TRUE(projected.has_value());
    j->MutableLeft() = *projected;
    j->MutableKeys()[0].Left = "alias_key";
    source->Stats_ = RowsWithKeyNdv(1'000'000, "lk", 1'000'000);
    (*projected)->Stats_ = RowsWithKeyNdv(
        1'000'000, "alias_key", 1'000'000);
    j->Right()->Stats_ = RowsWithKeyNdv(100'000, "rk", 10'000);

    auto emitted = AttachTo(join);
    ASSERT_TRUE(emitted);
    EXPECT_TRUE(source->RuntimeFilterUses.empty());
    ASSERT_EQ((*projected)->RuntimeFilterUses.size(), 1u);
    EXPECT_EQ((*projected)->RuntimeFilterUses[0].ProbeKeys,
        (std::vector<std::string>{"alias_key"}));
}

TEST(AttachRuntimeFilters, PricesUnionBranchesIndependently) {
    auto join = JoinOfType(EJoinType::Inner);
    ASSERT_TRUE(join);
    auto* j = static_cast<TJoinOperator*>(join.get());
    auto first = j->Left();
    auto second = std::make_shared<TFakeSource>(
        std::vector<std::pair<std::string, TTypePtr>>{
            {"lk", I64Type()}, {"lv", I64Type()}});
    auto filtered = MakeFilter(first, "(> lk 0)");
    ASSERT_TRUE(filtered.has_value());
    auto un = std::make_shared<TUnionAllOperator>(
        std::vector<TOperatorPtr>{*filtered, second});
    j->MutableLeft() = un;
    first->Stats_ = RowsWithKeyNdv(5'000'000, "lk", 1'000'000);
    (*filtered)->Stats_ = RowsWithKeyNdv(500'000, "lk", 500'000);
    second->Stats_ = RowsWithKeyNdv(500'000, "lk", 500'000);
    un->Stats_ = RowsWithKeyNdv(1'000'000, "lk", 1'000'000);
    j->Right()->Stats_ = RowsWithKeyNdv(100'000, "rk", 10'000);

    auto emitted = AttachTo(join);
    ASSERT_TRUE(emitted);
    EXPECT_TRUE(un->RuntimeFilterUses.empty());
    EXPECT_TRUE(first->RuntimeFilterUses.empty());
    ASSERT_EQ((*filtered)->RuntimeFilterUses.size(), 1u);
    ASSERT_EQ(second->RuntimeFilterUses.size(), 1u);
    EXPECT_EQ((*filtered)->RuntimeFilterUses[0].Id,
        second->RuntimeFilterUses[0].Id);
}

TEST(AttachRuntimeFilters, UnionChoosesEachBranchBySavedWork) {
    auto join = JoinOfType(EJoinType::Inner);
    ASSERT_TRUE(join);
    auto* j = static_cast<TJoinOperator*>(join.get());
    auto first = j->Left();
    auto second = std::make_shared<TFakeSource>(
        std::vector<std::pair<std::string, TTypePtr>>{
            {"lk", I64Type()}, {"lv", I64Type()}});
    auto filtered = MakeFilter(first, "(> lk 0)");
    ASSERT_TRUE(filtered.has_value());
    auto un = std::make_shared<TUnionAllOperator>(
        std::vector<TOperatorPtr>{*filtered, second});
    j->MutableLeft() = un;
    first->Stats_ = RowsWithKeyNdv(2'500'000, "lk", 1'000'000);
    (*filtered)->Stats_ = RowsWithKeyNdv(1'000'000, "lk", 100'000);
    second->Stats_ = RowsWithKeyNdv(1'000'000, "lk", 100'000);
    un->Stats_ = RowsWithKeyNdv(2'000'000, "lk", 200'000);
    j->Right()->Stats_ = RowsWithKeyNdv(100'000, "rk", 10'000);

    ASSERT_TRUE(AttachTo(join));
    EXPECT_TRUE(un->RuntimeFilterUses.empty());
    EXPECT_TRUE((*filtered)->RuntimeFilterUses.empty());
    ASSERT_EQ(first->RuntimeFilterUses.size(), 1u);
    ASSERT_EQ(second->RuntimeFilterUses.size(), 1u);
    EXPECT_EQ(first->RuntimeFilterUses[0].Id, second->RuntimeFilterUses[0].Id);
}

TEST(AttachRuntimeFilters, SharedStageAboveUnionIsCreditedOnce) {
    auto join = JoinOfType(EJoinType::Inner);
    ASSERT_TRUE(join);
    auto* j = static_cast<TJoinOperator*>(join.get());
    auto first = j->Left();
    auto second = std::make_shared<TFakeSource>(
        std::vector<std::pair<std::string, TTypePtr>>{
            {"lk", I64Type()}, {"lv", I64Type()}});
    auto un = std::make_shared<TUnionAllOperator>(
        std::vector<TOperatorPtr>{first, second});
    auto filtered = MakeFilter(un, "(> lk 0)");
    ASSERT_TRUE(filtered.has_value());
    j->MutableLeft() = *filtered;
    first->Stats_ = RowsWithKeyNdv(1'000'000, "lk", 50'000);
    second->Stats_ = RowsWithKeyNdv(1'000'000, "lk", 50'000);
    un->Stats_ = RowsWithKeyNdv(2'000'000, "lk", 100'000);
    (*filtered)->Stats_ = RowsWithKeyNdv(1'000'000, "lk", 100'000);
    j->Right()->Stats_ = RowsWithKeyNdv(100'000, "rk", 20'000);

    ASSERT_TRUE(AttachTo(join));
    ASSERT_EQ((*filtered)->RuntimeFilterUses.size(), 1u);
    EXPECT_TRUE(un->RuntimeFilterUses.empty());
    EXPECT_TRUE(first->RuntimeFilterUses.empty());
    EXPECT_TRUE(second->RuntimeFilterUses.empty());
}

TEST(AttachRuntimeFilters, LeftOuterFiltersOnlyNullableRightInput) {
    auto join = JoinOfType(EJoinType::Left);
    ASSERT_TRUE(join);
    auto* j = static_cast<TJoinOperator*>(join.get());
    j->Left()->Stats_ = RowsWithKeyNdv(100'000, "lk", 10'000);
    j->Right()->Stats_ = RowsWithKeyNdv(1'000'000, "rk", 1'000'000);

    auto emitted = AttachTo(join);
    ASSERT_TRUE(emitted);
    EXPECT_EQ(emitted->BuildSide, EJoinFilterSide::Left);
    EXPECT_EQ(ChooseJoinBuildSide(*j), EJoinBuildSide::Left);
    EXPECT_TRUE(j->Left()->RuntimeFilterUses.empty());
    ASSERT_EQ(j->Right()->RuntimeFilterUses.size(), 1u);
    EXPECT_EQ(j->Right()->RuntimeFilterUses[0].ProbeKeys,
        (std::vector<std::string>{"rk"}));

    // Even forced placement may not reject preserved left rows.
    uint32_t nextId = 1;
    AttachRuntimeFilters(join, nextId, /*force=*/true);
    ASSERT_TRUE(j->RuntimeFilter());
    EXPECT_EQ(j->RuntimeFilter()->BuildSide, EJoinFilterSide::Left);
    EXPECT_TRUE(j->Left()->RuntimeFilterUses.empty());
}

TEST(AttachRuntimeFilters, LeftOuterCrossesAggregateOnGroupKeys) {
    auto join = JoinOfType(EJoinType::Left);
    ASSERT_TRUE(join);
    auto* j = static_cast<TJoinOperator*>(join.get());
    auto source = j->Right();
    auto grouped = MakeAggregate(source, {"rk"}, {{"total", "sum", "rv"}});
    ASSERT_TRUE(grouped.has_value());
    j->MutableRight() = *grouped;
    j->Left()->Stats_ = RowsWithKeyNdv(100'000, "lk", 10'000);
    source->Stats_ = RowsWithKeyNdv(10'000'000, "rk", 10'000'000);
    (*grouped)->Stats_ = RowsWithKeyNdv(1'000'000, "rk", 1'000'000);

    ASSERT_TRUE(AttachTo(join));
    ASSERT_EQ(source->RuntimeFilterUses.size(), 1u);
    EXPECT_TRUE((*grouped)->RuntimeFilterUses.empty());

    auto aggregate = std::static_pointer_cast<TAggregateOperator>(*grouped);
    aggregate->MutableGroupingSets() = {{0}};
    ASSERT_TRUE(AttachTo(join));
    EXPECT_TRUE(source->RuntimeFilterUses.empty());
    ASSERT_EQ((*grouped)->RuntimeFilterUses.size(), 1u);
}

struct TLeftJoinChain {
    TOperatorPtr Outer;
    std::shared_ptr<TJoinOperator> First;
    TOperatorPtr Third;
};

TLeftJoinChain MakeLeftJoinChain() {
    auto left = std::make_shared<TFakeSource>(
        std::vector<std::pair<std::string, TTypePtr>>{
            {"lk", I64Type()}, {"lv", I64Type()}});
    auto middle = std::make_shared<TFakeSource>(
        std::vector<std::pair<std::string, TTypePtr>>{
            {"rk", I64Type()}, {"rv", I64Type()}});
    auto third = std::make_shared<TFakeSource>(
        std::vector<std::pair<std::string, TTypePtr>>{
            {"ck", I64Type()}, {"cv", I64Type()}});
    auto first = MakeJoin(left, middle, {{"lk", "rk"}}, EJoinType::Left);
    EXPECT_TRUE(first.has_value());
    auto outer = MakeJoin(*first, third, {{"lk", "ck"}}, EJoinType::Left);
    EXPECT_TRUE(outer.has_value());
    left->Stats_ = RowsWithKeyNdv(100'000, "lk", 10'000);
    middle->Stats_ = RowsWithKeyNdv(1'000'000, "rk", 1'000'000);
    (*first)->Stats_ = RowsWithKeyNdv(10'000'000, "lk", 10'000);
    third->Stats_ = RowsWithKeyNdv(1'000'000, "ck", 1'000'000);
    return {.Outer = *outer,
        .First = std::static_pointer_cast<TJoinOperator>(*first),
        .Third = third};
}

TEST(AttachRuntimeFilters, ReusesPreservedLeftKeyAcrossLeftJoins) {
    auto chain = MakeLeftJoinChain();
    uint32_t nextId = 1;
    AttachRuntimeFilters(chain.Outer, nextId);
    ASSERT_TRUE(chain.First->RuntimeFilter());
    EXPECT_FALSE(static_cast<TJoinOperator&>(*chain.Outer).RuntimeFilter());
    ASSERT_EQ(chain.First->Right()->RuntimeFilterUses.size(), 1u);
    ASSERT_EQ(chain.Third->RuntimeFilterUses.size(), 1u);
    EXPECT_EQ(chain.First->Right()->RuntimeFilterUses[0].Id,
        chain.Third->RuntimeFilterUses[0].Id);
    EXPECT_EQ(chain.Third->RuntimeFilterUses[0].ProbeKeys,
        (std::vector<std::string>{"ck"}));

    chain.Third->Stats_ = RowsWithKeyNdv(1'000'000, "ck", 100);
    nextId = 100;
    AttachRuntimeFilters(chain.Outer, nextId);
    EXPECT_TRUE(chain.Third->RuntimeFilterUses.empty());
    ASSERT_EQ(chain.First->Right()->RuntimeFilterUses.size(), 1u);
    EXPECT_EQ(chain.First->Right()->RuntimeFilterUses[0].Id, 100u);
}

TEST(AttachRuntimeFilters, ReuseFollowsOnlyDirectKeyAliases) {
    for (const auto& expression : {"lk", "(+ lk 0)"}) {
        auto chain = MakeLeftJoinChain();
        auto projected = MakeProject(chain.First,
            {{"mapped", expression}, {"lv", "lv"}});
        ASSERT_TRUE(projected.has_value());
        (*projected)->Stats_ = RowsWithKeyNdv(10'000'000, "mapped", 10'000);
        static_cast<TJoinOperator&>(*chain.Outer).MutableLeft() = *projected;
        static_cast<TJoinOperator&>(*chain.Outer).MutableKeys()[0].Left =
            "mapped";
        uint32_t nextId = 10;
        AttachRuntimeFilters(chain.Outer, nextId);
        EXPECT_EQ(chain.Third->RuntimeFilterUses.size(),
            expression == std::string("lk") ? 1u : 0u);
    }
}

TEST(AttachRuntimeFilters, ReuseRequiresTheSameKeyAndNullSemantics) {
    for (int variant = 0; variant < 3; ++variant) {
        auto chain = MakeLeftJoinChain();
        auto& outer = static_cast<TJoinOperator&>(*chain.Outer);
        if (variant == 0) {
            outer.MutableKeys()[0].Left = "lv";
        } else if (variant == 1) {
            outer.MutableKeys()[0].NullsEqual = true;
        } else {
            outer.MutableKeys()[0].Left = "rk";
        }
        uint32_t nextId = 1;
        AttachRuntimeFilters(chain.Outer, nextId);
        ASSERT_TRUE(chain.First->RuntimeFilter());
        EXPECT_TRUE(chain.Third->RuntimeFilterUses.empty());
    }
}

TEST(AttachRuntimeFilters, PrefersExistingFilterToBuildingTheSameKeysAgain) {
    auto chain = MakeLeftJoinChain();
    chain.First->Stats_ = RowsWithKeyNdv(1'000'000, "lk", 10'000);
    chain.Third->Stats_ = RowsWithKeyNdv(5'000'000, "ck", 5'000'000);
    uint32_t nextId = 1;
    AttachRuntimeFilters(chain.Outer, nextId);
    ASSERT_TRUE(chain.First->RuntimeFilter());
    EXPECT_FALSE(static_cast<TJoinOperator&>(*chain.Outer).RuntimeFilter());
    ASSERT_EQ(chain.Third->RuntimeFilterUses.size(), 1u);
    EXPECT_EQ(chain.Third->RuntimeFilterUses[0].Id,
        chain.First->RuntimeFilter()->Id);
}

TEST(AttachRuntimeFilters, ReuseMapsCompositeKeysInProducerOrder) {
    auto left = std::make_shared<TFakeSource>(
        std::vector<std::pair<std::string, TTypePtr>>{
            {"a", I64Type()}, {"b", I64Type()}});
    auto middle = std::make_shared<TFakeSource>(
        std::vector<std::pair<std::string, TTypePtr>>{
            {"x", I64Type()}, {"y", I64Type()}});
    auto third = std::make_shared<TFakeSource>(
        std::vector<std::pair<std::string, TTypePtr>>{
            {"u", I64Type()}, {"v", I64Type()}});
    auto first = MakeJoin(left, middle,
        {{"a", "x"}, {"b", "y"}}, EJoinType::Left);
    ASSERT_TRUE(first.has_value());
    auto outer = MakeJoin(*first, third,
        {{"b", "v"}, {"a", "u"}}, EJoinType::Left);
    ASSERT_TRUE(outer.has_value());
    auto stats = [](uint64_t rows, const std::string& one,
                     const std::string& two, uint64_t ndv) {
        auto result = RowsWithKeyNdv(rows, one, ndv);
        result->ColumnStats[two] =
            RowsWithKeyNdv(rows, two, ndv)->ColumnStats.at(two);
        return result;
    };
    left->Stats_ = stats(100'000, "a", "b", 100);
    middle->Stats_ = stats(1'000'000, "x", "y", 1'000);
    (*first)->Stats_ = stats(10'000'000, "a", "b", 100);
    third->Stats_ = stats(1'000'000, "u", "v", 1'000);
    uint32_t nextId = 1;
    AttachRuntimeFilters(*outer, nextId);
    ASSERT_TRUE(static_cast<TJoinOperator&>(**first).RuntimeFilter());
    ASSERT_EQ(third->RuntimeFilterUses.size(), 1u);
    EXPECT_EQ(third->RuntimeFilterUses[0].ProbeKeys,
        (std::vector<std::string>{"u", "v"}));
}

TEST(AttachRuntimeFilters, RejectsUnsafeManualOuterDirection) {
    auto join = JoinOfType(EJoinType::Left);
    ASSERT_TRUE(join);
    auto* j = static_cast<TJoinOperator*>(join.get());
    j->MutableRuntimeFilter() = TRuntimeFilterSpec{
        .Id = 7, .BuildSide = EJoinFilterSide::Right};
    EXPECT_THROW(PlaceRuntimeFilterConsumers(join), std::runtime_error);
}

TEST(AttachRuntimeFilters, RejectsResidualSemiManualFilter) {
    auto left = std::make_shared<TFakeSource>(
        std::vector<std::pair<std::string, TTypePtr>>{
            {"lk", I64Type()}, {"lv", I64Type()}});
    auto right = std::make_shared<TFakeSource>(
        std::vector<std::pair<std::string, TTypePtr>>{
            {"rk", I64Type()}, {"rv", I64Type()}});
    auto join = MakeJoin(left, right, {{"lk", "rk"}},
        EJoinType::LeftSemi, "(> rv 0)");
    ASSERT_TRUE(join.has_value());
    static_cast<TJoinOperator&>(**join).MutableRuntimeFilter() =
        TRuntimeFilterSpec{.Id = 7, .BuildSide = EJoinFilterSide::Right};
    EXPECT_THROW(PlaceRuntimeFilterConsumers(*join), std::runtime_error);
}

TEST(AttachRuntimeFilters, ClearsOrphanConsumerBindings) {
    auto join = JoinOfType(EJoinType::Inner);
    ASSERT_TRUE(join);
    auto& j = static_cast<TJoinOperator&>(*join);
    j.Left()->Stats_ = RowsWithKeyNdv(1'000'000, "lk", 1'000'000);
    j.Right()->Stats_ = RowsWithKeyNdv(100'000, "rk", 10'000);
    j.Left()->RuntimeFilterUses.push_back(
        {.Id = 999, .ProbeKeys = {"lk"}});
    uint32_t nextId = 1;
    AttachRuntimeFilters(join, nextId);
    ASSERT_TRUE(j.RuntimeFilter());
    ASSERT_EQ(j.Left()->RuntimeFilterUses.size(), 1u);
    EXPECT_EQ(j.Left()->RuntimeFilterUses[0].Id, j.RuntimeFilter()->Id);

    j.Left()->RuntimeFilterUses.push_back(
        {.Id = 999, .ProbeKeys = {"lk"}});
    nextId = 1;
    AttachRuntimeFilters(join, nextId);
    ASSERT_EQ(j.Left()->RuntimeFilterUses.size(), 1u);
    EXPECT_EQ(j.Left()->RuntimeFilterUses[0].Id, j.RuntimeFilter()->Id);
}

TEST(AttachRuntimeFilters, SkipsUncappedUnknownAndUnsupportedJoins) {
    auto inner = JoinOfType(EJoinType::Inner);
    ASSERT_TRUE(inner);
    auto* j = static_cast<TJoinOperator*>(inner.get());

    j->Left()->Stats_ = RowsWithKeyNdv(40000, "lk", 40000);
    j->Right()->Stats_ = RowsWithKeyNdv(40000, "rk", 40000);
    EXPECT_FALSE(AttachTo(inner).has_value()) << "balanced";

    j->Left()->Stats_ = RowsWithKeyNdv(100000000, "lk", 100000000);
    j->Right()->Stats_ = RowsWithKeyNdv(20000000, "rk", 20000000);
    EXPECT_FALSE(AttachTo(inner).has_value()) << "over the build row cap";

    j->Left()->Stats_ = Rows(40000);
    j->Right()->Stats_ = RowsWithKeyNdv(40000, "rk", 100);
    EXPECT_FALSE(AttachTo(inner).has_value()) << "no ndv";

    j->Left()->Stats_ = RowsWithKeyNdv(40000, "lk", 40000);
    j->Right()->Stats_ = nullptr;
    EXPECT_FALSE(AttachTo(inner).has_value()) << "no stats";

    for (auto type : {EJoinType::LeftAnti, EJoinType::Left, EJoinType::Full}) {
        auto join = JoinOfType(type);
        ASSERT_TRUE(join) << JoinTypeName(type);
        auto* other = static_cast<TJoinOperator*>(join.get());
        other->Left()->Stats_ = RowsWithKeyNdv(40000, "lk", 40000);
        other->Right()->Stats_ = RowsWithKeyNdv(40000, "rk", 100);
        EXPECT_FALSE(AttachTo(join).has_value()) << JoinTypeName(type);
    }
}

TEST(AttachRuntimeFilters, AdmitsLargeFilteredBuilds) {
    auto join = JoinOfType(EJoinType::Inner);
    ASSERT_TRUE(join);
    auto* j = static_cast<TJoinOperator*>(join.get());
    j->Left()->Stats_ = RowsWithKeyNdv(80000000, "lk", 80000000);
    j->Right()->Stats_ = RowsWithKeyNdv(30000000, "rk", 30000000);
    EXPECT_FALSE(AttachTo(join).has_value());

    auto filtered = MakeFilter(j->Right(), "(> rk 0)");
    ASSERT_TRUE(filtered.has_value());
    j->MutableRight() = *filtered;
    j->Right()->Stats_ = RowsWithKeyNdv(30000000, "rk", 30000000);
    auto emitted = AttachTo(join);
    ASSERT_TRUE(emitted.has_value());
    EXPECT_EQ(emitted->BuildSide, EJoinFilterSide::Right);

    j->Left()->Stats_ = Rows(40000000);
    EXPECT_FALSE(AttachTo(join).has_value())
        << "a nearly balanced join still pays the build barrier";

    j->Left()->Stats_ = Rows(500000);
    j->Right()->Stats_ = Rows(100000);
    EXPECT_FALSE(AttachTo(join).has_value())
        << "small probe inputs should use the NDV cost path";
}

TEST(AttachRuntimeFilters, FilteredBuildCanCompensateForUnknownNdv) {
    auto join = JoinOfType(EJoinType::LeftSemi);
    ASSERT_TRUE(join);
    auto* j = static_cast<TJoinOperator*>(join.get());
    j->Left()->Stats_ = Rows(150000000);
    auto filtered = MakeFilter(j->Right(), "(> rk 0)");
    ASSERT_TRUE(filtered.has_value());
    j->MutableRight() = *filtered;
    j->Right()->Stats_ = Rows(75000000);
    auto emitted = AttachTo(join);
    ASSERT_TRUE(emitted.has_value());
    EXPECT_EQ(emitted->BuildSide, EJoinFilterSide::Right);
}

TEST(AttachRuntimeFilters, SemiPublishesOnlyFromTheRight) {
    auto join = JoinOfType(EJoinType::LeftSemi);
    ASSERT_TRUE(join);
    auto* j = static_cast<TJoinOperator*>(join.get());

    j->Left()->Stats_ = RowsWithKeyNdv(400000, "lk", 400000);
    j->Right()->Stats_ = RowsWithKeyNdv(40000, "rk", 10000);
    auto emitted = AttachTo(join);
    ASSERT_TRUE(emitted.has_value());
    EXPECT_EQ(emitted->BuildSide, EJoinFilterSide::Right);

    j->Left()->Stats_ = RowsWithKeyNdv(40000, "lk", 10000);
    j->Right()->Stats_ = RowsWithKeyNdv(400000, "rk", 400000);
    EXPECT_FALSE(AttachTo(join).has_value());

    j->Left()->Stats_ = RowsWithKeyNdv(400000, "lk", 400000);
    j->Right()->Stats_ = RowsWithKeyNdv(40000, "rk", 10000);
    j->MutableFilter() = std::make_shared<TBinaryExpr>(
        NQumir::TLocation{}, TOperator("!="),
        std::make_shared<TIdentExpr>(NQumir::TLocation{}, "lv"),
        std::make_shared<TIdentExpr>(NQumir::TLocation{}, "rv"));
    EXPECT_FALSE(AttachTo(join).has_value());
}

TEST(AttachRuntimeFilters, IdsComeFromTheCallersAllocator) {
    auto first = JoinOfType(EJoinType::Inner);
    auto second = JoinOfType(EJoinType::Inner);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    for (auto* j : {static_cast<TJoinOperator*>(first.get()),
                    static_cast<TJoinOperator*>(second.get())}) {
        j->Left()->Stats_ = RowsWithKeyNdv(400000, "lk", 400000);
        j->Right()->Stats_ = RowsWithKeyNdv(40000, "rk", 10000);
    }

    uint32_t nextId = 1;
    AttachRuntimeFilters(first, nextId);
    AttachRuntimeFilters(second, nextId);
    EXPECT_EQ(static_cast<TJoinOperator*>(first.get())->RuntimeFilter()->Id, 1u);
    EXPECT_EQ(static_cast<TJoinOperator*>(second.get())->RuntimeFilter()->Id, 2u);
}

class SemiAntiOrientation : public testing::TestWithParam<EJoinType> {};

INSTANTIATE_TEST_SUITE_P(
    Types,
    SemiAntiOrientation,
    testing::Values(EJoinType::LeftSemi, EJoinType::LeftAnti),
    [](const testing::TestParamInfo<EJoinType>& info) {
        return info.param == EJoinType::LeftAnti ? "Anti" : "Semi";
    });

TEST_P(SemiAntiOrientation, BuildRightMatchesBuildLeft) {
    EXPECT_EQ(RunSemiAnti(GetParam(), EJoinBuildSide::Right),
              ExpectedSemiAnti(GetParam()));
    EXPECT_EQ(RunSemiAnti(GetParam(), EJoinBuildSide::Auto),
              ExpectedSemiAnti(GetParam()));
}

TEST_P(SemiAntiOrientation, NullKeysNeverMatchInEitherOrientation) {
    const std::vector<TOut2> expected = GetParam() == EJoinType::LeftAnti
        ? std::vector<TOut2>{{0, 20}, {0, 40}}
        : std::vector<TOut2>{{1, 10}, {1, 30}};
    for (auto buildSide : {EJoinBuildSide::Auto, EJoinBuildSide::Right}) {
        EXPECT_EQ(RunSemiAnti(GetParam(), buildSide, true), expected);
    }
}

TEST_P(SemiAntiOrientation, NullEqualKeysMatchInEitherOrientation) {
    const std::vector<TOut2> expected = GetParam() == EJoinType::LeftSemi
        ? std::vector<TOut2>{{0, 20}, {0, 40}, {1, 10}, {1, 30}}
        : std::vector<TOut2>{};
    for (auto buildSide : {EJoinBuildSide::Auto, EJoinBuildSide::Right}) {
        EXPECT_EQ(RunSemiAnti(GetParam(), buildSide, true, true), expected);
    }
}

// Only right-build can stream the left output.
TEST(ChooseJoinBuildSide, SemiAntiTakesRightBuildOnly) {
    const uint64_t small = 1000;
    const uint64_t big = static_cast<uint64_t>(std::ceil(small * JoinAsymmetryRatio)) + 1;

    for (auto type : {EJoinType::LeftSemi, EJoinType::LeftAnti}) {
        auto join = JoinOfType(type);
        ASSERT_TRUE(join);
        auto* j = static_cast<TJoinOperator*>(join.get());

        j->Left()->Stats_ = Rows(big);
        j->Right()->Stats_ = Rows(small);
        EXPECT_EQ(ChooseJoinBuildSide(*j), EJoinBuildSide::Right);

        j->Left()->Stats_ = Rows(small);
        j->Right()->Stats_ = Rows(big);
        EXPECT_EQ(ChooseJoinBuildSide(*j), EJoinBuildSide::Auto);
    }
}

// A residual predicate needs right rows, which key-only build discards.
TEST(ChooseJoinBuildSide, SemiAntiWithResidualStaysAuto) {
    auto join = JoinOfType(EJoinType::LeftSemi);
    ASSERT_TRUE(join);
    auto* j = static_cast<TJoinOperator*>(join.get());
    j->Left()->Stats_ = Rows(1000000);
    j->Right()->Stats_ = Rows(1);
    ASSERT_EQ(ChooseJoinBuildSide(*j), EJoinBuildSide::Right);

    j->MutableFilter() = std::make_shared<TBinaryExpr>(
        NQumir::TLocation{}, TOperator("!="),
        std::make_shared<TIdentExpr>(NQumir::TLocation{}, "lv"),
        std::make_shared<TIdentExpr>(NQumir::TLocation{}, "rv"));
    EXPECT_EQ(ChooseJoinBuildSide(*j), EJoinBuildSide::Auto);
}

// Decision reads JoinAsymmetryRatio so it tracks the constant rather than a
// hardcoded ratio: size the larger side just past the threshold.
TEST(ChooseJoinBuildSide, PicksSmallerSideWhenRatioMet) {
    const uint64_t small = 1000;
    const uint64_t big = static_cast<uint64_t>(std::ceil(small * JoinAsymmetryRatio)) + 1;

    auto join = InnerJoin();
    ASSERT_TRUE(join);
    auto* j = static_cast<TJoinOperator*>(join.get());

    j->Left()->Stats_ = Rows(big);
    j->Right()->Stats_ = Rows(small);
    EXPECT_EQ(ChooseJoinBuildSide(*j), EJoinBuildSide::Right);

    j->Left()->Stats_ = Rows(small);
    j->Right()->Stats_ = Rows(big);
    EXPECT_EQ(ChooseJoinBuildSide(*j), EJoinBuildSide::Left);
}

// An outer join builds its smaller input: the preserved one with matched
// marks, or the null-supplying one that pads unmatched stream rows. The general
// path stores both inputs, so neither side needs a size margin.
TEST(ChooseJoinBuildSide, OuterBuildsTheSmallerSide) {
    for (auto type : {EJoinType::Left, EJoinType::Right}) {
        SCOPED_TRACE(JoinTypeName(type));
        auto join = JoinOfType(type);
        ASSERT_TRUE(join);
        auto* j = static_cast<TJoinOperator*>(join.get());
        const bool isLeft = type == EJoinType::Left;
        const auto& preserved = isLeft ? j->Left() : j->Right();
        const auto& other = isLeft ? j->Right() : j->Left();
        const auto preservedSide =
            isLeft ? EJoinBuildSide::Left : EJoinBuildSide::Right;
        const auto otherSide =
            isLeft ? EJoinBuildSide::Right : EJoinBuildSide::Left;

        preserved->Stats_ = Rows(1000);
        other->Stats_ = Rows(1000);
        EXPECT_EQ(ChooseJoinBuildSide(*j), preservedSide);

        other->Stats_ = Rows(999);
        EXPECT_EQ(ChooseJoinBuildSide(*j), otherSide)
            << "the null-supplying side is smaller";

        other->Stats_ = nullptr;
        EXPECT_EQ(ChooseJoinBuildSide(*j), EJoinBuildSide::Auto);
    }

    auto full = JoinOfType(EJoinType::Full);
    ASSERT_TRUE(full);
    auto* f = static_cast<TJoinOperator*>(full.get());
    f->Left()->Stats_ = Rows(1);
    f->Right()->Stats_ = Rows(1000);
    EXPECT_EQ(ChooseJoinBuildSide(*f), EJoinBuildSide::Auto);
}

// The general path finds unmatched rows by key alone and the row join table
// runs no residual, so a residual predicate needs the preserved build side
// whatever the sizes are.
TEST(ChooseJoinBuildSide, OuterWithResidualBuildsThePreservedSide) {
    for (auto type : {EJoinType::Left, EJoinType::Right}) {
        SCOPED_TRACE(JoinTypeName(type));
        auto join = JoinOfType(type);
        ASSERT_TRUE(join);
        auto* j = static_cast<TJoinOperator*>(join.get());
        const bool isLeft = type == EJoinType::Left;
        (isLeft ? j->Left() : j->Right())->Stats_ = Rows(1000);
        (isLeft ? j->Right() : j->Left())->Stats_ = Rows(10);
        ASSERT_EQ(ChooseJoinBuildSide(*j),
            isLeft ? EJoinBuildSide::Right : EJoinBuildSide::Left);

        j->MutableFilter() = std::make_shared<TBinaryExpr>(
            NQumir::TLocation{}, "<"_op,
            std::make_shared<TIdentExpr>(NQumir::TLocation{}, "lv"),
            std::make_shared<TIdentExpr>(NQumir::TLocation{}, "rv"));
        EXPECT_EQ(ChooseJoinBuildSide(*j),
            isLeft ? EJoinBuildSide::Left : EJoinBuildSide::Right);

        (isLeft ? j->Left() : j->Right())->Stats_ = nullptr;
        EXPECT_EQ(ChooseJoinBuildSide(*j),
            isLeft ? EJoinBuildSide::Left : EJoinBuildSide::Right)
            << "the choice must not depend on statistics";
    }
}

// A runtime filter already makes the probe input wait for the preserved side,
// so streaming costs no extra wait whatever the sizes are.
TEST(ChooseJoinBuildSide, OuterWithRuntimeFilterBuildsItsProducerSide) {
    auto join = JoinOfType(EJoinType::Left);
    ASSERT_TRUE(join);
    auto* j = static_cast<TJoinOperator*>(join.get());
    j->Left()->Stats_ = Rows(1000);
    j->Right()->Stats_ = Rows(10);
    ASSERT_EQ(ChooseJoinBuildSide(*j), EJoinBuildSide::Right);

    j->MutableRuntimeFilter() = TRuntimeFilterSpec{
        .Id = 7, .BuildSide = EJoinFilterSide::Left};
    EXPECT_EQ(ChooseJoinBuildSide(*j), EJoinBuildSide::Left);
}

TEST(ChooseJoinBuildSide, AutoWhenBalancedOrStatsMissing) {
    auto join = InnerJoin();
    ASSERT_TRUE(join);
    auto* j = static_cast<TJoinOperator*>(join.get());

    // Equal sizes: ratio (>1) is never met.
    j->Left()->Stats_ = Rows(1000);
    j->Right()->Stats_ = Rows(1000);
    EXPECT_EQ(ChooseJoinBuildSide(*j), EJoinBuildSide::Auto);

    // Missing stats fall back to the adaptive symmetric path.
    j->Right()->Stats_ = nullptr;
    EXPECT_EQ(ChooseJoinBuildSide(*j), EJoinBuildSide::Auto);
}

// The row join table needs a fixed build side, so an inner equi-join on it
// builds its smaller input at any ratio. Missing estimates and a residual,
// which runs on the symmetric table, keep the adaptive build-up.
TEST(ChooseJoinBuildSide, RowTableBuildsTheSmallerSideAtAnyRatio) {
    const auto row = NScheduler::EJoinTableKind::Row;
    auto join = InnerJoin();
    ASSERT_TRUE(join);
    auto* j = static_cast<TJoinOperator*>(join.get());

    j->Left()->Stats_ = Rows(1200);
    j->Right()->Stats_ = Rows(1000);
    EXPECT_EQ(ChooseJoinBuildSide(*j, row), EJoinBuildSide::Right);
    EXPECT_EQ(ChooseJoinBuildSide(*j), EJoinBuildSide::Auto)
        << "the symmetric table keeps its ratio";

    j->Left()->Stats_ = Rows(999);
    EXPECT_EQ(ChooseJoinBuildSide(*j, row), EJoinBuildSide::Left);

    j->Left()->Stats_ = Rows(1000);
    EXPECT_EQ(ChooseJoinBuildSide(*j, row), EJoinBuildSide::Right);

    j->Right()->Stats_ = nullptr;
    EXPECT_EQ(ChooseJoinBuildSide(*j, row), EJoinBuildSide::Auto);

    j->Right()->Stats_ = Rows(1200);
    j->MutableFilter() = std::make_shared<TBinaryExpr>(
        NQumir::TLocation{}, "<"_op,
        std::make_shared<TIdentExpr>(NQumir::TLocation{}, "lv"),
        std::make_shared<TIdentExpr>(NQumir::TLocation{}, "rv"));
    EXPECT_EQ(ChooseJoinBuildSide(*j, row), EJoinBuildSide::Auto);
}

namespace {

// One input row of a join side: a nullable key and a payload.
struct TKeyVal {
    std::optional<int64_t> Key;
    int64_t Val = 0;
};

using TSideRows = std::vector<std::vector<TKeyVal>>;

// One output row of a LEFT/RIGHT join; a NULL column reads as nullopt.
struct TOuterRow {
    std::optional<int64_t> Lk, Lv, Rk, Rv;
    auto operator<=>(const TOuterRow&) const = default;
};

std::vector<TOuterRow> Sorted(std::vector<TOuterRow> rows) {
    std::sort(rows.begin(), rows.end());
    return rows;
}

// Serves the batches of one join input and counts how many of them the
// processor has taken and how many it has given back.
class TSideInput {
public:
    // `stalls` NO_DATA answers come before every batch and before EOF.
    explicit TSideInput(const TSideRows& batches, int stalls = 0)
        : Stalls_(stalls)
        , StallsLeft_(stalls)
    {
        for (const auto& rows : batches) {
            auto& batch = Batches_.emplace_back();
            batch.KeyMask.assign((rows.size() + 7) / 8, 0);
            for (size_t i = 0; i < rows.size(); ++i) {
                batch.Keys.push_back(rows[i].Key.value_or(0));
                batch.Vals.push_back(rows[i].Val);
                if (rows[i].Key) {
                    batch.KeyMask[i / 8] |= static_cast<uint8_t>(1u << (i % 8));
                }
            }
            batch.Columns = {
                TColumn{.Data = reinterpret_cast<char*>(batch.Keys.data()),
                        .Mask = batch.KeyMask.data()},
                TColumn{.Data = reinterpret_cast<char*>(batch.Vals.data())}};
        }
    }
    TSideInput(const TSideInput&) = delete;
    TSideInput& operator=(const TSideInput&) = delete;

    EJoinFetchResult Fetch(TRowSet& rowSet) {
        if (StallsLeft_ > 0) {
            --StallsLeft_;
            return EJoinFetchResult::NO_DATA;
        }
        StallsLeft_ = Stalls_;
        if (Fetched_ == Batches_.size()) {
            return EJoinFetchResult::FINISHED;
        }
        auto& batch = Batches_[Fetched_++];
        rowSet = TRowSet{
            .Columns = batch.Columns.data(),
            .ColumnCount = 2,
            .RowCount = static_cast<int64_t>(batch.Keys.size()),
            .Destroy = [](TRowSet* released) {
                ++static_cast<TSideInput*>(released->Private)->Released_;
            },
            .Private = this,
            .RefCount = 1};
        return EJoinFetchResult::OK;
    }

    size_t Fetched() const { return Fetched_; }
    size_t Released() const { return Released_; }

private:
    struct TBatch {
        std::vector<int64_t> Keys;
        std::vector<int64_t> Vals;
        std::vector<uint8_t> KeyMask;
        std::array<TColumn, 2> Columns;
    };

    // A deque keeps the column arrays of earlier batches in place.
    std::deque<TBatch> Batches_;
    int Stalls_ = 0;
    int StallsLeft_ = 0;
    size_t Fetched_ = 0;
    size_t Released_ = 0;
};

// LEFT/RIGHT joins of (lk, lv) with (rk, rv) on nullable lk = rk, run directly
// on the join processor.
class OuterStreamJoin : public testing::Test {
protected:
    TJoinKernels Compile(EJoinType type, const TExprPtr& residual = nullptr) {
        auto spec = NKernel::BuildJoinKernelSpec(
            static_cast<TStructType&>(*LeftType_),
            static_cast<TStructType&>(*RightType_),
            {{"lk", "rk", false}}, type, residual);
        return Compiler_.CompileJoin(spec);
    }

    static void AppendRows(const TRowSet& out, std::vector<TOuterRow>& rows) {
        auto cell = [&](int column, int64_t row) -> std::optional<int64_t> {
            const auto& col = out.Columns[column];
            if (col.Mask && !((col.Mask[row / 8] >> (row % 8)) & 1)) {
                return std::nullopt;
            }
            return reinterpret_cast<const int64_t*>(col.Data)[row];
        };
        ASSERT_EQ(out.ColumnCount, 4);
        for (int64_t i = 0; i < out.RowCount; ++i) {
            rows.push_back({cell(0, i), cell(1, i), cell(2, i), cell(3, i)});
        }
    }

    // Runs the join to completion and returns its rows in sorted order.
    std::vector<TOuterRow> Run(
        EJoinType type, EJoinBuildSide buildSide,
        TSideInput& left, TSideInput& right,
        const TExprPtr& residual = nullptr)
    {
        TInnerJoinProcessor processor(Compile(type, residual), type, buildSide);
        auto fetchLeft = [&](TRowSet& rowSet) { return left.Fetch(rowSet); };
        auto fetchRight = [&](TRowSet& rowSet) { return right.Fetch(rowSet); };
        std::vector<TOuterRow> rows;
        TRowSet out{};
        for (;;) {
            auto result = processor.Process(fetchLeft, fetchRight, out);
            if (result == EJoinProcessorResult::FINISHED) {
                break;
            }
            if (result == EJoinProcessorResult::OK) {
                AppendRows(out, rows);
                Release(&out);
            }
        }
        return Sorted(std::move(rows));
    }

    std::vector<TOuterRow> Run(
        EJoinType type, EJoinBuildSide buildSide,
        const TSideRows& leftRows, const TSideRows& rightRows)
    {
        TSideInput left(leftRows);
        TSideInput right(rightRows);
        return Run(type, buildSide, left, right);
    }

    // Runs the join on the row join table, which builds the null-supplying
    // side and streams the preserved one.
    std::vector<TOuterRow> RunRowTable(
        EJoinType type, const TSideRows& leftRows, const TSideRows& rightRows)
    {
        auto spec = NKernel::BuildJoinKernelSpec(
            static_cast<TStructType&>(*LeftType_),
            static_cast<TStructType&>(*RightType_),
            {{"lk", "rk", false}}, type);
        const auto buildSide = type == EJoinType::Left
            ? EJoinBuildSide::Right
            : EJoinBuildSide::Left;
        TRowJoinProcessor processor(
            Compiler_.CompileRowJoin(spec, buildSide == EJoinBuildSide::Left), buildSide);
        TSideInput left(leftRows);
        TSideInput right(rightRows);
        auto& probe = buildSide == EJoinBuildSide::Left ? right : left;
        auto fetchLeft = [&](TRowSet& rowSet) { return left.Fetch(rowSet); };
        auto fetchRight = [&](TRowSet& rowSet) { return right.Fetch(rowSet); };
        std::vector<TOuterRow> rows;
        TRowSet out{};
        for (;;) {
            auto result = processor.Process(fetchLeft, fetchRight, out);
            EXPECT_EQ(probe.Released(), probe.Fetched())
                << "a preserved batch outlived its probe call";
            if (result == EJoinProcessorResult::FINISHED) {
                break;
            }
            if (result == EJoinProcessorResult::OK) {
                AppendRows(out, rows);
                Release(&out);
            }
        }
        return Sorted(std::move(rows));
    }

    // The streaming paths must agree with the general (keep both sides) path.
    void ExpectStreamingMatchesGeneral(
        const TSideRows& left, const TSideRows& right)
    {
        const auto leftJoin =
            Run(EJoinType::Left, EJoinBuildSide::Auto, left, right);
        const auto rightJoin =
            Run(EJoinType::Right, EJoinBuildSide::Auto, left, right);
        // An outer join returns at least one row per preserved row.
        EXPECT_GE(leftJoin.size(), RowCount(left));
        EXPECT_GE(rightJoin.size(), RowCount(right));
        EXPECT_EQ(Run(EJoinType::Left, EJoinBuildSide::Left, left, right),
                  leftJoin);
        EXPECT_EQ(Run(EJoinType::Right, EJoinBuildSide::Right, left, right),
                  rightJoin);
        EXPECT_EQ(RunRowTable(EJoinType::Left, left, right), leftJoin);
        EXPECT_EQ(RunRowTable(EJoinType::Right, left, right), rightJoin);
    }

    static size_t RowCount(const TSideRows& batches) {
        size_t rows = 0;
        for (const auto& batch : batches) {
            rows += batch.size();
        }
        return rows;
    }

    TKernelCompiler Compiler_;
    TTypePtr LeftType_ = std::make_shared<TStructType>(
        std::vector<std::pair<std::string, TTypePtr>>{
            {"lk", std::make_shared<TNullable>(I64Type())}, {"lv", I64Type()}});
    TTypePtr RightType_ = std::make_shared<TStructType>(
        std::vector<std::pair<std::string, TTypePtr>>{
            {"rk", std::make_shared<TNullable>(I64Type())}, {"rv", I64Type()}});
};

} // namespace

// The SQL-level results are fixed by the outer_join regression suite. This runs
// the same inputs through every implementation of the outer join.
TEST_F(OuterStreamJoin, MatchesGeneralPathOnTheSameInputs) {
    const TSideRows duplicates = {
        {{1, 10}, {1, 11}, {2, 20}}, {{3, 40}, {1, 12}}};
    const TSideRows moreDuplicates = {
        {{1, 100}, {1, 101}}, {{4, 400}, {3, 500}, {3, 501}}};
    const TSideRows nullKeys = {
        {{1, 10}, {std::nullopt, 20}}, {{std::nullopt, 30}}};
    const TSideRows moreNullKeys = {
        {{std::nullopt, 100}, {1, 200}}, {{std::nullopt, 300}}};
    const TSideRows disjoint = {{{7, 700}}, {{8, 800}, {9, 900}}};

    ExpectStreamingMatchesGeneral(duplicates, moreDuplicates);
    ExpectStreamingMatchesGeneral(nullKeys, moreNullKeys);
    ExpectStreamingMatchesGeneral(duplicates, duplicates);
    ExpectStreamingMatchesGeneral(duplicates, disjoint);
    ExpectStreamingMatchesGeneral({}, duplicates);
    ExpectStreamingMatchesGeneral(duplicates, {});
    ExpectStreamingMatchesGeneral({}, {});
}

// The probe input can be at EOF before the build has delivered anything, and
// either input can run dry in between; neither changes the result.
TEST_F(OuterStreamJoin, InputStallsAndEofOrderDoNotChangeTheResult) {
    const TSideRows build = {{{1, 10}, {2, 20}}, {{3, 30}}};
    const TSideRows probe = {{{2, 200}}, {{2, 201}, {9, 900}}};
    const auto expected = Run(
        EJoinType::Left, EJoinBuildSide::Left, build, probe);
    ASSERT_EQ(expected.size(), 4u);

    for (const auto& [buildStalls, probeStalls] :
            {std::pair{3, 0}, std::pair{0, 3}, std::pair{2, 2}}) {
        SCOPED_TRACE(buildStalls);
        SCOPED_TRACE(probeStalls);
        TSideInput left(build, buildStalls);
        TSideInput right(probe, probeStalls);
        EXPECT_EQ(Run(EJoinType::Left, EJoinBuildSide::Left, left, right),
                  expected);
    }

    TSideInput slowBuild(build, 3);
    TSideInput finishedProbe(TSideRows{});
    EXPECT_EQ(
        Run(EJoinType::Left, EJoinBuildSide::Left, slowBuild, finishedProbe),
        Sorted({
            {1, 10, std::nullopt, std::nullopt},
            {2, 20, std::nullopt, std::nullopt},
            {3, 30, std::nullopt, std::nullopt}}));
}

// Unmatched rows leave in output-sized batches, so the task can block on its
// output in between. The stream side is given back batch by batch, while the
// build batches behind the pending rows stay alive.
TEST_F(OuterStreamJoin, EmitsUnmatchedRowsInBatchesFromLiveBuildRows) {
    const int64_t buildRows = 3 * JoinOutputBatchRows;
    TSideRows build(3);
    for (int64_t key = 0; key < buildRows; ++key) {
        build[key % 3].push_back({key, key * 10});
    }
    const TSideRows probe = {{{0, 1}, {1, 2}}, {{1, 3}, {buildRows, 4}}};
    TSideInput left(build);
    TSideInput right(probe);

    int64_t matched = 0;
    std::vector<int64_t> unmatchedKeys;
    {
        TInnerJoinProcessor processor(
            Compile(EJoinType::Left), EJoinType::Left, EJoinBuildSide::Left);
        auto fetchLeft = [&](TRowSet& rowSet) { return left.Fetch(rowSet); };
        auto fetchRight = [&](TRowSet& rowSet) { return right.Fetch(rowSet); };
        TRowSet out{};
        for (;;) {
            if (processor.RequiredInputSide() == EJoinBuildSide::Left) {
                EXPECT_EQ(right.Fetched(), 0u) << "probe read during the build";
            }
            auto result = processor.Process(fetchLeft, fetchRight, out);
            EXPECT_EQ(right.Released(), right.Fetched())
                << "a probe batch outlived its stream call";
            EXPECT_EQ(left.Released(), 0u);
            if (result == EJoinProcessorResult::FINISHED) {
                break;
            }
            if (result != EJoinProcessorResult::OK) {
                continue;
            }
            EXPECT_LE(out.RowCount, JoinOutputBatchRows);
            std::vector<TOuterRow> rows;
            AppendRows(out, rows);
            Release(&out);
            for (const auto& row : rows) {
                ASSERT_TRUE(row.Lk.has_value());
                EXPECT_EQ(row.Lv, *row.Lk * 10);
                if (row.Rk) {
                    ++matched;
                } else {
                    unmatchedKeys.push_back(*row.Lk);
                }
            }
        }
        EXPECT_EQ(left.Fetched(), 3u);
        EXPECT_EQ(right.Fetched(), 2u);
    }
    EXPECT_EQ(left.Released(), 3u);

    EXPECT_EQ(matched, 3);
    std::sort(unmatchedKeys.begin(), unmatchedKeys.end());
    std::vector<int64_t> expectedUnmatched;
    for (int64_t key = 2; key < buildRows; ++key) {
        expectedUnmatched.push_back(key);
    }
    EXPECT_EQ(unmatchedKeys, expectedUnmatched);
}

// This processor streams an outer join only against its preserved side; any
// other request runs the general path and still returns the right rows.
TEST_F(OuterStreamJoin, NonPreservedBuildSideFallsBackToGeneralPath) {
    const TSideRows left = {{{1, 10}, {2, 20}}};
    const TSideRows right = {{{2, 200}, {3, 300}}};

    EXPECT_EQ(Run(EJoinType::Left, EJoinBuildSide::Right, left, right),
        Sorted({{1, 10, std::nullopt, std::nullopt}, {2, 20, 2, 200}}));
    EXPECT_EQ(Run(EJoinType::Right, EJoinBuildSide::Left, left, right),
        Sorted({{2, 20, 2, 200}, {std::nullopt, std::nullopt, 3, 300}}));
    EXPECT_EQ(Run(EJoinType::Full, EJoinBuildSide::Left, left, right),
        Sorted({
            {1, 10, std::nullopt, std::nullopt},
            {2, 20, 2, 200},
            {std::nullopt, std::nullopt, 3, 300}}));
}

// A FULL join stores both inputs, and the later row of a pair finds the
// earlier one. Both are marked when the residual accepts the pair, so the
// arrival order cannot change which rows come out NULL-extended.
TEST_F(OuterStreamJoin, FullResidualMarksBothSidesInAnyArrivalOrder) {
    const TSideRows leftRows = {
        {{1, 10}, {1, 30}, {2, 20}}, {{3, 40}, {std::nullopt, 50}, {1, 5}}};
    const TSideRows rightRows = {
        {{1, 15}, {1, 5}}, {{2, 25}, {std::nullopt, 60}, {4, 1}, {1, 3}}};
    const auto residual = std::make_shared<TBinaryExpr>(
        NQumir::TLocation{}, "<"_op,
        std::make_shared<TIdentExpr>(NQumir::TLocation{}, "lv"),
        std::make_shared<TIdentExpr>(NQumir::TLocation{}, "rv"));

    std::vector<TOuterRow> expected;
    std::vector<bool> rightMatched;
    for (const auto& batch : rightRows) {
        rightMatched.resize(rightMatched.size() + batch.size());
    }
    for (const auto& lb : leftRows) {
        for (const auto& l : lb) {
            bool matched = false;
            size_t r = 0;
            for (const auto& rb : rightRows) {
                for (const auto& row : rb) {
                    if (l.Key && l.Key == row.Key && l.Val < row.Val) {
                        expected.push_back({l.Key, l.Val, row.Key, row.Val});
                        matched = true;
                        rightMatched[r] = true;
                    }
                    ++r;
                }
            }
            if (!matched) {
                expected.push_back({l.Key, l.Val, std::nullopt, std::nullopt});
            }
        }
    }
    size_t r = 0;
    for (const auto& rb : rightRows) {
        for (const auto& row : rb) {
            if (!rightMatched[r++]) {
                expected.push_back({std::nullopt, std::nullopt, row.Key, row.Val});
            }
        }
    }
    expected = Sorted(std::move(expected));
    ASSERT_EQ(expected.size(), 10u);

    for (const auto& [leftStalls, rightStalls] :
            {std::pair{0, 0}, std::pair{3, 0}, std::pair{0, 3}, std::pair{1, 2}}) {
        SCOPED_TRACE(leftStalls);
        SCOPED_TRACE(rightStalls);
        TSideInput left(leftRows, leftStalls);
        TSideInput right(rightRows, rightStalls);
        EXPECT_EQ(
            Run(EJoinType::Full, EJoinBuildSide::Auto, left, right, residual),
            expected);
    }
}

// Each join partition builds its own slice of the preserved side and marks the
// rows its probe matches. Together the partitions must return every unmatched
// row exactly once, as the general path does.
TEST(OuterStreamJoinPlan, MatchesGeneralPathAcrossPartitions) {
    // Keys repeat on both sides; keys below 40 exist only on the left and keys
    // above 96 only on the right.
    std::vector<int64_t> lk, lv, rk, rv;
    for (int64_t i = 0; i < 400; ++i) {
        lk.push_back(i % 97);
        lv.push_back(i);
    }
    for (int64_t i = 0; i < 600; ++i) {
        rk.push_back((i * 7) % 150 + 40);
        rv.push_back(1000 + i);
    }

    using TRow = std::tuple<std::optional<int64_t>, std::optional<int64_t>,
        std::optional<int64_t>, std::optional<int64_t>>;
    auto expected = [&](EJoinType type) {
        std::vector<TRow> rows;
        std::vector<bool> rightMatched(rk.size());
        for (size_t l = 0; l < lk.size(); ++l) {
            bool matched = false;
            for (size_t r = 0; r < rk.size(); ++r) {
                if (lk[l] == rk[r]) {
                    rows.emplace_back(lk[l], lv[l], rk[r], rv[r]);
                    matched = true;
                    rightMatched[r] = true;
                }
            }
            if (!matched && type == EJoinType::Left) {
                rows.emplace_back(lk[l], lv[l], std::nullopt, std::nullopt);
            }
        }
        for (size_t r = 0; type == EJoinType::Right && r < rk.size(); ++r) {
            if (!rightMatched[r]) {
                rows.emplace_back(std::nullopt, std::nullopt, rk[r], rv[r]);
            }
        }
        std::sort(rows.begin(), rows.end());
        return rows;
    };

    auto run = [&](EJoinType type, bool streaming,
                   NScheduler::EExecutionMode mode) {
        std::vector<TColumn> lc0, lc1, rc0, rc1, rc2;
        TMockSource left({"lk", "lv"}, {
            KeyValBatch(lk.data(), lv.data(), 200, lc0),
            KeyValBatch(lk.data() + 200, lv.data() + 200, 200, lc1)});
        TMockSource right({"rk", "rv"}, {
            KeyValBatch(rk.data(), rv.data(), 200, rc0),
            KeyValBatch(rk.data() + 200, rv.data() + 200, 200, rc1),
            KeyValBatch(rk.data() + 400, rv.data() + 400, 200, rc2)});
        auto join = std::make_shared<TJoinOperator>(
            std::make_shared<TSourceOperator>(left, "L"),
            std::make_shared<TSourceOperator>(right, "R"),
            std::vector<TJoinKey>{{"lk", "rk"}}, type, nullptr);
        TOperatorPtr root = join;
        AnnotateTypes(root);

        const bool isLeft = type == EJoinType::Left;
        if (streaming) {
            // Only the estimates steer the plan: make the preserved side the
            // smaller one.
            (isLeft ? join->Left() : join->Right())->Stats_ = Rows(1);
            (isLeft ? join->Right() : join->Left())->Stats_ = Rows(2);
        }
        const auto preserved =
            isLeft ? EJoinBuildSide::Left : EJoinBuildSide::Right;
        EXPECT_EQ(ChooseJoinBuildSide(*join),
            streaming ? preserved : EJoinBuildSide::Auto);

        NScheduler::TSettings settings;
        settings.Scheduler.Mode = mode;
        settings.Scheduler.WorkerCount = mode
                == NScheduler::EExecutionMode::SingleThreadedScheduler
            ? 1 : 4;
        settings.HashShuffle.PartitionCount = 4;
        settings.HashShuffle.MaxPartitionCount = 4;
        auto plan = RunPlan(root, settings);

        std::vector<TRow> got;
        TRowSet out{};
        while (plan->Next(out)) {
            auto cell = [&](int column, int64_t row) -> std::optional<int64_t> {
                const auto& col = out.Columns[column];
                if (col.Mask && !((col.Mask[row / 8] >> (row % 8)) & 1)) {
                    return std::nullopt;
                }
                return reinterpret_cast<const int64_t*>(col.Data)[row];
            };
            for (int64_t i = 0; i < out.RowCount; ++i) {
                if (!out.Selection || out.Selection[i]) {
                    got.emplace_back(
                        cell(0, i), cell(1, i), cell(2, i), cell(3, i));
                }
            }
            Release(&out);
        }
        std::sort(got.begin(), got.end());
        return got;
    };

    for (auto type : {EJoinType::Left, EJoinType::Right}) {
        for (auto mode : {NScheduler::EExecutionMode::SingleThreadedScheduler,
                          NScheduler::EExecutionMode::ThreadedScheduler}) {
            SCOPED_TRACE(JoinTypeName(type));
            SCOPED_TRACE(static_cast<int>(mode));
            const auto want = expected(type);
            EXPECT_EQ(run(type, /*streaming=*/true, mode), want);
            EXPECT_EQ(run(type, /*streaming=*/false, mode), want);
        }
    }
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    NQumir::NCodeGen::TLLVMInitializer initializer;
    return RUN_ALL_TESTS();
}
