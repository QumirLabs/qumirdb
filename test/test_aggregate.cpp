#include <gtest/gtest.h>
#include "mock_source.h"

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>
#include <qumir/parser/core/printer.h>
#include <qumir/parser/type.h>

#include "plan_runner.h"
#include <qdb/exec/aggregate_exec.h>
#include <qdb/io/io.h>
#include <qdb/io/parquet/source.h>
#include <qdb/kernel/compiler.h>
#include <qdb/modules/qumirdb_runtime.h>
#include <qdb/plan/build.h>
#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/operator.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/passes/column_pruning.h>
#include <qdb/plan/passes/typing.h>
#include <qdb/plan/pipeline.h>
#include <qdb/sexp/parser.h>
#include <qdb/plan/types/decimal.h>
#include <qdb/plan/types/nullable.h>
#include <qdb/sql/parser.h>

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/writer.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace NQdb;
using namespace NQdb::NSexp;
using namespace NQumir::NAst::NCore;
using namespace NQumir::NAst;

namespace {

// Parses `sexp` as a (rel aggregate ...) plan whose (rel source "...") leaf is
// backed by `source`, runs AnnotateTypes + ApplyColumnPruning, and returns the
// resulting logical plan root.
TOperatorPtr ParsePlan(const std::string& sexp, ISource& source) {
    TRelParserOptions opts;
    opts.SourceFactory = [&](std::string_view path, NQumir::TLocation) -> TOperatorPtr {
        return std::make_shared<TSourceOperator>(source, std::string(path));
    };

    NQumir::NAst::NCore::TParser parser;
    for (auto& [name, fn] : MakeRelParsers(std::move(opts))) {
        parser.NodeParsers[name] = std::move(fn);
    }

    std::istringstream in(sexp);
    NQumir::NAst::NCore::TTokenStream ts(in);
    auto result = parser.Parse(ts);
    if (!result.has_value()) {
        throw std::runtime_error(result.error().ToString());
    }

    auto root = std::static_pointer_cast<IOperator>(result.value());
    AnnotateTypes(root);
    ApplyColumnPruning(root);
    return root;
}

TOperatorPtr BuildSqlPlan(const std::string& sql, const TTableSourceFactory& sources) {
    std::istringstream in(sql);
    NSql::TTokenStream tokens(in);
    NSql::TParser parser;
    auto parsed = parser.Parse(tokens);
    if (!parsed) {
        throw std::runtime_error(parsed.error().ToString());
    }
    auto plan = BuildPlan(*parsed, sources);
    if (!plan) {
        throw std::runtime_error(plan.error().ToString());
    }
    return std::move(*plan);
}

TOperatorPtr BuildSqlPlan(const std::string& sql, ISource& source) {
    return BuildSqlPlan(sql, [&](std::string_view table)
        -> std::expected<TOperatorPtr, NQumir::TError>
    {
        return std::make_shared<TSourceOperator>(source, std::string(table));
    });
}

std::shared_ptr<TAggregateOperator> FindAggregate(const TOperatorPtr& op) {
    if (auto aggregate = TMaybeOp<TAggregateOperator>(op)) {
        return aggregate.Cast();
    }
    for (const auto& child : op->Children()) {
        if (auto childOp = TMaybeNode<IOperator>(child)) {
            if (auto aggregate = FindAggregate(childOp.Cast())) {
                return aggregate;
            }
        }
    }
    return nullptr;
}

struct TGroupStats {
    int64_t Count = 0;
    int64_t Sum = 0;
    int64_t Min = 0;
    int64_t Max = 0;
    bool Seen = false;
};

std::unordered_map<int64_t, TGroupStats> ComputeReference(
    const std::vector<int64_t>& keys, const std::vector<int64_t>& vals) {
    std::unordered_map<int64_t, TGroupStats> reference;
    for (size_t i = 0; i < keys.size(); ++i) {
        auto& group = reference[keys[i]];
        if (!group.Seen) {
            group.Min = vals[i];
            group.Max = vals[i];
            group.Seen = true;
        } else {
            group.Min = std::min(group.Min, vals[i]);
            group.Max = std::max(group.Max, vals[i]);
        }
        group.Count += 1;
        group.Sum += vals[i];
    }
    return reference;
}

constexpr const char* PlanSexp =
    "(rel aggregate (rel source \"data.parquet\") (keys k) "
    "(agg c count) (agg s sum v) (agg mn min v) (agg mx max v))";

bool IsValid(const TColumn& column, int64_t row) {
    if (!column.Mask) {
        return true;
    }
    const int64_t bit = column.MaskBitOffset + row;
    return ((column.Mask[bit / 8] >> (bit % 8)) & 1) != 0;
}

std::string StringAt(const TColumn& column, int64_t row) {
    EXPECT_EQ(column.OffsetWidth, 8);
    const auto* offsets = static_cast<const int64_t*>(column.Offsets);
    return {column.Data + offsets[row], column.Data + offsets[row + 1]};
}

// Parquet fixture directory, removed with the test.
class TTempDir {
public:
    TTempDir() {
        auto pattern = (std::filesystem::temp_directory_path() / "qdb-aggregate-XXXXXX").string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const char* created = ::mkdtemp(writable.data());
        if (!created) {
            throw std::runtime_error("failed to create temporary directory");
        }
        Path_ = created;
    }

    ~TTempDir() {
        std::error_code error;
        std::filesystem::remove_all(Path_, error);
    }

    const std::filesystem::path& Path() const {
        return Path_;
    }

private:
    std::filesystem::path Path_;
};

// Writes 0..rows-1 as the int64 column `v`, `rowGroupRows` per row group.
void WriteValueParquet(const std::string& path, int64_t rows, int64_t rowGroupRows) {
    auto check = [](const arrow::Status& status) {
        if (!status.ok()) {
            throw std::runtime_error(status.ToString());
        }
    };
    arrow::Int64Builder values;
    for (int64_t row = 0; row < rows; ++row) {
        check(values.Append(row));
    }
    auto schema = arrow::schema({arrow::field("v", arrow::int64(), false)});
    auto batch = arrow::RecordBatch::Make(schema, rows, {values.Finish().ValueOrDie()});
    auto table = arrow::Table::FromRecordBatches({batch}).ValueOrDie();
    auto output = arrow::io::FileOutputStream::Open(path).ValueOrDie();
    check(parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), output, rowGroupRows));
    check(output->Close());
}

} // namespace

TEST(AggregateE2E, GlobalIdentitySurvivesParallelCascade) {
    for (bool nullable : {false, true}) {
        for (bool cascade : {false, true}) {
            // Two independent sources force the gather or partial/combine path.
            for (int input : {0, 1, 2, 3}) {
                if (!nullable && input == 3) {
                    continue;
                }
                SCOPED_TRACE(::testing::Message() << "cascade=" << cascade << " nullable=" << nullable << " input=" << input);
                std::array<int64_t, 3> values = {7, 0, -2};
                std::array<uint8_t, 1> mask = {static_cast<uint8_t>(input == 3 ? 0 : 0b101)};
                TColumn column{.Data = reinterpret_cast<char*>(values.data()), .Mask = nullable ? mask.data() : nullptr};
                auto batches = [&]() {
                    std::vector<TRowSet> result;
                    if (input != 0) {
                        result.push_back(TRowSet{
                            .Columns = &column, .ColumnCount = 1, .RowCount = 3, .RefCount = 1});
                    }
                    return result;
                };
                TTypePtr type = std::make_shared<TIntegerType>();
                if (nullable) {
                    type = std::make_shared<TNullable>(type);
                }
                TMockSource left({"v"}, batches(), {type});
                TMockSource right({"v"}, batches(), {type});
                std::string sql = "SELECT COUNT(*), COUNT(v), SUM(v), MIN(v), MAX(v), AVG(v) "
                    "FROM (SELECT v FROM l UNION ALL SELECT v FROM r) u";
                if (input == 1) {
                    sql += " WHERE v > 100";
                }
                auto root = BuildSqlPlan(sql, [&](std::string_view table)
                    -> std::expected<TOperatorPtr, NQumir::TError>
                {
                    ISource& source = table == "l"
                        ? left
                        : right;
                    return std::make_shared<TSourceOperator>(source, std::string(table));
                });
                ApplyPlanPasses(root);
                NScheduler::TSettings settings;
                settings.Scheduler.WorkerCount = 2;
                settings.Aggregate.CascadeGlobal = cascade
                    ? NScheduler::EGlobalCascadeMode::On
                    : NScheduler::EGlobalCascadeMode::Off;
                auto runtime = RunPlan(root, settings);
                TRowSet result{};
                ASSERT_TRUE(runtime->Next(result));
                ASSERT_EQ(result.RowCount, 1);
                ASSERT_EQ(result.ColumnCount, 6);
                EXPECT_EQ(result.Columns[0].Mask, nullptr);
                EXPECT_EQ(result.Columns[1].Mask, nullptr);
                EXPECT_EQ(reinterpret_cast<int64_t*>(result.Columns[0].Data)[0], input >= 2 ? 6 : 0);
                EXPECT_EQ(reinterpret_cast<int64_t*>(result.Columns[1].Data)[0], input == 2 ? (nullable ? 4 : 6) : 0);
                for (int columnIndex = 2; columnIndex < 6; ++columnIndex) {
                    EXPECT_EQ(IsValid(result.Columns[columnIndex], 0), input == 2);
                }
                if (input == 2) {
                    EXPECT_EQ(reinterpret_cast<int64_t*>(result.Columns[2].Data)[0], 10);
                    EXPECT_EQ(reinterpret_cast<int64_t*>(result.Columns[3].Data)[0], -2);
                    EXPECT_EQ(reinterpret_cast<int64_t*>(result.Columns[4].Data)[0], 7);
                    EXPECT_DOUBLE_EQ(reinterpret_cast<double*>(result.Columns[5].Data)[0], nullable ? 2.5 : 5.0 / 3.0);
                }
                Release(&result);
                EXPECT_FALSE(runtime->Next(result));
            }
        }
    }
}

TEST(AggregateE2E, AutoCascadeFollowsTheParquetEstimate) {
    // Auto end to end: the estimate comes from the Parquet metadata, the lanes
    // from scan splitting.
    constexpr int64_t rows = 1024;
    TTempDir dir;
    const auto path = (dir.Path() / "values.parquet").string();
    WriteValueParquet(path, rows, /*rowGroupRows*/ 256);
    TParquetSource source(path);

    const std::vector<EAggregatePhase> cascade{EAggregatePhase::Partial, EAggregatePhase::Final};
    const std::vector<EAggregatePhase> gather{EAggregatePhase::Single};
    for (const uint64_t bound : {rows, rows + 1}) {
        SCOPED_TRACE(::testing::Message() << "partial_min_input_rows=" << bound);
        auto root = BuildSqlPlan("SELECT COUNT(*), SUM(v), MIN(v), MAX(v) FROM t", source);
        ApplyPlanPasses(root);
        auto aggregate = FindAggregate(root);
        ASSERT_NE(aggregate, nullptr);
        ASSERT_TRUE(aggregate->Input()->Stats_);
        ASSERT_EQ(aggregate->Input()->Stats_->RowCount, rows);

        NScheduler::TSettings settings;
        settings.Scheduler.WorkerCount = 4;
        settings.ScanSplit.Strategy = NScheduler::EScanSplitStrategy::RowGroupRange;
        settings.ScanSplit.MaxScanTasks = 4;
        settings.Aggregate.PartialMinInputRows = bound;
        ASSERT_EQ(settings.Aggregate.CascadeGlobal, NScheduler::EGlobalCascadeMode::Auto);

        auto lowered = NScheduler::LowerPlanToGraph(root, settings, nullptr);
        EXPECT_EQ(std::ranges::count_if(lowered.Graph->Nodes(),
            [](const auto& node) { return node->DebugKind == "source"; }), 4);
        std::vector<EAggregatePhase> phases;
        for (const auto& stage : lowered.ExecStages) {
            if (stage.Kind == EExecPlanNodeKind::Aggregate) {
                phases.push_back(stage.AggregatePhase);
            }
        }
        EXPECT_EQ(phases, bound <= static_cast<uint64_t>(rows) ? cascade : gather);

        auto runtime = RunPlan(root, settings);
        TRowSet result{};
        ASSERT_TRUE(runtime->Next(result));
        ASSERT_EQ(result.RowCount, 1);
        ASSERT_EQ(result.ColumnCount, 4);
        EXPECT_EQ(reinterpret_cast<int64_t*>(result.Columns[0].Data)[0], rows);
        EXPECT_EQ(reinterpret_cast<int64_t*>(result.Columns[1].Data)[0], rows * (rows - 1) / 2);
        EXPECT_EQ(reinterpret_cast<int64_t*>(result.Columns[2].Data)[0], 0);
        EXPECT_EQ(reinterpret_cast<int64_t*>(result.Columns[3].Data)[0], rows - 1);
        Release(&result);
        EXPECT_FALSE(runtime->Next(result));
    }
}

