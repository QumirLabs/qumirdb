#include <gtest/gtest.h>
#include "mock_source.h"

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>
#include <qumir/parser/type.h>

#include "plan_runner.h"
#include <qdb/exec/runtime_filter.h>
#include <qdb/io/io.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/passes/column_pruning.h>
#include <qdb/plan/passes/typing.h>
#include <qdb/sexp/parser.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

using namespace NQdb;
using namespace NQdb::NSexp;
using namespace NQumir::NAst::NCore;
using namespace NQumir::NAst;

namespace {


TRowSet KeyValBatch(int64_t* keys, int64_t* vals, int64_t rows, std::vector<TColumn>& cols) {
    cols = {TColumn{.Data = reinterpret_cast<char*>(keys)},
            TColumn{.Data = reinterpret_cast<char*>(vals)}};
    return TRowSet{.Columns = cols.data(), .ColumnCount = 2, .RowCount = rows, .RefCount = 1};
}

class TCountingFilterProbe final : public IRuntimeFilterProbe {
public:
    explicit TCountingFilterProbe(std::shared_ptr<IRuntimeFilterProbe> inner)
        : Inner_(std::move(inner))
    {}

    bool MayContain(uint64_t hash) const override {
        Probed.fetch_add(1, std::memory_order_relaxed);
        const bool keep = Inner_->MayContain(hash);
        if (!keep) {
            Rejected.fetch_add(1, std::memory_order_relaxed);
        }
        return keep;
    }

    mutable std::atomic<size_t> Probed{0};
    mutable std::atomic<size_t> Rejected{0};

private:
    std::shared_ptr<IRuntimeFilterProbe> Inner_;
};

class TCountingFilterBindingFactory final : public IRuntimeFilterBindingFactory {
public:
    TCountingFilterBindingFactory(
        size_t maxKeys = TRuntimeFilter::DefaultMaxKeys,
        size_t bloomBytes = TRuntimeFilter::DefaultBloomBytes)
        : MaxKeys_(maxKeys), BloomBytes_(bloomBytes) {}

    TRuntimeFilterBinding Create(uint32_t id, size_t producerCount) override {
        (void)id;
        ++Created;
        ProducerCount = producerCount;
        Filter = std::make_shared<TRuntimeFilter>(MaxKeys_, BloomBytes_);
        Filter->SetProducerCount(producerCount);
        Probe = std::make_shared<TCountingFilterProbe>(Filter);
        return {.Producer = Filter, .Probe = Probe};
    }

    size_t Created = 0;
    size_t ProducerCount = 0;
    std::shared_ptr<TRuntimeFilter> Filter;
    std::shared_ptr<TCountingFilterProbe> Probe;

private:
    size_t MaxKeys_;
    size_t BloomBytes_;
};

// Parses `sexp`, wiring "L" -> left source, anything else -> right source, then
// runs the full logical pipeline + physical planner.
std::unique_ptr<TTestRuntime> PlanJoin(
    const std::string& sexp,
    ISource& left,
    ISource& right,
    NScheduler::TSettings schedulerSettings = {},
    std::shared_ptr<IRuntimeFilterBindingFactory> filterBindings = nullptr)
{
    TRelParserOptions opts;
    opts.SourceFactory = [&](std::string_view path, NQumir::TLocation) -> TOperatorPtr {
        ISource& src = (path == "L") ? left : right;
        return std::make_shared<TSourceOperator>(src, std::string(path));
    };
    TParser parser;
    for (auto& [name, fn] : MakeRelParsers(std::move(opts))) {
        parser.NodeParsers[name] = std::move(fn);
    }
    std::istringstream in(sexp);
    TTokenStream ts(in);
    auto parsed = parser.Parse(ts);
    if (!parsed) throw std::runtime_error(parsed.error().ToString());
    auto root = std::static_pointer_cast<IOperator>(*parsed);
    AnnotateTypes(root);
    ApplyColumnPruning(root);
    return RunPlan(root, schedulerSettings, nullptr, std::move(filterBindings));
}

std::unique_ptr<TTestRuntime> PlanJoin3(
    const std::string& sexp,
    ISource& left,
    ISource& right,
    ISource& third)
{
    TRelParserOptions opts;
    opts.SourceFactory = [&](std::string_view path, NQumir::TLocation) -> TOperatorPtr {
        if (path == "L") {
            return std::make_shared<TSourceOperator>(left, std::string(path));
        }
        if (path == "R") {
            return std::make_shared<TSourceOperator>(right, std::string(path));
        }
        return std::make_shared<TSourceOperator>(third, std::string(path));
    };
    TParser parser;
    for (auto& [name, fn] : MakeRelParsers(std::move(opts))) {
        parser.NodeParsers[name] = std::move(fn);
    }
    std::istringstream in(sexp);
    TTokenStream ts(in);
    auto parsed = parser.Parse(ts);
    if (!parsed) throw std::runtime_error(parsed.error().ToString());
    auto root = std::static_pointer_cast<IOperator>(*parsed);
    AnnotateTypes(root);
    ApplyColumnPruning(root);
    return RunPlan(root);
}

std::unique_ptr<TTestRuntime> PlanJoinMany(
    const std::string& sexp,
    const std::unordered_map<std::string, ISource*>& sources,
    NScheduler::TSettings settings,
    std::shared_ptr<IRuntimeFilterBindingFactory> filterBindings)
{
    TRelParserOptions opts;
    opts.SourceFactory = [&](std::string_view path, NQumir::TLocation)
        -> TOperatorPtr {
        auto it = sources.find(std::string(path));
        if (it == sources.end()) {
            return nullptr;
        }
        return std::make_shared<TSourceOperator>(*it->second, std::string(path));
    };
    TParser parser;
    for (auto& [name, fn] : MakeRelParsers(std::move(opts))) {
        parser.NodeParsers[name] = std::move(fn);
    }
    std::istringstream in(sexp);
    TTokenStream ts(in);
    auto parsed = parser.Parse(ts);
    if (!parsed) {
        throw std::runtime_error(parsed.error().ToString());
    }
    auto root = std::static_pointer_cast<IOperator>(*parsed);
    AnnotateTypes(root);
    ApplyColumnPruning(root);
    return RunPlan(root, settings, nullptr, std::move(filterBindings));
}

} // namespace

