#include <gtest/gtest.h>

#include <qdb/exec/join_exec.h>
#include <qdb/exec/planner_helpers.h>
#include <qdb/io/io.h>
#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/operator.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/union.h>
#include <qdb/plan/ops/stats.h>
#include <qdb/plan/types/nullable.h>
#include <qdb/plan/passes/runtime_filters.h>

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/parser/type.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace NQdb;
using namespace NQumir::NAst;

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
    EXPECT_EQ(ChooseJoinBuildSide(*j), EJoinBuildSide::Auto);
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

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    NQumir::NCodeGen::TLLVMInitializer initializer;
    return RUN_ALL_TESTS();
}