TEST(AggregateE2E, EmptyExplicitGroupsHaveNoIdentityRow) {
    for (bool cascade : {false, true}) {
        for (const auto& sql : {
            "SELECT v, COUNT(*) FROM t GROUP BY v",
            "SELECT COUNT(*) FROM t GROUP BY 1 + 1"})
        {
            SCOPED_TRACE(::testing::Message() << "cascade=" << cascade << " sql=" << sql);
            TMockSource source({"v"});
            auto root = BuildSqlPlan(sql, source);
            ASSERT_FALSE(FindAggregate(root)->IsGlobal());
            ApplyPlanPasses(root);
            NScheduler::TSettings settings;
            settings.Scheduler.WorkerCount = 2;
            settings.Aggregate.CascadeGlobal = cascade
                ? NScheduler::EGlobalCascadeMode::On
                : NScheduler::EGlobalCascadeMode::Off;
            auto runtime = RunPlan(root, settings);
            TRowSet result{};
            while (runtime->Next(result)) {
                EXPECT_EQ(result.RowCount, 0);
                Release(&result);
            }
        }
    }
}

TEST(AggregateLogical, ReusesSumAndCountForAffineIntegerSums) {
    std::ostringstream sql;
    sql << "SELECT ";
    for (int64_t offset = 0; offset < 90; ++offset) {
        if (offset != 0) {
            sql << ", ";
        }
        sql << "SUM(\"ResolutionWidth\"";
        if (offset != 0) {
            sql << " + " << offset;
        }
        sql << ") AS s" << offset;
    }
    sql << " FROM hits";

    TMockSource source({"ResolutionWidth"});
    auto root = BuildSqlPlan(sql.str(), source);
    auto aggregate = FindAggregate(root);
    ASSERT_NE(aggregate, nullptr);
    ASSERT_EQ(aggregate->Aggs().size(), 2u);
    EXPECT_EQ(aggregate->Aggs()[0].Func, "sum");
    EXPECT_EQ(aggregate->Aggs()[1].Func, "count");
    ASSERT_TRUE(aggregate->Aggs()[0].Arg);
    ASSERT_TRUE(aggregate->Aggs()[1].Arg);
    EXPECT_EQ(PrintAst(aggregate->Aggs()[0].Arg), "|hits.ResolutionWidth|");
    EXPECT_EQ(PrintAst(aggregate->Aggs()[1].Arg), "|hits.ResolutionWidth|");

    auto argProject = TMaybeOp<TProjectOperator>(aggregate->Input());
    ASSERT_TRUE(argProject);
    // One shared value column plus the synthetic global-aggregate key. Without
    // the rewrite this project contains 90 independently materialized arguments.
    EXPECT_EQ(argProject.Cast()->Projections().size(), 2u);

    auto output = TMaybeOp<TProjectOperator>(root);
    ASSERT_TRUE(output);
    ASSERT_EQ(output.Cast()->Projections().size(), 90u);
    EXPECT_EQ(PrintAst(output.Cast()->Projections()[0].Expression), "sum_0");
    EXPECT_EQ(
        PrintAst(output.Cast()->Projections()[89].Expression),
        "(+ sum_0 (* 89 count_0))");
}

TEST(AggregateLogical, ReusesReducersAcrossAffineSumAverageAndCount) {
    TMockSource source({"x"});
    auto root = BuildSqlPlan(
        "SELECT SUM(x + 10), SUM(x + 11), SUM(x + 100), "
        "AVG(x + 123), AVG(x + 1234), COUNT(x + 17) FROM hits",
        source);
    auto aggregate = FindAggregate(root);
    ASSERT_NE(aggregate, nullptr);
    ASSERT_EQ(aggregate->Aggs().size(), 2u);
    EXPECT_EQ(aggregate->Aggs()[0].Func, "sum");
    EXPECT_EQ(aggregate->Aggs()[1].Func, "count");

    auto output = TMaybeOp<TProjectOperator>(root);
    ASSERT_TRUE(output);
    ASSERT_EQ(output.Cast()->Projections().size(), 6u);
    EXPECT_EQ(
        PrintAst(output.Cast()->Projections()[0].Expression),
        "(+ sum_0 (* 10 count_0))");
    EXPECT_EQ(
        PrintAst(output.Cast()->Projections()[3].Expression),
        "(/ (+ sum_0 (* 123 count_0)) (cast count_0 f64))");
    EXPECT_EQ(
        PrintAst(output.Cast()->Projections()[5].Expression),
        "count_0");
}

// L6 (multiple groups): full pipeline sexp -> AnnotateTypes -> ApplyColumnPruning
// -> TPhysicalPlanner::Build -> Next(), over an in-memory source with several
// groups split across two batches.
TEST(AggregateE2E, MultipleGroups) {
    std::vector<int64_t> keys = {1, 2, 1, 3, 2, 1, 4, 4, 2, 3, 4, 1};
    std::vector<int64_t> vals = {10, 20, 5, 7, -3, 9, 100, -100, 50, 0, 25, -1};
    ASSERT_EQ(keys.size(), vals.size());

    constexpr size_t batchSize = 6;
    std::vector<std::vector<TColumn>> batchColumns(2);
    std::vector<TRowSet> batches;
    for (size_t b = 0; b < 2; ++b) {
        batchColumns[b] = {
            TColumn{.Data = reinterpret_cast<char*>(keys.data() + b * batchSize)},
            TColumn{.Data = reinterpret_cast<char*>(vals.data() + b * batchSize)},
        };
        batches.push_back(TRowSet{
            .Columns = batchColumns[b].data(),
            .ColumnCount = 2,
            .RowCount = static_cast<int64_t>(batchSize),
            .Selection = nullptr,
            .Destroy = nullptr,
            .Private = nullptr,
            .RefCount = 1,
        });
    }

    NQdb::TMockSource source({"k", "v"}, std::move(batches));
    auto root = ParsePlan(PlanSexp, source);

    auto runtime = RunPlan(root);

    auto reference = ComputeReference(keys, vals);

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.ColumnCount, 5); // k, c, s, mn, mx
    ASSERT_EQ(result.RowCount, static_cast<int64_t>(reference.size()));
    EXPECT_EQ(result.Columns[0].Mask, nullptr);

    auto* outKeys = reinterpret_cast<int64_t*>(result.Columns[0].Data);
    auto* outCounts = reinterpret_cast<int64_t*>(result.Columns[1].Data);
    auto* outSums = reinterpret_cast<int64_t*>(result.Columns[2].Data);
    auto* outMins = reinterpret_cast<int64_t*>(result.Columns[3].Data);
    auto* outMaxs = reinterpret_cast<int64_t*>(result.Columns[4].Data);

    for (int64_t i = 0; i < result.RowCount; ++i) {
        const int64_t key = outKeys[i];
        auto it = reference.find(key);
        ASSERT_NE(it, reference.end()) << "unexpected key " << key;
        EXPECT_EQ(outCounts[i], it->second.Count) << "key " << key;
        EXPECT_EQ(outSums[i], it->second.Sum) << "key " << key;
        EXPECT_EQ(outMins[i], it->second.Min) << "key " << key;
        EXPECT_EQ(outMaxs[i], it->second.Max) << "key " << key;
    }

    Release(&result);

    TRowSet second{};
    EXPECT_FALSE(runtime->Next(second));
}

// L6 (single group): same pipeline, but every row shares one key - the
// HashTable ends up with exactly one entry.
TEST(AggregateE2E, SingleGroup) {
    std::vector<int64_t> keys = {7, 7, 7, 7, 7, 7};
    std::vector<int64_t> vals = {1, 2, 3, 4, 5, 6};
    ASSERT_EQ(keys.size(), vals.size());

    std::vector<TColumn> columns = {
        TColumn{.Data = reinterpret_cast<char*>(keys.data())},
        TColumn{.Data = reinterpret_cast<char*>(vals.data())},
    };
    std::vector<TRowSet> batches = {TRowSet{
        .Columns = columns.data(),
        .ColumnCount = 2,
        .RowCount = static_cast<int64_t>(keys.size()),
        .Selection = nullptr,
        .Destroy = nullptr,
        .Private = nullptr,
        .RefCount = 1,
    }};

    NQdb::TMockSource source({"k", "v"}, std::move(batches));
    auto root = ParsePlan(PlanSexp, source);

    auto runtime = RunPlan(root);

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.ColumnCount, 5);
    ASSERT_EQ(result.RowCount, 1);

    EXPECT_EQ(reinterpret_cast<int64_t*>(result.Columns[0].Data)[0], 7);
    EXPECT_EQ(reinterpret_cast<int64_t*>(result.Columns[1].Data)[0], 6);
    EXPECT_EQ(reinterpret_cast<int64_t*>(result.Columns[2].Data)[0], 21);
    EXPECT_EQ(reinterpret_cast<int64_t*>(result.Columns[3].Data)[0], 1);
    EXPECT_EQ(reinterpret_cast<int64_t*>(result.Columns[4].Data)[0], 6);

    Release(&result);

    TRowSet second{};
    EXPECT_FALSE(runtime->Next(second));
}

TEST(AggregateE2E, SumDecimalUsesBinIntState) {
    std::array<int64_t, 5> keys = {1, 1, 2, 2, 1};
    std::array<qdb_bin_int, 5> values = {{
        {.Lo = 1000, .Hi = 0}, // 10.00
        {.Lo = 250, .Hi = 0},  //  2.50
        {.Lo = 3000, .Hi = 0}, // 30.00
        {.Lo = 125, .Hi = 0},  //  1.25
        {.Lo = 75, .Hi = 0},   //  0.75
    }};
    std::vector<TColumn> columns = {
        TColumn{.Data = reinterpret_cast<char*>(keys.data())},
        TColumn{.Data = reinterpret_cast<char*>(values.data())},
    };
    std::vector<TRowSet> batches = {TRowSet{
        .Columns = columns.data(), .ColumnCount = 2, .RowCount = 5,
        .Selection = nullptr, .RefCount = 1}};
    NQdb::TMockSource source(
        {"k", "v"}, std::move(batches),
        {std::make_shared<TIntegerType>(),
         std::make_shared<NQdb::TDecimal>(7, 2)});
    auto root = ParsePlan(
        "(rel aggregate (rel source \"data.parquet\") (keys k) (agg s sum v))",
        source);
    auto runtime = RunPlan(root);

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.ColumnCount, 2);
    ASSERT_EQ(result.RowCount, 2);
    EXPECT_EQ(result.Columns[1].Mask, nullptr);
    auto* outKeys = reinterpret_cast<int64_t*>(result.Columns[0].Data);
    auto* sums = reinterpret_cast<qdb_bin_int*>(result.Columns[1].Data);
    for (int64_t row = 0; row < result.RowCount; ++row) {
        if (outKeys[row] == 1) {
            EXPECT_EQ(sums[row].Lo, 1325u);
            EXPECT_EQ(sums[row].Hi, 0u);
        } else {
            ASSERT_EQ(outKeys[row], 2);
            EXPECT_EQ(sums[row].Lo, 3125u);
            EXPECT_EQ(sums[row].Hi, 0u);
        }
    }
    Release(&result);
}