TEST(JoinPlanner, InnerJoinE2E) {
    std::vector<int64_t> lk = {1, 2, 1}, lv = {10, 20, 30};
    std::vector<int64_t> rk = {1, 1, 3}, rv = {100, 200, 300};
    std::vector<TColumn> lcols, rcols;
    NQdb::TMockSource left({"lk", "lv"}, {KeyValBatch(lk.data(), lv.data(), 3, lcols)});
    NQdb::TMockSource right({"rk", "rv"}, {KeyValBatch(rk.data(), rv.data(), 3, rcols)});

    auto plan = PlanJoin(
        "(rel join (rel source \"L\") (rel source \"R\") ((lk rk)) (inner))",
        left, right);

    std::vector<std::tuple<int64_t, int64_t, int64_t, int64_t>> got;
    TRowSet out{};
    while (plan->Next(out)) {
        ASSERT_EQ(out.ColumnCount, 4);
        for (int64_t i = 0; i < out.RowCount; ++i) {
            got.emplace_back(
                reinterpret_cast<const int64_t*>(out.Columns[0].Data)[i],
                reinterpret_cast<const int64_t*>(out.Columns[1].Data)[i],
                reinterpret_cast<const int64_t*>(out.Columns[2].Data)[i],
                reinterpret_cast<const int64_t*>(out.Columns[3].Data)[i]);
        }
        Release(&out);
    }

    std::vector<std::tuple<int64_t, int64_t, int64_t, int64_t>> expected = {
        {1, 10, 1, 100}, {1, 10, 1, 200}, {1, 30, 1, 100}, {1, 30, 1, 200}};
    std::sort(got.begin(), got.end());
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(got, expected);
}

TEST(JoinPlanner, InnerJoinHonorsLeftSelection) {
    std::vector<int64_t> lk = {1, 2, 3, 4}, lv = {10, 20, 30, 40};
    std::vector<int64_t> rk = {1, 2, 3, 4}, rv = {100, 200, 300, 400};
    std::vector<uint8_t> selection = {1, 0, 1, 0};
    std::vector<TColumn> lcols, rcols;
    auto lbatch = KeyValBatch(lk.data(), lv.data(), 4, lcols);
    lbatch.Selection = selection.data();
    NQdb::TMockSource left({"lk", "lv"}, {lbatch});
    NQdb::TMockSource right({"rk", "rv"}, {KeyValBatch(rk.data(), rv.data(), 4, rcols)});

    auto plan = PlanJoin(R"qdb(
(rel join (rel source "L") (rel source "R") ((lk rk)) (inner))
)qdb", left, right);

    std::vector<std::tuple<int64_t, int64_t, int64_t, int64_t>> got;
    TRowSet out{};
    while (plan->Next(out)) {
        ASSERT_EQ(out.ColumnCount, 4);
        for (int64_t i = 0; i < out.RowCount; ++i) {
            got.emplace_back(
                reinterpret_cast<const int64_t*>(out.Columns[0].Data)[i],
                reinterpret_cast<const int64_t*>(out.Columns[1].Data)[i],
                reinterpret_cast<const int64_t*>(out.Columns[2].Data)[i],
                reinterpret_cast<const int64_t*>(out.Columns[3].Data)[i]);
        }
        Release(&out);
    }

    std::vector<std::tuple<int64_t, int64_t, int64_t, int64_t>> expected = {
        {1, 10, 1, 100}, {3, 30, 3, 300}};
    EXPECT_EQ(got, expected);
}

