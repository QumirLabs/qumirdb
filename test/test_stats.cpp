#include <gtest/gtest.h>

#include <qdb/io/parquet/source.h>
#include <qdb/exec/planner_helpers.h>
#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/ops/stats.h>
#include <qdb/plan/pipeline.h>
#include <qdb/plan/passes/cbo/dpccp.h>
#include <qdb/plan/passes/const_fold.h>
#include <qdb/plan/passes/estimate_stats.h>
#include <qdb/plan/passes/qualify_columns.h>
#include <qdb/plan/passes/row_group_predicate.h>
#include <qdb/plan/passes/typing.h>
#include <qdb/plan/types/nullable.h>
#include <qdb/scheduler/plan_lowerer.h>

#include <qumir/parser/core/printer.h>
#include <qumir/parser/type.h>

#include <bit>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/writer.h>

using NQdb::TStats;
using TColumnStats = NQdb::TStats::TColumnStats;

namespace {

// Build a histogram (raw uint64 bit patterns) from typed boundary values.
template<typename T>
std::vector<uint64_t> Hist(std::initializer_list<T> values) {
    std::vector<uint64_t> out;
    out.reserve(values.size());
    for (T v : values) {
        out.push_back(std::bit_cast<uint64_t>(v));
    }
    return out;
}

void WriteParquet(
    const std::string& path,
    std::shared_ptr<arrow::RecordBatch> batch,
    int64_t rowGroupRows = 1024)
{
    auto outfile = arrow::io::FileOutputStream::Open(path).ValueOrDie();
    auto table = arrow::Table::FromRecordBatches({batch}).ValueOrDie();
    auto status = parquet::arrow::WriteTable(
        *table, arrow::default_memory_pool(), outfile, rowGroupRows);
    ASSERT_TRUE(status.ok()) << status.ToString();
}

} // namespace

// --- TColumnStats scalar accessors: bit_cast round-trip ----------------------

TEST(StatsTest, MinMaxRoundTrip) {
    TColumnStats cs;
    cs.MinValue = std::bit_cast<uint64_t>(int64_t{-7});
    cs.MaxValue = std::bit_cast<uint64_t>(int64_t{42});
    EXPECT_EQ(cs.GetMinValue<int64_t>().value(), -7);
    EXPECT_EQ(cs.GetMaxValue<int64_t>().value(), 42);

    TColumnStats fd;
    fd.MinValue = std::bit_cast<uint64_t>(double{0.5});
    fd.MaxValue = std::bit_cast<uint64_t>(double{3.5});
    EXPECT_DOUBLE_EQ(fd.GetMinValue<double>().value(), 0.5);
    EXPECT_DOUBLE_EQ(fd.GetMaxValue<double>().value(), 3.5);

    TColumnStats empty;
    EXPECT_FALSE(empty.GetMinValue<int64_t>().has_value());
}

// --- FractionBelow: equi-depth histogram selectivity -------------------------

TEST(StatsTest, FractionBelowUniformInt) {
    // 100 buckets over [0, 1000): boundaries 0,10,...,1000.
    TColumnStats cs;
    std::vector<uint64_t> h;
    for (int64_t v = 0; v <= 1000; v += 10) h.push_back(std::bit_cast<uint64_t>(v));
    cs.Histogram = std::move(h);

    EXPECT_DOUBLE_EQ(cs.FractionBelow<int64_t>(-1).value(), 0.0);   // below min
    EXPECT_DOUBLE_EQ(cs.FractionBelow<int64_t>(1000).value(), 1.0); // at/above max
    EXPECT_DOUBLE_EQ(cs.FractionBelow<int64_t>(500).value(), 0.5);  // boundary
    EXPECT_DOUBLE_EQ(cs.FractionBelow<int64_t>(250).value(), 0.25);
    EXPECT_NEAR(cs.FractionBelow<int64_t>(505).value(), 0.505, 1e-9); // mid-bucket interp
}

TEST(StatsTest, FractionBelowDouble) {
    TColumnStats cs;
    cs.Histogram = Hist<double>({0.0, 0.5, 1.0}); // 2 buckets
    EXPECT_DOUBLE_EQ(cs.FractionBelow<double>(0.5).value(), 0.5);
    EXPECT_DOUBLE_EQ(cs.FractionBelow<double>(0.25).value(), 0.25);
    EXPECT_DOUBLE_EQ(cs.FractionBelow<double>(-1.0).value(), 0.0);
    EXPECT_DOUBLE_EQ(cs.FractionBelow<double>(2.0).value(), 1.0);
}

TEST(StatsTest, FractionBelowRepeatedBoundaries) {
    // Heavy low value: several buckets collapse at 0 (low-cardinality column).
    TColumnStats cs;
    cs.Histogram = Hist<int64_t>({0, 0, 0, 10, 20}); // 4 buckets
    // No divide-by-zero on the repeated [0,0] buckets; monotone result.
    const double at0 = cs.FractionBelow<int64_t>(0).value();
    const double at5 = cs.FractionBelow<int64_t>(5).value();
    const double at15 = cs.FractionBelow<int64_t>(15).value();
    EXPECT_GE(at0, 0.0);
    EXPECT_LE(at0, at5);
    EXPECT_LE(at5, at15);
    EXPECT_LE(at15, 1.0);
}