TEST(AggregateE2E, CompositeIntegerKeysProduceSeparateColumns) {
    struct TPair {
        int64_t First;
        int64_t Second;

        bool operator==(const TPair&) const = default;
    };
    struct TPairHash {
        size_t operator()(const TPair& value) const {
            return std::hash<int64_t>{}(value.First) ^
                (std::hash<int64_t>{}(value.Second) << 1);
        }
    };

    std::vector<int64_t> first = {
        1, 2, 1, 3, 4, 5, 2, 6, 3, 7, 8, 1};
    std::vector<int64_t> second = {
        10, 20, 10, 30, 40, 50, 20, 60, 30, 70, 80, 11};
    std::vector<int64_t> values = {
        5, 7, 11, 13, 17, 19, 3, 23, -2, 29, 31, 37};

    constexpr size_t batchSize = 6;
    std::vector<std::vector<TColumn>> batchColumns(2);
    std::vector<TRowSet> batches;
    for (size_t b = 0; b < 2; ++b) {
        batchColumns[b] = {
            TColumn{.Data = reinterpret_cast<char*>(first.data() + b * batchSize)},
            TColumn{.Data = reinterpret_cast<char*>(second.data() + b * batchSize)},
            TColumn{.Data = reinterpret_cast<char*>(values.data() + b * batchSize)},
        };
        batches.push_back(TRowSet{
            .Columns = batchColumns[b].data(),
            .ColumnCount = 3,
            .RowCount = static_cast<int64_t>(batchSize),
            .Selection = nullptr,
            .Destroy = nullptr,
            .Private = nullptr,
            .RefCount = 1,
        });
    }

    NQdb::TMockSource source({"k1", "k2", "v"}, std::move(batches));
    auto root = ParsePlan(
        "(rel aggregate (rel source \"data.parquet\") (keys k1 k2) "
        "(agg c count) (agg s sum v))",
        source);
    auto runtime = RunPlan(root);

    std::unordered_map<TPair, std::pair<int64_t, int64_t>, TPairHash> reference;
    for (size_t i = 0; i < first.size(); ++i) {
        auto& state = reference[{first[i], second[i]}];
        state.first += 1;
        state.second += values[i];
    }

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.ColumnCount, 4); // k1, k2, count, sum
    ASSERT_EQ(result.RowCount, static_cast<int64_t>(reference.size()));
    auto* outFirst = reinterpret_cast<int64_t*>(result.Columns[0].Data);
    auto* outSecond = reinterpret_cast<int64_t*>(result.Columns[1].Data);
    auto* outCounts = reinterpret_cast<int64_t*>(result.Columns[2].Data);
    auto* outSums = reinterpret_cast<int64_t*>(result.Columns[3].Data);
    for (int64_t i = 0; i < result.RowCount; ++i) {
        auto it = reference.find({outFirst[i], outSecond[i]});
        ASSERT_NE(it, reference.end());
        EXPECT_EQ(outCounts[i], it->second.first);
        EXPECT_EQ(outSums[i], it->second.second);
    }
    Release(&result);
}

TEST(AggregateE2E, ScalarI32KeyPreservesTypedOutput) {
    std::vector<int32_t> keys = {-1, 2, -1, 3, 4, 5, 2, 6, 3, 7, 8, -1};
    std::vector<int64_t> values = {5, 7, 11, 13, 17, 19, 3, 23, -2, 29, 31, 37};
    std::vector<TColumn> columns = {
        TColumn{.Data = reinterpret_cast<char*>(keys.data())},
        TColumn{.Data = reinterpret_cast<char*>(values.data())},
    };
    std::vector<TRowSet> batches = {TRowSet{
        .Columns = columns.data(),
        .ColumnCount = 2,
        .RowCount = static_cast<int64_t>(keys.size()),
        .Selection = nullptr,
        .Destroy = nullptr,
        .Private = nullptr,
        .RefCount = 1,
    }};
    NQdb::TMockSource source(
        {"k", "v"}, std::move(batches),
        {std::make_shared<TIntegerType>(TIntegerType::I32),
         std::make_shared<TIntegerType>(TIntegerType::I64)});
    auto root = ParsePlan(
        "(rel aggregate (rel source \"data.parquet\") (keys k) "
        "(agg c count) (agg s sum v))",
        source);
    auto runtime = RunPlan(root);

    std::unordered_map<int32_t, std::pair<int64_t, int64_t>> reference;
    for (size_t i = 0; i < keys.size(); ++i) {
        auto& state = reference[keys[i]];
        state.first += 1;
        state.second += values[i];
    }

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.ColumnCount, 3);
    ASSERT_EQ(result.RowCount, static_cast<int64_t>(reference.size()));
    auto* outKeys = reinterpret_cast<int32_t*>(result.Columns[0].Data);
    auto* outCounts = reinterpret_cast<int64_t*>(result.Columns[1].Data);
    auto* outSums = reinterpret_cast<int64_t*>(result.Columns[2].Data);
    for (int64_t i = 0; i < result.RowCount; ++i) {
        auto it = reference.find(outKeys[i]);
        ASSERT_NE(it, reference.end());
        EXPECT_EQ(outCounts[i], it->second.first);
        EXPECT_EQ(outSums[i], it->second.second);
    }
    Release(&result);
}

TEST(AggregateE2E, ScalarStringKeyOwnsFinalizedOutput) {
    std::string data1 = "alphabetaalpha";
    std::array<int32_t, 4> offsets1 = {0, 5, 9, 14};
    std::array<int64_t, 3> values1 = {10, 20, 5};
    std::array<uint8_t, 3> selection1 = {1, 0, 1};
    std::string data2 = "betaмир";
    std::array<int64_t, 4> offsets2 = {
        0, 4, 4, static_cast<int64_t>(data2.size())};
    std::array<int64_t, 3> values2 = {-3, 11, 7};

    std::vector<std::vector<TColumn>> batchColumns(2);
    batchColumns[0] = {
        TColumn{
            .Data = data1.data(), .Mask = nullptr,
            .Offsets = offsets1.data(), .OffsetWidth = 4},
        TColumn{.Data = reinterpret_cast<char*>(values1.data())},
    };
    batchColumns[1] = {
        TColumn{
            .Data = data2.data(), .Mask = nullptr,
            .Offsets = offsets2.data(), .OffsetWidth = 8},
        TColumn{.Data = reinterpret_cast<char*>(values2.data())},
    };
    std::vector<TRowSet> batches = {
        TRowSet{
            .Columns = batchColumns[0].data(), .ColumnCount = 2, .RowCount = 3,
            .Selection = selection1.data(), .RefCount = 1},
        TRowSet{
            .Columns = batchColumns[1].data(), .ColumnCount = 2, .RowCount = 3,
            .Selection = nullptr, .RefCount = 1},
    };
    NQdb::TMockSource source(
        {"k", "v"}, std::move(batches),
        {std::make_shared<TStringType>(),
         std::make_shared<TIntegerType>(TIntegerType::I64)});
    auto root = ParsePlan(
        "(rel aggregate (rel source \"data.parquet\") (keys k) "
        "(agg c count) (agg s sum v))",
        source);
    auto runtime = RunPlan(root);

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.ColumnCount, 3);
    ASSERT_EQ(result.RowCount, 4);
    ASSERT_EQ(result.Columns[0].OffsetWidth, 8);
    auto* outputOffsets = static_cast<int64_t*>(result.Columns[0].Offsets);
    auto* counts = reinterpret_cast<int64_t*>(result.Columns[1].Data);
    auto* sums = reinterpret_cast<int64_t*>(result.Columns[2].Data);
    std::map<std::string, std::pair<int64_t, int64_t>> actual;
    for (int64_t row = 0; row < result.RowCount; ++row) {
        std::string key(result.Columns[0].Data + outputOffsets[row],
            result.Columns[0].Data + outputOffsets[row + 1]);
        actual[key] = {counts[row], sums[row]};
    }
    EXPECT_EQ(actual.at("alpha"), (std::pair<int64_t, int64_t>{2, 15}));
    EXPECT_EQ(actual.at("beta"), (std::pair<int64_t, int64_t>{1, -3}));
    EXPECT_EQ(actual.at(""), (std::pair<int64_t, int64_t>{1, 11}));
    EXPECT_EQ(actual.at("мир"), (std::pair<int64_t, int64_t>{1, 7}));
    Release(&result);
}

TEST(AggregateE2E, StringMinMaxOwnsAndGrowsReducerValues) {
    std::array<int64_t, 4> keys1 = {1, 1, 2, 2};
    std::string data1 = "malphabetz";
    std::array<int32_t, 5> offsets1 = {0, 1, 9, 10, 10};
    std::array<int64_t, 4> keys2 = {1, 1, 2, 2};
    std::string data2 = "azzzzzzzzaardvarkyak";
    std::array<int64_t, 5> offsets2 = {0, 1, 9, 17, 20};

    std::vector<std::vector<TColumn>> batchColumns(2);
    batchColumns[0] = {
        TColumn{.Data = reinterpret_cast<char*>(keys1.data())},
        TColumn{.Data = data1.data(), .Offsets = offsets1.data(), .OffsetWidth = 4},
    };
    batchColumns[1] = {
        TColumn{.Data = reinterpret_cast<char*>(keys2.data())},
        TColumn{.Data = data2.data(), .Offsets = offsets2.data(), .OffsetWidth = 8},
    };
    std::vector<TRowSet> batches = {
        TRowSet{.Columns = batchColumns[0].data(), .ColumnCount = 2,
            .RowCount = 4, .RefCount = 1},
        TRowSet{.Columns = batchColumns[1].data(), .ColumnCount = 2,
            .RowCount = 4, .RefCount = 1},
    };
    TMockSource source(
        {"k", "v"}, std::move(batches),
        {std::make_shared<TIntegerType>(), std::make_shared<TStringType>()});
    auto runtime = RunPlan(ParsePlan(
        "(rel aggregate (rel source \"data.parquet\") (keys k) "
        "(agg c count v) (agg mn min v) (agg mx max v))", source));

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.ColumnCount, 4);
    ASSERT_EQ(result.RowCount, 2);
    auto* keys = reinterpret_cast<int64_t*>(result.Columns[0].Data);
    auto* counts = reinterpret_cast<int64_t*>(result.Columns[1].Data);
    for (int64_t row = 0; row < result.RowCount; ++row) {
        EXPECT_EQ(counts[row], 4);
        if (keys[row] == 1) {
            EXPECT_EQ(StringAt(result.Columns[2], row), "a");
            EXPECT_EQ(StringAt(result.Columns[3], row), "zzzzzzzz");
        } else {
            ASSERT_EQ(keys[row], 2);
            EXPECT_EQ(StringAt(result.Columns[2], row), "");
            EXPECT_EQ(StringAt(result.Columns[3], row), "z");
        }
    }
    Release(&result);
}

TEST(AggregateE2E, NullableStringMinMaxReturnsNullForAllNullGroup) {
    std::array<int64_t, 5> keys = {1, 1, 2, 2, 3};
    std::string data = "hiddenbetahiddenalpha";
    std::array<int32_t, 6> offsets = {0, 6, 10, 16, 21, 21};
    std::array<uint8_t, 1> mask = {0b00001010};
    std::vector<TColumn> columns = {
        TColumn{.Data = reinterpret_cast<char*>(keys.data())},
        TColumn{.Data = data.data(), .Mask = mask.data(),
            .Offsets = offsets.data(), .OffsetWidth = 4},
    };
    std::vector<TRowSet> batches = {TRowSet{
        .Columns = columns.data(), .ColumnCount = 2, .RowCount = 5,
        .RefCount = 1}};
    TMockSource source(
        {"k", "v"}, std::move(batches),
        {std::make_shared<TIntegerType>(),
         std::make_shared<TNullable>(std::make_shared<TStringType>())});
    auto runtime = RunPlan(ParsePlan(
        "(rel aggregate (rel source \"data.parquet\") (keys k) "
        "(agg mn min v) (agg mx max v))", source));

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.RowCount, 3);
    auto* outKeys = reinterpret_cast<int64_t*>(result.Columns[0].Data);
    for (int64_t row = 0; row < result.RowCount; ++row) {
        if (outKeys[row] == 1) {
            EXPECT_EQ(StringAt(result.Columns[1], row), "beta");
            EXPECT_EQ(StringAt(result.Columns[2], row), "beta");
            EXPECT_TRUE(IsValid(result.Columns[1], row));
            EXPECT_TRUE(IsValid(result.Columns[2], row));
        } else if (outKeys[row] == 2) {
            EXPECT_EQ(StringAt(result.Columns[1], row), "alpha");
            EXPECT_EQ(StringAt(result.Columns[2], row), "alpha");
            EXPECT_TRUE(IsValid(result.Columns[1], row));
            EXPECT_TRUE(IsValid(result.Columns[2], row));
        } else {
            ASSERT_EQ(outKeys[row], 3);
            EXPECT_FALSE(IsValid(result.Columns[1], row));
            EXPECT_FALSE(IsValid(result.Columns[2], row));
        }
    }
    Release(&result);
}