TEST(JoinPlanner, InnerJoinResidualHonorsLeftSelection) {
    std::vector<int64_t> lk = {1, 1, 1, 1}, lv = {2, 1, 4, 3};
    std::vector<int64_t> rk = {1, 1, 1, 1}, rv = {1, 2, 3, 4};
    std::vector<uint8_t> selection = {1, 0, 0, 1};
    std::vector<TColumn> lcols, rcols;
    auto lbatch = KeyValBatch(lk.data(), lv.data(), 4, lcols);
    lbatch.Selection = selection.data();
    NQdb::TMockSource left({"lk", "lv"}, {lbatch});
    NQdb::TMockSource right({"rk", "rv"}, {KeyValBatch(rk.data(), rv.data(), 4, rcols)});

    auto plan = PlanJoin(R"qdb(
(rel join (rel source "L") (rel source "R") ((lk rk)) (inner) (residual (== lv (+ rv 1))))
)qdb", left, right);

    std::vector<std::tuple<int64_t, int64_t, int64_t, int64_t>> got;
    TRowSet out{};
    while (plan->Next(out)) {
        ASSERT_EQ(out.ColumnCount, 4);
        for (int64_t i = 0; i < out.RowCount; ++i) {
            got.emplace_back(
                reinterpret_cast<const int64_t*>(out.Columns[0].Data)[i],
                reinterpret_cast<const int64_t*>(out.Columns[1].Data)[i],
                reinterpret_cast<const int64_t*>(out.Columns[2].Data)[i],
                reinterpret_cast<const int64_t*>(out.Columns[3].Data)[i]);
        }
        Release(&out);
    }

    std::vector<std::tuple<int64_t, int64_t, int64_t, int64_t>> expected = {
        {1, 2, 1, 1}, {1, 3, 1, 2}};
    EXPECT_EQ(got, expected);
}

TEST(JoinPlanner, ChainedInnerJoinResidualHonorsLeftSelection) {
    std::vector<int64_t> lk = {1, 1, 1, 1}, lv = {2, 1, 4, 3};
    std::vector<int64_t> rk = {1, 1, 1, 1}, rv = {1, 2, 3, 4};
    std::vector<int64_t> sk = {1, 1, 1, 1}, sv = {1, 2, 3, 4};
    std::vector<uint8_t> selection = {1, 0, 0, 1};
    std::vector<TColumn> lcols, rcols, scols;
    auto lbatch = KeyValBatch(lk.data(), lv.data(), 4, lcols);
    lbatch.Selection = selection.data();
    NQdb::TMockSource left({"lk", "lv"}, {lbatch});
    NQdb::TMockSource right({"rk", "rv"}, {KeyValBatch(rk.data(), rv.data(), 4, rcols)});
    NQdb::TMockSource third({"sk", "sv"}, {KeyValBatch(sk.data(), sv.data(), 4, scols)});

    auto plan = PlanJoin3(R"qdb(
(rel join
  (rel join (rel source "L") (rel source "R") ((lk rk)) (inner) (residual (== lv (+ rv 1))))
  (rel source "S")
  ((lk sk))
  (inner)
  (residual (== lv (- sv 1))))
)qdb", left, right, third);

    std::vector<std::tuple<int64_t, int64_t, int64_t, int64_t, int64_t, int64_t>> got;
    TRowSet out{};
    while (plan->Next(out)) {
        ASSERT_EQ(out.ColumnCount, 6);
        for (int64_t i = 0; i < out.RowCount; ++i) {
            got.emplace_back(
                reinterpret_cast<const int64_t*>(out.Columns[0].Data)[i],
                reinterpret_cast<const int64_t*>(out.Columns[1].Data)[i],
                reinterpret_cast<const int64_t*>(out.Columns[2].Data)[i],
                reinterpret_cast<const int64_t*>(out.Columns[3].Data)[i],
                reinterpret_cast<const int64_t*>(out.Columns[4].Data)[i],
                reinterpret_cast<const int64_t*>(out.Columns[5].Data)[i]);
        }
        Release(&out);
    }

    std::vector<std::tuple<int64_t, int64_t, int64_t, int64_t, int64_t, int64_t>> expected = {
        {1, 2, 1, 1, 1, 3}, {1, 3, 1, 2, 1, 4}};
    EXPECT_EQ(got, expected);
}

