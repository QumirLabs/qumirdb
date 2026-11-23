#include <qdb/scheduler/connections/local.h>
#include <qdb/scheduler/tasks/runtime_adapter.h>
#include "mock_source.h"
#include "plan_runner.h"
#include "test_types.h"

#include <qdb/exec/aggregate_exec.h>
#include <qdb/exec/instrumentation/operator.h>
#include <qdb/io/row_layouts.h>
#include <qdb/kernel/compiler.h>
#include <qdb/kernel/builder.h>
#include <qdb/kernel/spec.h>
#include <qdb/plan/build.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/ops/union.h>
#include <qdb/plan/passes/column_pruning.h>
#include <qdb/plan/passes/typing.h>
#include <qdb/plan/types/decimal.h>
#include <qdb/plan/types/nullable.h>
#include <qdb/scheduler/instrumentation/task.h>
#include <qdb/sql/parser.h>

#include <qumir/codegen/llvm/llvm_initializer.h>

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <chrono>
#include <numeric>
#include <sstream>
#include <thread>
#include <unordered_map>

using namespace NQdb;
using namespace NQdb::NScheduler;
using namespace NQumir::NAst;
using namespace NQumir::NAst::NLiterals;

namespace {

std::shared_ptr<TKernelSlot> MeasurementKernel() {
    // Prepared code may be shared; every test owns its mutable counters.
    return TKernelCompiler({.Types = SharedTestTypes(), .Annotation = TestAnnotation()}).CompileInstrumentation();
}

TOperatorPtr SqlPlan(const std::string& sql, ITable& source) {
    std::istringstream input(sql);
    NSql::TTokenStream tokens(input);
    NSql::TParser parser(TestTypes());
    auto parsed = parser.Parse(tokens);
    if (!parsed) {
        throw parsed.error();
    }
    auto plan = BuildPlan(TestTypes(), TestAnnotation(), *parsed, [&](NQumir::NAst::TTypeFactory& types, std::string_view table)
        -> std::expected<TOperatorPtr, NQumir::TError>
    {
        return NQumir::MakeIntrusive<TSourceOperator>(types, Borrow(source), std::string(table));
    });
    if (!plan) {
        throw plan.error();
    }
    AnnotateTypes(TestTypes(), *plan, TestAnnotation());
    ApplyColumnPruning(TestTypes(), *plan);
    return *plan;
}

uint64_t StageSum(const std::string& text, const std::string& kind, const std::string& field) {
    uint64_t total = 0;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.starts_with("operator-stage ") || line.find(" kind=" + kind + " ") == std::string::npos) {
            continue;
        }
        auto begin = line.find(" " + field + "=");
        if (begin == std::string::npos) {
            throw std::runtime_error("missing instrumentation field " + field);
        }
        total += std::stoull(line.substr(begin + field.size() + 2));
    }
    return total;
}

} // namespace

TEST(OperatorInstrumentation, ResolvesLogicalAndPhysicalColumnWidths) {
    auto schema = TestTypes().MakeStruct(std::vector<std::pair<std::string, TTypePtr>>{
        {"decimal", MakeNullableType(TestTypes(), MakeDecimalType(TestTypes(), {18, 2}))},
        {"binint", BinIntStorageType(TestTypes())},
        {"date", TestTypes().MakeNamed("DATE", TestTypes().MakeInteger(TIntegerType::I32))},
        {"boolean", TestTypes().Make<TBoolType>()},
        {"text", MakeNullableType(TestTypes(), TestTypes().Make<TStringType>())},
        {"float", TestTypes().MakeFloat()},
    });
    // DECIMAL(18,2) is stored in i64.
    EXPECT_EQ(InstrumentationColumnWidths(schema), (std::vector<int64_t>{8, 16, 4, 1, -1, 8}));
    // Bool is bit-packed in row sets and strings carry no width.
    const auto layouts = RowLayouts(schema);
    ASSERT_EQ(layouts.size(), 6u);
    const std::vector<std::pair<int32_t, bool>> expected{{8, false}, {16, false}, {4, false}, {0, true}, {0, false}, {8, false}};
    for (size_t i = 0; i < layouts.size(); ++i) {
        EXPECT_EQ(layouts[i].Width, expected[i].first) << i;
        EXPECT_EQ(layouts[i].Bitmap, expected[i].second) << i;
    }
    EXPECT_THROW(RowLayouts(TestTypes().MakeInteger(TIntegerType::I32)), std::runtime_error);
    auto unsupported = TestTypes().MakeStruct(std::vector<std::pair<std::string, TTypePtr>>{
        {"bad", TestTypes().MakeStruct(std::vector<std::pair<std::string, TTypePtr>>{})},
    });
    EXPECT_THROW(RowLayouts(unsupported), std::runtime_error);
}

