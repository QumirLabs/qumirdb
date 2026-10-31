#include <gtest/gtest.h>
#include "mock_source.h"

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>
#include <qumir/parser/type.h>

#include "plan_runner.h"
#include <qdb/io/io.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/passes/column_pruning.h>
#include <qdb/plan/passes/typing.h>
#include <qdb/sexp/parser.h>

#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <vector>

using namespace NQdb;
using namespace NQdb::NSexp;
using namespace NQumir::NAst::NCore;
using namespace NQumir::NAst;

namespace {


std::unique_ptr<TTestRuntime> Plan(
    const std::string& sexp,
    ISource& src,
    NScheduler::TSettings schedulerSettings = {})
{
    TRelParserOptions opts;
    opts.SourceFactory = [&](std::string_view path, NQumir::TLocation) -> TOperatorPtr {
        return std::make_shared<TSourceOperator>(src, std::string(path));
    };
    TParser parser;
    for (auto& [name, fn] : MakeRelParsers(std::move(opts))) parser.NodeParsers[name] = std::move(fn);
    std::istringstream in(sexp);
    TTokenStream ts(in);
    auto parsed = parser.Parse(ts);
    if (!parsed) throw std::runtime_error(parsed.error().ToString());
    auto root = std::static_pointer_cast<IOperator>(*parsed);
    AnnotateTypes(root);
    ApplyColumnPruning(root);
    return RunPlan(root, schedulerSettings);
}

} // namespace

TEST(AggregateF64, SumMinMaxOverFloatColumn) {
    // key (i64), v (f64). Exact f64 values -> sums are exact (no rounding).
    std::array<int64_t, 4> k = {1, 2, 1, 2};
    std::array<double, 4> v = {1.5, 10.0, 2.5, 0.25};
    std::array<TColumn, 2> cols = {
        TColumn{.Data = reinterpret_cast<char*>(k.data())},
        TColumn{.Data = reinterpret_cast<char*>(v.data())},
    };
    TRowSet batch{.Columns = cols.data(), .ColumnCount = 2, .RowCount = 4, .RefCount = 1};
    NQdb::TMockSource src({"k", "v"},
        {std::make_shared<TIntegerType>(TIntegerType::I64), std::make_shared<TFloatType>()},
        {batch});

    auto plan = Plan(
        "(rel aggregate (rel source \"L\") (keys k) "
        "(agg s sum v) (agg mn min v) (agg mx max v))", src);

    // Output: k (i64), s/mn/mx (f64).
    auto* outType = static_cast<TStructType*>(plan->OutputType().get());
    ASSERT_EQ(outType->Fields.size(), 4u);
    EXPECT_TRUE(TMaybeType<TFloatType>(outType->Fields[1].second));

    struct TStats { double s, mn, mx; };
    std::map<int64_t, TStats> got;
    TRowSet out{};
    while (plan->Next(out)) {
        const auto* kc = reinterpret_cast<const int64_t*>(out.Columns[0].Data);
        const auto* s = reinterpret_cast<const double*>(out.Columns[1].Data);
        const auto* mn = reinterpret_cast<const double*>(out.Columns[2].Data);
        const auto* mx = reinterpret_cast<const double*>(out.Columns[3].Data);
        for (int64_t i = 0; i < out.RowCount; ++i) {
            got[kc[i]] = {s[i], mn[i], mx[i]};
        }
        Release(&out);
    }

    ASSERT_EQ(got.size(), 2u);
    EXPECT_DOUBLE_EQ(got[1].s, 4.0);   // 1.5 + 2.5
    EXPECT_DOUBLE_EQ(got[1].mn, 1.5);
    EXPECT_DOUBLE_EQ(got[1].mx, 2.5);
    EXPECT_DOUBLE_EQ(got[2].s, 10.25); // 10.0 + 0.25
    EXPECT_DOUBLE_EQ(got[2].mn, 0.25);
    EXPECT_DOUBLE_EQ(got[2].mx, 10.0);
}

