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
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/union.h>
#include <qdb/plan/passes/column_pruning.h>
#include <qdb/plan/passes/runtime_filters.h>
#include <qdb/plan/passes/typing.h>
#include <qdb/plan/types/nullable.h>
#include <qdb/sexp/parser.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
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

class TCountingFilterBindingFactory final : public IRuntimeFilterBindingFactory {
public:
    TCountingFilterBindingFactory(
        size_t maxKeys = TRuntimeFilter::DefaultMaxKeys,
        size_t bloomBytes = TRuntimeFilter::DefaultBloomBytes)
        : MaxKeys_(maxKeys), BloomBytes_(bloomBytes) {}

    TRuntimeFilterBinding Create(
        uint32_t id, size_t producerCount, size_t bloomBytes,
        std::shared_ptr<TRuntimeContext> runtimeContext) override {
        (void)id;
        (void)bloomBytes;
        ++Created;
        ProducerCount = producerCount;
        Filter = std::make_shared<TRuntimeFilter>(
            std::move(runtimeContext), MaxKeys_, BloomBytes_);
        Filter->SetProducerCount(producerCount);
        return {.Producer = Filter, .Probe = Filter};
    }

    size_t Created = 0;
    size_t ProducerCount = 0;
    std::shared_ptr<TRuntimeFilter> Filter;

private:
    size_t MaxKeys_;
    size_t BloomBytes_;
};

std::unique_ptr<TTestRuntime> PlanJoinWithSources(
    const std::string& sexp,
    const std::function<ISource*(std::string_view)>& sourceLookup,
    NScheduler::TSettings settings = {},
    std::shared_ptr<IRuntimeFilterBindingFactory> filterBindings = nullptr,
    std::function<void(const TOperatorPtr&)> configure = {})
{
    TRelParserOptions opts;
    opts.SourceFactory = [&](std::string_view path, NQumir::TLocation)
        -> TOperatorPtr {
        auto* source = sourceLookup(path);
        return source
            ? std::make_shared<TSourceOperator>(*source, std::string(path))
            : nullptr;
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
    if (configure) {
        configure(root);
    }
    return RunPlan(root, settings, nullptr, std::move(filterBindings));
}

// Parses `sexp`, wiring "L" -> left source, anything else -> right source, then
// runs the full logical pipeline + physical planner.
std::unique_ptr<TTestRuntime> PlanJoin(
    const std::string& sexp,
    ISource& left,
    ISource& right,
    NScheduler::TSettings schedulerSettings = {},
    std::shared_ptr<IRuntimeFilterBindingFactory> filterBindings = nullptr)
{
    return PlanJoinWithSources(sexp,
        [&](std::string_view path) -> ISource* {
            return path == "L" ? &left : &right;
        }, schedulerSettings, std::move(filterBindings));
}

std::unique_ptr<TTestRuntime> PlanJoin3(
    const std::string& sexp,
    ISource& left,
    ISource& right,
    ISource& third)
{
    return PlanJoinWithSources(sexp, [&](std::string_view path) -> ISource* {
        if (path == "L") {
            return &left;
        }
        if (path == "R") {
            return &right;
        }
        return &third;
    });
}

std::unique_ptr<TTestRuntime> PlanJoinMany(
    const std::string& sexp,
    const std::unordered_map<std::string, ISource*>& sources,
    NScheduler::TSettings settings,
    std::shared_ptr<IRuntimeFilterBindingFactory> filterBindings,
    std::function<void(const TOperatorPtr&)> configure = {})
{
    return PlanJoinWithSources(sexp,
        [&](std::string_view path) -> ISource* {
            auto it = sources.find(std::string(path));
            return it == sources.end() ? nullptr : it->second;
        }, settings, std::move(filterBindings), std::move(configure));
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
        ASSERT_TRUE(bindings->Filter);
        EXPECT_EQ(bindings->Filter->StatsSnapshot().ProbeRows, 3u);
        EXPECT_EQ(bindings->Filter->StatsSnapshot().RejectedRows, 2u);
        EXPECT_EQ(bindings->Filter->StatsSnapshot().SkippedProbeRows, 0u);
    }
}