TEST(OperatorInstrumentation, RowLayoutsUnwrapNamedNullableChains) {
    auto named = [](TTypePtr type) {
        return TestTypes().MakeNamed("ALIAS", std::move(type));
    };
    auto schema = TestTypes().MakeStruct(std::vector<std::pair<std::string, TTypePtr>>{
        {"flag", named(MakeNullableType(TestTypes(), TestTypes().Make<TBoolType>()))},
        {"text", named(MakeNullableType(TestTypes(), TestTypes().Make<TStringType>()))},
    });
    const auto layouts = RowLayouts(schema);
    ASSERT_EQ(layouts.size(), 2u);
    EXPECT_TRUE(layouts[0].Bitmap);
    EXPECT_EQ(layouts[0].Width, 0);
    EXPECT_FALSE(layouts[1].Bitmap);
    EXPECT_EQ(layouts[1].Width, 0);
}

TEST(OperatorInstrumentation, CountsSelectedNonNullPayloadAndBothOffsetWidths) {
    auto kernel = MeasurementKernel();
    std::array<int32_t, 4> values = {10, 20, 30, 40};
    std::array<int32_t, 5> narrow = {0, 1, 4, 4, 9};
    std::array<int64_t, 5> wide = {0, 1, 4, 4, 9};
    std::array<uint8_t, 4> selection = {0, 2, 1, 255};
    uint8_t numericMask = 0b01011000;
    uint8_t stringMask = 0b00001110;
    char strings[] = "abcdefghi";
    for (const bool large : {false, true}) {
        TColumn columns[] = {
            {.Data = reinterpret_cast<char*>(values.data()), .Mask = &numericMask, .MaskBitOffset = 3},
            {.Data = strings, .Mask = &stringMask, .MaskBitOffset = 1,
                .Offsets = large ? static_cast<void*>(wide.data()) : static_cast<void*>(narrow.data()),
                .OffsetWidth = static_cast<uint8_t>(large ? 8 : 4)},
        };
        TRowSet batch{.Columns = columns, .ColumnCount = 2, .RowCount = 4,
            .Selection = selection.data(), .RefCount = 1};
        TRowSetInstrumentation counters{.Kernel = kernel, .Widths = {4, -1}};
        counters.Add(batch);
        EXPECT_EQ(counters.Rows, 3u);
        EXPECT_EQ(counters.Bytes, 11u);
        batch.Selection = nullptr;
        counters.Add(batch);
        EXPECT_EQ(counters.Rows, 7u);
        EXPECT_EQ(counters.Bytes, 27u);
        batch.RowCount = 0;
        counters.Add(batch);
        EXPECT_EQ(counters.Rows, 7u);
    }
}

