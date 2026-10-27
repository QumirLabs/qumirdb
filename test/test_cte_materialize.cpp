#include <gtest/gtest.h>
#include "mock_source.h"
#include "plan_runner.h"

#include <qumir/codegen/llvm/llvm_initializer.h>

#include <qdb/plan/build.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/pipeline.h>
#include <qdb/scheduler/graph.h>
#include <qdb/scheduler/plan_lowerer.h>
#include <qdb/scheduler/scan_split.h>
#include <qdb/sql/parser.h>

#include <algorithm>
#include <cstdint>
#include <expected>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace NQdb;

namespace {

// Backing storage for an in-memory source split across several batches; the
// TRowSet columns point into Keys/Vals, so it must outlive the run.
struct TTable {
    std::vector<int64_t> Keys;
    std::vector<int64_t> Vals;
    std::vector<std::vector<TColumn>> Columns;
    std::vector<TRowSet> Batches;
};

std::unique_ptr<TTable> MakeTable(int64_t rows, size_t batches) {
    auto t = std::make_unique<TTable>();
    for (int64_t i = 0; i < rows; ++i) {
        t->Keys.push_back(i + 1);
        t->Vals.push_back((i + 1) * 10);
    }
    t->Columns.resize(batches);
    const size_t perBatch = batches ? (t->Keys.size() + batches - 1) / batches : 0;
    for (size_t b = 0; b < batches; ++b) {
        const size_t off = b * perBatch;
        if (off >= t->Keys.size() && b > 0) {
            t->Columns[b] = {};
            continue;
        }
        const size_t count = std::min(perBatch, t->Keys.size() - std::min(off, t->Keys.size()));
        t->Columns[b] = {
            TColumn{.Data = reinterpret_cast<char*>(t->Keys.data() + off)},
            TColumn{.Data = reinterpret_cast<char*>(t->Vals.data() + off)},
        };
        t->Batches.push_back(TRowSet{
            .Columns = t->Columns[b].data(),
            .ColumnCount = 2,
            .RowCount = static_cast<int64_t>(count),
            .Selection = nullptr,
            .Destroy = nullptr,
            .Private = nullptr,
            .RefCount = 1,
        });
    }
    return t;
}

TOperatorPtr BuildSqlPlan(
    const std::string& sql,
    const std::unordered_map<std::string, ISource*>& sources)
{
    std::istringstream in(sql);
    NSql::TTokenStream ts(in);
    NSql::TParser parser;
    auto parsed = parser.Parse(ts);
    if (!parsed) {
        throw std::runtime_error(parsed.error().ToString());
    }
    auto root = BuildPlan(*parsed, [&](std::string_view table)
        -> std::expected<TOperatorPtr, NQumir::TError>
    {
        auto it = sources.find(std::string(table));
        if (it == sources.end()) {
            return std::unexpected(NQumir::TError("unknown table: " + std::string(table)));
        }
        return std::make_shared<TSourceOperator>(*it->second, std::string(table));
    });
    if (!root) {
        throw std::runtime_error(root.error().ToString());
    }
    TOperatorPtr plan = *root;
    ApplyPlanPasses(plan);
    return plan;
}

std::unique_ptr<TTestRuntime> RunSql(
    const std::string& sql,
    const std::unordered_map<std::string, ISource*>& sources,
    NScheduler::TSettings settings = {})
{
    return RunPlan(BuildSqlPlan(sql, sources), settings);
}

std::shared_ptr<TFilterOperator> FindFilter(const TOperatorPtr& op) {
    if (auto filter = TMaybeOp<TFilterOperator>(op)) {
        return filter.Cast();
    }
    for (const auto& child : op->Children()) {
        if (auto childOp = NQumir::NAst::TMaybeNode<IOperator>(child)) {
            if (auto found = FindFilter(childOp.Cast())) {
                return found;
            }
        }
    }
    return nullptr;
}

TJoinOperator* FindJoin(const TOperatorPtr& op) {
    if (auto join = TMaybeOp<TJoinOperator>(op)) {
        return join.Cast().get();
    }
    for (const auto& child : op->Inputs()) {
        if (auto* join = FindJoin(child)) {
            return join;
        }
    }
    return nullptr;
}

int64_t CountRows(TTestRuntime& runtime) {
    int64_t total = 0;
    TRowSet batch{};
    while (runtime.Next(batch)) {
        total += batch.RowCount;
        Release(&batch);
        batch = {};
    }
    return total;
}

// Collects the first int64 column, honouring the selection mask, sorted.
std::vector<int64_t> CollectFirstColumnSorted(TTestRuntime& runtime) {
    std::vector<int64_t> out;
    TRowSet batch{};
    while (runtime.Next(batch)) {
        const auto* data = reinterpret_cast<const int64_t*>(batch.Columns[0].Data);
        for (int64_t i = 0; i < batch.RowCount; ++i) {
            if (!batch.Selection || batch.Selection[i]) {
                out.push_back(data[i]);
            }
        }
        Release(&batch);
        batch = {};
    }
    std::sort(out.begin(), out.end());
    return out;
}

// Four row groups give the source four scan lanes. The lanes share one batch
// cursor, so threaded lanes can read it concurrently.
class TSplitSource final
    : public TMockSource
    , public NScheduler::IScanMetadataSource
{
public:
    using TMockSource::TMockSource;

    bool Next(TRowSet& rowSet) override {
        std::lock_guard lock(Mutex_);
        return TMockSource::Next(rowSet);
    }

    std::vector<NScheduler::TScanRowGroup> ScanRowGroups() const override {
        return {
            {.RowGroup = 0, .RowCount = 10, .ByteSize = 160},
            {.RowGroup = 1, .RowCount = 10, .ByteSize = 160},
            {.RowGroup = 2, .RowCount = 10, .ByteSize = 160},
            {.RowGroup = 3, .RowCount = 10, .ByteSize = 160},
        };
    }

private:
    std::mutex Mutex_;
};

size_t CountTasks(const NScheduler::TTaskGraph& graph, std::string_view kind) {
    return std::ranges::count_if(graph.Nodes(), [&](const auto& node) {
        return node->DebugKind == kind;
    });
}

} // namespace