TEST(StatsTest, FractionBelowAcceptsAnyBound) {
    TColumnStats cs;
    cs.Histogram = Hist<double>({0.0, 0.5, 1.0});
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    EXPECT_FALSE(cs.FractionBelow(nan).has_value());
    EXPECT_FALSE(cs.FractionBelow(nan, true).has_value());
    EXPECT_DOUBLE_EQ(cs.FractionBelow(inf, true).value(), 1.0);
    EXPECT_DOUBLE_EQ(cs.FractionBelow(-inf).value(), 0.0);

    // The width of this bucket does not fit in int64.
    TColumnStats wide;
    wide.Histogram = Hist<int64_t>({std::numeric_limits<int64_t>::min(), 0,
        std::numeric_limits<int64_t>::max()});
    EXPECT_DOUBLE_EQ(wide.FractionBelow<int64_t>(0, true).value(), 0.5);
}

TEST(StatsTest, FractionBelowEmptyHistogram) {
    TColumnStats cs;
    EXPECT_FALSE(cs.FractionBelow<int64_t>(5).has_value());
}

TEST(StatsTest, FractionBelowStrictExcludesTheValue) {
    // The value 5 holds half of the rows, so it repeats as a boundary.
    TColumnStats cs;
    cs.Histogram = Hist<int64_t>({0, 5, 5, 5, 10}); // 4 buckets
    EXPECT_DOUBLE_EQ(cs.FractionBelow<int64_t>(5).value(), 0.75);       // <= 5
    EXPECT_DOUBLE_EQ(cs.FractionBelow<int64_t>(5, true).value(), 0.25); // < 5
    // Between heavy values there is nothing to exclude.
    EXPECT_DOUBLE_EQ(cs.FractionBelow<int64_t>(7, true).value(),
        cs.FractionBelow<int64_t>(7).value());
    EXPECT_DOUBLE_EQ(cs.FractionBelow<int64_t>(0, true).value(), 0.0);  // nothing below min
    EXPECT_DOUBLE_EQ(cs.FractionBelow<int64_t>(11, true).value(), 1.0);

    // A heavy maximum is not below itself.
    TColumnStats top;
    top.Histogram = Hist<int64_t>({0, 5, 10, 10, 10});
    EXPECT_DOUBLE_EQ(top.FractionBelow<int64_t>(10).value(), 1.0);
    EXPECT_DOUBLE_EQ(top.FractionBelow<int64_t>(10, true).value(), 0.5);
}

// --- TParquetSource::Stats(): row count + standard column stats --------------

TEST(StatsTest, ParquetStandardStats) {
    const std::string path = "/tmp/test_stats_qdb.parquet";

    arrow::Int64Builder id;
    arrow::DoubleBuilder val;
    arrow::StringBuilder name;
    (void)id.AppendValues({1, 2, 3, 4});
    (void)val.AppendValues({1.5, 2.5, 3.5, 0.5});
    (void)name.Append("a");
    (void)name.Append("b");
    (void)name.AppendNull();
    (void)name.Append("d");

    auto schema = arrow::schema({
        arrow::field("id", arrow::int64(), /*nullable*/ false),
        arrow::field("val", arrow::float64(), false),
        arrow::field("name", arrow::utf8(), /*nullable*/ true),
    });
    auto batch = arrow::RecordBatch::Make(schema, 4, {
        id.Finish().ValueOrDie(),
        val.Finish().ValueOrDie(),
        name.Finish().ValueOrDie(),
    });
    WriteParquet(path, batch);

    NQdb::TParquetSource source(path);
    auto st = source.Stats();
    ASSERT_TRUE(st != nullptr);          // always present: parquet carries num_rows
    EXPECT_EQ(st->RowCount, 4u);

    const auto& idStats = st->ColumnStats.at("id");
    EXPECT_EQ(idStats->GetMinValue<int64_t>().value(), 1);
    EXPECT_EQ(idStats->GetMaxValue<int64_t>().value(), 4);
    EXPECT_EQ(idStats->NullCount.value(), 0u);

    const auto& valStats = st->ColumnStats.at("val");
    EXPECT_DOUBLE_EQ(valStats->GetMinValue<double>().value(), 0.5);
    EXPECT_DOUBLE_EQ(valStats->GetMaxValue<double>().value(), 3.5);

    const auto& nameStats = st->ColumnStats.at("name");
    EXPECT_EQ(nameStats->NullCount.value(), 1u);   // one null aggregated
    EXPECT_FALSE(nameStats->MinValue.has_value());  // string: no numeric min/max
}