TEST(JoinPlanner, LeftOuterRuntimeFilterPreservesUnmatchedRows) {
    for (auto mode : {NScheduler::EExecutionMode::SingleThreadedScheduler,
             NScheduler::EExecutionMode::ThreadedScheduler}) {
        SCOPED_TRACE(static_cast<int>(mode));
        std::vector<int64_t> lk = {1, 2, 4}, lv = {10, 20, 40};
        std::vector<int64_t> rk0 = {1, 9000}, rv0 = {100, 900};
        std::vector<int64_t> rk1 = {1, 9001}, rv1 = {101, 901};
        std::vector<TColumn> lcols, rcols0, rcols1;
        TMockSource left({"lk", "lv"},
            {KeyValBatch(lk.data(), lv.data(), 3, lcols)});
        TMockSource right({"rk", "rv"},
            {KeyValBatch(rk0.data(), rv0.data(), 2, rcols0),
             KeyValBatch(rk1.data(), rv1.data(), 2, rcols1)});
        auto bindings = std::make_shared<TCountingFilterBindingFactory>();
        NScheduler::TSettings settings;
        settings.Scheduler.Mode = mode;
        settings.Scheduler.WorkerCount = 2;
        settings.Queue.RowsetCapacityPerLane = 1;
        settings.HashShuffle.MaxQueuedRowsetsPerLane = 1;
        settings.HashShuffle.PartitionCount = 2;
        settings.HashShuffle.MaxPartitionCount = 2;

        auto plan = PlanJoin(
            "(rel join (rel source \"L\") (rel source \"R\") "
            "((lk rk)) (left) (emit-filter 7 left))",
            left, right, settings, bindings);

        std::vector<std::pair<int64_t, bool>> got;
        TRowSet out{};
        while (plan->Next(out)) {
            for (int64_t i = 0; i < out.RowCount; ++i) {
                if (out.Selection && !out.Selection[i]) {
                    continue;
                }
                const bool matched = !out.Columns[2].Mask
                    || ((out.Columns[2].Mask[i / 8] >> (i % 8)) & 1);
                got.emplace_back(
                    reinterpret_cast<const int64_t*>(out.Columns[0].Data)[i],
                    matched);
            }
            Release(&out);
        }
        std::sort(got.begin(), got.end());
        EXPECT_EQ(got,
            (std::vector<std::pair<int64_t, bool>>{
                {1, true}, {1, true}, {2, false}, {4, false}}));
        ASSERT_TRUE(bindings->Filter);
        EXPECT_EQ(bindings->Filter->StatsSnapshot().ProbeRows, 4u);
        EXPECT_EQ(bindings->Filter->StatsSnapshot().RejectedRows, 2u);
    }
}