TEST(OperatorInstrumentation, BackpressureRetriesDoNotCountFailedPushOrEmptyFetch) {
    auto kernel = MeasurementKernel();
    TOneToOneConnection connection(1);
    connection.Resize(1, 1);
    int64_t value = 42;
    TColumn column{.Data = reinterpret_cast<char*>(&value)};
    TRowSet batch{.Columns = &column, .ColumnCount = 1, .RowCount = 1, .RefCount = 1};
    TRowSetInstrumentation sent{.Kernel = kernel, .Widths = {8}};
    TRowSetInstrumentation received{.Kernel = kernel, .Widths = {8}};
    TOutputPort output{.Connection = &connection, .Instrumentation = &sent};
    TInputPort input{.Connection = &connection, .Instrumentation = &received};
    auto first = batch;
    EXPECT_TRUE(output.Push(std::move(first)));
    auto second = batch;
    EXPECT_FALSE(output.Push(std::move(second)));
    EXPECT_EQ(sent.Rows, 1u);
    TRowSet fetched{};
    EXPECT_EQ(input.Fetch(fetched), EFetchResult::OK);
    Release(&fetched);
    EXPECT_EQ(input.Fetch(fetched), EFetchResult::NO_DATA);
    EXPECT_EQ(received.Rows, 1u);
    EXPECT_EQ(received.Bytes, 8u);
    EXPECT_TRUE(output.Push(std::move(second)));
    EXPECT_EQ(sent.Rows, 2u);
    EXPECT_EQ(input.Fetch(fetched), EFetchResult::OK);
    Release(&fetched);
}

TEST(OperatorInstrumentation, ObservesEveryGeometricGrowthInsideABatch) {
    auto i64 = TestTypes().MakeInteger();
    const auto& inputType = *TestTypes().MakeStruct({{"k", i64}});
    auto spec = NKernel::BuildAggregateKernelSpec(TestTypes(), TestAnnotation(), inputType, {"k"}, {{.Name = "n", .Func = "count"}});
    auto kernels = TKernelCompiler({.Types = SharedTestTypes(), .Annotation = TestAnnotation()}).CompileAggregate(spec);
    TOperatorInstrumentation counters;
    counters.Kernel = MeasurementKernel();
    TAggregateProcessor processor(kernels, {.InitialCapacity = 8});
    processor.SetInstrumentation(&counters);
    std::array<int64_t, 100> keys;
    std::iota(keys.begin(), keys.end(), 0);
    TColumn column{.Data = reinterpret_cast<char*>(keys.data())};
    TRowSet batch{.Columns = &column, .ColumnCount = 1, .RowCount = 100, .RefCount = 1};
    batch.RowCount = 0;
    processor.Add(batch);
    const auto initialCapacity = counters.HashCapacity;
    EXPECT_TRUE(initialCapacity == 8 || initialCapacity == 16);
    EXPECT_EQ(counters.Rehashes, 0u);
    batch.RowCount = 100;
    processor.Add(batch);
    EXPECT_TRUE(counters.HasHash);
    EXPECT_EQ(counters.Groups, 100u);
    EXPECT_EQ(counters.HashCapacity, 128u);
    EXPECT_EQ(counters.Rehashes,
        std::countr_zero(uint64_t{128}) - std::countr_zero(initialCapacity));
    EXPECT_EQ(counters.Rehashes, counters.HashGrowthEvents);
    const auto memory = counters.HashMemoryPeakBytes;
    EXPECT_GT(memory, 128u * 8u);
    EXPECT_EQ(counters.BuildMemoryPeakBytes, memory);
    processor.Add(batch);
    EXPECT_EQ(counters.Groups, 100u);
    EXPECT_EQ(counters.HashMemoryPeakBytes, memory);
    TRowSet output{};
    ASSERT_TRUE(processor.Finish(output));
    EXPECT_EQ(output.RowCount, 100);
    Release(&output);
    EXPECT_EQ(counters.Groups, 100u);
}

