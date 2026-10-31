#include <gtest/gtest.h>

#include <qdb/io/io.h>
#include <qdb/exec/runtime_context.h>
#include <qdb/kernel/compiler.h>
#include <qdb/kernel/annotate_type.h>
#include <qdb/kernel/spec.h>
#include <qdb/modules/qumirdb_runtime.h>
#include <qdb/modules/qumirdb_types.h>
#include <qdb/utils/regex.h>

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>
#include <qumir/parser/type.h>

#include <array>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace NQdb;
using namespace NQumir::NAst;

namespace {

TExprPtr Parse(const std::string& src) {
    std::istringstream in(src);
    NCore::TTokenStream tokens(in);
    NCore::TParser parser;
    auto parsed = parser.Parse(tokens);
    if (!parsed) {
        throw std::runtime_error(parsed.error().ToString());
    }
    return *parsed;
}

} // namespace

TEST(CompileProject, ComputesF64AndI64Columns) {
    NQdb::TThreadExecutionContext context;
    // Input: p (f64), d (f64), k (i64).
    TStructType inputType({
        {"p", std::make_shared<TFloatType>()},
        {"d", std::make_shared<TFloatType>()},
        {"k", std::make_shared<TIntegerType>(TIntegerType::I64)},
    });

    // Computed: disc = p * (1.0 - d)  [f64];  bumped = k + 100  [i64].
    std::vector<TExprPtr> exprs = {Parse("(* p (- 1.0 d))"), Parse("(+ k 100)")};
    std::vector<TTypePtr> types;
    for (auto& e : exprs) {
        types.push_back(NKernel::AnnotateExprType(e, inputType));
    }
    ASSERT_TRUE(TMaybeType<TFloatType>(types[0]));
    ASSERT_TRUE(TMaybeType<TIntegerType>(types[1]));

    TKernelCompiler compiler;
    auto dispatch = compiler.CompileProject(
        NKernel::BuildProjectKernelSpec(inputType, exprs, types));

    std::array<double, 3> p = {100.0, 200.0, 50.0};
    std::array<double, 3> d = {0.1, 0.05, 0.2};
    std::array<int64_t, 3> k = {5, 6, 7};
    std::array<TColumn, 3> cols = {
        TColumn{.Data = reinterpret_cast<char*>(p.data())},
        TColumn{.Data = reinterpret_cast<char*>(d.data())},
        TColumn{.Data = reinterpret_cast<char*>(k.data())},
    };
    TRowSet batch{.Columns = cols.data(), .ColumnCount = 3, .RowCount = 3, .RefCount = 1};

    std::array<double, 3> outDisc{};
    std::array<int64_t, 3> outBumped{};
    std::array<void*, 2> outBuffers = {outDisc.data(), outBumped.data()};
    dispatch(context, &batch, outBuffers.data(), nullptr);

    for (int i = 0; i < 3; ++i) {
        EXPECT_DOUBLE_EQ(outDisc[i], p[i] * (1.0 - d[i]));
        EXPECT_EQ(outBumped[i], k[i] + 100);
    }
}

TEST(CompileProject, CompilesFromKernelSpec) {
    NQdb::TThreadExecutionContext context;
    TStructType inputType({
        {"value", std::make_shared<TIntegerType>(TIntegerType::I64)},
    });
    std::vector<TExprPtr> exprs = {Parse("(+ value 10)")};
    std::vector<TTypePtr> types = {
        NKernel::AnnotateExprType(exprs[0], inputType),
    };
    auto spec = NKernel::BuildProjectKernelSpec(inputType, exprs, types);
    auto dispatch = TKernelCompiler().CompileProject(spec);

    std::array<int64_t, 3> values = {1, 2, 3};
    std::array<TColumn, 1> cols = {
        TColumn{.Data = reinterpret_cast<char*>(values.data())},
    };
    TRowSet batch{.Columns = cols.data(), .ColumnCount = 1, .RowCount = 3, .RefCount = 1};
    std::array<int64_t, 3> output{};
    std::array<void*, 1> outBuffers = {output.data()};

    dispatch(context, &batch, outBuffers.data(), nullptr);

    EXPECT_EQ(output, (std::array<int64_t, 3>{11, 12, 13}));
}