TEST(JoinPlanner, CrossJoinE2E) {
    std::vector<int64_t> lk = {1, 2};
    std::vector<int64_t> lv = {10, 20};
    std::vector<int64_t> rk = {7, 8};
    std::vector<int64_t> rv = {70, 80};
    std::vector<TColumn> lcols, rcols;
    NQdb::TMockSource left({"lk", "lv"}, {KeyValBatch(lk.data(), lv.data(), 2, lcols)});
    NQdb::TMockSource right({"rk", "rv"}, {KeyValBatch(rk.data(), rv.data(), 2, rcols)});

    auto plan = PlanJoin(
        "(rel join (rel source \"L\") (rel source \"R\") () (inner))",
        left, right);

    std::vector<std::tuple<int64_t, int64_t, int64_t, int64_t>> got;
    TRowSet out{};
    while (plan->Next(out)) {
        ASSERT_EQ(out.ColumnCount, 4);
        for (int64_t i = 0; i < out.RowCount; ++i) {
            got.emplace_back(
                reinterpret_cast<const int64_t*>(out.Columns[0].Data)[i],
                reinterpret_cast<const int64_t*>(out.Columns[1].Data)[i],
                reinterpret_cast<const int64_t*>(out.Columns[2].Data)[i],
                reinterpret_cast<const int64_t*>(out.Columns[3].Data)[i]);
        }
        Release(&out);
    }

    std::vector<std::tuple<int64_t, int64_t, int64_t, int64_t>> expected = {
        {1, 10, 7, 70}, {1, 10, 8, 80},
        {2, 20, 7, 70}, {2, 20, 8, 80},
    };
    std::sort(got.begin(), got.end());
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(got, expected);
}

TEST(JoinPlanner, CrossJoinResidualSupportsRightGroupKeys) {
    for (auto mode : {NScheduler::EExecutionMode::SingleThreadedScheduler,
                      NScheduler::EExecutionMode::ThreadedScheduler}) {
        SCOPED_TRACE(static_cast<int>(mode));
        std::vector<int64_t> lk = {1, 2, 3}, lv = {10, 20, 30};
        std::vector<int64_t> rk = {7, 8, 9}, rv = {15, 25, 25};
        std::vector<TColumn> lcols, rcols;
        NQdb::TMockSource left({"lk", "lv"},
            {KeyValBatch(lk.data(), lv.data(), 3, lcols)});
        NQdb::TMockSource right({"rk", "rv"},
            {KeyValBatch(rk.data(), rv.data(), 3, rcols)});
        NScheduler::TSettings settings;
        settings.Scheduler.Mode = mode;
        settings.Scheduler.WorkerCount = 2;

        auto plan = PlanJoin(R"qdb(
(rel aggregate
  (rel join (rel source "L") (rel source "R") () (inner) (residual (> rv lv)))
  (keys rv) (agg n count) (agg total sum lv))
)qdb", left, right, settings);

        std::vector<std::tuple<int64_t, int64_t, int64_t>> got;
        TRowSet out{};
        while (plan->Next(out)) {
            EXPECT_EQ(out.ColumnCount, 3);
            for (int64_t i = 0; i < out.RowCount; ++i) {
                got.emplace_back(
                    reinterpret_cast<const int64_t*>(out.Columns[0].Data)[i],
                    reinterpret_cast<const int64_t*>(out.Columns[1].Data)[i],
                    reinterpret_cast<const int64_t*>(out.Columns[2].Data)[i]);
            }
            Release(&out);
        }
        std::sort(got.begin(), got.end());
        EXPECT_EQ(got, (std::vector<std::tuple<int64_t, int64_t, int64_t>>{
            {15, 1, 10}, {25, 4, 60}}));
    }
}