TEST(OperatorInstrumentation, StringStateMemoryCountsOnlyTheGroupsAfterRehash) {
    // The string reducer buffers are indexed by dense group; a rehash leaves
    // the entries past Size uninitialized, so measuring up to the capacity
    // summed garbage. The same groups with strings 8 bytes longer must
    // measure exactly 8 bytes more per group.
    constexpr int64_t groups = 200000;
    auto measure = [](int64_t length) {
        const auto& inputType = *TestTypes().MakeStruct({{"k", TestTypes().Make<TStringType>()},
            {"s", TestTypes().Make<TStringType>()}});
        auto spec = NKernel::BuildAggregateKernelSpec(TestTypes(), TestAnnotation(), inputType, {"k"}, {{.Name = "m",
            .Func = "min", .Arg = NQumir::MakeIntrusive<TIdentExpr>(NQumir::TLocation{}, "s")}});
        auto kernels = TKernelCompiler({.Types = SharedTestTypes(), .Annotation = TestAnnotation()}).CompileAggregate(spec);
        TOperatorInstrumentation counters;
        counters.Kernel = MeasurementKernel();
        TAggregateProcessor processor(kernels, {.InitialCapacity = 8});
        processor.SetInstrumentation(&counters);
        std::string keys;
        std::vector<int64_t> keyOffsets(groups + 1);
        for (int64_t i = 0; i < groups; ++i) {
            keyOffsets[i] = keys.size();
            keys += "key-" + std::to_string(i);
        }
        keyOffsets[groups] = keys.size();
        std::string data(static_cast<size_t>(groups * length), 'x');
        std::vector<int64_t> offsets(groups + 1);
        for (int64_t i = 0; i <= groups; ++i) {
            offsets[i] = i * length;
        }
        TColumn columns[] = {{.Data = keys.data(), .Offsets = keyOffsets.data(), .OffsetWidth = 8},
            {.Data = data.data(), .Offsets = offsets.data(), .OffsetWidth = 8}};
        TRowSet batch{.Columns = columns, .ColumnCount = 2, .RowCount = groups, .RefCount = 1};
        processor.Add(batch);
        EXPECT_EQ(counters.Groups, static_cast<uint64_t>(groups));
        EXPECT_GT(counters.Rehashes, 0u);
        TRowSet output{};
        EXPECT_TRUE(processor.Finish(output));
        Release(&output);
        return counters.HashMemoryPeakBytes;
    };
    const auto shortStrings = measure(8);
    const auto longStrings = measure(16);
    EXPECT_EQ(longStrings - shortStrings, static_cast<uint64_t>(groups * 8));
}

TEST(OperatorInstrumentation, MeasuresResidualSemiJoinMatchSetWithoutRowBuckets) {
    TKernelHeap heap;
    TKernelHeapScope scope(heap);
    auto i64 = TestTypes().MakeInteger();
    const auto& leftType = *TestTypes().MakeStruct({{"lk", i64}});
    const auto& rightType = *TestTypes().MakeStruct({{"rk", i64}});
    auto spec = NKernel::BuildJoinKernelSpec(TestTypes(), leftType, rightType, {{"lk", "rk"}},
        EJoinType::LeftSemi, NKernel::NOz::Bool(TestTypes(), true));
    auto kernels = TKernelCompiler({.Types = SharedTestTypes(), .Annotation = TestAnnotation()}).CompileJoin(spec);
    alignas(8) std::array<uint8_t, TKernelCompiler::HashTableSize> left{};
    alignas(8) std::array<uint8_t, TKernelCompiler::HashTableSize> right{};
    alignas(8) std::array<uint8_t, TKernelCompiler::PairBufferSize> pairs{};
    ASSERT_TRUE(kernels.Dispatch(left.data(), right.data(), nullptr, 0, pairs.data(),
        nullptr, nullptr, 8, static_cast<int64_t>(EJoinKernelOp::Init)));
    TOperatorInstrumentation counters;
    counters.Kernel = MeasurementKernel();
    counters.ObserveHash(left.data(), 0, true);
    counters.ObserveHash(right.data(), 1, true);
    EXPECT_EQ(counters.Groups, 0u);
    EXPECT_GT(counters.HashMemoryPeakBytes, 0u);
    EXPECT_EQ(counters.Rehashes, 0u);
}