TEST(AggregateE2E, NullStringKeysGroupTogetherAndDifferFromEmpty) {
    std::string data = "hidden-onehidden-two";
    std::array<int32_t, 5> offsets = {0, 10, 20, 20, 20};
    std::array<uint8_t, 1> mask = {0b00001100};
    std::array<int64_t, 4> values = {10, 20, 3, 4};
    std::vector<TColumn> columns = {
        TColumn{.Data = data.data(), .Mask = mask.data(),
            .Offsets = offsets.data(), .OffsetWidth = 4},
        TColumn{.Data = reinterpret_cast<char*>(values.data())},
    };
    std::vector<TRowSet> batches = {TRowSet{
        .Columns = columns.data(), .ColumnCount = 2, .RowCount = 4,
        .Selection = nullptr, .RefCount = 1}};
    NQdb::TMockSource source(
        {"k", "v"}, std::move(batches),
        {std::make_shared<TNullable>(std::make_shared<TStringType>()),
         std::make_shared<TIntegerType>(TIntegerType::I64)});
    auto root = ParsePlan(
        "(rel aggregate (rel source \"data.parquet\") (keys k) "
        "(agg c count) (agg s sum v))",
        source);
    auto runtime = RunPlan(root);

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.RowCount, 2);
    auto* counts = reinterpret_cast<int64_t*>(result.Columns[1].Data);
    auto* sums = reinterpret_cast<int64_t*>(result.Columns[2].Data);
    auto* outputOffsets = static_cast<int64_t*>(result.Columns[0].Offsets);
    std::map<int64_t, int64_t> countBySum;
    for (int64_t row = 0; row < result.RowCount; ++row) {
        countBySum[sums[row]] = counts[row];
        const std::string key(result.Columns[0].Data + outputOffsets[row],
            result.Columns[0].Data + outputOffsets[row + 1]);
        EXPECT_TRUE(key.empty());
        EXPECT_EQ(IsValid(result.Columns[0], row), sums[row] == 7);
    }
    EXPECT_EQ(countBySum.at(30), 2);
    EXPECT_EQ(countBySum.at(7), 2);
    Release(&result);
}

TEST(AggregateE2E, CountDistinctString) {
    std::array<int64_t, 5> groups = {0, 0, 0, 0, 0};
    std::string data = "alphabetaalpha";
    std::array<int32_t, 6> offsets = {0, 5, 9, 14, 14, 14};
    std::vector<TColumn> columns = {
        TColumn{.Data = reinterpret_cast<char*>(groups.data())},
        TColumn{.Data = data.data(), .Offsets = offsets.data(), .OffsetWidth = 4},
    };
    std::vector<TRowSet> batches = {TRowSet{
        .Columns = columns.data(), .ColumnCount = 2, .RowCount = 5,
        .RefCount = 1}};
    TMockSource source(
        {"g", "v"}, std::move(batches),
        {std::make_shared<TIntegerType>(), std::make_shared<TStringType>()});
    auto runtime = RunPlan(ParsePlan(
        "(rel aggregate "
        "  (rel aggregate (rel source \"data.parquet\") (keys g v)) "
        "  (keys g) (agg c count v))",
        source));

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.RowCount, 1);
    EXPECT_EQ(reinterpret_cast<int64_t*>(result.Columns[1].Data)[0], 3);
    Release(&result);
}

TEST(AggregateE2E, CountDistinctNullableStringIgnoresNull) {
    std::array<int64_t, 5> groups = {0, 0, 0, 0, 0};
    std::string data = "alphahiddenalphabetaignored";
    std::array<int32_t, 6> offsets = {0, 5, 11, 16, 20, 27};
    std::array<uint8_t, 1> mask = {0b00001101};
    std::vector<TColumn> columns = {
        TColumn{.Data = reinterpret_cast<char*>(groups.data())},
        TColumn{.Data = data.data(), .Mask = mask.data(),
            .Offsets = offsets.data(), .OffsetWidth = 4},
    };
    std::vector<TRowSet> batches = {TRowSet{
        .Columns = columns.data(), .ColumnCount = 2, .RowCount = 5,
        .RefCount = 1}};
    TMockSource source(
        {"g", "v"}, std::move(batches),
        {std::make_shared<TIntegerType>(),
         std::make_shared<TNullable>(std::make_shared<TStringType>())});
    auto runtime = RunPlan(ParsePlan(
        "(rel aggregate "
        "  (rel aggregate (rel source \"data.parquet\") (keys g v)) "
        "  (keys g) (agg c count v))",
        source));

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.RowCount, 1);
    EXPECT_EQ(reinterpret_cast<int64_t*>(result.Columns[1].Data)[0], 2);
    Release(&result);
}

TEST(AggregateE2E, NullIntegerKeysIgnorePayloadAndDifferFromZero) {
    std::array<int64_t, 4> keys = {41, 99, 0, 0};
    std::array<uint8_t, 1> mask = {0b00001100};
    std::array<int64_t, 4> values = {10, 20, 3, 4};
    std::vector<TColumn> columns = {
        TColumn{.Data = reinterpret_cast<char*>(keys.data()), .Mask = mask.data()},
        TColumn{.Data = reinterpret_cast<char*>(values.data())},
    };
    std::vector<TRowSet> batches = {TRowSet{
        .Columns = columns.data(), .ColumnCount = 2, .RowCount = 4,
        .Selection = nullptr, .RefCount = 1}};
    NQdb::TMockSource source(
        {"k", "v"}, std::move(batches),
        {std::make_shared<TNullable>(
             std::make_shared<TIntegerType>(TIntegerType::I64)),
         std::make_shared<TIntegerType>(TIntegerType::I64)});
    auto root = ParsePlan(
        "(rel aggregate (rel source \"data.parquet\") (keys k) "
        "(agg c count) (agg s sum v))",
        source);
    auto runtime = RunPlan(root);

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.RowCount, 2);
    auto* counts = reinterpret_cast<int64_t*>(result.Columns[1].Data);
    auto* sums = reinterpret_cast<int64_t*>(result.Columns[2].Data);
    auto* outputKeys = reinterpret_cast<int64_t*>(result.Columns[0].Data);
    std::map<int64_t, int64_t> countBySum;
    for (int64_t row = 0; row < result.RowCount; ++row) {
        countBySum[sums[row]] = counts[row];
        EXPECT_EQ(outputKeys[row], 0);
        EXPECT_EQ(IsValid(result.Columns[0], row), sums[row] == 7);
    }
    EXPECT_EQ(countBySum.at(30), 2);
    EXPECT_EQ(countBySum.at(7), 2);
    Release(&result);
}

TEST(AggregateE2E, CompositeNullKeysTrackValidityPerPosition) {
    std::array<int64_t, 6> first = {41, 99, 0, 17, 23, 88};
    std::array<int64_t, 6> second = {1, 1, 1, 2, 31, 77};
    std::array<uint8_t, 1> firstMask = {0b00000100};
    std::array<uint8_t, 1> secondMask = {0b00001111};
    std::array<int64_t, 6> values = {10, 20, 3, 4, 5, 6};
    std::vector<TColumn> columns = {
        TColumn{
            .Data = reinterpret_cast<char*>(first.data()),
            .Mask = firstMask.data()},
        TColumn{
            .Data = reinterpret_cast<char*>(second.data()),
            .Mask = secondMask.data()},
        TColumn{.Data = reinterpret_cast<char*>(values.data())},
    };
    std::vector<TRowSet> batches = {TRowSet{
        .Columns = columns.data(), .ColumnCount = 3, .RowCount = 6,
        .Selection = nullptr, .RefCount = 1}};
    NQdb::TMockSource source(
        {"k1", "k2", "v"}, std::move(batches),
        {std::make_shared<TNullable>(std::make_shared<TIntegerType>()),
         std::make_shared<TNullable>(std::make_shared<TIntegerType>()),
         std::make_shared<TIntegerType>()});
    auto root = ParsePlan(
        "(rel aggregate (rel source \"data.parquet\") (keys k1 k2) "
        "(agg c count) (agg s sum v))",
        source);
    auto runtime = RunPlan(root);

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.RowCount, 4);
    auto* counts = reinterpret_cast<int64_t*>(result.Columns[2].Data);
    auto* sums = reinterpret_cast<int64_t*>(result.Columns[3].Data);
    std::map<int64_t, int64_t> countBySum;
    for (int64_t row = 0; row < result.RowCount; ++row) {
        countBySum[sums[row]] = counts[row];
        const bool firstValid = IsValid(result.Columns[0], row);
        const bool secondValid = IsValid(result.Columns[1], row);
        if (sums[row] == 3) {
            EXPECT_TRUE(firstValid);
            EXPECT_TRUE(secondValid);
        } else if (sums[row] == 11) {
            EXPECT_FALSE(firstValid);
            EXPECT_FALSE(secondValid);
        } else {
            EXPECT_FALSE(firstValid);
            EXPECT_TRUE(secondValid);
        }
    }
    EXPECT_EQ(countBySum.at(30), 2);
    EXPECT_EQ(countBySum.at(3), 1);
    EXPECT_EQ(countBySum.at(4), 1);
    EXPECT_EQ(countBySum.at(11), 2);
    Release(&result);
}

TEST(AggregateE2E, MixedStringI32KeysFinalizeToSeparateColumns) {
    std::array<int32_t, 4> ids1 = {1, 1, 2, 3};
    std::string data1 = "alphaalphabetagamma";
    std::array<int32_t, 5> offsets1 = {0, 5, 10, 14, 19};
    std::array<int64_t, 4> values1 = {10, 5, 20, 100};
    std::array<uint8_t, 4> selection1 = {1, 1, 1, 0};
    std::array<int32_t, 3> ids2 = {2, 4, 1};
    std::string data2 = "betadeltaalpha";
    std::array<int32_t, 4> offsets2 = {0, 4, 9, 14};
    std::array<int64_t, 3> values2 = {-3, 11, 7};

    std::vector<std::vector<TColumn>> columns(2);
    columns[0] = {
        TColumn{.Data = reinterpret_cast<char*>(ids1.data())},
        TColumn{.Data = data1.data(), .Mask = nullptr,
            .Offsets = offsets1.data(), .OffsetWidth = 4},
        TColumn{.Data = reinterpret_cast<char*>(values1.data())},
    };
    columns[1] = {
        TColumn{.Data = reinterpret_cast<char*>(ids2.data())},
        TColumn{.Data = data2.data(), .Mask = nullptr,
            .Offsets = offsets2.data(), .OffsetWidth = 4},
        TColumn{.Data = reinterpret_cast<char*>(values2.data())},
    };
    std::vector<TRowSet> batches = {
        TRowSet{.Columns = columns[0].data(), .ColumnCount = 3, .RowCount = 4,
            .Selection = selection1.data(), .RefCount = 1},
        TRowSet{.Columns = columns[1].data(), .ColumnCount = 3, .RowCount = 3,
            .Selection = nullptr, .RefCount = 1},
    };
    NQdb::TMockSource source(
        {"id", "name", "v"}, std::move(batches),
        {std::make_shared<TIntegerType>(TIntegerType::I32),
         std::make_shared<TStringType>(),
         std::make_shared<TIntegerType>(TIntegerType::I64)});
    auto root = ParsePlan(
        "(rel aggregate (rel source \"data.parquet\") (keys name id) "
        "(agg c count) (agg s sum v))",
        source);
    auto runtime = RunPlan(root);

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.ColumnCount, 4);
    ASSERT_EQ(result.RowCount, 3);
    auto* outOffsets = static_cast<int64_t*>(result.Columns[0].Offsets);
    auto* outIds = reinterpret_cast<int32_t*>(result.Columns[1].Data);
    auto* outCounts = reinterpret_cast<int64_t*>(result.Columns[2].Data);
    auto* outSums = reinterpret_cast<int64_t*>(result.Columns[3].Data);
    std::map<std::pair<int64_t, std::string>,
        std::pair<int64_t, int64_t>> actual;
    for (int64_t row = 0; row < result.RowCount; ++row) {
        std::string name(result.Columns[0].Data + outOffsets[row],
            result.Columns[0].Data + outOffsets[row + 1]);
        actual[{outIds[row], name}] = {outCounts[row], outSums[row]};
    }
    EXPECT_EQ(actual.at({1, "alpha"}),
        (std::pair<int64_t, int64_t>{3, 22}));
    EXPECT_EQ(actual.at({2, "beta"}),
        (std::pair<int64_t, int64_t>{2, 17}));
    EXPECT_EQ(actual.at({4, "delta"}),
        (std::pair<int64_t, int64_t>{1, 11}));
    Release(&result);
}