TEST(JoinPlanner, OneRuntimeFilterServesTwoLeftJoinInputs) {
    for (auto mode : {NScheduler::EExecutionMode::SingleThreadedScheduler,
             NScheduler::EExecutionMode::ThreadedScheduler}) {
        SCOPED_TRACE(static_cast<int>(mode));
        std::vector<int64_t> lk = {1, 2, 4}, lv = {10, 20, 40};
        std::vector<int64_t> rk = {1, 9000}, rv = {100, 900};
        std::vector<int64_t> ck = {1, 2, 9001}, cv = {1000, 2000, 9000};
        std::vector<TColumn> lcols, rcols, ccols;
        TMockSource left({"lk", "lv"},
            {KeyValBatch(lk.data(), lv.data(), 3, lcols)});
        TMockSource middle({"rk", "rv"},
            {KeyValBatch(rk.data(), rv.data(), 2, rcols)});
        TMockSource third({"ck", "cv"},
            {KeyValBatch(ck.data(), cv.data(), 3, ccols)});
        auto bindings = std::make_shared<TCountingFilterBindingFactory>();
        NScheduler::TSettings settings;
        settings.Scheduler.Mode = mode;
        settings.Scheduler.WorkerCount = 2;
        settings.Queue.RowsetCapacityPerLane = 1;
        settings.HashShuffle.MaxQueuedRowsetsPerLane = 1;
        settings.HashShuffle.PartitionCount = 2;
        settings.HashShuffle.MaxPartitionCount = 2;

        auto plan = PlanJoinMany(
            "(rel join "
            "(rel join (rel source \"L\") (rel source \"R\") "
            "((lk rk)) (left) (emit-filter 7 left)) "
            "(rel source \"C\") ((lk ck)) (left))",
            {{"L", &left}, {"R", &middle}, {"C", &third}},
            settings, bindings,
            [](const TOperatorPtr& root) {
                auto& outer = static_cast<TJoinOperator&>(*root);
                outer.Right()->RuntimeFilterUses.push_back(
                    {.Id = 7, .ProbeKeys = {"ck"}});
                PlaceRuntimeFilterConsumers(root);
            });

        std::vector<std::tuple<int64_t, bool, bool>> got;
        TRowSet out{};
        while (plan->Next(out)) {
            for (int64_t i = 0; i < out.RowCount; ++i) {
                if (out.Selection && !out.Selection[i]) {
                    continue;
                }
                auto valid = [&](size_t column) {
                    return !out.Columns[column].Mask
                        || ((out.Columns[column].Mask[i / 8] >> (i % 8)) & 1);
                };
                got.emplace_back(
                    reinterpret_cast<const int64_t*>(out.Columns[0].Data)[i],
                    valid(2), valid(4));
            }
            Release(&out);
        }
        std::sort(got.begin(), got.end());
        EXPECT_EQ(got,
            (std::vector<std::tuple<int64_t, bool, bool>>{
                {1, true, true}, {2, false, true}, {4, false, false}}));
        ASSERT_EQ(bindings->Created, 1u);
        ASSERT_TRUE(bindings->Filter);
        EXPECT_EQ(bindings->Filter->StatsSnapshot().ProbeRows, 5u);
        EXPECT_EQ(bindings->Filter->StatsSnapshot().RejectedRows, 2u);
    }
}