TEST(OperatorInstrumentation, DisabledPlanHasNoMeasurementProgramOrCounters) {
    TMockSource source({"k"});
    auto plan = SqlPlan("SELECT k FROM t", source);
    auto lowered = LowerPlanToGraph(plan, SharedTestTypes(), {}, nullptr, TestAnnotation());
    for (const auto& kernel : lowered.Kernels) {
        EXPECT_NE(kernel.Name, "instrumentation");
    }
    for (const auto& node : lowered.Graph->Nodes()) {
        EXPECT_EQ(node->Instrumentation, nullptr);
    }
}

TEST(OperatorInstrumentation, ReportsFilterAggregateCardinalitiesInBothSchedulers) {
    for (const auto mode : {EExecutionMode::SingleThreadedScheduler, EExecutionMode::ThreadedScheduler}) {
        std::array<int64_t, 100> keys;
        std::array<int64_t, 100> values;
        for (size_t i = 0; i < keys.size(); ++i) {
            keys[i] = i % 10;
            values[i] = i;
        }
        TColumn columns[] = {{.Data = reinterpret_cast<char*>(keys.data())},
            {.Data = reinterpret_cast<char*>(values.data())}};
        TRowSet batch{.Columns = columns, .ColumnCount = 2, .RowCount = 100, .RefCount = 1};
        TMockSource source({"k", "v"}, {batch});
        auto plan = SqlPlan("SELECT k, SUM(v) AS s FROM t WHERE v >= 10 GROUP BY k", source);
        TSettings settings;
        settings.Scheduler.Mode = mode;
        settings.Scheduler.WorkerCount = 2;
        settings.HashShuffle.PartitionCount = 4;
        std::ostringstream instrumentation;
        settings.Instrumentation.Operators = true;
        settings.Instrumentation.Output = &instrumentation;
        auto runtime = RunPlan(plan, settings);
        TRowSet output{};
        size_t groups = 0;
        while (runtime->Next(output)) {
            groups += output.RowCount;
            for (int64_t i = 0; i < output.RowCount; ++i) {
                const auto key = reinterpret_cast<int64_t*>(output.Columns[0].Data)[i];
                const auto sum = reinterpret_cast<int64_t*>(output.Columns[1].Data)[i];
                EXPECT_EQ(sum, 450 + key * 9);
            }
            Release(&output);
        }
        EXPECT_EQ(groups, 10u);
        EXPECT_EQ(StageSum(instrumentation.str(), "source", "rows_out"), 100u);
        EXPECT_EQ(StageSum(instrumentation.str(), "filter", "rows_out"), 90u);
        EXPECT_EQ(StageSum(instrumentation.str(), "aggregate", "groups"), 10u);
        EXPECT_EQ(StageSum(instrumentation.str(), "aggregate", "rows_in"), 90u);
        EXPECT_EQ(StageSum(instrumentation.str(), "aggregate", "rows_out"), 10u);
        EXPECT_NE(instrumentation.str().find("partition_skew="), std::string::npos);
    }
}