TEST(JoinPlanner, SchedulerThreadedInnerJoinE2E) {
    std::vector<int64_t> lk = {1, 2, 1, 3}, lv = {10, 20, 30, 40};
    std::vector<int64_t> rk = {1, 1, 3, 4}, rv = {100, 200, 300, 400};
    std::vector<TColumn> lcols, rcols;
    NQdb::TMockSource left({"lk", "lv"}, {KeyValBatch(lk.data(), lv.data(), 4, lcols)});
    NQdb::TMockSource right({"rk", "rv"}, {KeyValBatch(rk.data(), rv.data(), 4, rcols)});
    NScheduler::TSettings settings;
    settings.Scheduler.Mode = NScheduler::EExecutionMode::ThreadedScheduler;
    settings.Scheduler.WorkerCount = 2;

    auto plan = PlanJoin(
        "(rel join (rel source \"L\") (rel source \"R\") ((lk rk)) (inner))",
        left,
        right,
        settings);

    std::vector<std::tuple<int64_t, int64_t, int64_t, int64_t>> got;
    TRowSet out{};
    while (plan->Next(out)) {
        ASSERT_EQ(out.ColumnCount, 4);
        for (int64_t i = 0; i < out.RowCount; ++i) {
            got.emplace_back(
                reinterpret_cast<const int64_t*>(out.Columns[0].Data)[i],
                reinterpret_cast<const int64_t*>(out.Columns[1].Data)[i],
                reinterpret_cast<const int64_t*>(out.Columns[2].Data)[i],
                reinterpret_cast<const int64_t*>(out.Columns[3].Data)[i]);
        }
        Release(&out);
    }

    std::vector<std::tuple<int64_t, int64_t, int64_t, int64_t>> expected = {
        {1, 10, 1, 100},
        {1, 10, 1, 200},
        {1, 30, 1, 100},
        {1, 30, 1, 200},
        {3, 40, 3, 300},
    };
    std::sort(got.begin(), got.end());
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(got, expected);
}

TEST(JoinPlanner, RuntimeFilterBindingKeepsThreadedJoinResult) {
    std::vector<int64_t> lk = {1, 2, 3}, lv = {10, 20, 30};
    std::vector<int64_t> rk = {1}, rv = {100};
    std::vector<TColumn> lcols, rcols;
    TMockSource left({"lk", "lv"}, {KeyValBatch(lk.data(), lv.data(), 3, lcols)});
    TMockSource right({"rk", "rv"}, {KeyValBatch(rk.data(), rv.data(), 1, rcols)});
    NScheduler::TSettings settings;
    settings.Scheduler.Mode = NScheduler::EExecutionMode::ThreadedScheduler;
    settings.Scheduler.WorkerCount = 2;
    settings.HashShuffle.PartitionCount = 2;
    settings.HashShuffle.MaxPartitionCount = 2;

    auto plan = PlanJoin(
        "(rel join (rel source \"L\") (rel source \"R\") "
        "((lk rk)) (inner) (emit-filter 7 right))",
        left, right, settings);

    std::vector<std::tuple<int64_t, int64_t, int64_t, int64_t>> got;
    TRowSet out{};
    while (plan->Next(out)) {
        for (int64_t i = 0; i < out.RowCount; ++i) {
            got.emplace_back(
                reinterpret_cast<const int64_t*>(out.Columns[0].Data)[i],
                reinterpret_cast<const int64_t*>(out.Columns[1].Data)[i],
                reinterpret_cast<const int64_t*>(out.Columns[2].Data)[i],
                reinterpret_cast<const int64_t*>(out.Columns[3].Data)[i]);
        }
        Release(&out);
    }
    EXPECT_EQ(got,
        (std::vector<std::tuple<int64_t, int64_t, int64_t, int64_t>>{
            {1, 10, 1, 100}}));
}

TEST(JoinPlanner, RuntimeFilterRejectsRowsWithOneLanePerSide) {
    for (auto mode : {NScheduler::EExecutionMode::SingleThreadedScheduler,
             NScheduler::EExecutionMode::ThreadedScheduler}) {
        SCOPED_TRACE(static_cast<int>(mode));
        std::vector<int64_t> lk = {1, 2, 3}, lv = {10, 20, 30};
        std::vector<int64_t> rk = {1}, rv = {100};
        std::vector<TColumn> lcols, rcols;
        TMockSource left({"lk", "lv"}, {KeyValBatch(lk.data(), lv.data(), 3, lcols)});
        TMockSource right({"rk", "rv"}, {KeyValBatch(rk.data(), rv.data(), 1, rcols)});
        auto bindings = std::make_shared<TCountingFilterBindingFactory>();
        NScheduler::TSettings settings;
        settings.Scheduler.Mode = mode;
        settings.Scheduler.WorkerCount = 2;

        auto plan = PlanJoin(
            "(rel join (rel source \"L\") (rel source \"R\") "
            "((lk rk)) (inner) (emit-filter 7 right))",
            left, right, settings, bindings);

        std::vector<int64_t> keys;
        TRowSet out{};
        while (plan->Next(out)) {
            for (int64_t i = 0; i < out.RowCount; ++i) {
                keys.push_back(
                    reinterpret_cast<const int64_t*>(out.Columns[0].Data)[i]);
            }
            Release(&out);
        }
        EXPECT_EQ(keys, (std::vector<int64_t>{1}));
        ASSERT_EQ(bindings->Created, 1u);
        ASSERT_TRUE(bindings->Probe);
        EXPECT_EQ(bindings->Probe->Probed.load(), 3u);
        EXPECT_EQ(bindings->Probe->Rejected.load(), 2u);
    }
}