TEST(StatsTest, ParquetRowGroupPredicateUsesPerGroupBounds) {
    const std::string path = "/tmp/test_stats_qdb_row_groups.parquet";
    arrow::Int64Builder x;
    (void)x.AppendValues({0, 1, 100, 101, 0, 1});
    auto schema = arrow::schema({
        arrow::field("x", arrow::int64(), /*nullable*/ false),
    });
    auto batch = arrow::RecordBatch::Make(
        schema, 6, {x.Finish().ValueOrDie()});
    WriteParquet(path, batch, /*rowGroupRows*/ 2);

    NQdb::TParquetSource source(path);
    ASSERT_EQ(source.ScanRowGroups().size(), 3u);
    NQdb::TOperatorPtr sourceOp =
        std::make_shared<NQdb::TSourceOperator>(source, path);

    auto filter = NQdb::MakeFilter(sourceOp, "(> x 50)");
    ASSERT_TRUE(filter.has_value()) << filter.error().ToString();
    NQdb::AnnotateTypes(*filter);
    auto typedFilter = NQdb::TMaybeOp<NQdb::TFilterOperator>(*filter).Cast();
    auto kept = source.PruneRowGroups(typedFilter->Predicate(), "");
    ASSERT_EQ(kept.size(), 1u);
    EXPECT_EQ(kept[0].RowGroup, 1u);

    auto sparse = NQdb::MakeFilter(sourceOp, "(< x 10)");
    ASSERT_TRUE(sparse.has_value()) << sparse.error().ToString();
    NQdb::AnnotateTypes(*sparse);
    auto sparseFilter = NQdb::TMaybeOp<NQdb::TFilterOperator>(*sparse).Cast();
    auto sparseGroups = source.PruneRowGroups(sparseFilter->Predicate(), "");
    ASSERT_EQ(sparseGroups.size(), 2u);
    EXPECT_EQ(sparseGroups[0].RowGroup, 0u);
    EXPECT_EQ(sparseGroups[1].RowGroup, 2u);

    auto impossible = NQdb::MakeFilter(sourceOp, "(> x 200)");
    ASSERT_TRUE(impossible.has_value()) << impossible.error().ToString();
    NQdb::AnnotateTypes(*impossible);
    auto impossibleFilter =
        NQdb::TMaybeOp<NQdb::TFilterOperator>(*impossible).Cast();
    EXPECT_TRUE(source.PruneRowGroups(impossibleFilter->Predicate(), "").empty());

    auto emptySource = source.MakeRowGroupsSource({});
    NQdb::TRowSet rowSet{};
    EXPECT_FALSE(emptySource->Next(rowSet));
}

// --- EstimateStats: filter selectivity propagation (source -> filter) ---------
// NOTE: exercises the whole stats-propagation pipeline. Expected to go green
// once EstimateStats actually computes per-node stats (calls ComputeStatsFor),
// the source's per-column stats are keyed by the qualified name the plan uses,
// and range predicates dispatch FractionBelow on the column's real type.

namespace {

// Source with known column statistics (bare column names, like TParquetSource).
// Each column is i64 with a uniform histogram over [0,1000); ndv is per-column.
struct TStatsSource : NQdb::ISource {
    std::vector<std::string> NameStore;         // owns the strings TColumnSchema views
    std::vector<NQdb::TColumnSchema> Cols;      // span target for TSchema
    NQdb::TSchema Schema_;
    NQdb::TStatsPtr Stats_;

    explicit TStatsSource(
        std::vector<std::pair<std::string, uint64_t>> cols = {{"x", 100}},
        uint64_t rowCount = 1000)
    {
        using NQumir::NAst::TIntegerType;
        Stats_ = std::make_shared<TStats>();
        Stats_->RowCount = rowCount;
        NameStore.reserve(cols.size());                     // no realloc: views stay valid
        for (const auto& [name, ndv] : cols) {
            NameStore.push_back(name);
        }
        for (size_t i = 0; i < cols.size(); ++i) {
            Cols.push_back({NameStore[i], std::make_shared<TIntegerType>(TIntegerType::I64)});
            auto cs = std::make_shared<TColumnStats>();
            cs->Ndv = cols[i].second;
            cs->MinValue = std::bit_cast<uint64_t>(int64_t{0});
            cs->MaxValue = std::bit_cast<uint64_t>(int64_t{999});
            for (int64_t v = 0; v <= 1000; v += 10) {       // uniform, 100 buckets
                cs->Histogram.push_back(std::bit_cast<uint64_t>(v));
            }
            Stats_->ColumnStats[NameStore[i]] = std::move(cs); // bare name
        }
        Schema_ = NQdb::TSchema{Cols};
    }

    const NQdb::TSchema& Schema() const override { return Schema_; }
    const NQdb::TStatsPtr Stats() const override { return Stats_; }
    bool Next(NQdb::TRowSet&) override { return false; }
};

} // namespace

TEST(StatsTest, FilterSelectivityPropagation) {
    TStatsSource src;
    auto sourceOp = std::make_shared<NQdb::TSourceOperator>(src, "t");
    NQdb::TOperatorPtr plan = sourceOp;

    auto filter = NQdb::MakeFilter(plan, "(< x 250)");
    ASSERT_TRUE(filter.has_value()) << filter.error().ToString();
    plan = *filter;

    NQdb::ApplyPlanPasses(plan);

    ASSERT_TRUE(plan->Stats_ != nullptr) << "filter got no propagated stats";
    // x < 250 over uniform [0,1000) => ~25% of 1000 rows.
    EXPECT_GE(plan->Stats_->RowCount, 200u);
    EXPECT_LE(plan->Stats_->RowCount, 300u);
    EXPECT_TRUE(sourceOp->RowGroupPredicate());
    EXPECT_TRUE(NQdb::TMaybeOp<NQdb::TFilterOperator>(plan));
}