TEST(OperatorInstrumentation, StreamingOuterResidualsKeepCountersAcrossFinalization) {
    for (const auto mode : {EExecutionMode::SingleThreadedScheduler, EExecutionMode::ThreadedScheduler}) {
        for (const auto joinType : {EJoinType::Left, EJoinType::Right}) {
            std::array<int64_t, 4> leftKeys = {1, 1, 2, 3};
            std::array<int64_t, 4> leftValues = {10, 30, 20, 40};
            std::array<int64_t, 4> rightKeys = {1, 1, 2, 4};
            std::array<int64_t, 4> rightValues = {5, 15, 25, 50};
            TColumn leftColumns[] = {{.Data = reinterpret_cast<char*>(leftKeys.data())},
                {.Data = reinterpret_cast<char*>(leftValues.data())}};
            TColumn rightColumns[] = {{.Data = reinterpret_cast<char*>(rightKeys.data())},
                {.Data = reinterpret_cast<char*>(rightValues.data())}};
            TRowSet leftBatch{.Columns = leftColumns, .ColumnCount = 2, .RowCount = 4, .RefCount = 1};
            TRowSet rightBatch{.Columns = rightColumns, .ColumnCount = 2, .RowCount = 4, .RefCount = 1};
            TMockSource left({"lk", "lv"}, {leftBatch});
            TMockSource right({"rk", "rv"}, {rightBatch});
            auto residual = NKernel::NOz::Bin("<"_op, NKernel::NOz::Ident("lv"), NKernel::NOz::Ident("rv"));
            auto plan = NQumir::MakeIntrusive<TJoinOperator>(TestTypes(),
                NQumir::MakeIntrusive<TSourceOperator>(TestTypes(), Borrow(left), "l"),
                NQumir::MakeIntrusive<TSourceOperator>(TestTypes(), Borrow(right), "r"),
                std::vector<TJoinKey>{{"lk", "rk"}}, joinType, std::move(residual));
            AnnotateTypes(TestTypes(), plan, TestAnnotation());
            ApplyColumnPruning(TestTypes(), plan);
            TSettings settings;
            settings.Scheduler.Mode = mode;
            settings.Scheduler.WorkerCount = 2;
            settings.HashShuffle.PartitionCount = 1;
            std::ostringstream instrumentation;
            settings.Instrumentation.Operators = true;
            settings.Instrumentation.Output = &instrumentation;
            auto runtime = RunPlan(plan, settings);
            TRowSet output{};
            size_t rows = 0;
            while (runtime->Next(output)) {
                rows += output.RowCount;
                Release(&output);
            }
            EXPECT_EQ(rows, 4u);
            EXPECT_EQ(StageSum(instrumentation.str(), "source", "rows_out"), 8u);
            EXPECT_EQ(StageSum(instrumentation.str(), "join", "rows_in"), 8u);
            EXPECT_EQ(StageSum(instrumentation.str(), "join", "rows_out"), 4u);
            // The report precedes graph teardown; only the four preserved
            // two-column rows remain owned by the join at this boundary.
            EXPECT_EQ(StageSum(instrumentation.str(), "join", "retained_input_bytes"), 64u);
            EXPECT_GT(StageSum(instrumentation.str(), "join", "retained_input_peak_bytes"), 0u);
            EXPECT_GT(StageSum(instrumentation.str(), "join", "hash_memory_peak_bytes"), 0u);
        }
    }
}