TEST(JoinPlanner, RuntimeFilterAtSourceComposesWithStaticFilter) {
    std::vector<int64_t> lk = {1, 2, 3}, lv = {20, 20, 5};
    std::vector<int64_t> rk = {1}, rv = {100};
    std::vector<TColumn> lcols, rcols;
    TMockSource left({"lk", "lv"}, {KeyValBatch(lk.data(), lv.data(), 3, lcols)});
    TMockSource right({"rk", "rv"}, {KeyValBatch(rk.data(), rv.data(), 1, rcols)});
    auto bindings = std::make_shared<TCountingFilterBindingFactory>();
    NScheduler::TSettings settings;
    settings.Scheduler.Mode = NScheduler::EExecutionMode::ThreadedScheduler;
    settings.Scheduler.WorkerCount = 2;

    auto plan = PlanJoin(
        "(rel join (rel filter (rel source \"L\") (> lv 10)) "
        "(rel source \"R\") ((lk rk)) (inner) (emit-filter 7 right))",
        left, right, settings, bindings);

    std::vector<int64_t> keys;
    TRowSet out{};
    while (plan->Next(out)) {
        for (int64_t i = 0; i < out.RowCount; ++i) {
            if (!out.Selection || out.Selection[i]) {
                keys.push_back(
                    reinterpret_cast<const int64_t*>(out.Columns[0].Data)[i]);
            }
        }
        Release(&out);
    }
    EXPECT_EQ(keys, (std::vector<int64_t>{1}));
    ASSERT_TRUE(bindings->Probe);
    EXPECT_EQ(bindings->Probe->Probed.load(), 3u);
    EXPECT_EQ(bindings->Probe->Rejected.load(), 2u);
}

TEST(JoinPlanner, RuntimeFilterMergesTwoBuildLanesBeforeProbing) {
    std::vector<int64_t> lk1 = {1, 2}, lv1 = {10, 20};
    std::vector<int64_t> lk2 = {3, 4}, lv2 = {30, 40};
    std::vector<int64_t> rk1 = {1}, rv1 = {100};
    std::vector<int64_t> rk2 = {3}, rv2 = {300};
    std::vector<TColumn> lc1, lc2, rc1, rc2;
    TMockSource left1({"lk", "lv"}, {KeyValBatch(lk1.data(), lv1.data(), 2, lc1)});
    TMockSource left2({"lk", "lv"}, {KeyValBatch(lk2.data(), lv2.data(), 2, lc2)});
    TMockSource right1({"rk", "rv"}, {KeyValBatch(rk1.data(), rv1.data(), 1, rc1)});
    TMockSource right2({"rk", "rv"}, {KeyValBatch(rk2.data(), rv2.data(), 1, rc2)});
    const std::unordered_map<std::string, ISource*> sources{
        {"L1", &left1}, {"L2", &left2},
        {"R1", &right1}, {"R2", &right2},
    };
    NScheduler::TSettings settings;
    settings.Scheduler.Mode = NScheduler::EExecutionMode::ThreadedScheduler;
    settings.Scheduler.WorkerCount = 4;
    settings.HashShuffle.PartitionCount = 2;
    settings.HashShuffle.MaxPartitionCount = 2;
    auto bindings = std::make_shared<TCountingFilterBindingFactory>(
        /*maxKeys=*/1, /*bloomBytes=*/512);

    auto plan = PlanJoinMany(
        "(rel join "
        "(rel union-all (rel source \"L1\") (rel source \"L2\")) "
        "(rel union-all (rel source \"R1\") (rel source \"R2\")) "
        "((lk rk)) (inner) (emit-filter 7 right))",
        sources, settings, bindings);

    std::vector<int64_t> keys;
    TRowSet out{};
    while (plan->Next(out)) {
        for (int64_t i = 0; i < out.RowCount; ++i) {
            keys.push_back(
                reinterpret_cast<const int64_t*>(out.Columns[0].Data)[i]);
        }
        Release(&out);
    }
    std::sort(keys.begin(), keys.end());
    EXPECT_EQ(keys, (std::vector<int64_t>{1, 3}));
    EXPECT_EQ(bindings->Created, 1u);
    EXPECT_EQ(bindings->ProducerCount, 2u);
    ASSERT_TRUE(bindings->Filter);
    EXPECT_TRUE(bindings->Filter->UsesBloom());
    ASSERT_TRUE(bindings->Probe);
    EXPECT_EQ(bindings->Probe->Probed.load(), 4u);
    EXPECT_EQ(bindings->Probe->Rejected.load(), 2u);
}