// A source drained more than once yields nothing the second time, so a correct
// self-join result over a single-use mock proves the definition was materialized
// once (inlining would leave the second reference empty -> 0 join rows).
TEST(CteMaterialize, DiamondSelfJoinRunsDefinitionOnce) {
    auto table = MakeTable(/*rows=*/12, /*batches=*/3);
    TMockSource t({"k", "v"}, std::move(table->Batches));
    std::unordered_map<std::string, ISource*> sources = {{"t", &t}};

    auto runtime = RunSql(
        "WITH x AS (SELECT k, v FROM t) "
        "SELECT p.k, q.v FROM x p JOIN x q ON p.k = q.k",
        sources);
    EXPECT_EQ(CountRows(*runtime), 12);
}

// The diamond is the classic multicast-deadlock shape; with a single-slot queue
// the blocking spool must still complete (producer never blocks on a consumer).
TEST(CteMaterialize, DiamondCapacityOneNoDeadlock) {
    auto table = MakeTable(/*rows=*/40, /*batches=*/20);
    TMockSource t({"k", "v"}, std::move(table->Batches));
    std::unordered_map<std::string, ISource*> sources = {{"t", &t}};

    NScheduler::TSettings settings;
    settings.Queue.RowsetCapacityPerLane = 1;

    auto runtime = RunSql(
        "WITH x AS (SELECT k, v FROM t) "
        "SELECT p.k, q.v FROM x p JOIN x q ON p.k = q.k",
        sources, settings);
    EXPECT_EQ(CountRows(*runtime), 40);
}

