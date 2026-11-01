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
    TExecutionContext context;
    dispatch(context, rowSet, nullptr);
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
    TExecutionContext context;
    dispatch(context, &rowSet, outBuffers.data(), nullptr);
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
    TExecutionContext context;
    TKeyValueBatch first{.Keys = {1, 2, 1, 3}, .Values = {10, 20, 30, 40}};
    TKeyValueBatch second{.Keys = {3, 4, 2}, .Values = {1, 2, 3}};
    auto rows = first.RowSet();
    processor.Add(context, rows);
    rows = second.RowSet();
    processor.Add(context, rows);
    TRowSet result{};
    EXPECT_TRUE(processor.Finish(context, result));
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

// An unfinished table is freed by the destroy kernel, which the destructor
// runs on the VM outside any worker.
TEST(KernelVm, AggregateDestroyRunsWithoutFinish) {
    TAggregateProcessor processor(CompileSumByKey(EKernelBackend::Vm));
    TExecutionContext context;
    TKeyValueBatch batch{.Keys = {1, 2}, .Values = {3, 4}};
    auto rows = batch.RowSet();
    processor.Add(context, rows);
    EXPECT_EQ(processor.Groups(), 2);
}

// Workers may each run the same program; their instances are independent.
TEST(KernelVm, ContextsShareOneProgram) {
    TKernelCompiler compiler({.Backend = EKernelBackend::Vm});
    auto dispatch = compiler.CompileFilter(NKernel::BuildFilterKernelSpec(
        KeyValueType(), Parse("(> v 2)")));
    TKeyValueBatch batch{.Keys = {1, 2}, .Values = {3, 1}};
    for (int run = 0; run < 2; ++run) {
        std::vector<uint8_t> selection(batch.Keys.size());
        auto rowSet = batch.RowSet(selection.data());
        TExecutionContext context;
        dispatch(context, rowSet, nullptr);
        dispatch(context, rowSet, nullptr);
        EXPECT_NE(selection[0], 0);
        EXPECT_EQ(selection[1], 0);
    }
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