TEST(AggregateE2E, MixedI32StringKeysPreserveReducersAcrossGrow) {
    constexpr int32_t rowCount = 40;
    std::vector<int32_t> ids(rowCount);
    std::string names;
    std::vector<int32_t> offsets(rowCount + 1);
    std::vector<int64_t> values(rowCount);
    std::map<std::pair<int32_t, std::string>,
        std::pair<int64_t, int64_t>> expected;
    for (int32_t row = 0; row < rowCount; ++row) {
        ids[row] = row % 23;
        const std::string name = "status_" + std::to_string(row % 3);
        names += name;
        offsets[row + 1] = static_cast<int32_t>(names.size());
        values[row] = row + 1;
        auto& state = expected[{ids[row], name}];
        ++state.first;
        state.second += values[row];
    }

    std::vector<TColumn> columns = {
        TColumn{.Data = reinterpret_cast<char*>(ids.data())},
        TColumn{.Data = names.data(), .Mask = nullptr,
            .Offsets = offsets.data(), .OffsetWidth = 4},
        TColumn{.Data = reinterpret_cast<char*>(values.data())},
    };
    std::vector<TRowSet> batches = {TRowSet{
        .Columns = columns.data(), .ColumnCount = 3, .RowCount = rowCount,
        .Selection = nullptr, .RefCount = 1}};
    NQdb::TMockSource source(
        {"id", "name", "v"}, std::move(batches),
        {std::make_shared<TIntegerType>(TIntegerType::I32),
         std::make_shared<TStringType>(),
         std::make_shared<TIntegerType>(TIntegerType::I64)});
    auto root = ParsePlan(
        "(rel aggregate (rel source \"data.parquet\") (keys id name) "
        "(agg c count) (agg s sum v))",
        source);
    auto runtime = RunPlan(root);

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.ColumnCount, 4);
    ASSERT_EQ(result.RowCount, static_cast<int64_t>(expected.size()));
    auto* outIds = reinterpret_cast<int32_t*>(result.Columns[0].Data);
    auto* outOffsets = static_cast<int64_t*>(result.Columns[1].Offsets);
    auto* outCounts = reinterpret_cast<int64_t*>(result.Columns[2].Data);
    auto* outSums = reinterpret_cast<int64_t*>(result.Columns[3].Data);
    for (int64_t row = 0; row < result.RowCount; ++row) {
        std::string name(result.Columns[1].Data + outOffsets[row],
            result.Columns[1].Data + outOffsets[row + 1]);
        EXPECT_EQ((std::pair<int64_t, int64_t>{outCounts[row], outSums[row]}),
            expected.at({outIds[row], name}));
    }
    Release(&result);
}

TEST(AggregateE2E, TwoStringKeysFinalizeIndependentColumns) {
    std::string leftData = "ababa";
    std::string rightData = "xyxzy";
    std::array<int32_t, 6> offsets = {0, 1, 2, 3, 4, 5};
    std::array<int64_t, 5> values = {10, 20, 5, 7, 11};
    std::vector<TColumn> columns = {
        TColumn{.Data = leftData.data(), .Mask = nullptr,
            .Offsets = offsets.data(), .OffsetWidth = 4},
        TColumn{.Data = rightData.data(), .Mask = nullptr,
            .Offsets = offsets.data(), .OffsetWidth = 4},
        TColumn{.Data = reinterpret_cast<char*>(values.data())},
    };
    std::vector<TRowSet> batches = {TRowSet{
        .Columns = columns.data(), .ColumnCount = 3, .RowCount = 5,
        .Selection = nullptr, .RefCount = 1}};
    NQdb::TMockSource source(
        {"left", "right", "v"}, std::move(batches),
        {std::make_shared<TStringType>(), std::make_shared<TStringType>(),
         std::make_shared<TIntegerType>(TIntegerType::I64)});
    auto root = ParsePlan(
        "(rel aggregate (rel source \"data.parquet\") (keys left right) "
        "(agg c count) (agg s sum v))",
        source);
    auto runtime = RunPlan(root);

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.ColumnCount, 4);
    ASSERT_EQ(result.RowCount, 4);
    ASSERT_EQ(result.Columns[0].OffsetWidth, 8);
    ASSERT_EQ(result.Columns[1].OffsetWidth, 8);
    auto* leftOffsets = static_cast<int64_t*>(result.Columns[0].Offsets);
    auto* rightOffsets = static_cast<int64_t*>(result.Columns[1].Offsets);
    auto* counts = reinterpret_cast<int64_t*>(result.Columns[2].Data);
    auto* sums = reinterpret_cast<int64_t*>(result.Columns[3].Data);
    std::map<std::pair<std::string, std::string>,
        std::pair<int64_t, int64_t>> actual;
    for (int64_t row = 0; row < result.RowCount; ++row) {
        std::string left(result.Columns[0].Data + leftOffsets[row],
            result.Columns[0].Data + leftOffsets[row + 1]);
        std::string right(result.Columns[1].Data + rightOffsets[row],
            result.Columns[1].Data + rightOffsets[row + 1]);
        actual[{left, right}] = {counts[row], sums[row]};
    }
    EXPECT_EQ(actual.at({"a", "x"}),
        (std::pair<int64_t, int64_t>{2, 15}));
    EXPECT_EQ(actual.at({"b", "y"}),
        (std::pair<int64_t, int64_t>{1, 20}));
    EXPECT_EQ(actual.at({"b", "z"}),
        (std::pair<int64_t, int64_t>{1, 7}));
    EXPECT_EQ(actual.at({"a", "y"}),
        (std::pair<int64_t, int64_t>{1, 11}));
    Release(&result);
}

TEST(AggregateE2E, ScalarF64KeyCanonicalizesSignedZero) {
    const double nan1 = std::numeric_limits<double>::quiet_NaN();
    const double nan2 = std::bit_cast<double>(UINT64_C(0x7ff0000000000001));
    std::vector<double> keys = {
        0.0, -0.0, 1.5, -2.25, 3.0, 4.5,
        1.5, 5.75, -2.25, 6.0, nan1, nan2};
    std::vector<int64_t> values = {
        5, 7, 11, 13, 17, 19, 3, 23, -2, 29, 31, 37};
    std::vector<TColumn> columns = {
        TColumn{.Data = reinterpret_cast<char*>(keys.data())},
        TColumn{.Data = reinterpret_cast<char*>(values.data())},
    };
    std::vector<TRowSet> batches = {TRowSet{
        .Columns = columns.data(),
        .ColumnCount = 2,
        .RowCount = static_cast<int64_t>(keys.size()),
        .Selection = nullptr,
        .Destroy = nullptr,
        .Private = nullptr,
        .RefCount = 1,
    }};
    NQdb::TMockSource source(
        {"k", "v"}, std::move(batches),
        {std::make_shared<TFloatType>(),
         std::make_shared<TIntegerType>(TIntegerType::I64)});
    auto root = ParsePlan(
        "(rel aggregate (rel source \"data.parquet\") (keys k) "
        "(agg c count) (agg s sum v))",
        source);
    auto runtime = RunPlan(root);

    auto canonicalBits = [](double value) {
        uint64_t bits = std::bit_cast<uint64_t>(value);
        constexpr uint64_t signMask = UINT64_C(0x8000000000000000);
        constexpr uint64_t exponentMask = UINT64_C(0x7ff0000000000000);
        constexpr uint64_t fractionMask = UINT64_C(0x000fffffffffffff);
        if ((bits & ~signMask) == 0) {
            return UINT64_C(0);
        }
        if ((bits & exponentMask) == exponentMask &&
            (bits & fractionMask) != 0) {
            return UINT64_C(0x7ff8000000000000);
        }
        return bits;
    };
    std::map<uint64_t, std::pair<int64_t, int64_t>> reference;
    for (size_t i = 0; i < keys.size(); ++i) {
        auto& state = reference[canonicalBits(keys[i])];
        state.first += 1;
        state.second += values[i];
    }

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.ColumnCount, 3);
    ASSERT_EQ(result.RowCount, static_cast<int64_t>(reference.size()));
    auto* outKeys = reinterpret_cast<double*>(result.Columns[0].Data);
    auto* outCounts = reinterpret_cast<int64_t*>(result.Columns[1].Data);
    auto* outSums = reinterpret_cast<int64_t*>(result.Columns[2].Data);
    for (int64_t i = 0; i < result.RowCount; ++i) {
        auto it = reference.find(canonicalBits(outKeys[i]));
        ASSERT_NE(it, reference.end());
        EXPECT_EQ(outCounts[i], it->second.first);
        EXPECT_EQ(outSums[i], it->second.second);
    }
    Release(&result);
}

TEST(AggregateE2E, MixedI32F64CompositeKeyPreservesLayoutAndTypedColumns) {
    std::vector<int32_t> first = {1, 1, 2, 2, 3, 4, 1, 5, 3, 6, 7, 1};
    std::vector<double> second = {
        0.0, -0.0, 407986.23, 417231.63, -3.25, 4.0,
        1.01, 5.5, -3.25, 6.0, 7.0, 1.01};
    std::vector<int64_t> values = {5, 7, 11, 13, 17, 19, 3, 23, -2, 29, 31, 37};

    constexpr size_t batchSize = 6;
    std::vector<std::vector<TColumn>> batchColumns(2);
    std::vector<TRowSet> batches;
    for (size_t b = 0; b < 2; ++b) {
        batchColumns[b] = {
            TColumn{.Data = reinterpret_cast<char*>(first.data() + b * batchSize)},
            TColumn{.Data = reinterpret_cast<char*>(second.data() + b * batchSize)},
            TColumn{.Data = reinterpret_cast<char*>(values.data() + b * batchSize)},
        };
        batches.push_back(TRowSet{
            .Columns = batchColumns[b].data(),
            .ColumnCount = 3,
            .RowCount = static_cast<int64_t>(batchSize),
            .Selection = nullptr,
            .Destroy = nullptr,
            .Private = nullptr,
            .RefCount = 1,
        });
    }
    NQdb::TMockSource source(
        {"k1", "k2", "v"}, std::move(batches),
        {std::make_shared<TIntegerType>(TIntegerType::I32),
         std::make_shared<TFloatType>(),
         std::make_shared<TIntegerType>(TIntegerType::I64)});
    auto root = ParsePlan(
        "(rel aggregate (rel source \"data.parquet\") (keys k1 k2) "
        "(agg c count) (agg s sum v))",
        source);
    auto runtime = RunPlan(root);

    std::map<std::pair<int32_t, double>, std::pair<int64_t, int64_t>> reference;
    for (size_t i = 0; i < first.size(); ++i) {
        auto& state = reference[{first[i], second[i]}];
        state.first += 1;
        state.second += values[i];
    }

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.ColumnCount, 4);
    ASSERT_EQ(result.RowCount, static_cast<int64_t>(reference.size()));
    auto* outFirst = reinterpret_cast<int32_t*>(result.Columns[0].Data);
    auto* outSecond = reinterpret_cast<double*>(result.Columns[1].Data);
    auto* outCounts = reinterpret_cast<int64_t*>(result.Columns[2].Data);
    auto* outSums = reinterpret_cast<int64_t*>(result.Columns[3].Data);
    for (int64_t i = 0; i < result.RowCount; ++i) {
        auto it = reference.find({outFirst[i], outSecond[i]});
        ASSERT_NE(it, reference.end());
        EXPECT_EQ(outCounts[i], it->second.first);
        EXPECT_EQ(outSums[i], it->second.second);
    }
    Release(&result);
}

TEST(AggregateE2E, PlannerUsesPhysicalPrunedSchemaForKeyDescriptor) {
    class TPruningSource final : public ISource {
    public:
        TPruningSource() {
            OriginalColumns_ = {
                {"unused", std::make_shared<TStringType>()},
                {"key_i32", std::make_shared<TIntegerType>(TIntegerType::I32)},
                {"value", std::make_shared<TIntegerType>(TIntegerType::I64)},
                {"key_f64", std::make_shared<TFloatType>()},
            };
            PhysicalColumns_ = {
                {"value", std::make_shared<TIntegerType>(TIntegerType::I64)},
                {"key_f64", std::make_shared<TFloatType>()},
                {"key_i32", std::make_shared<TIntegerType>(TIntegerType::I32)},
            };
            CurrentSchema_ = TSchema{OriginalColumns_};
        }

        const TSchema& Schema() const override {
            return CurrentSchema_;
        }

        const TStatsPtr Stats() const override {
            return nullptr;
        }

        void RestrictColumns(const std::unordered_set<std::string>& names) override {
            RestrictedColumns_ = names;
            CurrentSchema_ = TSchema{PhysicalColumns_};
        }

        bool Next(TRowSet& rowSet) override {
            if (Done_) {
                return false;
            }
            Columns_[0].Data = reinterpret_cast<char*>(Values_.data());
            Columns_[1].Data = reinterpret_cast<char*>(F64Keys_.data());
            Columns_[2].Data = reinterpret_cast<char*>(I32Keys_.data());
            rowSet = TRowSet{
                .Columns = Columns_.data(),
                .ColumnCount = static_cast<int64_t>(Columns_.size()),
                .RowCount = static_cast<int64_t>(Values_.size()),
                .Selection = nullptr,
                .Destroy = nullptr,
                .Private = nullptr,
                .RefCount = 1,
            };
            Done_ = true;
            return true;
        }

        const std::unordered_set<std::string>& RestrictedColumns() const {
            return RestrictedColumns_;
        }

    private:
        std::vector<TColumnSchema> OriginalColumns_;
        std::vector<TColumnSchema> PhysicalColumns_;
        TSchema CurrentSchema_{};
        std::unordered_set<std::string> RestrictedColumns_;
        std::vector<int64_t> Values_ = {5, 7, 11, 13, -2, 17};
        std::vector<double> F64Keys_ = {1.25, 2.5, 1.25, 3.75, 2.5, 4.125};
        std::vector<int32_t> I32Keys_ = {10, 20, 10, 30, 20, 40};
        std::array<TColumn, 3> Columns_{};
        bool Done_ = false;
    } source;

    auto root = ParsePlan(
        "(rel aggregate (rel source \"data.parquet\") "
        "(keys key_i32 key_f64) (agg s sum value))",
        source);
    auto runtime = RunPlan(root);

    EXPECT_EQ(source.RestrictedColumns(),
        (std::unordered_set<std::string>{"key_i32", "key_f64", "value"}));
    auto outputType = TMaybeType<TStructType>(runtime->OutputType());
    ASSERT_TRUE(outputType);
    ASSERT_EQ(outputType.Cast()->Fields.size(), 3u);
    EXPECT_TRUE(TMaybeType<TIntegerType>(outputType.Cast()->Fields[0].second));
    EXPECT_TRUE(TMaybeType<TFloatType>(outputType.Cast()->Fields[1].second));

    std::map<std::pair<int32_t, double>, int64_t> expected = {
        {{10, 1.25}, 16}, {{20, 2.5}, 5},
        {{30, 3.75}, 13}, {{40, 4.125}, 17},
    };
    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.ColumnCount, 3);
    ASSERT_EQ(result.RowCount, static_cast<int64_t>(expected.size()));
    auto* outI32 = reinterpret_cast<int32_t*>(result.Columns[0].Data);
    auto* outF64 = reinterpret_cast<double*>(result.Columns[1].Data);
    auto* outSums = reinterpret_cast<int64_t*>(result.Columns[2].Data);
    for (int64_t i = 0; i < result.RowCount; ++i) {
        auto it = expected.find({outI32[i], outF64[i]});
        ASSERT_NE(it, expected.end());
        EXPECT_EQ(outSums[i], it->second);
    }
    Release(&result);
}

