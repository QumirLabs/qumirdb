#include <gtest/gtest.h>

#include <qdb/exec/aggregate_exec.h>
#include <qdb/io/io.h>
#include <qdb/kernel/annotate_type.h>
#include <qdb/kernel/compiler.h>
#include <qdb/kernel/finalize.h>
#include <qdb/kernel/spec.h>
#include <qdb/kernel/execution_context.h>
#include <qdb/kernel/vm_program.h>

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>
#include <qumir/parser/type.h>

#include <array>
#include <cstdint>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace NQdb;
using namespace NQumir::NAst;

namespace {

TExprPtr Parse(const std::string& source) {
    std::istringstream input(source);
    NCore::TTokenStream tokens(input);
    NCore::TParser parser;
    auto parsed = parser.Parse(tokens);
    if (!parsed) {
        throw std::runtime_error(parsed.error().ToString());
    }
    return std::move(*parsed);
}

TStructType KeyValueType() {
    return TStructType({
        {"k", std::make_shared<TIntegerType>(TIntegerType::I64)},
        {"v", std::make_shared<TIntegerType>(TIntegerType::I64)},
    });
}

struct TKeyValueBatch {
    std::vector<int64_t> Keys;
    std::vector<int64_t> Values;
    std::array<TColumn, 2> Columns{};

    TRowSet RowSet(uint8_t* selection = nullptr) {
        Columns = {
            TColumn{.Data = reinterpret_cast<char*>(Keys.data())},
            TColumn{.Data = reinterpret_cast<char*>(Values.data())},
        };
        return TRowSet{
            .Columns = Columns.data(),
            .ColumnCount = 2,
            .RowCount = static_cast<int64_t>(Keys.size()),
            .Selection = selection,
            .RefCount = 1,
        };
    }
};

std::vector<uint8_t> RunFilter(EKernelBackend backend) {
    TKernelCompiler compiler({.Backend = backend});
    auto dispatch = compiler.CompileFilter(NKernel::BuildFilterKernelSpec(
        KeyValueType(), Parse("(&& (> v 2) (!= k 4))")));
    TKeyValueBatch batch{.Keys = {1, 2, 3, 4, 5}, .Values = {5, 1, 3, 9, 2}};
    std::vector<uint8_t> selection(batch.Keys.size());
    auto rowSet = batch.RowSet(selection.data());
    dispatch(rowSet, nullptr);
    return selection;
}

std::vector<int64_t> RunProject(EKernelBackend backend) {
    const auto inputType = KeyValueType();
    std::vector<TExprPtr> exprs = {Parse("(+ (* k 10) v)")};
    std::vector<TTypePtr> types = {NKernel::AnnotateExprType(exprs[0], inputType)};
    TKernelCompiler compiler({.Backend = backend});
    auto dispatch = compiler.CompileProject(
        NKernel::BuildProjectKernelSpec(inputType, exprs, types));
    TKeyValueBatch batch{.Keys = {1, 2, 3}, .Values = {7, 8, 9}};
    auto rowSet = batch.RowSet();
    std::vector<int64_t> output(batch.Keys.size());
    std::array<void*, 2> outBuffers = {output.data(), nullptr};
    dispatch(&rowSet, outBuffers.data(), nullptr);
    return output;
}

TAggregateKernels CompileSumByKey(EKernelBackend backend) {
    std::vector<TAggregateSpec> aggs = {{
        .Name = "s",
        .Func = "sum",
        .Arg = std::make_shared<TIdentExpr>(NQumir::TLocation{}, "v"),
    }};
    TKernelCompiler compiler({.Backend = backend});
    return compiler.CompileAggregate(NKernel::BuildAggregateKernelSpec(KeyValueType(), {"k"}, aggs));
}

// Update over two batches, then finish; the result keyed by group.
std::map<int64_t, int64_t> RunAggregate(EKernelBackend backend) {
    TAggregateProcessor processor(CompileSumByKey(backend));
    TKeyValueBatch first{.Keys = {1, 2, 1, 3}, .Values = {10, 20, 30, 40}};
    TKeyValueBatch second{.Keys = {3, 4, 2}, .Values = {1, 2, 3}};
    auto rows = first.RowSet();
    processor.Add(rows);
    rows = second.RowSet();
    processor.Add(rows);
    TRowSet result{};
    EXPECT_TRUE(processor.Finish(result));
    std::map<int64_t, int64_t> groups;
    const auto* keys = reinterpret_cast<const int64_t*>(result.Columns[0].Data);
    const auto* sums = reinterpret_cast<const int64_t*>(result.Columns[1].Data);
    for (int64_t row = 0; row < result.RowCount; ++row) {
        groups.emplace(keys[row], sums[row]);
    }
    Release(&result);
    return groups;
}

} // namespace

TEST(KernelVm, FilterMatchesLlvm) {
    const auto vm = RunFilter(EKernelBackend::Vm);
    EXPECT_EQ(vm, RunFilter(EKernelBackend::Llvm));
    EXPECT_EQ(vm, (std::vector<uint8_t>{0xff, 0, 0xff, 0, 0}));
}

TEST(KernelVm, ProjectMatchesLlvm) {
    const auto vm = RunProject(EKernelBackend::Vm);
    EXPECT_EQ(vm, RunProject(EKernelBackend::Llvm));
    EXPECT_EQ(vm, (std::vector<int64_t>{17, 28, 39}));
}