TEST(JoinPlanner, ProjectOnTopPrunesJoinInputs) {
    // Project keeps only lk and rv; lv and rk(beyond the key) are not selected.
    // Pruning narrows each source, but the key columns survive.
    std::vector<int64_t> lk = {5, 6, 5}, lv = {10, 20, 30};
    std::vector<int64_t> rk = {5, 5, 7}, rv = {100, 200, 300};
    std::vector<TColumn> lcols, rcols;
    NQdb::TMockSource left({"lk", "lv"}, {KeyValBatch(lk.data(), lv.data(), 3, lcols)});
    NQdb::TMockSource right({"rk", "rv"}, {KeyValBatch(rk.data(), rv.data(), 3, rcols)});

    auto plan = PlanJoin(
        "(rel project (rel join (rel source \"L\") (rel source \"R\") "
        "((lk rk)) (inner)) (a lk) (b rv))",
        left, right);

    std::vector<std::tuple<int64_t, int64_t>> got;
    TRowSet out{};
    while (plan->Next(out)) {
        ASSERT_EQ(out.ColumnCount, 2);
        for (int64_t i = 0; i < out.RowCount; ++i) {
            got.emplace_back(
                reinterpret_cast<const int64_t*>(out.Columns[0].Data)[i],
                reinterpret_cast<const int64_t*>(out.Columns[1].Data)[i]);
        }
        Release(&out);
    }

    // lk==rk matches: l0(5),l2(5) x r0(5),r1(5) -> 4 rows, each (lk=5, rv in {100,200}).
    std::vector<std::tuple<int64_t, int64_t>> expected = {
        {5, 100}, {5, 200}, {5, 100}, {5, 200}};
    std::sort(got.begin(), got.end());
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(got, expected);
}

namespace {

TTypePtr I32() { return std::make_shared<TIntegerType>(TIntegerType::I32); }
TTypePtr I64T() { return std::make_shared<TIntegerType>(TIntegerType::I64); }

} // namespace

TEST(JoinPlanner, Int32KeyE2E) {
    // int32 join key (the TPC-H *_key case) flows through the generic path.
    std::vector<int32_t> lk = {1, 2, 1}; std::vector<int64_t> lv = {10, 20, 30};
    std::vector<int32_t> rk = {1, 1, 3}; std::vector<int64_t> rv = {100, 200, 300};
    std::vector<TColumn> lcols = {TColumn{.Data = reinterpret_cast<char*>(lk.data())},
                                  TColumn{.Data = reinterpret_cast<char*>(lv.data())}};
    std::vector<TColumn> rcols = {TColumn{.Data = reinterpret_cast<char*>(rk.data())},
                                  TColumn{.Data = reinterpret_cast<char*>(rv.data())}};
    TRowSet lbatch{.Columns = lcols.data(), .ColumnCount = 2, .RowCount = 3, .RefCount = 1};
    TRowSet rbatch{.Columns = rcols.data(), .ColumnCount = 2, .RowCount = 3, .RefCount = 1};

    NQdb::TMockSource left({"lk", "lv"}, {lbatch}, {I32(), I64T()});
    NQdb::TMockSource right({"rk", "rv"}, {rbatch}, {I32(), I64T()});

    auto plan = PlanJoin(
        "(rel join (rel source \"L\") (rel source \"R\") ((lk rk)) (inner))", left, right);

    std::vector<std::tuple<int64_t, int64_t>> got; // (lv, rv) of matched rows
    TRowSet out{};
    while (plan->Next(out)) {
        ASSERT_EQ(out.ColumnCount, 4); // lk(i32), lv(i64), rk(i32), rv(i64)
        for (int64_t i = 0; i < out.RowCount; ++i) {
            // Keys equal: out col0 (lk i32) == out col2 (rk i32).
            EXPECT_EQ(reinterpret_cast<const int32_t*>(out.Columns[0].Data)[i],
                      reinterpret_cast<const int32_t*>(out.Columns[2].Data)[i]);
            got.emplace_back(reinterpret_cast<const int64_t*>(out.Columns[1].Data)[i],
                             reinterpret_cast<const int64_t*>(out.Columns[3].Data)[i]);
        }
        Release(&out);
    }
    std::vector<std::tuple<int64_t, int64_t>> expected = {
        {10, 100}, {10, 200}, {30, 100}, {30, 200}};
    std::sort(got.begin(), got.end());
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(got, expected);
}