TEST(AggregateE2E, CompilesNullableReducerArgumentWithUnwrappedAstType) {
    TStructType inputType({
        {"k", std::make_shared<TIntegerType>()},
        {"v", std::make_shared<TNullable>(std::make_shared<TIntegerType>())},
    });
    NQumir::TLocation loc{};
    std::vector<NQdb::TAggregateSpec> aggs = {{
        .Name = "s",
        .Func = "sum",
        .Arg = std::make_shared<TIdentExpr>(loc, "v"),
    }};
    auto spec = NQdb::NKernel::BuildAggregateKernelSpec(inputType, {"k"}, aggs);
    EXPECT_NO_THROW(
        NQdb::TKernelCompiler().CompileAggregate(spec));
}

// CompileAggregateHash uses the same rh_hash generator as the aggregate's table.
TEST(AggregateE2E, CompileAggregateHashMatchesEqualCompositeKeys) {
    TStructType inputType({
        {"k1", std::make_shared<TIntegerType>()},
        {"k2", std::make_shared<TIntegerType>()},
    });
    auto hash = NQdb::TKernelCompiler().CompileAggregateHash(inputType, {"k1", "k2"});

    std::vector<int64_t> k1 = {1, 2, 1, 1};
    std::vector<int64_t> k2 = {10, 10, 10, 99};
    std::vector<TColumn> cols = {
        TColumn{.Data = reinterpret_cast<char*>(k1.data())},
        TColumn{.Data = reinterpret_cast<char*>(k2.data())},
    };
    TRowSet batch{
        .Columns = cols.data(),
        .ColumnCount = 2,
        .RowCount = static_cast<int64_t>(k1.size()),
        .RefCount = 1,
    };

    std::vector<uint64_t> hashes(k1.size(), 0);
    ASSERT_TRUE(hash(&batch, hashes.data()));

    EXPECT_EQ(hashes[0], hashes[2]) << "identical (k1, k2) must hash equal";
    EXPECT_NE(hashes[0], hashes[1]) << "different k1 must (almost certainly) hash differently";
    EXPECT_NE(hashes[0], hashes[3]) << "different k2 must (almost certainly) hash differently";
}

TEST(AggregateE2E, CompileAggregateHashDistinguishesNullFromValue) {
    TStructType inputType({
        {"k", std::make_shared<TIntegerType>()},
        {"v", std::make_shared<TNullable>(std::make_shared<TIntegerType>())},
    });
    auto hash = NQdb::TKernelCompiler().CompileAggregateHash(inputType, {"k", "v"});

    std::vector<int64_t> k = {1, 1};
    std::vector<int64_t> v = {5, 5};
    // Row 0 valid, row 1 null (same underlying bit pattern in `v`).
    uint8_t mask = 0b01;
    std::vector<TColumn> cols = {
        TColumn{.Data = reinterpret_cast<char*>(k.data())},
        TColumn{.Data = reinterpret_cast<char*>(v.data()), .Mask = &mask},
    };
    TRowSet batch{
        .Columns = cols.data(),
        .ColumnCount = 2,
        .RowCount = static_cast<int64_t>(k.size()),
        .RefCount = 1,
    };

    std::vector<uint64_t> hashes(k.size(), 0);
    ASSERT_TRUE(hash(&batch, hashes.data()));

    EXPECT_NE(hashes[0], hashes[1]) << "null vs. same-bits-but-valid must hash differently";
}

TEST(AggregateE2E, RejectsUnsupportedStringReducer) {
    TStructType inputType({
        {"k", std::make_shared<TIntegerType>()},
        {"v", std::make_shared<TStringType>()},
    });
    NQumir::TLocation loc{};
    std::vector<NQdb::TAggregateSpec> aggs = {{
        .Name = "s",
        .Func = "sum",
        .Arg = std::make_shared<TIdentExpr>(loc, "v"),
    }};
    auto spec = NQdb::NKernel::BuildAggregateKernelSpec(inputType, {"k"}, aggs);
    EXPECT_THROW(NQdb::TKernelCompiler().CompileAggregate(spec), NQumir::TError);
}

// M13.8: a nullable reducer argument must distinguish count(*) (every row) from
// count(arg)/sum/min/max (only non-null arguments). A group whose argument is
// NULL in every row produces a NULL sum/min/max (empty-input semantics) but a
// non-null count(*).
TEST(AggregateE2E, NullableReducerArgumentSeparatesCountStarFromCountArg) {
    // k=1: v in {10, NULL, 4}; k=2: all NULL; k=3: v in {7}.
    std::array<int64_t, 6> keys = {1, 1, 1, 2, 2, 3};
    std::array<int64_t, 6> values = {10, 999, 4, 888, 777, 7};
    // valid (non-null) at rows 0, 2, 5.
    std::array<uint8_t, 1> mask = {0b00100101};
    std::vector<TColumn> columns = {
        TColumn{.Data = reinterpret_cast<char*>(keys.data())},
        TColumn{.Data = reinterpret_cast<char*>(values.data()),
            .Mask = mask.data()},
    };
    std::vector<TRowSet> batches = {TRowSet{
        .Columns = columns.data(), .ColumnCount = 2, .RowCount = 6,
        .Selection = nullptr, .RefCount = 1}};
    NQdb::TMockSource source(
        {"k", "v"}, std::move(batches),
        {std::make_shared<TIntegerType>(),
         std::make_shared<TNullable>(std::make_shared<TIntegerType>())});
    auto root = ParsePlan(
        "(rel aggregate (rel source \"data.parquet\") (keys k) "
        "(agg c count) (agg cn count v) (agg s sum v) "
        "(agg mn min v) (agg mx max v))",
        source);
    auto runtime = RunPlan(root);

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.ColumnCount, 6); // k, c, cn, s, mn, mx
    ASSERT_EQ(result.RowCount, 3);
    auto* outKeys = reinterpret_cast<int64_t*>(result.Columns[0].Data);
    auto* countStar = reinterpret_cast<int64_t*>(result.Columns[1].Data);
    auto* countArg = reinterpret_cast<int64_t*>(result.Columns[2].Data);
    auto* sums = reinterpret_cast<int64_t*>(result.Columns[3].Data);
    auto* mins = reinterpret_cast<int64_t*>(result.Columns[4].Data);
    auto* maxs = reinterpret_cast<int64_t*>(result.Columns[5].Data);
    // count(*) and count(arg) are always non-nullable.
    EXPECT_EQ(result.Columns[1].Mask, nullptr);
    EXPECT_EQ(result.Columns[2].Mask, nullptr);

    for (int64_t row = 0; row < result.RowCount; ++row) {
        const int64_t key = outKeys[row];
        const bool sumValid = IsValid(result.Columns[3], row);
        const bool minValid = IsValid(result.Columns[4], row);
        const bool maxValid = IsValid(result.Columns[5], row);
        if (key == 1) {
            EXPECT_EQ(countStar[row], 3);
            EXPECT_EQ(countArg[row], 2);
            EXPECT_EQ(sums[row], 14);
            EXPECT_EQ(mins[row], 4);
            EXPECT_EQ(maxs[row], 10);
            EXPECT_TRUE(sumValid);
            EXPECT_TRUE(minValid);
            EXPECT_TRUE(maxValid);
        } else if (key == 2) {
            EXPECT_EQ(countStar[row], 2);
            EXPECT_EQ(countArg[row], 0);
            EXPECT_FALSE(sumValid); // all-null group -> NULL aggregate output
            EXPECT_FALSE(minValid);
            EXPECT_FALSE(maxValid);
        } else {
            ASSERT_EQ(key, 3);
            EXPECT_EQ(countStar[row], 1);
            EXPECT_EQ(countArg[row], 1);
            EXPECT_EQ(sums[row], 7);
            EXPECT_EQ(mins[row], 7);
            EXPECT_EQ(maxs[row], 7);
            EXPECT_TRUE(sumValid);
            EXPECT_TRUE(minValid);
            EXPECT_TRUE(maxValid);
        }
    }
    Release(&result);
}

// A row whose argument is NULL must not contribute even when it is selected, and
// an unselected row must not contribute regardless of its argument validity.
TEST(AggregateE2E, NullableReducerArgumentHonoursSelectionMask) {
    // All rows share key 1. Selected: 0, 3, 4 (row 4 has a NULL argument).
    std::array<int64_t, 5> keys = {1, 1, 1, 1, 1};
    std::array<int64_t, 5> values = {10, 999, 777, 5, 888};
    std::array<uint8_t, 1> mask = {0b00001011}; // valid at rows 0, 1, 3
    std::array<uint8_t, 5> selection = {1, 0, 0, 1, 1};
    std::vector<TColumn> columns = {
        TColumn{.Data = reinterpret_cast<char*>(keys.data())},
        TColumn{.Data = reinterpret_cast<char*>(values.data()),
            .Mask = mask.data()},
    };
    std::vector<TRowSet> batches = {TRowSet{
        .Columns = columns.data(), .ColumnCount = 2, .RowCount = 5,
        .Selection = selection.data(), .RefCount = 1}};
    NQdb::TMockSource source(
        {"k", "v"}, std::move(batches),
        {std::make_shared<TIntegerType>(),
         std::make_shared<TNullable>(std::make_shared<TIntegerType>())});
    auto root = ParsePlan(
        "(rel aggregate (rel source \"data.parquet\") (keys k) "
        "(agg c count) (agg cn count v) (agg s sum v) "
        "(agg mn min v) (agg mx max v))",
        source);
    auto runtime = RunPlan(root);

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.RowCount, 1);
    EXPECT_EQ(reinterpret_cast<int64_t*>(result.Columns[1].Data)[0], 3); // count(*)
    EXPECT_EQ(reinterpret_cast<int64_t*>(result.Columns[2].Data)[0], 2); // count(v)
    EXPECT_EQ(reinterpret_cast<int64_t*>(result.Columns[3].Data)[0], 15); // 10 + 5
    EXPECT_EQ(reinterpret_cast<int64_t*>(result.Columns[4].Data)[0], 5);
    EXPECT_EQ(reinterpret_cast<int64_t*>(result.Columns[5].Data)[0], 10);
    EXPECT_TRUE(IsValid(result.Columns[3], 0));
    EXPECT_TRUE(IsValid(result.Columns[4], 0));
    EXPECT_TRUE(IsValid(result.Columns[5], 0));
    Release(&result);
}