TEST(CteMaterialize, DiamondCapacityOneThreadedNoDeadlock) {
    auto table = MakeTable(/*rows=*/40, /*batches=*/20);
    TMockSource t({"k", "v"}, std::move(table->Batches));
    std::unordered_map<std::string, ISource*> sources = {{"t", &t}};

    NScheduler::TSettings settings;
    settings.Queue.RowsetCapacityPerLane = 1;
    settings.Scheduler.Mode = NScheduler::EExecutionMode::ThreadedScheduler;
    settings.Scheduler.WorkerCount = 4;

    auto runtime = RunSql(
        "WITH x AS (SELECT k, v FROM t) "
        "SELECT p.k, q.v FROM x p JOIN x q ON p.k = q.k",
        sources, settings);
    EXPECT_EQ(CountRows(*runtime), 40);
}

TEST(CteMaterialize, RuntimeFilterAfterSharedReaderDoesNotDeadlock) {
    for (bool threaded : {false, true}) {
        auto table = MakeTable(/*rows=*/40, /*batches=*/20);
        TMockSource t({"k", "v"}, std::move(table->Batches));
        std::unordered_map<std::string, ISource*> sources = {{"t", &t}};
        auto plan = BuildSqlPlan(
            "WITH x AS (SELECT k, v FROM t) "
            "SELECT p.k FROM x p JOIN "
            "(SELECT k FROM x WHERE k <= 10) q ON p.k = q.k",
            sources);
        auto* join = FindJoin(plan);
        ASSERT_NE(join, nullptr);
        join->MutableRuntimeFilter() = TRuntimeFilterSpec{
            .Id = 1, .BuildSide = EJoinFilterSide::Right};

        NScheduler::TSettings settings;
        settings.Queue.RowsetCapacityPerLane = 1;
        if (threaded) {
            settings.Scheduler.Mode =
                NScheduler::EExecutionMode::ThreadedScheduler;
            settings.Scheduler.WorkerCount = 4;
        }
        auto runtime = RunPlan(plan, settings);
        auto keys = CollectFirstColumnSorted(*runtime);
        std::vector<int64_t> expected;
        for (int64_t key = 1; key <= 10; ++key) {
            expected.push_back(key);
        }
        EXPECT_EQ(keys, expected) << "threaded=" << threaded;
    }
}

// Every lane of the definition writes its own spool segment, and every
// reference reads the spool on as many lanes, each batch on one of them.
TEST(CteMaterialize, ParallelSpoolKeepsDefinitionLanes) {
    for (bool threaded : {false, true}) {
        auto table = MakeTable(/*rows=*/40, /*batches=*/20);
        TSplitSource t({"k", "v"}, std::move(table->Batches));
        std::unordered_map<std::string, ISource*> sources = {{"t", &t}};
        const std::string sql =
            "WITH x AS (SELECT k, v FROM t WHERE v > 0) "
            "SELECT k FROM x UNION ALL SELECT k FROM x";

        NScheduler::TSettings settings;
        settings.Queue.RowsetCapacityPerLane = 1;
        settings.ScanSplit.Strategy = NScheduler::EScanSplitStrategy::RowGroupRange;
        settings.ScanSplit.MaxScanTasks = 4;
        if (threaded) {
            settings.Scheduler.Mode = NScheduler::EExecutionMode::ThreadedScheduler;
            settings.Scheduler.WorkerCount = 4;
        }
        auto lowered = NScheduler::LowerPlanToGraph(
            BuildSqlPlan(sql, sources), settings, nullptr);
        EXPECT_EQ(CountTasks(*lowered.Graph, "source"), 4u);
        EXPECT_EQ(CountTasks(*lowered.Graph, "cte-producer"), 4u);
        EXPECT_EQ(CountTasks(*lowered.Graph, "cte-consumer"), 8u);
        EXPECT_EQ(lowered.Lanes, 8u);

        auto runtime = RunSql(sql, sources, settings);
        std::vector<int64_t> expected;
        for (int64_t key = 1; key <= 40; ++key) {
            expected.push_back(key);
            expected.push_back(key);
        }
        EXPECT_EQ(CollectFirstColumnSorted(*runtime), expected)
            << "threaded=" << threaded;
    }
}

