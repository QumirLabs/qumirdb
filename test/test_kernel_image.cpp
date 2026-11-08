#include <gtest/gtest.h>

#include <qdb/exec/aggregate_exec.h>
#include <qdb/exec/join_exec.h>
#include <qdb/exec/row_join_exec.h>
#include <qdb/io/io.h>
#include <qdb/kernel/annotate_type.h>
#include <qdb/kernel/compiler.h>
#include <qdb/kernel/image.h>
#include <qdb/kernel/spec.h>

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>
#include <qumir/parser/type.h>

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace NQdb;
using namespace NQumir::NAst;

extern char** environ;

namespace {

std::string ImagePathArg;
std::string Argv0;

TExprPtr Parse(const std::string& source)
{
    std::istringstream input(source);
    NCore::TTokenStream tokens(input);
    NCore::TParser parser;
    auto parsed = parser.Parse(tokens);
    if (!parsed) {
        throw std::runtime_error(parsed.error().ToString());
    }
    return std::move(*parsed);
}

TStructType KeyValueType()
{
    return TStructType({
        {"k", std::make_shared<TIntegerType>(TIntegerType::I64)},
        {"v", std::make_shared<TIntegerType>(TIntegerType::I64)},
    });
}

struct TKeyValueBatch {
    std::vector<int64_t> Keys;
    std::vector<int64_t> Values;
    std::array<TColumn, 2> Columns{};