// Valid-count buffers must accumulate across multiple update batches, including
// through a hash-table grow triggered by exceeding the initial capacity.
TEST(AggregateE2E, NullableReducerArgumentAccumulatesAcrossBatchesAndGrow) {
    constexpr int64_t Batches = 3;
    constexpr int64_t PerBatch = 8;
    std::vector<std::vector<int64_t>> keyStore(Batches);
    std::vector<std::vector<int64_t>> valStore(Batches);
    std::vector<std::vector<uint8_t>> maskStore(Batches);
    std::vector<std::vector<TColumn>> columnStore(Batches);
    std::vector<TRowSet> batches;

    struct TRef {
        int64_t CountStar = 0;
        int64_t CountArg = 0;
        int64_t Sum = 0;
        int64_t Min = 0;
        int64_t Max = 0;
        bool Seen = false;
    };
    std::map<int64_t, TRef> reference;

    for (int64_t b = 0; b < Batches; ++b) {
        keyStore[b].resize(PerBatch);
        valStore[b].resize(PerBatch);
        maskStore[b].assign((PerBatch + 7) / 8, 0);
        for (int64_t i = 0; i < PerBatch; ++i) {
            const int64_t row = b * PerBatch + i;
            const int64_t key = row % 10; // 10 distinct keys forces a grow
            const int64_t value = row + 1;
            const bool valid = (row % 3 != 0);
            keyStore[b][i] = key;
            valStore[b][i] = valid ? value : -123456; // poison ignored values
            if (valid) {
                maskStore[b][i / 8] |= (1u << (i % 8));
            }
            auto& ref = reference[key];
            ref.CountStar += 1;
            if (valid) {
                if (!ref.Seen) {
                    ref.Min = value;
                    ref.Max = value;
                    ref.Seen = true;
                } else {
                    ref.Min = std::min(ref.Min, value);
                    ref.Max = std::max(ref.Max, value);
                }
                ref.CountArg += 1;
                ref.Sum += value;
            }
        }
        columnStore[b] = {
            TColumn{.Data = reinterpret_cast<char*>(keyStore[b].data())},
            TColumn{.Data = reinterpret_cast<char*>(valStore[b].data()),
                .Mask = maskStore[b].data()},
        };
        batches.push_back(TRowSet{
            .Columns = columnStore[b].data(), .ColumnCount = 2,
            .RowCount = PerBatch, .Selection = nullptr, .RefCount = 1});
    }

    NQdb::TMockSource source(
        {"k", "v"}, std::move(batches),
        {std::make_shared<TIntegerType>(),
         std::make_shared<TNullable>(std::make_shared<TIntegerType>())});
    auto root = ParsePlan(
        "(rel aggregate (rel source \"data.parquet\") (keys k) "
        "(agg c count) (agg cn count v) (agg s sum v) "
        "(agg mn min v) (agg mx max v))",
        source);
    auto runtime = RunPlan(root);

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.RowCount, static_cast<int64_t>(reference.size()));
    auto* outKeys = reinterpret_cast<int64_t*>(result.Columns[0].Data);
    auto* countStar = reinterpret_cast<int64_t*>(result.Columns[1].Data);
    auto* countArg = reinterpret_cast<int64_t*>(result.Columns[2].Data);
    auto* sums = reinterpret_cast<int64_t*>(result.Columns[3].Data);
    auto* mins = reinterpret_cast<int64_t*>(result.Columns[4].Data);
    auto* maxs = reinterpret_cast<int64_t*>(result.Columns[5].Data);
    for (int64_t row = 0; row < result.RowCount; ++row) {
        const auto it = reference.find(outKeys[row]);
        ASSERT_NE(it, reference.end());
        const auto& ref = it->second;
        EXPECT_EQ(countStar[row], ref.CountStar) << "key " << outKeys[row];
        EXPECT_EQ(countArg[row], ref.CountArg) << "key " << outKeys[row];
        EXPECT_EQ(IsValid(result.Columns[3], row), ref.Seen);
        if (ref.Seen) {
            EXPECT_EQ(sums[row], ref.Sum) << "key " << outKeys[row];
            EXPECT_EQ(mins[row], ref.Min) << "key " << outKeys[row];
            EXPECT_EQ(maxs[row], ref.Max) << "key " << outKeys[row];
        }
    }
    Release(&result);
}

// Regression: a non-nullable argument column must keep aggregate outputs
// non-nullable (no mask), and count(arg) must equal count(*) since no argument
// can be skipped.
TEST(AggregateE2E, NonNullableReducerArgumentKeepsAggregateOutputNonNull) {
    std::array<int64_t, 5> keys = {1, 1, 2, 2, 2};
    std::array<int64_t, 5> values = {10, 4, 7, 3, 5};
    std::vector<TColumn> columns = {
        TColumn{.Data = reinterpret_cast<char*>(keys.data())},
        TColumn{.Data = reinterpret_cast<char*>(values.data())},
    };
    std::vector<TRowSet> batches = {TRowSet{
        .Columns = columns.data(), .ColumnCount = 2, .RowCount = 5,
        .Selection = nullptr, .RefCount = 1}};
    NQdb::TMockSource source(
        {"k", "v"}, std::move(batches),
        {std::make_shared<TIntegerType>(),
         std::make_shared<TIntegerType>()});
    auto root = ParsePlan(
        "(rel aggregate (rel source \"data.parquet\") (keys k) "
        "(agg c count) (agg cn count v) (agg s sum v) "
        "(agg mn min v) (agg mx max v))",
        source);
    auto runtime = RunPlan(root);

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.RowCount, 2);
    // Non-nullable argument: no aggregate output carries a mask.
    for (int col = 1; col <= 5; ++col) {
        EXPECT_EQ(result.Columns[col].Mask, nullptr) << "column " << col;
    }
    auto* outKeys = reinterpret_cast<int64_t*>(result.Columns[0].Data);
    auto* countStar = reinterpret_cast<int64_t*>(result.Columns[1].Data);
    auto* countArg = reinterpret_cast<int64_t*>(result.Columns[2].Data);
    auto* sums = reinterpret_cast<int64_t*>(result.Columns[3].Data);
    auto* mins = reinterpret_cast<int64_t*>(result.Columns[4].Data);
    auto* maxs = reinterpret_cast<int64_t*>(result.Columns[5].Data);
    for (int64_t row = 0; row < result.RowCount; ++row) {
        EXPECT_EQ(countStar[row], countArg[row]);
        if (outKeys[row] == 1) {
            EXPECT_EQ(countStar[row], 2);
            EXPECT_EQ(sums[row], 14);
            EXPECT_EQ(mins[row], 4);
            EXPECT_EQ(maxs[row], 10);
        } else {
            ASSERT_EQ(outKeys[row], 2);
            EXPECT_EQ(countStar[row], 3);
            EXPECT_EQ(sums[row], 15);
            EXPECT_EQ(mins[row], 3);
            EXPECT_EQ(maxs[row], 7);
        }
    }
    Release(&result);
}

// Composite {i32, string} key combined with a nullable argument across all four
// aggregate functions.
TEST(AggregateE2E, NullableReducerArgumentWithCompositeStringKey) {
    std::array<int32_t, 5> ids = {1, 1, 2, 2, 1};
    std::string names = "aabba";
    std::array<int32_t, 6> offsets = {0, 1, 2, 3, 4, 5};
    std::array<int64_t, 5> values = {10, 999, 4, 6, 888};
    std::array<uint8_t, 1> mask = {0b00001101}; // valid at rows 0, 2, 3
    std::vector<TColumn> columns = {
        TColumn{.Data = reinterpret_cast<char*>(ids.data())},
        TColumn{.Data = names.data(), .Mask = nullptr,
            .Offsets = offsets.data(), .OffsetWidth = 4},
        TColumn{.Data = reinterpret_cast<char*>(values.data()),
            .Mask = mask.data()},
    };
    std::vector<TRowSet> batches = {TRowSet{
        .Columns = columns.data(), .ColumnCount = 3, .RowCount = 5,
        .Selection = nullptr, .RefCount = 1}};
    NQdb::TMockSource source(
        {"id", "name", "v"}, std::move(batches),
        {std::make_shared<TIntegerType>(TIntegerType::I32),
         std::make_shared<TStringType>(),
         std::make_shared<TNullable>(std::make_shared<TIntegerType>())});
    auto root = ParsePlan(
        "(rel aggregate (rel source \"data.parquet\") (keys id name) "
        "(agg c count) (agg cn count v) (agg s sum v) "
        "(agg mn min v) (agg mx max v))",
        source);
    auto runtime = RunPlan(root);

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.ColumnCount, 7); // id, name, c, cn, s, mn, mx
    ASSERT_EQ(result.RowCount, 2);
    auto* outIds = reinterpret_cast<int32_t*>(result.Columns[0].Data);
    auto* outOffsets = static_cast<int64_t*>(result.Columns[1].Offsets);
    auto* countStar = reinterpret_cast<int64_t*>(result.Columns[2].Data);
    auto* countArg = reinterpret_cast<int64_t*>(result.Columns[3].Data);
    auto* sums = reinterpret_cast<int64_t*>(result.Columns[4].Data);
    auto* mins = reinterpret_cast<int64_t*>(result.Columns[5].Data);
    auto* maxs = reinterpret_cast<int64_t*>(result.Columns[6].Data);
    for (int64_t row = 0; row < result.RowCount; ++row) {
        const std::string name(result.Columns[1].Data + outOffsets[row],
            result.Columns[1].Data + outOffsets[row + 1]);
        if (outIds[row] == 1) {
            EXPECT_EQ(name, "a");
            EXPECT_EQ(countStar[row], 3);
            EXPECT_EQ(countArg[row], 1);
            EXPECT_EQ(sums[row], 10);
            EXPECT_EQ(mins[row], 10);
            EXPECT_EQ(maxs[row], 10);
        } else {
            ASSERT_EQ(outIds[row], 2);
            EXPECT_EQ(name, "b");
            EXPECT_EQ(countStar[row], 2);
            EXPECT_EQ(countArg[row], 2);
            EXPECT_EQ(sums[row], 10);
            EXPECT_EQ(mins[row], 4);
            EXPECT_EQ(maxs[row], 6);
        }
        EXPECT_TRUE(IsValid(result.Columns[4], row));
        EXPECT_TRUE(IsValid(result.Columns[5], row));
        EXPECT_TRUE(IsValid(result.Columns[6], row));
    }
    Release(&result);
}

// Null string key combined with a nullable argument: the NULL-key group and the
// valid-key group must each track argument validity independently.
TEST(AggregateE2E, NullableReducerArgumentWithNullStringKey) {
    std::string data = "xx"; // rows: "x", "", "", "x"
    std::array<int32_t, 5> offsets = {0, 1, 1, 1, 2};
    std::array<uint8_t, 1> keyMask = {0b00001001}; // key valid at rows 0, 3
    std::array<int64_t, 4> values = {10, 999, 5, 888};
    std::array<uint8_t, 1> valMask = {0b00000101}; // arg valid at rows 0, 2
    std::vector<TColumn> columns = {
        TColumn{.Data = data.data(), .Mask = keyMask.data(),
            .Offsets = offsets.data(), .OffsetWidth = 4},
        TColumn{.Data = reinterpret_cast<char*>(values.data()),
            .Mask = valMask.data()},
    };
    std::vector<TRowSet> batches = {TRowSet{
        .Columns = columns.data(), .ColumnCount = 2, .RowCount = 4,
        .Selection = nullptr, .RefCount = 1}};
    NQdb::TMockSource source(
        {"k", "v"}, std::move(batches),
        {std::make_shared<TNullable>(std::make_shared<TStringType>()),
         std::make_shared<TNullable>(std::make_shared<TIntegerType>())});
    auto root = ParsePlan(
        "(rel aggregate (rel source \"data.parquet\") (keys k) "
        "(agg c count) (agg cn count v) (agg s sum v))",
        source);
    auto runtime = RunPlan(root);

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.RowCount, 2);
    auto* countStar = reinterpret_cast<int64_t*>(result.Columns[1].Data);
    auto* countArg = reinterpret_cast<int64_t*>(result.Columns[2].Data);
    auto* sums = reinterpret_cast<int64_t*>(result.Columns[3].Data);
    auto* outOffsets = static_cast<int64_t*>(result.Columns[0].Offsets);
    for (int64_t row = 0; row < result.RowCount; ++row) {
        const std::string key(result.Columns[0].Data + outOffsets[row],
            result.Columns[0].Data + outOffsets[row + 1]);
        // Valid string-key group: rows 0 ("x", v=10) and 3 (v NULL).
        // NULL-key group: rows 1 (v NULL) and 2 (v=5); finalizes to empty bytes.
        EXPECT_EQ(countStar[row], 2);
        EXPECT_EQ(countArg[row], 1);
        if (IsValid(result.Columns[0], row)) {
            EXPECT_EQ(key, "x");
            EXPECT_EQ(sums[row], 10);
        } else {
            EXPECT_TRUE(key.empty());
            EXPECT_EQ(sums[row], 5);
        }
        EXPECT_TRUE(IsValid(result.Columns[3], row));
    }
    Release(&result);
}