namespace {

// A 1000-row table: x is i64, uniform on [0, 1000); n is Nullable<i64> and
// NULL in 200 rows. SQL-level selectivity cases live in the regression suite
// test/regtest/cases/selectivity; these two need what it cannot set up.
class NullCountStats : public ::testing::Test {
protected:
    NullCountStats() {
        Src.Cols[1].Type = std::make_shared<NQdb::TNullable>(Src.Cols[1].Type);
        Src.Stats_->ColumnStats.at("n")->NullCount = 200;
    }

    // Row estimate of the filters stacked on the source, innermost first. No
    // pushdown runs, so the filters stay separate.
    uint64_t Rows(std::initializer_list<const char*> predicates) {
        NQdb::TOperatorPtr plan = std::make_shared<NQdb::TSourceOperator>(Src, "t");
        for (const char* predicate : predicates) {
            auto filter = NQdb::MakeFilter(plan, predicate);
            EXPECT_TRUE(filter.has_value()) << filter.error().ToString();
            plan = *filter;
        }
        NQdb::AssignSourceAliases(plan);
        NQdb::QualifyColumns(plan);
        NQdb::AnnotateTypes(plan);
        return NQdb::EstimateStats(plan)->RowCount;
    }

    TStatsSource Src{{{"x", 100}, {"n", 100}}, 1000};
};

} // namespace

// A Parquet file always records null counts, so a suite fixture has them.
TEST_F(NullCountStats, IsNullWithoutNullCountKeepsDefaultSelectivity) {
    Src.Stats_->ColumnStats.at("n")->NullCount.reset();
    EXPECT_EQ(Rows({"(call qdb_is_null n)"}), 500u);
    EXPECT_EQ(Rows({"(! (call qdb_is_null n))"}), 500u);
}

// The pipeline pushes a single-table filter down to the scan, so SQL does not
// leave an IS NULL filter above another operator with a null count.
TEST_F(NullCountStats, NullCountAppliesOnlyAboveTheSource) {
    // 100 rows reach the outer filter, fewer than the table has nulls: the
    // table-wide count says nothing about them.
    EXPECT_EQ(Rows({"(< x 100)", "(! (call qdb_is_null n))"}), 50u);
}

TEST(StatsTest, AggregateStatsAccountForGroupingSets) {
    TStatsSource src({{"x", 100}, {"y", 100}}, 100);
    auto input = std::make_shared<NQdb::TSourceOperator>(src, "");
    input->SetAlias("t");
    auto aggregate = std::make_shared<NQdb::TAggregateOperator>(
        input, std::vector<std::string>{"t.x", "t.y"},
        std::vector<NQdb::TAggregateSpec>{});
    aggregate->MutableGroupingSets() = {{0}, {1}};

    auto stats = NQdb::EstimateStats(aggregate);

    ASSERT_TRUE(stats);
    EXPECT_EQ(stats->RowCount, 200u);
    EXPECT_DOUBLE_EQ(stats->Cost, input->Stats_->Cost
        + 2.0 * src.Stats_->RowCount * NQdb::EstimateTypeWidth(aggregate->OutputColumns()));
    EXPECT_TRUE(stats->ColumnStats.at("t.x")->Histogram.empty());
    EXPECT_TRUE(stats->ColumnStats.at("t.y")->Histogram.empty());
}

TEST(StatsTest, AggregateInitialCapacityUsesNdvAndPartitionCount) {
    TStatsSource src({{"x", 100}}, 1000);
    auto input = std::make_shared<NQdb::TSourceOperator>(src, "t");
    input->Stats_ = src.Stats_;
    NQdb::TAggregateOperator aggregate(input, {"x"}, {});

    // ceil(100 / 0.875) rounds up to the next power of two.
    EXPECT_EQ(NQdb::EstimateAggregateInitialCapacity(aggregate, 1), 128);
    // Four disjoint partition-local tables each expect 25 groups.
    EXPECT_EQ(NQdb::EstimateAggregateInitialCapacity(aggregate, 4), 32);

    NQdb::TAggregateOperator missingStats(input, {"unknown"}, {});
    EXPECT_EQ(NQdb::EstimateAggregateInitialCapacity(missingStats, 1), 8);
}

TEST(StatsTest, AggregateGroupCountUsesNdvProductAndRowCountCap) {
    TStatsSource src({{"x", 100}, {"y", 20}}, 1000);
    auto input = std::make_shared<NQdb::TSourceOperator>(src, "t");
    input->Stats_ = src.Stats_;

    NQdb::TAggregateOperator product(input, {"x", "y"}, {});
    EXPECT_EQ(NQdb::EstimateAggregateGroupCount(product), 1000);

    NQdb::TAggregateOperator oneKey(input, {"x"}, {});
    EXPECT_EQ(NQdb::EstimateAggregateGroupCount(oneKey), 100);

    NQdb::TAggregateOperator missingStats(input, {"unknown"}, {});
    EXPECT_FALSE(NQdb::EstimateAggregateGroupCount(missingStats).has_value());
}