TEST(AggregateF64, MultipleDistinctColumns) {
    // key (i64), a (i64), b (f64). Different aggregates over different columns.
    std::array<int64_t, 4> k = {1, 2, 1, 2};
    std::array<int64_t, 4> a = {10, 5, 30, 7};
    std::array<double, 4> b = {1.5, 100.0, 2.5, 0.25};
    std::array<TColumn, 3> cols = {
        TColumn{.Data = reinterpret_cast<char*>(k.data())},
        TColumn{.Data = reinterpret_cast<char*>(a.data())},
        TColumn{.Data = reinterpret_cast<char*>(b.data())},
    };
    TRowSet batch{.Columns = cols.data(), .ColumnCount = 3, .RowCount = 4, .RefCount = 1};
    NQdb::TMockSource src({"k", "a", "b"},
        {std::make_shared<TIntegerType>(TIntegerType::I64),
         std::make_shared<TIntegerType>(TIntegerType::I64),
         std::make_shared<TFloatType>()},
        {batch});

    auto plan = Plan(
        "(rel aggregate (rel source \"L\") (keys k) "
        "(agg sa sum a) (agg sb sum b) (agg mxa max a) (agg cnt count))", src);

    // Output: k(i64), sa(i64), sb(f64), mxa(i64), cnt(i64).
    auto* outType = static_cast<TStructType*>(plan->OutputType().get());
    ASSERT_EQ(outType->Fields.size(), 5u);
    EXPECT_TRUE(TMaybeType<TIntegerType>(outType->Fields[1].second)); // sa
    EXPECT_TRUE(TMaybeType<TFloatType>(outType->Fields[2].second));   // sb

    struct TRow { int64_t sa; double sb; int64_t mxa; int64_t cnt; };
    std::map<int64_t, TRow> got;
    TRowSet out{};
    while (plan->Next(out)) {
        const auto* kc = reinterpret_cast<const int64_t*>(out.Columns[0].Data);
        const auto* sa = reinterpret_cast<const int64_t*>(out.Columns[1].Data);
        const auto* sb = reinterpret_cast<const double*>(out.Columns[2].Data);
        const auto* mxa = reinterpret_cast<const int64_t*>(out.Columns[3].Data);
        const auto* cnt = reinterpret_cast<const int64_t*>(out.Columns[4].Data);
        for (int64_t i = 0; i < out.RowCount; ++i) got[kc[i]] = {sa[i], sb[i], mxa[i], cnt[i]};
        Release(&out);
    }
    ASSERT_EQ(got.size(), 2u);
    EXPECT_EQ(got[1].sa, 40);        // 10 + 30
    EXPECT_DOUBLE_EQ(got[1].sb, 4.0); // 1.5 + 2.5
    EXPECT_EQ(got[1].mxa, 30);
    EXPECT_EQ(got[1].cnt, 2);
    EXPECT_EQ(got[2].sa, 12);         // 5 + 7
    EXPECT_DOUBLE_EQ(got[2].sb, 100.25);
    EXPECT_EQ(got[2].mxa, 7);
    EXPECT_EQ(got[2].cnt, 2);
}

TEST(AggregateF64, SchedulerUnaryPrefixFeedsAggregate) {
    std::array<int64_t, 4> k = {1, 1, 2, 2};
    std::array<double, 4> v = {0.5, 1.5, 2.0, 0.25};
    std::array<TColumn, 2> cols = {
        TColumn{.Data = reinterpret_cast<char*>(k.data())},
        TColumn{.Data = reinterpret_cast<char*>(v.data())},
    };
    TRowSet batch{
        .Columns = cols.data(),
        .ColumnCount = 2,
        .RowCount = 4,
        .RefCount = 1,
    };
    NQdb::TMockSource src(
        {"k", "v"},
        {std::make_shared<TIntegerType>(TIntegerType::I64),
         std::make_shared<TFloatType>()},
        {batch});
    NScheduler::TSettings settings;
    settings.Scheduler.Mode = NScheduler::EExecutionMode::ThreadedScheduler;
    settings.Scheduler.WorkerCount = 2;

    auto plan = Plan(
        "(rel aggregate "
        "(rel project "
        "(rel filter (rel source \"L\") (> v (: 1.0 f64))) "
        "(k k) (w (* v (: 2.0 f64)))) "
        "(keys k) (agg s sum w))",
        src,
        settings);

    std::map<int64_t, double> got;
    TRowSet out{};
    while (plan->Next(out)) {
        const auto* keys = reinterpret_cast<const int64_t*>(out.Columns[0].Data);
        const auto* sums = reinterpret_cast<const double*>(out.Columns[1].Data);
        for (int64_t i = 0; i < out.RowCount; ++i) {
            got[keys[i]] = sums[i];
        }
        Release(&out);
    }

    ASSERT_EQ(got.size(), 2u);
    EXPECT_DOUBLE_EQ(got[1], 3.0);
    EXPECT_DOUBLE_EQ(got[2], 4.0);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    NQumir::NCodeGen::TLLVMInitializer initializer;
    return RUN_ALL_TESTS();
}