TEST(CteMaterialize, EmptyCte) {
    auto table = MakeTable(/*rows=*/0, /*batches=*/1);
    TMockSource t({"k", "v"}, std::move(table->Batches));
    std::unordered_map<std::string, ISource*> sources = {{"t", &t}};

    auto runtime = RunSql(
        "WITH x AS (SELECT k, v FROM t) "
        "SELECT p.k, q.v FROM x p JOIN x q ON p.k = q.k",
        sources);
    EXPECT_EQ(CountRows(*runtime), 0);
}

// b is materialized (referenced twice); a is inlined inside b's producer plan,
// so a's single-use source is still scanned exactly once.
TEST(CteMaterialize, NestedMaterializedProducer) {
    auto table = MakeTable(/*rows=*/12, /*batches=*/3);
    TMockSource t({"k", "v"}, std::move(table->Batches));
    std::unordered_map<std::string, ISource*> sources = {{"t", &t}};

    auto runtime = RunSql(
        "WITH a AS (SELECT k, v FROM t), b AS (SELECT k, v FROM a) "
        "SELECT p.k, q.v FROM b p JOIN b q ON p.k = q.k",
        sources);
    EXPECT_EQ(CountRows(*runtime), 12);
}

// Both UNION ALL branches read the full shared spool independently.
TEST(CteMaterialize, UnionAllTwoConsumers) {
    auto table = MakeTable(/*rows=*/12, /*batches=*/3);
    TMockSource t({"k", "v"}, std::move(table->Batches));
    std::unordered_map<std::string, ISource*> sources = {{"t", &t}};

    auto runtime = RunSql(
        "WITH x AS (SELECT k FROM t) "
        "SELECT k FROM x UNION ALL SELECT k FROM x",
        sources);
    EXPECT_EQ(CountRows(*runtime), 24);
}

// One consumer feeds a sort (a potentially in-place operator); it must not
// corrupt the batches the other consumer streams from the shared spool.
TEST(CteMaterialize, SortBranchDoesNotCorruptSpool) {
    auto table = MakeTable(/*rows=*/12, /*batches=*/3);
    TMockSource t({"k", "v"}, std::move(table->Batches));
    std::unordered_map<std::string, ISource*> sources = {{"t", &t}};

    auto runtime = RunSql(
        "WITH x AS (SELECT k FROM t) "
        "SELECT k FROM x "
        "UNION ALL "
        "SELECT k FROM (SELECT k FROM x ORDER BY k) s",
        sources);
    EXPECT_EQ(CountRows(*runtime), 24);
}

TEST(CteMaterialize, ChainedSelfJoinHonorsFilteredConsumerSelection) {
    std::vector<int64_t> keys = {1, 1, 1, 1};
    std::vector<int64_t> rn = {1, 2, 3, 4};
    std::vector<TColumn> cols = {
        TColumn{.Data = reinterpret_cast<char*>(keys.data())},
        TColumn{.Data = reinterpret_cast<char*>(rn.data())},
    };
    TRowSet batch{
        .Columns = cols.data(),
        .ColumnCount = 2,
        .RowCount = 4,
        .RefCount = 1,
    };
    TMockSource t({"k", "rn"}, {batch});
    std::unordered_map<std::string, ISource*> sources = {{"t", &t}};

    auto runtime = RunSql(R"sql(
WITH x AS (SELECT k, rn FROM t)
SELECT v1.rn
FROM x v1
JOIN x lag ON v1.k = lag.k AND v1.rn = lag.rn + 1
JOIN x lead ON v1.k = lead.k AND v1.rn = lead.rn - 1
WHERE v1.rn = 2 OR v1.rn = 3
)sql", sources);
    EXPECT_EQ(CountRows(*runtime), 2);
}