TEST(JoinPlanner, ReusedInnerDomainPreservesDuplicatesNullsAndEmptyBuilds) {
    for (auto mode : {NScheduler::EExecutionMode::SingleThreadedScheduler,
             NScheduler::EExecutionMode::ThreadedScheduler}) {
        for (bool empty : {false, true}) {
            SCOPED_TRACE(static_cast<int>(mode));
            SCOPED_TRACE(empty);
            std::vector<int64_t> lk = {1, 1, 9, 5}, lv = {10, 20, 90, 50};
            std::vector<int64_t> rk = {1, 1, 2, 8, 7}, rv = {100, 200, 300, 400, 700};
            std::vector<int64_t> ck = {1, 1, 1, 3, 6}, cv = {1000, 2000, 3000, 4000, 6000};
            std::vector<TColumn> lcols, rcols, ccols;
            auto leftBatch = KeyValBatch(lk.data(), lv.data(), 4, lcols);
            auto rightBatch = KeyValBatch(rk.data(), rv.data(), 5, rcols);
            auto thirdBatch = KeyValBatch(ck.data(), cv.data(), 5, ccols);
            uint8_t leftValid = 0b00001011;
            uint8_t rightValid = 0b00010111;
            uint8_t thirdValid = 0b00001111;
            lcols[0].Mask = &leftValid;
            rcols[0].Mask = &rightValid;
            ccols[0].Mask = &thirdValid;
            auto nullable = std::make_shared<TNullable>(
                std::make_shared<TIntegerType>(TIntegerType::I64));
            std::vector<TTypePtr> types{nullable,
                std::make_shared<TIntegerType>(TIntegerType::I64)};
            TMockSource left({"lk", "lv"},
                empty ? std::vector<TRowSet>{} : std::vector<TRowSet>{leftBatch}, types);
            TMockSource middle({"rk", "rv"}, {rightBatch}, types);
            TMockSource third({"ck", "cv"}, {thirdBatch}, types);
            auto bindings = std::make_shared<TCountingFilterBindingFactory>();
            NScheduler::TSettings settings;
            settings.Scheduler.Mode = mode;
            settings.Scheduler.WorkerCount = 2;
            settings.Queue.RowsetCapacityPerLane = 1;
            settings.HashShuffle.MaxQueuedRowsetsPerLane = 1;
            settings.HashShuffle.PartitionCount = 2;
            settings.HashShuffle.MaxPartitionCount = 2;
            auto plan = PlanJoinMany(
                "(rel join (rel join (rel source \"L\") (rel source \"R\") "
                "((lk rk)) (inner)) (rel source \"C\") ((rk ck)) (inner))",
                {{"L", &left}, {"R", &middle}, {"C", &third}},
                settings, bindings, [](const TOperatorPtr& root) {
                    auto& outer = static_cast<TJoinOperator&>(*root);
                    auto& first = static_cast<TJoinOperator&>(*outer.Left());
                    auto stats = [](uint64_t rows, const std::string& key,
                                     uint64_t ndv) {
                        auto value = std::make_shared<TStats>();
                        value->RowCount = rows;
                        auto column = std::make_shared<TStats::TColumnStats>();
                        column->Ndv = ndv;
                        value->ColumnStats[key] = std::move(column);
                        return value;
                    };
                    first.Left()->Stats_ = stats(100, "lk", 100);
                    first.Right()->Stats_ = stats(1'000'000, "rk", 1'000'000);
                    first.Stats_ = stats(3'000'000, "rk", 1'000'000);
                    outer.Right()->Stats_ = stats(1'000'000, "ck", 1'000'000);
                    uint32_t nextId = 1;
                    AttachRuntimeFilters(root, nextId);
                    ASSERT_TRUE(first.RuntimeFilter());
                    EXPECT_FALSE(outer.RuntimeFilter());
                    ASSERT_EQ(outer.Right()->RuntimeFilterUses.size(), 1u);
                    EXPECT_EQ(outer.Right()->RuntimeFilterUses[0].Id,
                        first.RuntimeFilter()->Id);
                });
            std::vector<std::tuple<int64_t, int64_t, int64_t>> got;
            TRowSet out{};
            while (plan->Next(out)) {
                for (int64_t i = 0; i < out.RowCount; ++i) {
                    if (out.Selection && !out.Selection[i]) {
                        continue;
                    }
                    for (size_t key : {0u, 2u, 4u}) {
                        EXPECT_TRUE(!out.Columns[key].Mask
                            || ((out.Columns[key].Mask[i / 8] >> (i % 8)) & 1));
                        EXPECT_EQ(reinterpret_cast<const int64_t*>(
                            out.Columns[key].Data)[i], 1);
                    }
                    got.emplace_back(
                        reinterpret_cast<const int64_t*>(out.Columns[1].Data)[i],
                        reinterpret_cast<const int64_t*>(out.Columns[3].Data)[i],
                        reinterpret_cast<const int64_t*>(out.Columns[5].Data)[i]);
                }
                Release(&out);
            }
            std::vector<std::tuple<int64_t, int64_t, int64_t>> expected;
            if (!empty) {
                for (auto l : {10, 20}) {
                    for (auto r : {100, 200}) {
                        for (auto c : {1000, 2000, 3000}) {
                            expected.emplace_back(l, r, c);
                        }
                    }
                }
            }
            std::sort(got.begin(), got.end());
            std::sort(expected.begin(), expected.end());
            EXPECT_EQ(got, expected);
            ASSERT_EQ(bindings->Created, 1u);
            ASSERT_TRUE(bindings->Filter);
            if (!empty) {
                // A NULL hash can pass the domain as a false positive; the
                // ordinary joins above must still reject its NULL matches.
                EXPECT_EQ(bindings->Filter->StatsSnapshot().RejectedRows, 3u);
            }
        }
    }
}