TEST(OperatorInstrumentation, UnionShufflesKeepTheirOwnInputSchemas) {
    for (const auto mode : {EExecutionMode::SingleThreadedScheduler, EExecutionMode::ThreadedScheduler}) {
        std::array<int64_t, 100> keys;
        std::array<int64_t, 100> values;
        for (size_t i = 0; i < keys.size(); ++i) {
            keys[i] = i % 10;
            values[i] = i;
        }
        TColumn columns[] = {{.Data = reinterpret_cast<char*>(keys.data())},
            {.Data = reinterpret_cast<char*>(values.data())}};
        TRowSet batch{.Columns = columns, .ColumnCount = 2, .RowCount = 100, .RefCount = 1};
        TMockSource first({"k", "v"}, {batch});
        std::array<int64_t, 10> matches;
        std::iota(matches.begin(), matches.end(), 0);
        TColumn matchColumn{.Data = reinterpret_cast<char*>(matches.data())};
        TRowSet matchBatch{.Columns = &matchColumn, .ColumnCount = 1, .RowCount = 10, .RefCount = 1};
        TMockSource second({"r"}, {matchBatch});
        TMockSource third({"k", "v"}, {batch});
        TMockSource fourth({"r"}, {matchBatch});
        auto branch = [](ITable& firstInput, ITable& secondInput) -> TOperatorPtr {
            auto inputs = NQumir::MakeIntrusive<TJoinOperator>(TestTypes(),
                NQumir::MakeIntrusive<TSourceOperator>(TestTypes(), Borrow(firstInput), "t"),
                NQumir::MakeIntrusive<TSourceOperator>(TestTypes(), Borrow(secondInput), "u"),
                std::vector<TJoinKey>{{"k", "r"}}, EJoinType::Inner, nullptr);
            auto grouped = NQumir::MakeIntrusive<TAggregateOperator>(TestTypes(), inputs,
                std::vector<std::string>{"k"}, std::vector<TAggregateSpec>{
                    {.Name = "s", .Func = "sum", .Arg = NQumir::MakeIntrusive<TIdentExpr>(NQumir::TLocation{}, "v")}});
            return NQumir::MakeIntrusive<TProjectOperator>(TestTypes(), grouped,
                std::vector<TProjectionSpec>{
                    {.Name = "s", .Expression = NQumir::MakeIntrusive<TIdentExpr>(NQumir::TLocation{}, "s")}});
        };
        auto plan = NQumir::MakeIntrusive<TUnionAllOperator>(TestTypes(), std::vector<TOperatorPtr>{
            branch(first, second), branch(third, fourth)});
        AnnotateTypes(TestTypes(), plan, TestAnnotation());
        ApplyColumnPruning(TestTypes(), plan);
        TSettings settings;
        settings.Scheduler.Mode = mode;
        settings.Scheduler.WorkerCount = 2;
        settings.HashShuffle.PartitionCount = 4;
        std::ostringstream instrumentation;
        settings.Instrumentation.Operators = true;
        settings.Instrumentation.Output = &instrumentation;
        auto runtime = RunPlan(plan, settings);
        std::unordered_map<int64_t, size_t> counts;
        TRowSet output{};
        while (runtime->Next(output)) {
            ASSERT_EQ(output.ColumnCount, 1);
            for (int64_t i = 0; i < output.RowCount; ++i) {
                ++counts[reinterpret_cast<int64_t*>(output.Columns[0].Data)[i]];
            }
            Release(&output);
        }
        ASSERT_EQ(counts.size(), 10u);
        for (int64_t key = 0; key < 10; ++key) {
            EXPECT_EQ(counts[450 + key * 10], 2u);
        }
        EXPECT_EQ(StageSum(instrumentation.str(), "source", "rows_out"), 220u);
    }
}