TEST(StatsTest, AggregatePartitionsExpandForLargeFixedWidthCardinality) {
    TStatsSource src({{"x", 100'000'000}}, 100'000'000);
    auto input = std::make_shared<NQdb::TSourceOperator>(src, "t");
    input->Stats_ = src.Stats_;
    NQdb::TAggregateOperator aggregate(input, {"x"}, {});

    NQdb::NScheduler::TSettings settings;
    settings.Scheduler.WorkerCount = 18;
    EXPECT_EQ(NQdb::NScheduler::ChooseAggregatePartitionCount(
        aggregate, 32, settings), 18);

    settings.HashShuffle.MaxPartitionCount = 12;
    EXPECT_EQ(NQdb::NScheduler::ChooseAggregatePartitionCount(
        aggregate, 32, settings), 12);
}

TEST(StatsTest, AggregatePartitionsKeepBaseForUncertainOrCheapCases) {
    NQdb::NScheduler::TSettings settings;
    settings.Scheduler.WorkerCount = 18;

    TStatsSource lowGroups({{"x", 1000}}, 100'000'000);
    auto lowInput = std::make_shared<NQdb::TSourceOperator>(lowGroups, "t");
    lowInput->Stats_ = lowGroups.Stats_;
    NQdb::TAggregateOperator lowAggregate(lowInput, {"x"}, {});
    EXPECT_EQ(NQdb::NScheduler::ChooseAggregatePartitionCount(
        lowAggregate, 32, settings), 9);

    TStatsSource filtered({{"x", 30'000'000}}, 30'000'000);
    auto filteredInput = std::make_shared<NQdb::TSourceOperator>(filtered, "t");
    filteredInput->Stats_ = filtered.Stats_;
    NQdb::TAggregateOperator filteredAggregate(filteredInput, {"x"}, {});
    EXPECT_EQ(NQdb::NScheduler::ChooseAggregatePartitionCount(
        filteredAggregate, 32, settings), 9);

    TStatsSource strings({{"x", 100'000'000}}, 100'000'000);
    strings.Cols[0].Type = std::make_shared<NQumir::NAst::TStringType>();
    auto stringInput = std::make_shared<NQdb::TSourceOperator>(strings, "t");
    stringInput->Stats_ = strings.Stats_;
    NQdb::TAggregateOperator stringAggregate(stringInput, {"x"}, {});
    EXPECT_EQ(NQdb::NScheduler::ChooseAggregatePartitionCount(
        stringAggregate, 32, settings), 9);
}

TEST(StatsTest, ExplicitShufflePartitionsOverrideAggregateHeuristic) {
    TStatsSource src({{"x", 100'000'000}}, 100'000'000);
    auto input = std::make_shared<NQdb::TSourceOperator>(src, "t");
    input->Stats_ = src.Stats_;
    NQdb::TAggregateOperator aggregate(input, {"x"}, {});

    NQdb::NScheduler::TSettings settings;
    settings.Scheduler.WorkerCount = 18;
    settings.HashShuffle.PartitionCount = 7;
    settings.HashShuffle.MaxPartitionCount = 7;
    EXPECT_EQ(NQdb::NScheduler::ChooseAggregatePartitionCount(
        aggregate, 4, settings), 7);
}

TEST(StatsTest, PartialAggregateRequiresLargeEstimatedReduction) {
    NQdb::NScheduler::TSettings settings;

    TStatsSource useful({{"x", 10'000'000}}, 100'000'000);
    auto usefulInput = std::make_shared<NQdb::TSourceOperator>(useful, "t");
    usefulInput->Stats_ = useful.Stats_;
    NQdb::TAggregateOperator usefulAggregate(usefulInput, {"x"}, {});
    EXPECT_TRUE(NQdb::NScheduler::ShouldUsePartialAggregate(
        usefulAggregate, settings));

    TStatsSource weak({{"x", 30'000'000}}, 100'000'000);
    auto weakInput = std::make_shared<NQdb::TSourceOperator>(weak, "t");
    weakInput->Stats_ = weak.Stats_;
    NQdb::TAggregateOperator weakAggregate(weakInput, {"x"}, {});
    EXPECT_FALSE(NQdb::NScheduler::ShouldUsePartialAggregate(
        weakAggregate, settings));

    TStatsSource small({{"x", 10}}, 1000);
    auto smallInput = std::make_shared<NQdb::TSourceOperator>(small, "t");
    smallInput->Stats_ = small.Stats_;
    NQdb::TAggregateOperator smallAggregate(smallInput, {"x"}, {});
    EXPECT_FALSE(NQdb::NScheduler::ShouldUsePartialAggregate(
        smallAggregate, settings));

    settings.Aggregate.EnablePartial = false;
    EXPECT_FALSE(NQdb::NScheduler::ShouldUsePartialAggregate(
        usefulAggregate, settings));
}

