#include <gtest/gtest.h>

#include <qdb/exec/join_exec.h>
#include <qdb/exec/planner_helpers.h>
#include <qdb/io/io.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/operator.h>
#include <qdb/plan/ops/stats.h>
#include <qdb/plan/types/nullable.h>
#include <qdb/plan/passes/runtime_filters.h>

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/parser/type.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
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
    std::unordered_set<std::string> ComputeReferencedColumns() const override { return {}; }
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

    j->Left()->Stats_ = RowsWithKeyNdv(50000, "lk", 100000, true);
    EXPECT_FALSE(AttachTo(join).has_value())
        << "clamping a pre-filter key domain to output rows is not exact NDV";
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
    EXPECT_FALSE(AttachTo(inner).has_value()) << "over the key cap";

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