TEST(OperatorInstrumentation, ShuffleAccountsSparseRetainedOwnersOnceAcrossPartitions) {
    for (size_t retentionBudget : {1048576u, 1u}) {
        SCOPED_TRACE(retentionBudget);
        TTaskNode node;
        node.Instrumentation = std::make_unique<TOperatorInstrumentation>();
        node.Instrumentation->Kernel = MeasurementKernel();
        node.Instrumentation->Inputs.reserve(1);
        std::array<int64_t, 10000> keys;
        std::array<int64_t, 10000> values;
        std::array<uint8_t, 10000> selected{};
        for (size_t i = 0; i < keys.size(); ++i) {
            keys[i] = i % 2;
            values[i] = i;
        }
        selected[9998] = selected[9999] = 0xff;
        TColumn columns[] = {{.Data = reinterpret_cast<char*>(keys.data())},
            {.Data = reinterpret_cast<char*>(values.data())}};
        TRowSet batch{.Columns = columns, .ColumnCount = 2, .RowCount = 10000,
            .Selection = selected.data(), .RefCount = 1};
        auto i64 = TestTypes().MakeInteger();
        auto schema = TestTypes().MakeStruct(std::vector<std::pair<std::string, TTypePtr>>{
            {"k", i64}, {"v", i64}});
        auto hash = TKernelCompiler({.Types = SharedTestTypes(), .Annotation = TestAnnotation()}).CompileAggregateHash(*schema, {"k"});
        std::vector<uint64_t> hashes(10000);
        ASSERT_TRUE(hash(&batch, hashes.data()));
        ASSERT_NE(hashes[9998] % 4, hashes[9999] % 4);
        auto code = std::make_shared<THashShuffleCode>(hash, RowLayouts(schema), 16384, 65536, retentionBudget);
        TOneToOneConnection input(1);
        input.Resize(1, 1);
        std::vector<std::unique_ptr<TGatherConnection>> output;
        std::vector<TOutputPort> ports;
        std::unordered_map<const IConnection*, TTypePtr> types{{&input, schema}};
        for (size_t partition = 0; partition < 4; ++partition) {
            output.push_back(std::make_unique<TGatherConnection>(1));
            ports.push_back(TOutputPort{.Connection = output.back().get()});
            types.emplace(output.back().get(), schema);
        }
        THashShuffleTask task(code, TInputPort{.Connection = &input}, std::move(ports));
        node.Task = &task;
        TTaskInstrumentationSetup setup(node, types);
        task.ConfigureInstrumentation(setup);
        ASSERT_TRUE(input.Push(0, std::move(batch)));
        input.Finish(0);
        EXPECT_EQ(task.Execute(), ETaskResult::OK);
        EXPECT_EQ(node.Instrumentation->Inputs[0].Rows, 2u);
        const bool flushedBeforeEof = retentionBudget < 160000;
        // A pending copied row can overlap the original shared input owner.
        const uint64_t expectedPeak = flushedBeforeEof ? 160016u : 160000u;
        EXPECT_EQ(node.Instrumentation->Output.Rows, flushedBeforeEof ? 2u : 0u);
        EXPECT_EQ(node.Instrumentation->RetainedInputBytes, flushedBeforeEof ? 0u : 160000u);
        EXPECT_EQ(node.Instrumentation->RetainedInputPeakBytes, expectedPeak);
        EXPECT_EQ(task.Execute(), ETaskResult::FINISHED);
        EXPECT_EQ(node.Instrumentation->Output.Rows, 2u);
        EXPECT_EQ(node.Instrumentation->RetainedInputBytes, 0u);
        EXPECT_EQ(node.Instrumentation->RetainedInputPeakBytes, expectedPeak);
        EXPECT_EQ(node.Instrumentation->BuildMemoryPeakBytes, expectedPeak);
        for (size_t partition = 0; partition < 4; ++partition) {
            TRowSet rowSet{};
            while (output[partition]->Fetch(0, rowSet) == EFetchResult::OK) {
                EXPECT_EQ(rowSet.RowCount, flushedBeforeEof ? 1 : 10000);
                EXPECT_EQ(rowSet.Selection == nullptr, flushedBeforeEof);
                Release(&rowSet);
            }
        }
    }
}

TEST(OperatorInstrumentation, ThreadCpuExcludesSleepingAndSurvivesWorkerMigration) {
    class TSleepTask final : public ITask {
    public:
        ETaskResult Execute() override {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            return ETaskResult::OK;
        }
    } task;
    TTaskNode node;
    node.Task = &task;
    node.Instrumentation = std::make_unique<TOperatorInstrumentation>();
    const auto start = std::chrono::steady_clock::now();
    for (size_t worker = 0; worker < 2; ++worker) {
        std::thread([&] {
            TExecutionState state(1);
            TExecutionContext context{.State = &state};
            EXPECT_EQ(ExecuteTask(node, context), ETaskResult::OK);
        }).join();
    }
    const auto wall = std::chrono::steady_clock::now() - start;
    EXPECT_GE(wall, std::chrono::milliseconds(100));
    EXPECT_GT(node.Instrumentation->ExecutionCpuNanoseconds, 0u);
    EXPECT_LT(node.Instrumentation->ExecutionCpuNanoseconds, 50'000'000u);
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    NQumir::NCodeGen::TLLVMInitializer llvmInit;
    return RUN_ALL_TESTS();
}