TEST(AggregateF64, Float32ArgumentFeedsF64States) {
    // v is a nullable f32 column; sum/min/max results are f64 of the widened values.
    const float inf = std::numeric_limits<float>::infinity();
    std::array<int64_t, 8> k = {1, 1, 2, 2, 3, 3, 4, 4};
    std::array<float, 8> v = {0.1f, 0.2f, -inf, 5.0f, -0.0f, 7.0f, std::nanf(""), 1.0f};
    std::array<uint8_t, 1> mask = {0b11011111}; // row 5 is NULL
    std::array<TColumn, 2> cols = {
        TColumn{.Data = reinterpret_cast<char*>(k.data())},
        TColumn{.Data = reinterpret_cast<char*>(v.data()), .Mask = mask.data()},
    };
    TRowSet batch{.Columns = cols.data(), .ColumnCount = 2, .RowCount = 8, .RefCount = 1};
    NQdb::TMockSource src({"k", "v"},
        {std::make_shared<TIntegerType>(TIntegerType::I64),
         std::make_shared<TNullable>(std::make_shared<TFloatType>(TFloatType::F32))},
        {batch});

    auto plan = Plan(
        "(rel aggregate (rel source \"L\") (keys k) "
        "(agg s sum v) (agg mn min v) (agg mx max v))", src);

    auto* outType = static_cast<TStructType*>(plan->OutputType().get());
    ASSERT_EQ(outType->Fields.size(), 4u);
    for (size_t i = 1; i < 4; ++i) {
        auto inner = UnwrapNullableType(outType->Fields[i].second);
        auto floating = TMaybeType<TFloatType>(inner);
        ASSERT_TRUE(floating);
        EXPECT_EQ(floating.Cast()->BitWidth(), 64);
    }

    struct TStats { double s, mn, mx; };
    std::map<int64_t, TStats> got;
    TRowSet out{};
    while (plan->Next(out)) {
        const auto* kc = reinterpret_cast<const int64_t*>(out.Columns[0].Data);
        const auto* s = reinterpret_cast<const double*>(out.Columns[1].Data);
        const auto* mn = reinterpret_cast<const double*>(out.Columns[2].Data);
        const auto* mx = reinterpret_cast<const double*>(out.Columns[3].Data);
        for (int64_t i = 0; i < out.RowCount; ++i) {
            got[kc[i]] = {s[i], mn[i], mx[i]};
        }
        Release(&out);
    }

    ASSERT_EQ(got.size(), 4u);
    EXPECT_EQ(got[1].s, static_cast<double>(0.1f) + static_cast<double>(0.2f));
    EXPECT_EQ(got[1].mn, static_cast<double>(0.1f));
    EXPECT_EQ(got[1].mx, static_cast<double>(0.2f));
    EXPECT_EQ(got[2].s, -std::numeric_limits<double>::infinity());
    EXPECT_EQ(got[2].mn, -std::numeric_limits<double>::infinity());
    EXPECT_EQ(got[2].mx, 5.0);
    EXPECT_EQ(got[3].s, -0.0 + 0.0);  // the NULL row is skipped
    EXPECT_EQ(got[3].mn, 0.0);
    EXPECT_EQ(got[3].mx, 0.0);
    EXPECT_TRUE(std::isnan(got[4].s));
}