TEST(StatsTest, AggregateCombineOperatorMergesPartialStates) {
    TStatsSource src({{"k", 10}, {"v", 100}}, 1000);
    auto input = std::make_shared<NQdb::TSourceOperator>(src, "t");
    std::vector<NQdb::TAggregateSpec> aggregates{
        {.Name = "c", .Func = "count", .Arg = nullptr},
        {.Name = "s", .Func = "sum",
         .Arg = std::make_shared<NQumir::NAst::TIdentExpr>(
             NQumir::TLocation{}, "v")},
        {.Name = "mn", .Func = "min",
         .Arg = std::make_shared<NQumir::NAst::TIdentExpr>(
             NQumir::TLocation{}, "v")},
        {.Name = "mx", .Func = "max",
         .Arg = std::make_shared<NQumir::NAst::TIdentExpr>(
             NQumir::TLocation{}, "v")},
    };
    NQdb::TAggregateOperator partial(input, {"k"}, std::move(aggregates));

    auto combine = NQdb::BuildAggregateCombineOperator(partial, {"k"});
    ASSERT_EQ(combine->Aggs().size(), 4u);
    EXPECT_EQ(combine->Aggs()[0].Func, "sum");
    EXPECT_EQ(combine->Aggs()[1].Func, "sum");
    EXPECT_EQ(combine->Aggs()[2].Func, "min");
    EXPECT_EQ(combine->Aggs()[3].Func, "max");
    for (const auto& aggregate : combine->Aggs()) {
        auto ident = NQumir::NAst::TMaybeNode<NQumir::NAst::TIdentExpr>(
            aggregate.Arg);
        ASSERT_TRUE(ident);
        EXPECT_EQ(ident.Cast()->Name, aggregate.Name);
    }
}

TEST(StatsTest, RowGroupHintCollectsNestedFilterChain) {
    TStatsSource src;
    auto source = std::make_shared<NQdb::TSourceOperator>(src, "t");
    auto inner = NQdb::MakeFilter(source, "(> x 10)");
    ASSERT_TRUE(inner.has_value()) << inner.error().ToString();
    auto outer = NQdb::MakeFilter(*inner, "(< x 20)");
    ASSERT_TRUE(outer.has_value()) << outer.error().ToString();

    NQdb::AnnotateTypes(*outer);
    NQdb::AttachRowGroupPredicates(*outer);

    ASSERT_TRUE(source->RowGroupPredicate());
    const auto hint = NQumir::NAst::NCore::PrintAst(
        source->RowGroupPredicate());
    EXPECT_NE(hint.find("&&"), std::string::npos);
    EXPECT_NE(hint.find("(> x 10)"), std::string::npos);
    EXPECT_NE(hint.find("(< x 20)"), std::string::npos);
}

// Project renames x -> y. Projection preserves row count and carries the
// source column's stats over to the projected (renamed) output column.
TEST(StatsTest, ProjectStatsPropagation) {
    TStatsSource src;
    NQdb::TOperatorPtr plan = std::make_shared<NQdb::TSourceOperator>(src, "t");

    auto project = NQdb::MakeProject(plan, {{"y", "x"}}); // SELECT x AS y
    ASSERT_TRUE(project.has_value()) << project.error().ToString();
    plan = *project;

    NQdb::ApplyPlanPasses(plan);

    ASSERT_TRUE(plan->Stats_ != nullptr) << "project got no propagated stats";
    // Project drops no rows: output row count == input row count.
    EXPECT_EQ(plan->Stats_->RowCount, 1000u);

    // The renamed column carries x's stats (ndv=100) under the output name.
    bool carried = false;
    for (const auto& [name, cs] : plan->Stats_->ColumnStats) {
        if (cs && cs->Ndv && *cs->Ndv == 100) {
            carried = true;
        }
    }
    EXPECT_TRUE(carried) << "projected column lost its stats";
}

// Inner join on x=y where the two sides have different key cardinalities.
// The equi-join key collapses to min(ndvL, ndvR), and - critically - this must
// happen copy-on-write: the shared source column stats stay untouched.
TEST(StatsTest, JoinKeyNdvCollapse) {
    TStatsSource left({{"x", 100}});
    TStatsSource right({{"y", 10}});
    auto l = std::make_shared<NQdb::TSourceOperator>(left, "l");
    auto r = std::make_shared<NQdb::TSourceOperator>(right, "r");

    auto join = NQdb::MakeJoin(l, r, {{"x", "y"}}, NQdb::EJoinType::Inner);
    ASSERT_TRUE(join.has_value()) << join.error().ToString();
    NQdb::TOperatorPtr plan = *join;

    NQdb::ApplyPlanPasses(plan);

    ASSERT_TRUE(plan->Stats_ != nullptr) << "join got no propagated stats";
    const auto& cols = plan->Stats_->ColumnStats;
    auto ndv = [&](const std::string& n) -> uint64_t {
        auto it = cols.find(n);
        return (it != cols.end() && it->second && it->second->Ndv) ? *it->second->Ndv : 0;
    };
    // key collapses to min(100, 10) = 10 on both sides
    EXPECT_EQ(ndv("l.x"), 10u);
    EXPECT_EQ(ndv("r.y"), 10u);

    // copy-on-write: the source's own (shared) stats must NOT have been mutated
    EXPECT_EQ(left.Stats_->ColumnStats.at("x")->Ndv.value_or(0), 100u);
    EXPECT_EQ(right.Stats_->ColumnStats.at("y")->Ndv.value_or(0), 10u);
}

namespace {

// A dimension with keys x in [0, 1000) and a fact of 100000 rows whose key k
// references only [100, 200]: 101 keys, every one used.
class SharedKeyRangeStats : public ::testing::Test {
protected:
    SharedKeyRangeStats() {
        SetFactRange(100);
    }