TEST(KernelVm, AggregateMatchesLlvm) {
    const auto vm = RunAggregate(EKernelBackend::Vm);
    EXPECT_EQ(vm, RunAggregate(EKernelBackend::Llvm));
    EXPECT_EQ(vm, (std::map<int64_t, int64_t>{{1, 40}, {2, 23}, {3, 41}, {4, 2}}));
}

// Abandoned state belongs to the heap, without calling a teardown kernel.
TEST(KernelVm, UnfinishedAggregateUsesHeapCleanup) {
    TExecutionState state(1);
    TExecutionContext worker{.State = &state};
    auto& heap = state.Heap(0);
    {
        TTaskExecutionGuard pin(worker, 0);
        TAggregateProcessor processor(CompileSumByKey(EKernelBackend::Vm));
        TKeyValueBatch batch{.Keys = {1, 2}, .Values = {3, 4}};
        auto rows = batch.RowSet();
        processor.Add(rows);
        EXPECT_EQ(processor.Groups(), 2);
        EXPECT_GT(heap.Bytes(), 0u);
    }
    EXPECT_GT(heap.Bytes(), 0u);
    heap.Reset();
    EXPECT_EQ(heap.Bytes(), 0u);
}

TEST(KernelVm, AggregateMigratesAndOutputOutlivesExecutionState) {
    TRowSet result{};
    {
        TExecutionState state(1);
        TExecutionContext first{.State = &state};
        TExecutionContext second{.State = &state};
        TAggregateProcessor processor(CompileSumByKey(EKernelBackend::Vm));
        TKeyValueBatch batch{.Keys = {1, 2, 1}, .Values = {10, 20, 30}};
        auto rows = batch.RowSet();
        {
            TTaskExecutionGuard pin(first, 0);
            processor.Add(rows);
            ASSERT_TRUE(first.Vm);
            EXPECT_GT(state.Heap(0).Bytes(), 0u);
        }
        {
            TTaskExecutionGuard pin(second, 0);
            EXPECT_TRUE(processor.Finish(result));
            ASSERT_TRUE(second.Vm);
            EXPECT_NE(first.Vm, second.Vm);
            EXPECT_EQ(state.Heap(0).Bytes(), 0u);
        }
    }
    ASSERT_EQ(result.RowCount, 2);
    const auto* keys = reinterpret_cast<const int64_t*>(result.Columns[0].Data);
    const auto* sums = reinterpret_cast<const int64_t*>(result.Columns[1].Data);
    std::map<int64_t, int64_t> groups;
    for (int64_t row = 0; row < result.RowCount; ++row) {
        groups.emplace(keys[row], sums[row]);
    }
    EXPECT_EQ(groups, (std::map<int64_t, int64_t>{{1, 40}, {2, 20}}));
    Release(&result);
}

// Workers may each run the same program; their instances are independent.
TEST(KernelVm, ContextsShareOneProgram) {
    TKernelCompiler compiler({.Backend = EKernelBackend::Vm});
    auto dispatch = compiler.CompileFilter(NKernel::BuildFilterKernelSpec(
        KeyValueType(), Parse("(> v 2)")));
    TKeyValueBatch batch{.Keys = {1, 2}, .Values = {3, 1}};
    TExecutionState state(1);
    std::array<TExecutionContext, 2> workers{{{.State = &state}, {.State = &state}}};
    for (auto& worker : workers) {
        TTaskExecutionGuard pin(worker, 0);
        std::vector<uint8_t> selection(batch.Keys.size());
        auto rowSet = batch.RowSet(selection.data());
        dispatch(rowSet, nullptr);
        auto instance = worker.Vm;
        ASSERT_TRUE(instance);
        dispatch(rowSet, nullptr);
        EXPECT_EQ(worker.Vm, instance);
        EXPECT_NE(selection[0], 0);
        EXPECT_EQ(selection[1], 0);
    }
    EXPECT_NE(workers[0].Vm, workers[1].Vm);
}

TEST(KernelVm, KernelProgramHasNoModuleConstructor) {
    std::vector<TGeneratedKernel> kernels;
    TKernelCompiler compiler({.Sink = &kernels, .BindNow = false});
    compiler.CompileAggregate(NKernel::BuildAggregateKernelSpec(KeyValueType(), {"k"}, {}));
    compiler.CompileFilter(NKernel::BuildFilterKernelSpec(KeyValueType(), Parse("(> v 2)")));
    FinalizeKernels(kernels, EKernelBackend::Vm);
    ASSERT_TRUE(kernels.front().Slot->Vm);
    // Kernels hold no module state, so an instance needs no constructor run.
    EXPECT_EQ(kernels.front().Slot->Vm->Module.ModuleConstructorFunctionId, -1);
    // Both kernels share one prepared program.
    EXPECT_EQ(kernels.front().Slot->Vm, kernels.back().Slot->Vm);
}

TEST(KernelVm, RefinalizingForAnotherBackendThrows) {
    std::vector<TGeneratedKernel> kernels;
    TKernelCompiler compiler({.Sink = &kernels, .BindNow = false});
    compiler.CompileFilter(NKernel::BuildFilterKernelSpec(KeyValueType(), Parse("(> v 2)")));
    FinalizeKernels(kernels, EKernelBackend::Vm);
    EXPECT_NO_THROW(FinalizeKernels(kernels, EKernelBackend::Vm));
    EXPECT_THROW(FinalizeKernels(kernels, EKernelBackend::Llvm), std::logic_error);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    NQumir::NCodeGen::TLLVMInitializer llvmInit;
    return RUN_ALL_TESTS();
}