namespace {

void CheckAggregateStatesAcrossRepeatedGrowth(bool stringKeys) {
    auto keyType = stringKeys
        ? TTypePtr(std::make_shared<TStringType>())
        : TTypePtr(std::make_shared<TIntegerType>());
    TStructType input({{"k", keyType}, {"v", std::make_shared<TIntegerType>()}});
    std::vector<TAggregateSpec> aggs = {{
        .Name = "s", .Func = "sum",
        .Arg = std::make_shared<TIdentExpr>(NQumir::TLocation{}, "v"),
    }};
    TKernelCompiler compiler;
    TAggregateProcessor processor(compiler.CompileAggregate(
        NKernel::BuildAggregateKernelSpec(input, {"k"}, aggs)), {.InitialCapacity = 8});
    std::map<int64_t, int64_t> expected;
    std::vector<int64_t> insertionOrder;
    // Interleave inserts with updates across enough batches to grow repeatedly.
    // Each batch owns its input strings only until Add returns.
    for (int64_t batch = 0; batch < 12; ++batch) {
        std::vector<int64_t> keys, values, offsets{0};
        std::string data;
        for (int64_t row = 0; row < 197; ++row) {
            const int64_t key = ((batch * 197 + row) * 479) % 773;
            const int64_t value = row - 100 + batch;
            if (!expected.contains(key)) {
                insertionOrder.push_back(key);
            }
            expected[key] += value;
            keys.push_back(key);
            values.push_back(value);
            data += "owned_string_key_" + std::to_string(key);
            offsets.push_back(static_cast<int64_t>(data.size()));
        }
        std::array<TColumn, 2> columns = {
            TColumn{.Data = stringKeys ? data.data() : reinterpret_cast<char*>(keys.data()),
                .Offsets = stringKeys ? offsets.data() : nullptr, .OffsetWidth = 8},
            TColumn{.Data = reinterpret_cast<char*>(values.data())},
        };
        TRowSet rows{.Columns = columns.data(), .ColumnCount = 2,
            .RowCount = 197, .RefCount = 1};
        processor.Add(rows);
    }
    TRowSet result{};
    processor.Finish(result);
    ASSERT_EQ(result.RowCount, static_cast<int64_t>(expected.size()));
    auto* sums = reinterpret_cast<const int64_t*>(result.Columns[1].Data);
    auto* offsets = static_cast<const int64_t*>(result.Columns[0].Offsets);
    std::set<int64_t> seen;
    for (int64_t row = 0; row < result.RowCount; ++row) {
        int64_t key;
        if (stringKeys) {
            std::string value(result.Columns[0].Data + offsets[row],
                result.Columns[0].Data + offsets[row + 1]);
            key = std::stoll(value.substr(std::string("owned_string_key_").size()));
        } else {
            key = reinterpret_cast<const int64_t*>(result.Columns[0].Data)[row];
        }
        // Fixed-width keys are emitted in slot order, strings in insertion order.
        if (stringKeys) {
            EXPECT_EQ(key, insertionOrder[row]);
        } else {
            EXPECT_TRUE(seen.insert(key).second) << "key " << key;
        }
        EXPECT_EQ(sums[row], expected.at(key)) << "key " << key;
    }
    Release(&result);
}

} // namespace

TEST(AggregateE2E, IntegerStatesSurviveRepeatedGrowth) {
    CheckAggregateStatesAcrossRepeatedGrowth(false);
}

TEST(AggregateE2E, StringKeyStatesPreserveOwnershipAcrossRepeatedGrowth) {
    CheckAggregateStatesAcrossRepeatedGrowth(true);
}

TEST(AggregateE2E, NullableFloatAndWideStatesSurviveRepeatedGrowth) {
    TStructType input({
        {"k", std::make_shared<TIntegerType>()},
        {"f", std::make_shared<TNullable>(std::make_shared<TFloatType>())},
        {"d", std::make_shared<TNullable>(std::make_shared<TDecimal>(28, 0))},
    });
    std::vector<TAggregateSpec> aggs;
    auto add = [&](const std::string& name, const std::string& func, const std::string& arg) {
        aggs.push_back({
            .Name = name,
            .Func = func,
            .Arg = arg.empty()
                ? nullptr
                : std::make_shared<TIdentExpr>(NQumir::TLocation{}, arg),
        });
    };
    add("c", "count", "");
    add("cn", "count", "f");
    for (const auto& column : {"f", "d"}) {
        for (const auto& func : {"sum", "min", "max"}) {
            add(std::string(column) + func, func, column);
        }
    }
    TKernelCompiler compiler;
    TAggregateProcessor processor(compiler.CompileAggregate(
        NKernel::BuildAggregateKernelSpec(input, {"k"}, aggs)), {.InitialCapacity = 8});
    struct TReference {
        int64_t Count = 0;
        int64_t ValidCount = 0;
        double FloatSum = 0;
        double FloatMin = 0;
        double FloatMax = 0;
        __int128 WideSum = 0;
        __int128 WideMin = 0;
        __int128 WideMax = 0;
    };
    std::map<int64_t, TReference> expected;
    constexpr __int128 wideUnit = static_cast<__int128>(1) << 80;
    constexpr int64_t rowsPerBatch = 197;
    for (int64_t batch = 0; batch < 8; ++batch) {
        std::vector<int64_t> keys(rowsPerBatch);
        std::vector<double> floats(rowsPerBatch);
        std::vector<__int128> wide(rowsPerBatch);
        std::vector<uint8_t> validity((rowsPerBatch + 7) / 8, 0);
        std::vector<uint8_t> selection(rowsPerBatch, 0);
        for (int64_t row = 0; row < rowsPerBatch; ++row) {
            const auto key = ((batch * rowsPerBatch + row) * 191) % 353;
            const double value = (row - 100 + batch) * 0.25;
            const __int128 wideValue = wideUnit * (key - 176) + row - 100 + batch;
            const bool valid = key % 17 != 0 && row % 4 != 0;
            keys[row] = key;
            floats[row] = valid ? value : -123456.75;
            wide[row] = valid ? wideValue : -123456;
            if (valid) {
                validity[row / 8] |= uint8_t{1} << (row % 8);
            }
            if (row % 11 == 0) {
                continue;
            }
            selection[row] = 1;
            auto& state = expected[key];
            ++state.Count;
            if (!valid) {
                continue;
            }
            if (state.ValidCount == 0) {
                state.FloatMin = value;
                state.FloatMax = value;
                state.WideMin = wideValue;
                state.WideMax = wideValue;
            } else {
                state.FloatMin = std::min(state.FloatMin, value);
                state.FloatMax = std::max(state.FloatMax, value);
                state.WideMin = std::min(state.WideMin, wideValue);
                state.WideMax = std::max(state.WideMax, wideValue);
            }
            ++state.ValidCount;
            state.FloatSum += value;
            state.WideSum += wideValue;
        }
        std::array<TColumn, 3> columns = {
            TColumn{.Data = reinterpret_cast<char*>(keys.data())},
            TColumn{.Data = reinterpret_cast<char*>(floats.data()), .Mask = validity.data()},
            TColumn{.Data = reinterpret_cast<char*>(wide.data()), .Mask = validity.data()},
        };
        TRowSet rows{
            .Columns = columns.data(),
            .ColumnCount = 3,
            .RowCount = rowsPerBatch,
            .Selection = selection.data(),
            .RefCount = 1,
        };
        processor.Add(rows);
    }
    TRowSet result{};
    ASSERT_TRUE(processor.Finish(result));
    ASSERT_EQ(result.ColumnCount, 9);
    ASSERT_EQ(result.RowCount, static_cast<int64_t>(expected.size()));
    std::set<int64_t> seen;
    for (int64_t row = 0; row < result.RowCount; ++row) {
        const auto key = reinterpret_cast<const int64_t*>(result.Columns[0].Data)[row];
        ASSERT_TRUE(seen.insert(key).second) << "key " << key;
        ASSERT_TRUE(expected.contains(key)) << "key " << key;
        const auto& state = expected.at(key);
        EXPECT_EQ(reinterpret_cast<const int64_t*>(result.Columns[1].Data)[row], state.Count);
        EXPECT_EQ(reinterpret_cast<const int64_t*>(result.Columns[2].Data)[row], state.ValidCount);
        for (int col = 3; col < 9; ++col) {
            EXPECT_EQ(IsValid(result.Columns[col], row), state.ValidCount > 0);
        }
        if (state.ValidCount == 0) {
            continue;
        }
        EXPECT_DOUBLE_EQ(reinterpret_cast<const double*>(result.Columns[3].Data)[row], state.FloatSum);
        EXPECT_DOUBLE_EQ(reinterpret_cast<const double*>(result.Columns[4].Data)[row], state.FloatMin);
        EXPECT_DOUBLE_EQ(reinterpret_cast<const double*>(result.Columns[5].Data)[row], state.FloatMax);
        EXPECT_TRUE(reinterpret_cast<const __int128*>(result.Columns[6].Data)[row] == state.WideSum);
        EXPECT_TRUE(reinterpret_cast<const __int128*>(result.Columns[7].Data)[row] == state.WideMin);
        EXPECT_TRUE(reinterpret_cast<const __int128*>(result.Columns[8].Data)[row] == state.WideMax);
    }
    Release(&result);
}

TEST(AggregateE2E, MixedStringAndNumericReducersSurviveRepeatedGrowth) {
    TStructType input({
        {"k", std::make_shared<TIntegerType>()},
        {"v", std::make_shared<TIntegerType>()},
        {"s", std::make_shared<TStringType>()},
    });
    std::vector<TAggregateSpec> aggs = {
        {.Name = "sum", .Func = "sum", .Arg = std::make_shared<TIdentExpr>(NQumir::TLocation{}, "v")},
        {.Name = "min", .Func = "min", .Arg = std::make_shared<TIdentExpr>(NQumir::TLocation{}, "s")},
        {.Name = "max", .Func = "max", .Arg = std::make_shared<TIdentExpr>(NQumir::TLocation{}, "s")},
    };
    TKernelCompiler compiler;
    TAggregateProcessor processor(compiler.CompileAggregate(
        NKernel::BuildAggregateKernelSpec(input, {"k"}, aggs)), {.InitialCapacity = 8});
    struct TReference {
        int64_t Sum = 0;
        std::string Min;
        std::string Max;
    };
    std::map<int64_t, TReference> expected;
    std::vector<int64_t> insertionOrder;
    constexpr int64_t rowsPerBatch = 197;
    for (int64_t batch = 0; batch < 8; ++batch) {
        std::vector<int64_t> keys, values, offsets{0};
        std::string data;
        for (int64_t row = 0; row < rowsPerBatch; ++row) {
            const auto key = ((batch * rowsPerBatch + row) * 191) % 353;
            const auto value = row - 100 + batch;
            const std::string text = std::to_string((row + batch) % 23) +
                "_owned_reducer_string_" + std::to_string(key);
            if (!expected.contains(key)) {
                insertionOrder.push_back(key);
                expected[key].Min = text;
                expected[key].Max = text;
            }
            auto& state = expected[key];
            state.Sum += value;
            state.Min = std::min(state.Min, text);
            state.Max = std::max(state.Max, text);
            keys.push_back(key);
            values.push_back(value);
            data += text;
            offsets.push_back(static_cast<int64_t>(data.size()));
        }
        std::array<TColumn, 3> columns = {
            TColumn{.Data = reinterpret_cast<char*>(keys.data())},
            TColumn{.Data = reinterpret_cast<char*>(values.data())},
            TColumn{.Data = data.data(), .Offsets = offsets.data(), .OffsetWidth = 8},
        };
        TRowSet rows{
            .Columns = columns.data(),
            .ColumnCount = 3,
            .RowCount = rowsPerBatch,
            .RefCount = 1,
        };
        processor.Add(rows);
    }
    TRowSet result{};
    ASSERT_TRUE(processor.Finish(result));
    ASSERT_EQ(result.ColumnCount, 4);
    ASSERT_EQ(result.RowCount, static_cast<int64_t>(expected.size()));
    for (int64_t row = 0; row < result.RowCount; ++row) {
        const auto key = reinterpret_cast<const int64_t*>(result.Columns[0].Data)[row];
        ASSERT_EQ(key, insertionOrder[row]);
        const auto& state = expected.at(key);
        EXPECT_EQ(reinterpret_cast<const int64_t*>(result.Columns[1].Data)[row], state.Sum);
        EXPECT_EQ(StringAt(result.Columns[2], row), state.Min);
        EXPECT_EQ(StringAt(result.Columns[3], row), state.Max);
    }
    Release(&result);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    NQumir::NCodeGen::TLLVMInitializer llvmInit;
    return RUN_ALL_TESTS();
}
