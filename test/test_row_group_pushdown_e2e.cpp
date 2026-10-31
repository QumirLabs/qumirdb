#include <gtest/gtest.h>

#include "plan_runner.h"

#include <qdb/io/parquet/source.h>
#include <qdb/plan/build.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/pipeline.h>
#include <qdb/sql/parser.h>

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/parser/ast.h>

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/writer.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

using namespace NQdb;

void CheckArrow(const arrow::Status& status) {
    if (!status.ok()) {
        throw std::runtime_error(status.ToString());
    }
}

class TTempDir {
public:
    TTempDir() {
        auto pattern =
            (std::filesystem::temp_directory_path()
                / "qdb-row-group-pushdown-XXXXXX").string();
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

void WriteFixture(const std::string& path, bool storeDecimalAsInteger = false) {
    arrow::Int64Builder id;
    arrow::Int32Builder signedValue;
    arrow::UInt32Builder unsignedValue;
    arrow::DoubleBuilder floatValue;
    arrow::FloatBuilder realValue;
    arrow::Int64Builder nullableValue;
    arrow::Decimal128Builder decimalValue(arrow::decimal128(15, 2));
    arrow::Decimal128Builder wideValue(arrow::decimal128(38, 6));

    CheckArrow(id.AppendValues({0, 1, 2, 3, 4, 5}));
    CheckArrow(signedValue.AppendValues({0, 1, 100, 101, 0, 1}));
    CheckArrow(unsignedValue.AppendValues(
        {1, 2, 100, 101, 4000000000U, 4000000001U}));
    CheckArrow(floatValue.AppendValues(
        {0.5, 1.5, 100.5, 101.5, -10.0, -9.0}));
    CheckArrow(realValue.AppendValues(
        {0.5f, 1.5f, 100.5f, 101.5f, -10.0f, -9.0f}));
    CheckArrow(nullableValue.AppendNull());
    CheckArrow(nullableValue.AppendNull());
    CheckArrow(nullableValue.Append(100));
    CheckArrow(nullableValue.AppendNull());
    CheckArrow(nullableValue.Append(1));
    CheckArrow(nullableValue.Append(2));
    for (int64_t unscaled : {50, 150, 10050, 10150, -1000, -900}) {
        CheckArrow(decimalValue.Append(arrow::Decimal128(unscaled)));
    }
    const arrow::Decimal128 unit =
        arrow::Decimal128::FromString("1000000000000000000000000000000").ValueOrDie();
    for (int64_t row = 0; row < 6; ++row) {
        CheckArrow(wideValue.Append(unit * arrow::Decimal128(row - 2)));
    }

    auto schema = arrow::schema({
        arrow::field("id", arrow::int64(), false),
        arrow::field("signed_value", arrow::int32(), false),
        arrow::field("unsigned_value", arrow::uint32(), false),
        arrow::field("float_value", arrow::float64(), false),
        arrow::field("real_value", arrow::float32(), false),
        arrow::field("nullable_value", arrow::int64(), true),
        arrow::field("decimal_value", arrow::decimal128(15, 2), false),
        arrow::field("wide_value", arrow::decimal128(38, 6), false),
    });
    auto batch = arrow::RecordBatch::Make(schema, 6, {
        id.Finish().ValueOrDie(),
        signedValue.Finish().ValueOrDie(),
        unsignedValue.Finish().ValueOrDie(),
        floatValue.Finish().ValueOrDie(),
        realValue.Finish().ValueOrDie(),
        nullableValue.Finish().ValueOrDie(),
        decimalValue.Finish().ValueOrDie(),
        wideValue.Finish().ValueOrDie(),
    });
    auto output = arrow::io::FileOutputStream::Open(path).ValueOrDie();
    auto table = arrow::Table::FromRecordBatches({batch}).ValueOrDie();
    parquet::WriterProperties::Builder properties;
    if (storeDecimalAsInteger) {
        properties.enable_store_decimal_as_integer();
    }
    CheckArrow(parquet::arrow::WriteTable(
        *table, arrow::default_memory_pool(), output, /*chunk_size*/ 2,
        properties.build()));
    CheckArrow(output->Close());
}

// Two rows per row group; the values sit around the f32 grid points above 2^24.
void WriteLargeRealFixture(const std::string& path) {
    arrow::Int64Builder id;
    arrow::FloatBuilder real;
    CheckArrow(id.AppendValues({0, 1, 2, 3, 4, 5, 6, 7}));
    CheckArrow(real.AppendValues({
        16777216.0f, 16777216.0f, 16777220.0f, 16777220.0f,
        16777218.0f, 33554432.0f, 1.0f, 100.0f}));
    auto schema = arrow::schema({
        arrow::field("id", arrow::int64(), false),
        arrow::field("real_value", arrow::float32(), false),
    });
    auto batch = arrow::RecordBatch::Make(schema, 8, {
        id.Finish().ValueOrDie(), real.Finish().ValueOrDie()});
    auto output = arrow::io::FileOutputStream::Open(path).ValueOrDie();
    auto table = arrow::Table::FromRecordBatches({batch}).ValueOrDie();
    CheckArrow(parquet::arrow::WriteTable(
        *table, arrow::default_memory_pool(), output, /*chunk_size*/ 2));
    CheckArrow(output->Close());
}

std::shared_ptr<TSourceOperator> FindSource(const TOperatorPtr& node) {
    if (auto source = TMaybeOp<TSourceOperator>(node)) {
        return source.Cast();
    }
    for (const auto& child : node->Children()) {
        if (auto op = NQumir::NAst::TMaybeNode<IOperator>(child)) {
            if (auto source = FindSource(op.Cast())) {
                return source;
            }
        }
    }
    return nullptr;
}

TOperatorPtr BuildSqlPlan(
    std::string_view sql,
    const std::unordered_map<std::string, ISource*>& sources)
{
    std::istringstream input{std::string(sql)};
    NSql::TTokenStream tokens(input);
    NSql::TParser parser;
    auto parsed = parser.Parse(tokens);
    if (!parsed) {
        throw std::runtime_error(parsed.error().ToString());
    }
    auto plan = BuildPlan(*parsed, [&](std::string_view table)
        -> std::expected<TOperatorPtr, NQumir::TError>
    {
        return std::make_shared<TSourceOperator>(
            *sources.at(std::string(table)), std::string(table));
    });
    if (!plan) {
        throw std::runtime_error(plan.error().ToString());
    }
    ApplyPlanPasses(*plan, {.EnableCbo = false});
    return *plan;
}

TOperatorPtr BuildSqlPlan(std::string_view sql, TParquetSource& source) {
    return BuildSqlPlan(sql, {{"t", &source}});
}

std::vector<int64_t> ReadIds(TTestRuntime& runtime) {
    std::vector<int64_t> ids;
    TRowSet rows{};
    while (runtime.Next(rows)) {
        const auto* values = reinterpret_cast<const int64_t*>(rows.Columns[0].Data);
        for (int64_t row = 0; row < rows.RowCount; ++row) {
            if (!rows.Selection || rows.Selection[row]) {
                ids.push_back(values[row]);
            }
        }
        Release(&rows);
    }
    std::ranges::sort(ids);
    return ids;
}

struct TRunResult {
    std::vector<int64_t> Ids;
    std::string Diagnostics;
};

TRunResult RunQuery(
    const std::string& path,
    std::string_view sql,
    bool enablePruning,
    bool autoTasks = false)
{
    TParquetSource source(path);
    auto plan = BuildSqlPlan(sql, source);
    auto sourceOp = FindSource(plan);
    if (!sourceOp) {
        throw std::runtime_error("test plan has no parquet source");
    }
    if (!sourceOp->RowGroupPredicate()) {
        throw std::runtime_error("row-group predicate was not attached");
    }
    if (!enablePruning) {
        sourceOp->SetRowGroupPredicate(nullptr);
    }

    std::ostringstream diagnostics;
    NScheduler::TSettings settings;
    settings.ScanSplit.Strategy = NScheduler::EScanSplitStrategy::RowGroupRange;
    settings.ScanSplit.MaxScanTasks = 3;
    if (autoTasks) {
        settings.Scheduler.Mode = NScheduler::EExecutionMode::ThreadedScheduler;
        settings.Scheduler.WorkerCount = 4;
        settings.ScanSplit.MaxScanTasks = 0;
        settings.ScanSplit.TargetRowsPerTask = 1;
        settings.ScanSplit.TargetBytesPerTask = 0;
    }
    settings.Instrumentation.RowGroupPredicate = enablePruning;
    settings.Instrumentation.Output = &diagnostics;

    if (autoTasks) {
        const auto projectedGroups = source.ScanRowGroups(
            std::unordered_set<std::string>{"id", "signed_value"});
        int64_t projectedBytes = 0;
        for (size_t rg = 0; rg < projectedGroups.size(); ++rg) {
            if (!enablePruning || rg == 1) {
                projectedBytes += projectedGroups[rg].ByteSize;
            }
        }
        auto lowered = NScheduler::LowerPlanToGraph(plan, settings, &diagnostics);
        const auto lanes = std::ranges::count_if(
            lowered.Graph->Nodes(), [](const auto& node) {
                return node->DebugKind == "source";
            });
        EXPECT_EQ(lanes, enablePruning ? 1 : 3);
        EXPECT_NE(diagnostics.str().find(" bytes=" + std::to_string(projectedBytes)),
            std::string::npos);
    }
    auto runtime = RunPlan(plan, settings);
    return {.Ids = ReadIds(*runtime), .Diagnostics = diagnostics.str()};
}

void ExpectSameAsFullScan(
    const std::string& path,
    std::string_view sql,
    const std::vector<int64_t>& expected)
{
    const auto pruned = RunQuery(path, sql, true);
    const auto full = RunQuery(path, sql, false);
    EXPECT_EQ(pruned.Ids, full.Ids);
    EXPECT_EQ(pruned.Ids, expected);
    EXPECT_NE(pruned.Diagnostics.find("may_be_true=0"), std::string::npos);
}

} // namespace

TEST(RowGroupPushdownE2E, MatchesFullScanAcrossParquetPhysicalTypes) {
    TTempDir tempDir;
    const std::string path =
        (tempDir.Path() / "physical-types.parquet").string();
    WriteFixture(path);

    ExpectSameAsFullScan(
        path,
        "SELECT id FROM t WHERE signed_value > 50",
        {2, 3});
    ExpectSameAsFullScan(
        path,
        "SELECT id FROM t WHERE unsigned_value > 3000000000",
        {4, 5});
    ExpectSameAsFullScan(
        path,
        "SELECT id FROM t WHERE float_value < 0.0",
        {4, 5});
    ExpectSameAsFullScan(
        path,
        "SELECT id FROM t WHERE real_value < 0.0",
        {4, 5});
    ExpectSameAsFullScan(
        path,
        "SELECT id FROM t WHERE real_value > 50",
        {2, 3});
    ExpectSameAsFullScan(
        path,
        "SELECT id FROM t WHERE real_value = 101.5",
        {3});
    ExpectSameAsFullScan(
        path,
        "SELECT id FROM t WHERE real_value + real_value < 0",
        {4, 5});
    ExpectSameAsFullScan(
        path,
        "SELECT id FROM t WHERE real_value * real_value > 10000",
        {2, 3});
    ExpectSameAsFullScan(
        path,
        "SELECT id FROM t WHERE nullable_value > 50",
        {2});
}

// DECIMAL statistics are read from every Parquet storage: INT64 and
// FIXED_LEN_BYTE_ARRAY(16) with store_decimal_as_integer, FLBA(7) and FLBA(16)
// without it.
TEST(RowGroupPushdownE2E, PrunesDecimalColumnsOfEveryStorage) {
    TTempDir tempDir;
    for (const bool asInteger : {false, true}) {
        const std::string path = (tempDir.Path() /
            (asInteger ? "decimal-int.parquet" : "decimal-flba.parquet")).string();
        WriteFixture(path, asInteger);
        ExpectSameAsFullScan(
            path,
            "SELECT id FROM t WHERE decimal_value > 50.5",
            {2, 3});
        ExpectSameAsFullScan(
            path,
            "SELECT id FROM t WHERE decimal_value <= -9.5",
            {4});
        ExpectSameAsFullScan(
            path,
            "SELECT id FROM t WHERE decimal_value < 100",
            {0, 1, 4, 5});
        ExpectSameAsFullScan(
            path,
            "SELECT id FROM t WHERE wide_value < 0",
            {0, 1});
    }
}

TEST(RowGroupPushdownE2E, F32ColumnVsIntegerLiteralAbove2Pow24) {
    TTempDir tempDir;
    const std::string path = (tempDir.Path() / "large-real.parquet").string();
    WriteLargeRealFixture(path);

    // The kernel compares in f64, so 16777217 and 16777219 are not f32 grid points.
    ExpectSameAsFullScan(
        path, "SELECT id FROM t WHERE real_value < 16777217", {0, 1, 6, 7});
    ExpectSameAsFullScan(
        path, "SELECT id FROM t WHERE real_value > 16777219", {2, 3, 5});
    ExpectSameAsFullScan(
        path, "SELECT id FROM t WHERE real_value > 16777217", {2, 3, 4, 5});
    ExpectSameAsFullScan(
        path, "SELECT id FROM t WHERE real_value <= 16777217.5", {0, 1, 6, 7});
    ExpectSameAsFullScan(
        path, "SELECT id FROM t WHERE real_value >= 16777219.5", {2, 3, 5});
}

TEST(RowGroupPushdownE2E, AllPrunedProducesAnEmptyScan) {
    TTempDir tempDir;
    const std::string path =
        (tempDir.Path() / "empty-scan.parquet").string();
    WriteFixture(path);

    const auto pruned = RunQuery(
        path, "SELECT id FROM t WHERE signed_value > 1000", true);
    const auto full = RunQuery(
        path, "SELECT id FROM t WHERE signed_value > 1000", false);
    EXPECT_TRUE(pruned.Ids.empty());
    EXPECT_EQ(pruned.Ids, full.Ids);
    EXPECT_NE(pruned.Diagnostics.find("rg=0 may_be_true=0"), std::string::npos);
    EXPECT_NE(pruned.Diagnostics.find("rg=1 may_be_true=0"), std::string::npos);
    EXPECT_NE(pruned.Diagnostics.find("rg=2 may_be_true=0"), std::string::npos);
}

TEST(RowGroupPushdownE2E, AutoScanTasksUseSurvivingRowGroups) {
    TTempDir tempDir;
    const std::string path = (tempDir.Path() / "auto-scan.parquet").string();
    WriteFixture(path);
    constexpr std::string_view sql =
        "SELECT id FROM t WHERE signed_value > 50";
    const auto pruned = RunQuery(path, sql, true, true);
    const auto full = RunQuery(path, sql, false, true);
    EXPECT_EQ(pruned.Ids, (std::vector<int64_t>{2, 3}));
    EXPECT_EQ(pruned.Ids, full.Ids);
    EXPECT_NE(pruned.Diagnostics.find("scan_tasks=auto max_tasks=1 tasks=1"),
        std::string::npos);
    EXPECT_NE(full.Diagnostics.find("scan_tasks=auto max_tasks=3 tasks=3"),
        std::string::npos);
}

TEST(RowGroupPushdownE2E, AutoJoinRunsWithUnequalSourceWidths) {
    TTempDir tempDir;
    const std::string path = (tempDir.Path() / "auto-join.parquet").string();
    WriteFixture(path);
    TParquetFile file(path);
    auto left = file.MakeSource();
    auto right = left->MakeRowGroupsSource({1});
    auto plan = BuildSqlPlan(
        "SELECT l.id FROM l JOIN r ON l.signed_value = r.signed_value",
        {{"l", left.get()}, {"r", right.get()}});
    NScheduler::TSettings settings;
    settings.Scheduler.Mode = NScheduler::EExecutionMode::ThreadedScheduler;
    settings.Scheduler.WorkerCount = 2;
    settings.ScanSplit.TargetRowsPerTask = 1;
    settings.ScanSplit.TargetBytesPerTask = 0;
    settings.HashShuffle.PartitionCount = 2;
    auto lowered = NScheduler::LowerPlanToGraph(plan, settings, nullptr);
    std::unordered_map<std::string, size_t> lanes;
    for (const auto& node : lowered.Graph->Nodes()) {
        if (node->DebugKind == "source") {
            ++lanes[node->DebugLabel];
        }
    }
    EXPECT_EQ(lanes.at("source l"), 2u);
    EXPECT_EQ(lanes.at("source r"), 1u);
    auto threaded = RunPlan(plan, settings);
    EXPECT_EQ(ReadIds(*threaded), (std::vector<int64_t>{2, 3}));
    NScheduler::TSettings serialSettings;
    serialSettings.Scheduler.Mode = NScheduler::EExecutionMode::SingleThreadedScheduler;
    auto serial = RunPlan(plan, serialSettings);
    EXPECT_EQ(ReadIds(*serial), (std::vector<int64_t>{2, 3}));
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    NQumir::NCodeGen::TLLVMInitializer initializer;
    return RUN_ALL_TESTS();
}