// Regression test for filter selection ownership. The CTE is materialized (the
// self-join references it twice), so each filtered batch is parked in the spool
// while the next batch is filtered, reusing the kernel's selection scratch. If a
// parked batch does not own its selection mask, the next batch overwrites it and
// the join returns wrong rows. Two batches are needed to trigger the reuse; the
// expected result is exactly the rows the filter kept.
void RunFilteredCteSelfJoin(NScheduler::TSettings settings) {
    std::vector<int64_t> id1 = {10, 11};
    std::vector<int64_t> flag1 = {1, 0};
    std::vector<int64_t> id2 = {20, 21};
    std::vector<int64_t> flag2 = {0, 1};
    std::vector<TColumn> cols1 = {
        TColumn{.Data = reinterpret_cast<char*>(id1.data())},
        TColumn{.Data = reinterpret_cast<char*>(flag1.data())},
    };
    std::vector<TColumn> cols2 = {
        TColumn{.Data = reinterpret_cast<char*>(id2.data())},
        TColumn{.Data = reinterpret_cast<char*>(flag2.data())},
    };
    std::vector<TRowSet> batches = {
        TRowSet{.Columns = cols1.data(), .ColumnCount = 2, .RowCount = 2, .RefCount = 1},
        TRowSet{.Columns = cols2.data(), .ColumnCount = 2, .RowCount = 2, .RefCount = 1},
    };
    TMockSource t({"id", "flag"}, std::move(batches));
    std::unordered_map<std::string, ISource*> sources = {{"t", &t}};

    auto runtime = RunSql(
        "WITH x AS (SELECT id FROM t WHERE flag = 1) "
        "SELECT a.id FROM x a JOIN x b ON a.id = b.id",
        sources, settings);
    EXPECT_EQ(CollectFirstColumnSorted(*runtime), (std::vector<int64_t>{10, 21}));
}

TEST(CteMaterialize, FilteredCteSelfJoinPrunesAndKeepsSelection) {
    RunFilteredCteSelfJoin({});
}

// Capacity 1 interleaves the batches, stressing selection reuse.
TEST(CteMaterialize, FilteredCteSelfJoinCapacityOne) {
    NScheduler::TSettings settings;
    settings.Queue.RowsetCapacityPerLane = 1;
    RunFilteredCteSelfJoin(settings);
}

// Empty filter demand (COUNT(*) WHERE) keeps one technical column; id before
// flag exercises ordering.
TEST(CteMaterialize, CountStarWithFilterKeepsTechnicalColumn) {
    std::vector<int64_t> id = {10, 11, 20, 21};
    std::vector<int64_t> flag = {1, 0, 0, 1};
    std::vector<TColumn> cols = {
        TColumn{.Data = reinterpret_cast<char*>(id.data())},
        TColumn{.Data = reinterpret_cast<char*>(flag.data())},
    };
    TRowSet batch{
        .Columns = cols.data(), .ColumnCount = 2, .RowCount = 4, .RefCount = 1};
    TMockSource t({"id", "flag"}, {batch});
    std::unordered_map<std::string, ISource*> sources = {{"t", &t}};

    auto runtime = RunSql("SELECT COUNT(*) FROM t WHERE flag = 1", sources);
    EXPECT_EQ(CollectFirstColumnSorted(*runtime), (std::vector<int64_t>{2}));
}

// Proves the pruning actually fired: `flag` is gone from the filter's output
// schema (only the predicate uses it) but stays in its input schema.
TEST(CteMaterialize, FilterDropsPredicateOnlyColumnFromOutput) {
    TMockSource t({"id", "flag"});
    std::unordered_map<std::string, ISource*> sources = {{"t", &t}};
    auto plan = BuildSqlPlan("SELECT id FROM t WHERE flag = 1", sources);

    auto filter = FindFilter(plan);
    ASSERT_TRUE(filter);

    auto has = [](const NQumir::NAst::TTypePtr& type, const std::string& needle) {
        auto* st = static_cast<NQumir::NAst::TStructType*>(type.get());
        for (const auto& [name, _] : st->Fields) {
            if (name.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    };

    EXPECT_FALSE(has(filter->OutputColumns(), "flag"));
    EXPECT_TRUE(has(filter->OutputColumns(), "id"));
    EXPECT_TRUE(has(filter->RequiredColumns(), "flag"));
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    NQumir::NCodeGen::TLLVMInitializer initializer;
    return RUN_ALL_TESTS();
}