TEST(JoinPlanner, LeftOuterRuntimeFilterRunsBeforeGroupedAggregate) {
    for (auto mode : {NScheduler::EExecutionMode::SingleThreadedScheduler,
             NScheduler::EExecutionMode::ThreadedScheduler}) {
        SCOPED_TRACE(static_cast<int>(mode));
        std::vector<int64_t> lk = {1, 4}, lv = {10, 40};
        std::vector<int64_t> rk = {1, 1, 3, 3}, rv = {10, 20, 30, 40};
        std::vector<TColumn> lcols, rcols;
        TMockSource left({"lk", "lv"},
            {KeyValBatch(lk.data(), lv.data(), 2, lcols)});
        TMockSource right({"rk", "rv"},
            {KeyValBatch(rk.data(), rv.data(), 4, rcols)});
        auto bindings = std::make_shared<TCountingFilterBindingFactory>();
        NScheduler::TSettings settings;
        settings.Scheduler.Mode = mode;
        settings.Scheduler.WorkerCount = 2;
        settings.Queue.RowsetCapacityPerLane = 1;
        settings.HashShuffle.MaxQueuedRowsetsPerLane = 1;
        settings.HashShuffle.PartitionCount = 2;
        settings.HashShuffle.MaxPartitionCount = 2;

        auto plan = PlanJoin(
            "(rel join (rel source \"L\") "
            "(rel aggregate (rel source \"R\") (keys rk) (agg total sum rv)) "
            "((lk rk)) (left) (emit-filter 8 left))",
            left, right, settings, bindings);

        std::vector<std::tuple<int64_t, bool, int64_t>> got;
        TRowSet out{};
        while (plan->Next(out)) {
            for (int64_t i = 0; i < out.RowCount; ++i) {
                if (out.Selection && !out.Selection[i]) {
                    continue;
                }
                const bool matched = !out.Columns[2].Mask
                    || ((out.Columns[2].Mask[i / 8] >> (i % 8)) & 1);
                got.emplace_back(
                    reinterpret_cast<const int64_t*>(out.Columns[0].Data)[i],
                    matched,
                    matched
                        ? reinterpret_cast<const int64_t*>(out.Columns[3].Data)[i]
                        : 0);
            }
            Release(&out);
        }
        std::sort(got.begin(), got.end());
        EXPECT_EQ(got,
            (std::vector<std::tuple<int64_t, bool, int64_t>>{
                {1, true, 30}, {4, false, 0}}));
        ASSERT_TRUE(bindings->Filter);
        EXPECT_EQ(bindings->Filter->StatsSnapshot().ProbeRows, 4u);
        EXPECT_EQ(bindings->Filter->StatsSnapshot().RejectedRows, 2u);
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
    ASSERT_TRUE(bindings->Filter);
    EXPECT_EQ(bindings->Filter->StatsSnapshot().ProbeRows, 3u);
    EXPECT_EQ(bindings->Filter->StatsSnapshot().RejectedRows, 2u);
    EXPECT_EQ(bindings->Filter->StatsSnapshot().SkippedProbeRows, 0u);
}

TEST(JoinPlanner, RuntimeFilterCrossesOnlyDirectProjectKeyLineage) {
    for (const auto& [expression, reachesSource] :
         {std::pair{"lk", true}, std::pair{"(+ lk 0)", false}}) {
        SCOPED_TRACE(expression);
        std::vector<int64_t> lk = {1, 2, 3}, lv = {10, 20, 30};
        std::vector<int64_t> rk = {1}, rv = {100};
        std::vector<TColumn> lcols, rcols;
        TMockSource left({"lk", "lv"},
            {KeyValBatch(lk.data(), lv.data(), 3, lcols)});
        TMockSource right({"rk", "rv"},
            {KeyValBatch(rk.data(), rv.data(), 1, rcols)});
        auto source = std::make_shared<TSourceOperator>(left, "L");
        auto projected = MakeProject(source,
            {{"lv", "lv"}, {"alias_key", expression}});
        ASSERT_TRUE(projected);
        auto join = std::make_shared<TJoinOperator>(*projected,
            std::make_shared<TSourceOperator>(right, "R"),
            std::vector<TJoinKey>{{"alias_key", "rk"}},
            EJoinType::Inner, nullptr);
        join->MutableRuntimeFilter() = TRuntimeFilterSpec{
            .Id = 7, .BuildSide = EJoinFilterSide::Right};
        TOperatorPtr root = join;
        AnnotateTypes(root);
        PlaceRuntimeFilterConsumers(root);

        ASSERT_EQ(source->RuntimeFilterUses.size(), reachesSource ? 1u : 0u);
        ASSERT_EQ((*projected)->RuntimeFilterUses.size(), reachesSource ? 0u : 1u);
        const auto& use = reachesSource
            ? source->RuntimeFilterUses.front()
            : (*projected)->RuntimeFilterUses.front();
        EXPECT_EQ(use.ProbeKeys,
            (std::vector<std::string>{reachesSource ? "lk" : "alias_key"}));

        auto bindings = std::make_shared<TCountingFilterBindingFactory>();
        auto plan = RunPlan(root, {}, nullptr, bindings);
        std::vector<int64_t> keys;
        TRowSet out{};
        while (plan->Next(out)) {
            for (int64_t row = 0; row < out.RowCount; ++row) {
                if (!out.Selection || out.Selection[row]) {
                    keys.push_back(reinterpret_cast<const int64_t*>(
                        out.Columns[1].Data)[row]);
                }
            }
            Release(&out);
        }
        EXPECT_EQ(keys, (std::vector<int64_t>{1}));
        ASSERT_TRUE(bindings->Filter);
        EXPECT_EQ(bindings->Filter->StatsSnapshot().ProbeRows, 3u);
        EXPECT_EQ(bindings->Filter->StatsSnapshot().RejectedRows, 2u);
    }
}

TEST(JoinPlanner, RuntimeFilterTaskDropsEntireBatch) {
    for (auto mode : {NScheduler::EExecutionMode::SingleThreadedScheduler,
             NScheduler::EExecutionMode::ThreadedScheduler}) {
        SCOPED_TRACE(static_cast<int>(mode));
        std::vector<int64_t> lk = {2, 3}, lv = {20, 30};
        std::vector<int64_t> rk = {1}, rv = {100};
        std::vector<TColumn> lcols, rcols;
        TMockSource left({"lk", "lv"}, {KeyValBatch(lk.data(), lv.data(), 2, lcols)});
        TMockSource right({"rk", "rv"}, {KeyValBatch(rk.data(), rv.data(), 1, rcols)});
        auto bindings = std::make_shared<TCountingFilterBindingFactory>();
        NScheduler::TSettings settings;
        settings.Scheduler.Mode = mode;
        settings.Scheduler.WorkerCount = 2;
        auto plan = PlanJoin(
            "(rel join (rel source \"L\") (rel source \"R\") "
            "((lk rk)) (inner) (emit-filter 7 right))",
            left, right, settings, bindings);

        TRowSet out{};
        EXPECT_FALSE(plan->Next(out));
        ASSERT_TRUE(bindings->Filter);
        EXPECT_EQ(bindings->Filter->StatsSnapshot().ProbeRows, 2u);
        EXPECT_EQ(bindings->Filter->StatsSnapshot().RejectedRows, 2u);
    }
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
    EXPECT_EQ(bindings->Filter->StatsSnapshot().ProbeRows, 4u);
    EXPECT_EQ(bindings->Filter->StatsSnapshot().RejectedRows, 2u);
}

TEST(JoinPlanner, RuntimeFilterUsesEachUnionBranchKeyName) {
    std::vector<int64_t> lk1 = {1, 2}, lv1 = {10, 20};
    std::vector<int64_t> lk2 = {3, 4}, lv2 = {30, 40};
    std::vector<int64_t> lk3 = {5, 6}, lv3 = {50, 60};
    std::vector<int64_t> rk = {1, 3, 5}, rv = {100, 300, 500};
    std::vector<TColumn> lc1, lc2, lc3, rc;
    TMockSource left1({"lv", "lk"},
        {KeyValBatch(lv1.data(), lk1.data(), 2, lc1)});
    TMockSource left2({"alternate_v", "alternate_k"},
        {KeyValBatch(lv2.data(), lk2.data(), 2, lc2)});
    TMockSource left3({"raw_k", "raw_v"},
        {KeyValBatch(lk3.data(), lv3.data(), 2, lc3)});
    TMockSource right({"rk", "rv"},
        {KeyValBatch(rk.data(), rv.data(), 3, rc)});
    auto first = std::make_shared<TSourceOperator>(left1, "L1");
    auto second = std::make_shared<TSourceOperator>(left2, "L2");
    auto third = std::make_shared<TSourceOperator>(left3, "L3");
    auto computed = MakeProject(third,
        {{"raw_v", "raw_v"}, {"computed_k", "(+ raw_k 0)"}});
    ASSERT_TRUE(computed);
    auto unionAll = std::make_shared<TUnionAllOperator>(
        std::vector<TOperatorPtr>{first, second, *computed});
    auto join = std::make_shared<TJoinOperator>(unionAll,
        std::make_shared<TSourceOperator>(right, "R"),
        std::vector<TJoinKey>{{"lk", "rk"}}, EJoinType::Inner, nullptr);
    join->MutableRuntimeFilter() = TRuntimeFilterSpec{
        .Id = 7, .BuildSide = EJoinFilterSide::Right};
    TOperatorPtr root = join;
    AnnotateTypes(root);
    PlaceRuntimeFilterConsumers(root);

    ASSERT_TRUE(unionAll->RuntimeFilterUses.empty());
    ASSERT_EQ(first->RuntimeFilterUses.size(), 1u);
    ASSERT_EQ(second->RuntimeFilterUses.size(), 1u);
    ASSERT_EQ((*computed)->RuntimeFilterUses.size(), 1u);
    EXPECT_TRUE(third->RuntimeFilterUses.empty());
    EXPECT_EQ(first->RuntimeFilterUses[0].ProbeKeys,
        (std::vector<std::string>{"lk"}));
    EXPECT_EQ(second->RuntimeFilterUses[0].ProbeKeys,
        (std::vector<std::string>{"alternate_k"}));
    EXPECT_EQ((*computed)->RuntimeFilterUses[0].ProbeKeys,
        (std::vector<std::string>{"computed_k"}));

    NScheduler::TSettings settings;
    settings.Scheduler.Mode = NScheduler::EExecutionMode::ThreadedScheduler;
    settings.Scheduler.WorkerCount = 4;
    settings.HashShuffle.PartitionCount = 2;
    settings.HashShuffle.MaxPartitionCount = 2;
    auto bindings = std::make_shared<TCountingFilterBindingFactory>();
    auto plan = RunPlan(root, settings, nullptr, bindings);
    std::vector<int64_t> keys;
    TRowSet out{};
    while (plan->Next(out)) {
        for (int64_t row = 0; row < out.RowCount; ++row) {
            if (!out.Selection || out.Selection[row]) {
                keys.push_back(reinterpret_cast<const int64_t*>(
                    out.Columns[1].Data)[row]);
            }
        }
        Release(&out);
    }
    std::sort(keys.begin(), keys.end());
    EXPECT_EQ(keys, (std::vector<int64_t>{1, 3, 5}));
    ASSERT_TRUE(bindings->Filter);
    EXPECT_EQ(bindings->Filter->StatsSnapshot().ProbeRows, 6u);
    EXPECT_EQ(bindings->Filter->StatsSnapshot().RejectedRows, 3u);
}

TEST(JoinPlanner, RuntimeFilterCrossesOnlySameSideInnerJoinKeys) {
    for (const auto& [nestedType, parentKey, matchKey, targetSide, mode] : {
             std::tuple{EJoinType::Inner, "av", int64_t{10}, 0,
                 NScheduler::EExecutionMode::SingleThreadedScheduler},
             std::tuple{EJoinType::Inner, "av", int64_t{10}, 0,
                 NScheduler::EExecutionMode::ThreadedScheduler},
             std::tuple{EJoinType::Inner, "bv", int64_t{100}, 1,
                 NScheduler::EExecutionMode::ThreadedScheduler},
             std::tuple{EJoinType::Left, "bv", int64_t{100}, 2,
                 NScheduler::EExecutionMode::ThreadedScheduler}}) {
        SCOPED_TRACE(parentKey);
        SCOPED_TRACE(JoinTypeName(nestedType));
        SCOPED_TRACE(static_cast<int>(mode));
        std::vector<int64_t> ak = {1, 2, 3}, av = {10, 20, 30};
        std::vector<int64_t> bk = {1, 2, 3}, bv = {100, 200, 300};
        std::vector<int64_t> rk = {matchKey}, rv = {1000};
        std::vector<TColumn> ac, bc, rc;
        TMockSource a({"ak", "av"}, {KeyValBatch(ak.data(), av.data(), 3, ac)});
        TMockSource b({"bk", "bv"}, {KeyValBatch(bk.data(), bv.data(), 3, bc)});
        TMockSource r({"rk", "rv"}, {KeyValBatch(rk.data(), rv.data(), 1, rc)});
        auto aSource = std::make_shared<TSourceOperator>(a, "A");
        auto bSource = std::make_shared<TSourceOperator>(b, "B");
        auto nested = std::make_shared<TJoinOperator>(aSource, bSource,
            std::vector<TJoinKey>{{"ak", "bk"}}, nestedType, nullptr);
        if (nestedType == EJoinType::Inner) {
            nested->MutableRuntimeFilter() = TRuntimeFilterSpec{
                .Id = 8, .BuildSide = EJoinFilterSide::Right};
        }
        auto parent = std::make_shared<TJoinOperator>(nested,
            std::make_shared<TSourceOperator>(r, "R"),
            std::vector<TJoinKey>{{parentKey, "rk"}},
            EJoinType::Inner, nullptr);
        parent->MutableRuntimeFilter() = TRuntimeFilterSpec{
            .Id = 7, .BuildSide = EJoinFilterSide::Right};
        TOperatorPtr root = parent;
        AnnotateTypes(root);
        PlaceRuntimeFilterConsumers(root);

        auto hasUse = [](const TOperatorPtr& op, uint32_t id) {
            return std::any_of(op->RuntimeFilterUses.begin(),
                op->RuntimeFilterUses.end(),
                [&](const auto& use) { return use.Id == id; });
        };
        EXPECT_EQ(hasUse(aSource, 7), targetSide == 0);
        EXPECT_EQ(hasUse(bSource, 7), targetSide == 1);
        EXPECT_EQ(hasUse(nested, 7), targetSide == 2);
        EXPECT_EQ(hasUse(aSource, 8), nestedType == EJoinType::Inner);

        NScheduler::TSettings settings;
        settings.Scheduler.Mode = mode;
        settings.Scheduler.WorkerCount = mode
                == NScheduler::EExecutionMode::SingleThreadedScheduler
            ? 1 : 4;
        settings.HashShuffle.PartitionCount = 4;
        settings.HashShuffle.MaxPartitionCount = 4;
        auto bindings = std::make_shared<TCountingFilterBindingFactory>();
        auto plan = RunPlan(root, settings, nullptr, bindings);
        std::vector<int64_t> keys;
        TRowSet out{};
        while (plan->Next(out)) {
            for (int64_t row = 0; row < out.RowCount; ++row) {
                if (!out.Selection || out.Selection[row]) {
                    keys.push_back(reinterpret_cast<const int64_t*>(
                        out.Columns[0].Data)[row]);
                }
            }
            Release(&out);
        }
        EXPECT_EQ(keys, (std::vector<int64_t>{1}));
        EXPECT_EQ(bindings->Created,
            nestedType == EJoinType::Inner ? 2u : 1u);
        ASSERT_TRUE(bindings->Filter);
        EXPECT_EQ(bindings->Filter->StatsSnapshot().ProbeRows, 3u);
        EXPECT_EQ(bindings->Filter->StatsSnapshot().RejectedRows, 2u);
    }
}

TEST(JoinPlanner, RuntimeFilterStaysAboveInnerJoinForSplitKeys) {
    TMockSource a({"ak"}), b({"bk", "bv"}), r({"rk1", "rk2"});
    auto aSource = std::make_shared<TSourceOperator>(a, "A");
    auto bSource = std::make_shared<TSourceOperator>(b, "B");
    auto nested = std::make_shared<TJoinOperator>(aSource, bSource,
        std::vector<TJoinKey>{{"ak", "bk"}}, EJoinType::Inner, nullptr);
    auto parent = std::make_shared<TJoinOperator>(nested,
        std::make_shared<TSourceOperator>(r, "R"),
        std::vector<TJoinKey>{{"ak", "rk1"}, {"bv", "rk2"}},
        EJoinType::Inner, nullptr);
    parent->MutableRuntimeFilter() = TRuntimeFilterSpec{
        .Id = 7, .BuildSide = EJoinFilterSide::Right};
    TOperatorPtr root = parent;
    AnnotateTypes(root);
    PlaceRuntimeFilterConsumers(root);

    ASSERT_EQ(nested->RuntimeFilterUses.size(), 1u);
    EXPECT_EQ(nested->RuntimeFilterUses[0].ProbeKeys,
        (std::vector<std::string>{"ak", "bv"}));
    EXPECT_TRUE(aSource->RuntimeFilterUses.empty());
    EXPECT_TRUE(bSource->RuntimeFilterUses.empty());
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