    TRowSet RowSet(uint8_t* selection = nullptr)
    {
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

NKernel::TOperatorKernelSpec FilterSpec()
{
    return NKernel::BuildFilterKernelSpec(KeyValueType(), Parse("(&& (> v 2) (!= k 4))"));
}

std::vector<uint8_t> RunFilter(TKernelCompiler::TFilterDispatch& dispatch)
{
    TKeyValueBatch batch{.Keys = {1, 2, 3, 4, 5}, .Values = {5, 1, 3, 9, 2}};
    std::vector<uint8_t> selection(batch.Keys.size());
    auto rowSet = batch.RowSet(selection.data());
    dispatch(rowSet, nullptr);
    return selection;
}

NKernel::TOperatorKernelSpec ProjectSpec()
{
    const auto inputType = KeyValueType();
    std::vector<TExprPtr> exprs = {Parse("(+ (* k 10) v)")};
    std::vector<TTypePtr> types = {NKernel::AnnotateExprType(exprs[0], inputType)};
    return NKernel::BuildProjectKernelSpec(inputType, exprs, types);
}

std::vector<int64_t> RunProject(const TKernelCompiler::TProjectDispatch& dispatch)
{
    TKeyValueBatch batch{.Keys = {1, 2, 3}, .Values = {7, 8, 9}};
    auto rowSet = batch.RowSet();
    std::vector<int64_t> output(batch.Keys.size());
    std::array<void*, 2> outBuffers = {output.data(), nullptr};
    dispatch(&rowSet, outBuffers.data(), nullptr);
    return output;
}

std::vector<uint64_t> RunAggregateHash(const TKernelCompiler::TAggregateHashDispatch& dispatch)
{
    TKeyValueBatch batch{.Keys = {1, 2, 1}, .Values = {7, 8, 9}};
    auto rowSet = batch.RowSet();
    std::vector<uint64_t> hashes(batch.Keys.size());
    EXPECT_TRUE(dispatch(&rowSet, hashes.data()));
    return hashes;
}

NKernel::TOperatorKernelSpec AggregateSpec()
{
    std::vector<TAggregateSpec> aggs = {{
        .Name = "s",
        .Func = "sum",
        .Arg = std::make_shared<TIdentExpr>(NQumir::TLocation{}, "v"),
    }};
    return NKernel::BuildAggregateKernelSpec(KeyValueType(), {"k"}, aggs);
}

std::map<int64_t, int64_t> RunAggregate(TAggregateKernels kernels)
{
    TAggregateProcessor processor(std::move(kernels));
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

const std::map<int64_t, int64_t> ExpectedSums = {{1, 40}, {2, 23}, {3, 41}, {4, 2}};

// Compiles the aggregate kernels into an image without binding them.
std::string CompileAggregateImage()
{
    std::vector<TGeneratedKernel> kernels;
    TKernelCompiler compiler({.Sink = &kernels, .BindNow = false});
    compiler.CompileAggregate(AggregateSpec());
    return SerializeKernelImage(CompileKernelImage(kernels));
}

// Regenerates the aggregate kernels unbound and loads the image into their slots.
std::map<int64_t, int64_t> LoadAndRunAggregate(const std::string& bytes)
{
    std::vector<TGeneratedKernel> kernels;
    TKernelCompiler compiler({.Sink = &kernels, .BindNow = false});
    auto aggregate = compiler.CompileAggregate(AggregateSpec());
    std::vector<std::shared_ptr<TKernelSlot>> slots;
    for (const auto& kernel : kernels) {
        slots.push_back(kernel.Slot);
    }
    LoadKernelImage(DeserializeKernelImage(bytes), slots);
    return RunAggregate(std::move(aggregate));
}

TStructType LeftJoinType()
{
    return TStructType({
        {"lk", std::make_shared<TIntegerType>(TIntegerType::I64)},
        {"lv", std::make_shared<TIntegerType>(TIntegerType::I64)},
    });
}

TStructType RightJoinType()
{
    return TStructType({
        {"rk", std::make_shared<TIntegerType>(TIntegerType::I64)},
        {"rv", std::make_shared<TIntegerType>(TIntegerType::I64)},
    });
}

NKernel::TOperatorKernelSpec JoinSpec()
{
    return NKernel::BuildJoinKernelSpec(
        LeftJoinType(), RightJoinType(), {{"lk", "rk"}}, EJoinType::Inner);
}

// Runs the processor over fixed batches and returns the sorted (lv, rv) pairs.
template <class TProcessor>
std::vector<std::pair<int64_t, int64_t>> RunJoin(TProcessor& processor)
{
    TKeyValueBatch left{.Keys = {1, 2, 1}, .Values = {10, 20, 30}};
    TKeyValueBatch right{.Keys = {1, 1, 3}, .Values = {100, 200, 300}};
    const auto leftRows = left.RowSet();
    const auto rightRows = right.RowSet();
    bool leftDone = false;
    bool rightDone = false;
    auto fetch = [](const TRowSet& rows, bool& done) {
        return [&rows, &done](TRowSet& batch) {
            if (done) {
                return EJoinFetchResult::FINISHED;
            }
            batch = rows;
            done = true;
            return EJoinFetchResult::OK;
        };
    };

    std::vector<std::pair<int64_t, int64_t>> pairs;
    TRowSet out{};
    for (;;) {
        const auto status = processor.Process(fetch(leftRows, leftDone), fetch(rightRows, rightDone), out);
        if (status == EJoinProcessorResult::NEED_DATA) {
            continue;
        }
        if (status == EJoinProcessorResult::FINISHED) {
            break;
        }
        const auto* lv = reinterpret_cast<const int64_t*>(out.Columns[1].Data);
        const auto* rv = reinterpret_cast<const int64_t*>(out.Columns[3].Data);
        for (int64_t row = 0; row < out.RowCount; ++row) {
            pairs.emplace_back(lv[row], rv[row]);
        }
        Release(&out);
    }
    std::ranges::sort(pairs);
    return pairs;
}

const std::vector<std::pair<int64_t, int64_t>> ExpectedJoinPairs =
    {{10, 100}, {10, 200}, {30, 100}, {30, 200}};

std::vector<std::shared_ptr<TKernelSlot>> LoadImageSlots(std::vector<TGeneratedKernel>& kernels)
{
    const auto bytes = SerializeKernelImage(CompileKernelImage(kernels));
    std::vector<std::shared_ptr<TKernelSlot>> slots;
    for (const auto& kernel : kernels) {
        slots.push_back(kernel.Slot);
    }
    LoadKernelImage(DeserializeKernelImage(bytes), slots);
    return slots;
}

std::pair<std::vector<uint64_t>, std::vector<uint64_t>> RunJoinHash(const TJoinHashKernels& hash)
{
    TKeyValueBatch left{.Keys = {1, 2, 1, 4}, .Values = {0, 0, 0, 0}};
    TKeyValueBatch right{.Keys = {1, 3, 2, 1}, .Values = {0, 0, 0, 0}};
    auto leftRows = left.RowSet();
    auto rightRows = right.RowSet();
    std::vector<uint64_t> leftHashes(4);
    std::vector<uint64_t> rightHashes(4);
    EXPECT_TRUE(hash.Left(&leftRows, leftHashes.data()));
    EXPECT_TRUE(hash.Right(&rightRows, rightHashes.data()));
    return {leftHashes, rightHashes};
}

} // namespace

TEST(KernelImage, FilterRoundTrip)
{
    TKernelCompiler jit({});
    auto expected = jit.CompileFilter(FilterSpec());

    std::vector<TGeneratedKernel> kernels;
    TKernelCompiler compiler({.Sink = &kernels, .BindNow = false});
    auto dispatch = compiler.CompileFilter(FilterSpec());
    const auto bytes = SerializeKernelImage(CompileKernelImage(kernels));
    ASSERT_EQ(kernels.size(), 1u);
    EXPECT_EQ(kernels[dispatch.Kernel].Slot, dispatch.Slot);
    // Load through the recorded kernel index, as a deserialized graph would.
    std::vector<std::shared_ptr<TKernelSlot>> slots(kernels.size());
    slots[dispatch.Kernel] = dispatch.Slot;
    LoadKernelImage(DeserializeKernelImage(bytes), slots);

    EXPECT_EQ(dispatch.Slot->Runner, nullptr);
    EXPECT_EQ(RunFilter(dispatch), RunFilter(expected));
    EXPECT_EQ(RunFilter(dispatch), (std::vector<uint8_t>{0xff, 0, 0xff, 0, 0}));
}

TEST(KernelImage, FilterDispatchRecordsKernelIndex)
{
    std::vector<TGeneratedKernel> kernels;
    TKernelCompiler compiler({.Sink = &kernels, .BindNow = false});
    auto first = compiler.CompileFilter(FilterSpec());
    auto second = compiler.CompileFilter(FilterSpec());

    EXPECT_EQ(first.Kernel, 0u);
    EXPECT_EQ(second.Kernel, 1u);
    ASSERT_EQ(kernels.size(), 2u);
    EXPECT_EQ(kernels[first.Kernel].Slot, first.Slot);
    EXPECT_EQ(kernels[second.Kernel].Slot, second.Slot);

    TKernelCompiler unsunk({});
    EXPECT_EQ(unsunk.CompileFilter(FilterSpec()).Kernel, 0u);
}

TEST(KernelImage, ProjectAndAggregateHashBindFromImageSlots)
{
    TKernelCompiler jit({});
    const auto expectedProject = RunProject(jit.CompileProject(ProjectSpec()));
    const auto expectedHash = RunAggregateHash(jit.CompileAggregateHash(KeyValueType(), {"k"}));

    std::vector<TGeneratedKernel> kernels;
    TKernelCompiler compiler({.Sink = &kernels, .BindNow = false});
    auto project = compiler.CompileProject(ProjectSpec());
    auto hash = compiler.CompileAggregateHash(KeyValueType(), {"k"});
    ASSERT_EQ(kernels.size(), 2u);
    EXPECT_EQ(project.Kernel, 0u);
    EXPECT_EQ(hash.Kernel, 1u);
    EXPECT_EQ(kernels[project.Kernel].Slot, project.Slot);
    EXPECT_EQ(kernels[hash.Kernel].Slot, hash.Slot);

    const auto bytes = SerializeKernelImage(CompileKernelImage(kernels));
    std::vector<std::shared_ptr<TKernelSlot>> slots;
    for (const auto& kernel : kernels) {
        slots.push_back(kernel.Slot);
    }
    LoadKernelImage(DeserializeKernelImage(bytes), slots);

    auto boundProject = BindProject(slots[project.Kernel], project.Storage, project.Runtime);
    auto boundHash = BindAggregateHash(slots[hash.Kernel], hash.KeyColumns);
    EXPECT_EQ(RunProject(boundProject), expectedProject);
    EXPECT_EQ(RunAggregateHash(boundHash), expectedHash);
}

TEST(KernelImage, AggregateRoundTrip)
{
    TKernelCompiler jit({});
    const auto expected = RunAggregate(jit.CompileAggregate(AggregateSpec()));
    EXPECT_EQ(expected, ExpectedSums);
    EXPECT_EQ(LoadAndRunAggregate(CompileAggregateImage()), expected);
}

TEST(KernelImage, AggregateBindsFromImageSlot)
{
    TKernelCompiler jit({});
    const auto expected = RunAggregate(jit.CompileAggregate(AggregateSpec()));

    std::vector<TGeneratedKernel> kernels;
    TKernelCompiler compiler({.Sink = &kernels, .BindNow = false});
    // A leading filter shifts the aggregate kernel index.
    compiler.CompileFilter(FilterSpec());
    auto aggregate = compiler.CompileAggregate(AggregateSpec());
    ASSERT_EQ(kernels.size(), 2u);
    EXPECT_EQ(aggregate.Kernel, 1u);
    EXPECT_EQ(kernels[aggregate.Kernel].Slot, aggregate.Slot);

    const auto bytes = SerializeKernelImage(CompileKernelImage(kernels));
    std::vector<std::shared_ptr<TKernelSlot>> slots;
    for (const auto& kernel : kernels) {
        slots.push_back(kernel.Slot);
    }
    LoadKernelImage(DeserializeKernelImage(bytes), slots);

    auto bound = BindAggregateKernels(slots[aggregate.Kernel], aggregate);
    EXPECT_EQ(bound.Slot, slots[aggregate.Kernel]);
    EXPECT_EQ(bound.StateWords, aggregate.StateWords);
    EXPECT_EQ(RunAggregate(std::move(bound)), expected);
    EXPECT_EQ(expected, ExpectedSums);
}

TEST(KernelImage, JoinBindsFromImageSlot)
{
    TKernelCompiler jit({});
    TInnerJoinProcessor expectedProcessor(jit.CompileJoin(JoinSpec()));
    const auto expected = RunJoin(expectedProcessor);

    std::vector<TGeneratedKernel> kernels;
    TKernelCompiler compiler({.Sink = &kernels, .BindNow = false});
    compiler.CompileFilter(FilterSpec());
    auto join = compiler.CompileJoin(JoinSpec());
    ASSERT_EQ(kernels.size(), 2u);
    EXPECT_EQ(join.Kernel, 1u);
    EXPECT_EQ(kernels[join.Kernel].Slot, join.Slot);

    auto slots = LoadImageSlots(kernels);
    TInnerJoinProcessor processor(BindJoinKernels(slots[join.Kernel], join));
    EXPECT_EQ(RunJoin(processor), expected);
    EXPECT_EQ(expected, ExpectedJoinPairs);
}

TEST(KernelImage, RowJoinBindsFromImageSlots)
{
    constexpr size_t probeHoldBytes = 1 << 20;
    TKernelCompiler jit({});
    TRowJoinProcessor expectedProcessor(
        jit.CompileRowJoin(JoinSpec(), /*buildIsLeft=*/false), EJoinBuildSide::Right, probeHoldBytes);
    const auto expected = RunJoin(expectedProcessor);

    std::vector<TGeneratedKernel> kernels;
    TKernelCompiler compiler({.Sink = &kernels, .BindNow = false});
    compiler.CompileFilter(FilterSpec());
    auto join = compiler.CompileRowJoin(JoinSpec(), /*buildIsLeft=*/false);
    ASSERT_EQ(kernels.size(), 3u);
    EXPECT_EQ(join.Kernel, 1u);
    EXPECT_EQ(join.ProbeMeasureKernel, 2u);
    EXPECT_EQ(kernels[join.Kernel].Slot, join.Slot);
    EXPECT_EQ(kernels[join.ProbeMeasureKernel].Slot, join.ProbeMeasure.Kernel);

    auto slots = LoadImageSlots(kernels);
    auto bound = BindRowJoinKernels(slots[join.Kernel], slots[join.ProbeMeasureKernel], join);
    EXPECT_EQ(bound.ProbeMeasure.Kernel, slots[join.ProbeMeasureKernel]);
    EXPECT_EQ(bound.ProbeMeasure.Widths, join.ProbeMeasure.Widths);
    TRowJoinProcessor processor(std::move(bound), EJoinBuildSide::Right, probeHoldBytes);
    EXPECT_EQ(RunJoin(processor), expected);
    EXPECT_EQ(expected, ExpectedJoinPairs);
}

TEST(KernelImage, CrossJoinBindsFromImageSlot)
{
    const auto spec = NKernel::BuildCrossJoinKernelSpec(LeftJoinType(), RightJoinType());
    TKernelCompiler jit({});
    TCrossJoinProcessor expectedProcessor(jit.CompileCrossJoin(spec));
    const auto expected = RunJoin(expectedProcessor);

    std::vector<TGeneratedKernel> kernels;
    TKernelCompiler compiler({.Sink = &kernels, .BindNow = false});
    compiler.CompileFilter(FilterSpec());
    auto join = compiler.CompileCrossJoin(spec);
    ASSERT_EQ(kernels.size(), 2u);
    EXPECT_EQ(join.Kernel, 1u);
    EXPECT_EQ(kernels[join.Kernel].Slot, join.Slot);

    auto slots = LoadImageSlots(kernels);
    TCrossJoinProcessor processor(BindCrossJoinKernels(slots[join.Kernel], join));
    EXPECT_EQ(RunJoin(processor), expected);
    EXPECT_EQ(expected.size(), 9u);
}

TEST(KernelImage, JoinHashBindsFromImageSlot)
{
    TKernelCompiler jit({});
    const auto expected = RunJoinHash(jit.CompileJoinHash(JoinSpec()));

    std::vector<TGeneratedKernel> kernels;
    TKernelCompiler compiler({.Sink = &kernels, .BindNow = false});
    compiler.CompileFilter(FilterSpec());
    auto hash = compiler.CompileJoinHash(JoinSpec());
    ASSERT_EQ(kernels.size(), 2u);
    EXPECT_EQ(hash.Kernel, 1u);
    EXPECT_EQ(kernels[hash.Kernel].Slot, hash.Slot);

    auto slots = LoadImageSlots(kernels);
    const auto bound = RunJoinHash(BindJoinHashKernels(slots[hash.Kernel], hash));
    EXPECT_EQ(bound, expected);
    // Equal keys hash alike on both sides: left 1 = right 1.
    EXPECT_EQ(bound.first[0], bound.second[0]);
    EXPECT_EQ(bound.first[1], bound.second[2]);
}

TEST(KernelImage, AggregateInAnotherProcess)
{
    static int counter = 0;
    const auto path = std::filesystem::temp_directory_path() /
        ("qdb_kernel_image_" + std::to_string(::getpid()) + "_" + std::to_string(counter++) + ".bin");
    struct TRemove {
        std::filesystem::path Path;
        ~TRemove()
        {
            std::error_code ec;
            std::filesystem::remove(Path, ec);
        }
    } removeFile{path};
    {
        std::ofstream out(path, std::ios::binary);
        const auto bytes = CompileAggregateImage();
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    // Re-exec the test binary: a fork without exec would share this process's JIT state.
    const std::string exe = std::filesystem::absolute(Argv0).string();
    std::string filter = "--gtest_filter=KernelImage.LoadsAggregateImage";
    std::string imageArg = "--kernel-image=" + path.string();
    std::string exeArg = exe;
    char* args[] = {exeArg.data(), filter.data(), imageArg.data(), nullptr};
    pid_t pid = 0;
    ASSERT_EQ(posix_spawn(&pid, exe.c_str(), nullptr, nullptr, args, environ), 0);
    int status = 0;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    const int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    EXPECT_EQ(code, 0);
}

// Runs only as the child of AggregateInAnotherProcess.
TEST(KernelImage, LoadsAggregateImage)
{
    if (ImagePathArg.empty()) {
        GTEST_SKIP() << "needs --kernel-image";
    }
    std::ifstream in(ImagePathArg, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(LoadAndRunAggregate(bytes), ExpectedSums);
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    Argv0 = argv[0];
    const std::string flag = "--kernel-image=";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg.rfind(flag, 0) == 0) {
            ImagePathArg = arg.substr(flag.size());
        }
    }
    NQumir::NCodeGen::TLLVMInitializer llvmInit;
    return RUN_ALL_TESTS();
}