    void SetFactRange(int64_t min) {
        auto& key = *Fact.Stats_->ColumnStats.at("k");
        key.MinValue = std::bit_cast<uint64_t>(min);
        key.MaxValue = std::bit_cast<uint64_t>(min + 100);
        key.Histogram.clear();
        for (int64_t v = min; v <= min + 100; v += 10) {
            key.Histogram.push_back(std::bit_cast<uint64_t>(v));
        }
    }

    // Estimated stats of `dimension JOIN fact ON x = k`, the dimension
    // filtered by `predicate` when there is one.
    NQdb::TStatsPtr Join(const char* predicate = nullptr) {
        NQdb::TOperatorPtr dimension = std::make_shared<NQdb::TSourceOperator>(Dim, "d");
        if (predicate) {
            auto filter = NQdb::MakeFilter(dimension, predicate);
            EXPECT_TRUE(filter.has_value()) << filter.error().ToString();
            dimension = *filter;
        }
        auto fact = std::make_shared<NQdb::TSourceOperator>(Fact, "f");
        auto join = NQdb::MakeJoin(dimension, fact, {{"x", "k"}}, NQdb::EJoinType::Inner);
        EXPECT_TRUE(join.has_value()) << join.error().ToString();
        NQdb::TOperatorPtr plan = *join;
        NQdb::ApplyPlanPasses(plan);
        return plan->Stats_;
    }

    TStatsSource Dim{{{"x", 1000}, {"y", 1000}}, 1000};
    TStatsSource Fact{{{"k", 101}}, 100000};
};

} // namespace

// Every fact row finds its dimension row: the keys outside the fact's range
// do not multiply it.
TEST_F(SharedKeyRangeStats, UnfilteredDimensionMatchesEachFactRowOnce) {
    auto stats = Join();
    ASSERT_TRUE(stats);
    EXPECT_NEAR(static_cast<double>(stats->RowCount), 100000.0, 1000.0);
}

// The 50 dimension rows a filter keeps are judged against the 101 keys the
// fact references, not against all 1000 keys of the dimension.
TEST_F(SharedKeyRangeStats, FilteredDimensionIsJudgedWithinTheFactRange) {
    auto stats = Join("(< y 50)");
    ASSERT_TRUE(stats);
    EXPECT_NEAR(static_cast<double>(stats->RowCount), 50.0 * 100000.0 / 101.0, 1000.0);
}

// The join keeps 101 of the dimension's 1000 keys. The histogram described
// all of them, so the joined key keeps only its bounds.
TEST_F(SharedKeyRangeStats, CappedKeyDropsItsHistogram) {
    auto stats = Join();
    ASSERT_TRUE(stats);
    const auto& key = *stats->ColumnStats.at("d.x");
    EXPECT_EQ(key.Ndv, 101u);
    EXPECT_TRUE(key.Histogram.empty());
    EXPECT_EQ(key.GetMinValue<int64_t>(), 0);
    EXPECT_EQ(key.GetMaxValue<int64_t>(), 999);
    // The dimension's own statistics stay as they are.
    EXPECT_FALSE(Dim.Stats_->ColumnStats.at("x")->Histogram.empty());
}

// A join with a 10-row table keeps about 10 dimension rows, and the
// statistics do not say where their keys lie. The fact's range must not
// shrink them to the tenth the dimension's histogram would place inside it.
TEST_F(SharedKeyRangeStats, SelectedKeysAreNotSpreadByTheBaseHistogram) {
    TStatsSource small({{"z", 10}}, 10);
    NQdb::TOperatorPtr dimension = std::make_shared<NQdb::TSourceOperator>(Dim, "d");
    NQdb::TOperatorPtr selector = std::make_shared<NQdb::TSourceOperator>(small, "s");
    NQdb::TOperatorPtr fact = std::make_shared<NQdb::TSourceOperator>(Fact, "f");
    auto selected = NQdb::MakeJoin(dimension, selector, {{"y", "z"}}, NQdb::EJoinType::Inner);
    ASSERT_TRUE(selected.has_value()) << selected.error().ToString();
    auto join = NQdb::MakeJoin(*selected, fact, {{"x", "k"}}, NQdb::EJoinType::Inner);
    ASSERT_TRUE(join.has_value()) << join.error().ToString();
    // No reordering: the dimension is joined with the 10 rows first.
    NQdb::TOperatorPtr plan = *join;
    NQdb::AssignSourceAliases(plan);
    NQdb::QualifyColumns(plan);
    NQdb::AnnotateTypes(plan);
    auto stats = NQdb::EstimateStats(plan);
    ASSERT_TRUE(stats);
    const double selectedRows = static_cast<double>((*selected)->Stats_->RowCount);
    EXPECT_NEAR(selectedRows, 10.0, 1.0);
    EXPECT_NEAR(static_cast<double>(stats->RowCount), selectedRows * 100000.0 / 101.0, 100.0);
}