TEST(CompileProject, CastsIntegerIfBranchesBeforeLoweringPhi) {
    NQdb::TThreadExecutionContext context;
    TStructType inputType({
        {"d", std::make_shared<TIntegerType>(TIntegerType::I32)},
        {"qty", std::make_shared<TIntegerType>(TIntegerType::I32)},
    });
    std::vector<TExprPtr> exprs = {
        Parse("(if (< d (: 10 i32)) qty 0)"),
    };
    std::vector<TTypePtr> types = {
        NKernel::AnnotateExprType(exprs[0], inputType),
    };
    auto spec = NKernel::BuildProjectKernelSpec(inputType, exprs, types);
    auto dispatch = TKernelCompiler().CompileProject(spec);

    std::array<int32_t, 3> d = {5, 10, 3};
    std::array<int32_t, 3> qty = {11, 22, 33};
    std::array<TColumn, 2> cols = {
        TColumn{.Data = reinterpret_cast<char*>(d.data())},
        TColumn{.Data = reinterpret_cast<char*>(qty.data())},
    };
    TRowSet batch{.Columns = cols.data(), .ColumnCount = 2, .RowCount = 3, .RefCount = 1};
    // The native annotator unifies the branches (i32 qty, i64 literal 0) to i64.
    std::array<int64_t, 3> output{};
    std::array<void*, 1> outBuffers = {output.data()};

    dispatch(context, &batch, outBuffers.data(), nullptr);

    EXPECT_EQ(output, (std::array<int64_t, 3>{11, 0, 33}));
}

TEST(CompileProject, RegexpReplaceUsesCompiledQueryHandle) {
    NQdb::TThreadExecutionContext context;
    TStructType inputType({
        {"url", std::make_shared<TStringType>()},
    });
    std::vector<TExprPtr> exprs = {Parse(
        "(call regexp_replace url "
        "(cast \"^https?://(?:www\\\\.)?([^/]+)/.*$\" StringView) "
        "(cast \"\\\\1\" StringView))")};
    ASSERT_TRUE(TMaybeType<TStringType>(
        NKernel::AnnotateExprType(exprs[0], inputType)));
    std::vector<TTypePtr> types = {
        std::make_shared<TNamedType>("StringView", nullptr),
    };
    auto runtime = std::make_shared<TRuntimeContext>();
    auto spec = NKernel::BuildProjectKernelSpec(
        inputType, exprs, types, "<project>", nullptr,
        runtime.get());
    auto dispatch = TKernelCompiler(TKernelCompilerOptions{
        .RuntimeContext = runtime,
    }).CompileProject(spec);

    const std::array<std::string_view, 3> values = {
        "https://www.example.com/a",
        "http://openai.com/x",
        "not-a-url",
    };
    std::string data;
    std::array<int64_t, 4> offsets{};
    for (size_t i = 0; i < values.size(); ++i) {
        data += values[i];
        offsets[i + 1] = static_cast<int64_t>(data.size());
    }
    std::array<TColumn, 1> columns = {
        TColumn{
            .Data = data.data(),
            .Offsets = offsets.data(),
            .OffsetWidth = 8,
        },
    };
    TRowSet batch{
        .Columns = columns.data(),
        .ColumnCount = 1,
        .RowCount = static_cast<int64_t>(values.size()),
        .RefCount = 1,
    };
    std::array<TStringView, 3> output{};
    std::array<void*, 1> outBuffers = {output.data()};
    TStringArena arena;
    dispatch(context, &batch, outBuffers.data(), &arena);

    auto text = [&](size_t i) {
        return std::string_view(
            reinterpret_cast<const char*>(output[i].Data),
            static_cast<size_t>(output[i].Size));
    };
    EXPECT_EQ(text(0), "example.com");
    EXPECT_EQ(text(1), "openai.com");
    EXPECT_EQ(text(2), "not-a-url");
    EXPECT_EQ(runtime->Regexes.Specs().size(), 1u);
}

TEST(CompileProject, PcreUsesExplicitMatchContexts) {
    NUtils::TRegex regex("(a)(b)?");
    NUtils::TRegexContext first;
    NUtils::TRegexContext second;
    auto ab = regex.Search(first, "xab");
    auto a = regex.Search(second, "ya");
    ASSERT_TRUE(ab);
    ASSERT_TRUE(a);
    EXPECT_EQ(ab->Capture(2)->End - ab->Capture(2)->Begin, 1u);
    EXPECT_FALSE(a->Capture(2));
    EXPECT_THROW(NUtils::TRegex("("), NUtils::TRegexError);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    NQumir::NCodeGen::TLLVMInitializer initializer;
    return RUN_ALL_TESTS();
}