TEST(JoinPlanner, CompositeI32KeyE2E) {
    // Composite (i32, i32) key.
    std::vector<int32_t> la = {1, 1, 2}; std::vector<int32_t> lb = {7, 8, 7};
    std::vector<int32_t> ra = {1, 2, 1}; std::vector<int32_t> rb = {7, 7, 9};
    std::vector<TColumn> lcols = {TColumn{.Data = reinterpret_cast<char*>(la.data())},
                                  TColumn{.Data = reinterpret_cast<char*>(lb.data())}};
    std::vector<TColumn> rcols = {TColumn{.Data = reinterpret_cast<char*>(ra.data())},
                                  TColumn{.Data = reinterpret_cast<char*>(rb.data())}};
    TRowSet lbatch{.Columns = lcols.data(), .ColumnCount = 2, .RowCount = 3, .RefCount = 1};
    TRowSet rbatch{.Columns = rcols.data(), .ColumnCount = 2, .RowCount = 3, .RefCount = 1};

    NQdb::TMockSource left({"la", "lb"}, {lbatch}, {I32(), I32()});
    NQdb::TMockSource right({"ra", "rb"}, {rbatch}, {I32(), I32()});

    auto plan = PlanJoin(
        "(rel join (rel source \"L\") (rel source \"R\") ((la ra) (lb rb)) (inner))",
        left, right);

    int64_t rows = 0;
    TRowSet out{};
    while (plan->Next(out)) {
        for (int64_t i = 0; i < out.RowCount; ++i) {
            // Both key components equal.
            EXPECT_EQ(reinterpret_cast<const int32_t*>(out.Columns[0].Data)[i],
                      reinterpret_cast<const int32_t*>(out.Columns[2].Data)[i]);
            EXPECT_EQ(reinterpret_cast<const int32_t*>(out.Columns[1].Data)[i],
                      reinterpret_cast<const int32_t*>(out.Columns[3].Data)[i]);
        }
        rows += out.RowCount;
        Release(&out);
    }
    // (1,7)x(1,7) and (2,7)x(2,7) match -> 2 rows.
    EXPECT_EQ(rows, 2);
}

TEST(JoinPlanner, ResidualLeftSemiMarksInKernel) {
    std::vector<int64_t> lk = {1, 1, 2}, lv = {10, 20, 30};
    std::vector<int64_t> rk = {1, 2}, rv = {10, 30};
    std::vector<TColumn> lcols, rcols;
    NQdb::TMockSource left({"lk", "lv"}, {KeyValBatch(lk.data(), lv.data(), 3, lcols)});
    NQdb::TMockSource right({"rk", "rv"}, {KeyValBatch(rk.data(), rv.data(), 2, rcols)});

    auto plan = PlanJoin(
        "(rel join (rel source \"L\") (rel source \"R\") "
        "((lk rk)) (left_semi) (residual (!= lv rv)))",
        left, right);

    std::vector<std::tuple<int64_t, int64_t>> got;
    TRowSet out{};
    while (plan->Next(out)) {
        ASSERT_EQ(out.ColumnCount, 2);
        for (int64_t i = 0; i < out.RowCount; ++i) {
            got.emplace_back(
                reinterpret_cast<const int64_t*>(out.Columns[0].Data)[i],
                reinterpret_cast<const int64_t*>(out.Columns[1].Data)[i]);
        }
        Release(&out);
    }

    std::vector<std::tuple<int64_t, int64_t>> expected = {{1, 20}};
    EXPECT_EQ(got, expected);
}

TEST(JoinPlanner, ResidualLeftAntiHonorsLeftSelection) {
    std::vector<int64_t> lk = {1, 2, 3, 4}, lv = {10, 20, 30, 40};
    std::vector<uint8_t> selection = {1, 0, 1, 0};
    std::vector<TColumn> lcols;
    auto lbatch = KeyValBatch(lk.data(), lv.data(), 4, lcols);
    lbatch.Selection = selection.data();

    auto leftType = std::vector<TTypePtr>{I64T(), I64T()};
    NQdb::TMockSource left({"lk", "lv"}, {lbatch}, leftType);
    NQdb::TMockSource right({"rk", "rv"}, {}, leftType);

    auto plan = PlanJoin(
        "(rel join (rel source \"L\") (rel source \"R\") "
        "((lk rk)) (left_anti) (residual (!= lv rv)))",
        left, right);

    std::vector<std::tuple<int64_t, int64_t>> got;
    TRowSet out{};
    while (plan->Next(out)) {
        ASSERT_EQ(out.ColumnCount, 2);
        for (int64_t i = 0; i < out.RowCount; ++i) {
            got.emplace_back(
                reinterpret_cast<const int64_t*>(out.Columns[0].Data)[i],
                reinterpret_cast<const int64_t*>(out.Columns[1].Data)[i]);
        }
        Release(&out);
    }

    std::vector<std::tuple<int64_t, int64_t>> expected = {
        {1, 10},
        {3, 30},
    };
    EXPECT_EQ(got, expected);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    NQumir::NCodeGen::TLLVMInitializer initializer;
    return RUN_ALL_TESTS();
}