// Join ordering estimates a join as the plan does. The dimension rows that
// the 10-row table selects keep their keys within the fact's range, so each
// finds 100000 / 101 fact rows. Fixed selectivities of the two pairs would
// spread the selected keys over the dimension's range and give a tenth of
// that.
TEST_F(SharedKeyRangeStats, JoinOrderEstimatesKeysAnotherJoinSelected) {
    TStatsSource small({{"z", 10}}, 10);
    std::vector<NQdb::TOperatorPtr> leaves{
        std::make_shared<NQdb::TSourceOperator>(Dim, "d"),
        std::make_shared<NQdb::TSourceOperator>(small, "s"),
        std::make_shared<NQdb::TSourceOperator>(Fact, "f"),
    };
    for (auto& leaf : leaves) {
        NQdb::AssignSourceAliases(leaf);
        NQdb::QualifyColumns(leaf);
        NQdb::AnnotateTypes(leaf);
        NQdb::EstimateStats(leaf);
    }
    NQdb::TConstFolder folder;
    auto tree = NQdb::NCbo::DpccpJoinOrder(
        leaves, {{"d.y", "s.z"}, {"d.x", "f.k"}}, {}, folder);
    ASSERT_TRUE(tree && tree->Stats_);
    auto join = NQdb::TMaybeOp<NQdb::TJoinOperator>(tree);
    ASSERT_TRUE(join);
    auto selected = NQdb::TMaybeOp<NQdb::TJoinOperator>(join.Cast()->Left())
        ? join.Cast()->Left()
        : join.Cast()->Right();
    const double selectedRows = static_cast<double>(selected->Stats_->RowCount);
    EXPECT_NEAR(selectedRows, 10.0, 1.0);
    EXPECT_NEAR(static_cast<double>(tree->Stats_->RowCount), selectedRows * 100000.0 / 101.0, 100.0);
}

TEST_F(SharedKeyRangeStats, DisjointKeyRangesMatchNothing) {
    SetFactRange(2000);
    auto stats = Join();
    ASSERT_TRUE(stats);
    EXPECT_EQ(stats->RowCount, 1u);
}

// 1000 rows; x is i64, n is Nullable<i64> with 250 NULLs, f and g are f64;
// ndv 100 each.
class SelfEqualityStats : public ::testing::Test {
protected:
    SelfEqualityStats() {
        Src.Cols[1].Type = std::make_shared<NQdb::TNullable>(Src.Cols[1].Type);
        Src.Stats_->ColumnStats.at("n")->NullCount = 250;
        Src.Cols[2].Type = std::make_shared<NQumir::NAst::TFloatType>();
        Src.Cols[3].Type = std::make_shared<NQumir::NAst::TFloatType>();
        Source = std::make_shared<NQdb::TSourceOperator>(Src, "");
        Source->SetAlias("t");
    }

    // Row estimate of a filter over the source, with the predicate as written.
    uint64_t Rows(const std::string& predicate) {
        auto filter = NQdb::MakeFilter(Source, predicate);
        EXPECT_TRUE(filter.has_value()) << filter.error().ToString();
        return NQdb::EstimateStats(*filter)->RowCount;
    }

    TStatsSource Src{{{"x", 100}, {"n", 100}, {"f", 100}, {"g", 100}}, 1000};
    std::shared_ptr<NQdb::TSourceOperator> Source;
};

TEST_F(SelfEqualityStats, ColumnEqualToItselfKeepsNonNullRows) {
    EXPECT_EQ(Rows("(== |t.x| |t.x|)"), 1000u);
    EXPECT_EQ(Rows("(== |t.n| |t.n|)"), 750u);
    // Two different columns are still estimated through NDV, over the rows
    // where neither is NULL: 1000 * 0.75 / 100.
    EXPECT_EQ(Rows("(== |t.x| |t.n|)"), 7u);
}

TEST_F(SelfEqualityStats, ColumnUnequalToItselfKeepsNoRows) {
    EXPECT_EQ(Rows("(!= |t.x| |t.x|)"), 1u); // estimates never go below one row
}

// A float column may hold NaN, which is unequal to itself, and the statistics
// do not count NaNs. Its self-comparisons get the estimate of any two columns.
TEST_F(SelfEqualityStats, FloatColumnKeepsTheGeneralEstimate) {
    EXPECT_EQ(Rows("(== |t.f| |t.f|)"), Rows("(== |t.f| |t.g|)"));
    EXPECT_EQ(Rows("(!= |t.f| |t.f|)"), Rows("(!= |t.f| |t.g|)"));
    EXPECT_LT(Rows("(== |t.f| |t.f|)"), 1000u);
    EXPECT_GT(Rows("(!= |t.f| |t.f|)"), 1u);
}

// The shape decorrelation leaves under a correlated aggregate (TPC-H Q17).
TEST_F(SelfEqualityStats, GroupEstimateIsNotCappedBySelfEquality) {
    auto filter = NQdb::MakeFilter(
        std::make_shared<NQdb::TSourceOperator>(Src, "t"), "(== x x)");
    ASSERT_TRUE(filter.has_value()) << filter.error().ToString();
    NQdb::TOperatorPtr plan = std::make_shared<NQdb::TAggregateOperator>(
        *filter, std::vector<std::string>{"x"}, std::vector<NQdb::TAggregateSpec>{});

    NQdb::ApplyPlanPasses(plan);

    ASSERT_TRUE(plan->Stats_);
    EXPECT_EQ(plan->Stats_->RowCount, 100u); // one group per distinct x
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
