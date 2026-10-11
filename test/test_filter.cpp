#include <gtest/gtest.h>

#include <qdb/io/io.h>
#include <qdb/kernel/compiler.h>
#include <qdb/kernel/spec.h>
#include <qdb/plan/types/nullable.h>

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>
#include <qumir/parser/core/printer.h>
#include <qumir/parser/type.h>

#include <array>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace NQdb;
using namespace NQumir::NAst;

TExprPtr ParsePredicate(const std::string& source) {
    std::istringstream input(source);
    NCore::TTokenStream tokens(input);
    NCore::TParser parser;
    auto parsed = parser.Parse(tokens);
    if (!parsed) {
        throw std::runtime_error(parsed.error().ToString());
    }
    return std::move(*parsed);
}

std::array<uint8_t, 4> RunStringLiteralFilter(
    const std::string& data,
    const std::array<int32_t, 5>& offsets,
    const std::string& op,
    const std::string& literal,
    bool literalFirst,
    bool castLiteral = false)
{
    std::array<TColumn, 1> columns = {
        TColumn{.Data = const_cast<char*>(data.data()), .Mask = nullptr,
            .Offsets = const_cast<int32_t*>(offsets.data()), .OffsetWidth = 4},
    };
    TStructType inputType({
        {"value", std::make_shared<TStringType>()},
    });
    auto predicate = ParsePredicate(literalFirst
        ? "(" + op + " \"placeholder\" value)"
        : "(" + op + " value \"placeholder\")");
    auto binary = TMaybeNode<TBinaryExpr>(predicate).Cast();
    auto literalExpr = TMaybeNode<TStringLiteralExpr>(
        literalFirst ? binary->Left : binary->Right).Cast();
    literalExpr->Value = literal;
    if (castLiteral) {
        auto cast = std::make_shared<TCastExpr>(literalExpr->Location,
            literalExpr, std::make_shared<TStringType>());
        (literalFirst ? binary->Left : binary->Right) = std::move(cast);
    }

    auto dispatch = TKernelCompiler().CompileFilter(
        NKernel::BuildFilterKernelSpec(inputType, predicate));
    std::array<uint8_t, 4> selection{};
    TRowSet rowSet{
        .Columns = columns.data(),
        .ColumnCount = static_cast<int64_t>(columns.size()),
        .RowCount = static_cast<int64_t>(selection.size()),
        .Selection = selection.data(),
        .RefCount = 1,
    };
    dispatch(rowSet);
    return selection;
}

std::array<uint8_t, 6> RunNullableIntegerFilter(
    const std::string& predicateSource)
{
    std::array<int64_t, 6> left = {0, 1, 91, 92, 1, 0};
    std::array<int64_t, 6> right = {81, 82, 0, 1, 1, 0};
    std::array<uint8_t, 1> leftMask = {0b00110011};
    std::array<uint8_t, 1> rightMask = {0b00111100};
    std::array<TColumn, 2> columns = {
        TColumn{.Data = reinterpret_cast<char*>(left.data()),
            .Mask = leftMask.data()},
        TColumn{.Data = reinterpret_cast<char*>(right.data()),
            .Mask = rightMask.data()},
    };
    TStructType inputType({
        {"left", std::make_shared<TNullable>(std::make_shared<TIntegerType>())},
        {"right", std::make_shared<TNullable>(std::make_shared<TIntegerType>())},
    });
    auto dispatch = TKernelCompiler().CompileFilter(
        NKernel::BuildFilterKernelSpec(
            inputType, ParsePredicate(predicateSource)));
    std::array<uint8_t, 6> selection{};
    TRowSet rowSet{
        .Columns = columns.data(),
        .ColumnCount = static_cast<int64_t>(columns.size()),
        .RowCount = static_cast<int64_t>(selection.size()),
        .Selection = selection.data(),
        .RefCount = 1,
    };
    dispatch(rowSet);
    return selection;
}

TEST(FilterKernel, ComparesStringColumnsWithoutMutatingLogicalPredicate) {
    const std::string leftData = "absamez";
    const std::string rightData = "basamey";
    std::array<int32_t, 5> offsets = {0, 1, 2, 6, 7};
    std::array<TColumn, 2> columns = {
        TColumn{.Data = const_cast<char*>(leftData.data()), .Mask = nullptr,
            .Offsets = offsets.data(), .OffsetWidth = 4},
        TColumn{.Data = const_cast<char*>(rightData.data()), .Mask = nullptr,
            .Offsets = offsets.data(), .OffsetWidth = 4},
    };
    TStructType inputType({
        {"left", std::make_shared<TStringType>()},
        {"right", std::make_shared<TStringType>()},
    });
    const std::array<std::pair<const char*, std::array<uint8_t, 4>>, 6> cases = {{
        {"==", {0, 0, 0xff, 0}},
        {"!=", {0xff, 0xff, 0, 0xff}},
        {"<", {0xff, 0, 0, 0}},
        {"<=", {0xff, 0, 0xff, 0}},
        {">", {0, 0xff, 0, 0xff}},
        {">=", {0, 0xff, 0xff, 0xff}},
    }};

    for (const auto& [op, expected] : cases) {
        auto predicate = ParsePredicate(
            "(" + std::string(op) + " left right)");
        const std::string logical = NCore::PrintAst(predicate);
        auto dispatch = TKernelCompiler().CompileFilter(
            NKernel::BuildFilterKernelSpec(inputType, predicate));
        std::array<uint8_t, 4> selection{};
        TRowSet rowSet{
            .Columns = columns.data(),
            .ColumnCount = static_cast<int64_t>(columns.size()),
            .RowCount = static_cast<int64_t>(selection.size()),
            .Selection = selection.data(),
            .RefCount = 1,
        };
        dispatch(rowSet);
        EXPECT_EQ(selection, expected) << op;
        EXPECT_EQ(NCore::PrintAst(predicate), logical) << op;
    }
}

TEST(FilterKernel, ComparesStringColumnAndLiteralInBothOrders) {
    const std::string data = std::string("a") + "same" + "z" + "same";
    const std::array<int32_t, 5> offsets = {0, 1, 5, 6, 10};
    const std::array<std::pair<const char*, std::array<uint8_t, 4>>, 6> cases = {{
        {"==", {0, 0xff, 0, 0xff}},
        {"!=", {0xff, 0, 0xff, 0}},
        {"<", {0xff, 0, 0, 0}},
        {"<=", {0xff, 0xff, 0, 0xff}},
        {">", {0, 0, 0xff, 0}},
        {">=", {0, 0xff, 0xff, 0xff}},
    }};

    for (const auto& [op, expected] : cases) {
        EXPECT_EQ(RunStringLiteralFilter(data, offsets, op, "same", false),
            expected) << op;
        const std::string reversedOp = op == std::string("<") ? ">"
            : op == std::string("<=") ? ">="
            : op == std::string(">") ? "<"
            : op == std::string(">=") ? "<=" : op;
        EXPECT_EQ(RunStringLiteralFilter(
            data, offsets, reversedOp, "same", true), expected) << op;
    }
}

// A StringView literal carries its codegen length instead of a per-row strlen,
// so bytes after an embedded zero take part in the comparison.
TEST(FilterKernel, StringViewLiteralKeepsItsLength) {
    const std::string literal("a\0b", 3);
    const std::string data = "a" + literal + "z" + "a";
    const std::array<int32_t, 5> offsets = {0, 1, 4, 5, 6};
    std::array<TColumn, 1> columns = {
        TColumn{.Data = const_cast<char*>(data.data()), .Mask = nullptr,
            .Offsets = const_cast<int32_t*>(offsets.data()), .OffsetWidth = 4},
    };
    TStructType inputType({
        {"value", std::make_shared<TStringType>()},
    });
    auto predicate = ParsePredicate(
        "(== value (cast \"placeholder\" <named StringView>))");
    auto cast = TMaybeNode<TCastExpr>(
        TMaybeNode<TBinaryExpr>(predicate).Cast()->Right).Cast();
    TMaybeNode<TStringLiteralExpr>(cast->Operand).Cast()->Value = literal;

    std::ostringstream diagnostics;
    TKernelCompiler compiler(&diagnostics);
    auto dispatch = compiler.CompileFilter(
        NKernel::BuildFilterKernelSpec(inputType, predicate));
    std::array<uint8_t, 4> selection{};
    TRowSet rowSet{
        .Columns = columns.data(),
        .ColumnCount = static_cast<int64_t>(columns.size()),
        .RowCount = static_cast<int64_t>(selection.size()),
        .Selection = selection.data(),
        .RefCount = 1,
    };
    dispatch(rowSet);

    EXPECT_EQ(selection, (std::array<uint8_t, 4>{0, 0xff, 0, 0}));
    EXPECT_NE(diagnostics.str().find("qdb_sv_from_ptr"), std::string::npos);
}

TEST(FilterKernel, AppliesSqlThreeValuedLogicToNullableColumns) {
    EXPECT_EQ(RunNullableIntegerFilter(
        "(&& (== left 1) (== right 1))"),
        (std::array<uint8_t, 6>{0, 0, 0, 0, 0xff, 0}));
    EXPECT_EQ(RunNullableIntegerFilter(
        "(|| (== left 1) (== right 1))"),
        (std::array<uint8_t, 6>{0, 0xff, 0, 0xff, 0xff, 0}));
    EXPECT_EQ(RunNullableIntegerFilter("(! (== left 1))"),
        (std::array<uint8_t, 6>{0xff, 0, 0, 0, 0, 0xff}));
}

TEST(FilterKernel, InListHoistsNullableLhsGuard) {
    EXPECT_EQ(RunNullableIntegerFilter(
        "(call qdb_in_list left 1 92)"),
        (std::array<uint8_t, 6>{0, 0xff, 0, 0, 0xff, 0}));
    EXPECT_EQ(RunNullableIntegerFilter(
        "(! (call qdb_in_list left 1 92))"),
        (std::array<uint8_t, 6>{0xff, 0, 0, 0, 0, 0xff}));
}

TEST(FilterKernel, InListPreservesNullableItemSemantics) {
    EXPECT_EQ(RunNullableIntegerFilter(
        "(call qdb_in_list left (+ right 1))"),
        (std::array<uint8_t, 6>{0, 0, 0, 0, 0, 0}));
    EXPECT_EQ(RunNullableIntegerFilter(
        "(! (call qdb_in_list left (+ right 1)))"),
        (std::array<uint8_t, 6>{0, 0, 0, 0, 0xff, 0xff}));
}

TEST(FilterKernel, InListPreservesBareNullItemSemantics) {
    EXPECT_EQ(RunNullableIntegerFilter(
        "(call qdb_in_list left 1 (call qdb_sql_null))"),
        (std::array<uint8_t, 6>{0, 0xff, 0, 0, 0xff, 0}));
    EXPECT_EQ(RunNullableIntegerFilter(
        "(! (call qdb_in_list left 1 (call qdb_sql_null)))"),
        (std::array<uint8_t, 6>{0, 0, 0, 0, 0, 0}));
}

TEST(FilterKernel, CompilesFromKernelSpec) {
    std::array<int64_t, 4> values = {0, 1, 2, 3};
    std::array<TColumn, 1> columns = {
        TColumn{.Data = reinterpret_cast<char*>(values.data())},
    };
    TStructType inputType({
        {"value", std::make_shared<TIntegerType>()},
    });
    auto spec = NKernel::BuildFilterKernelSpec(
        inputType, ParsePredicate("(> value 1)"));
    auto dispatch = TKernelCompiler().CompileFilter(spec);
    std::array<uint8_t, 4> selection{};
    TRowSet rowSet{
        .Columns = columns.data(),
        .ColumnCount = static_cast<int64_t>(columns.size()),
        .RowCount = static_cast<int64_t>(selection.size()),
        .Selection = selection.data(),
        .RefCount = 1,
    };

    dispatch(rowSet);

    EXPECT_EQ(selection, (std::array<uint8_t, 4>{0, 0, 0xff, 0xff}));
}

TEST(FilterKernel, SkipsThreeValuedLogicForNonNullablePredicate) {
    TStructType inputType({
        {"plain", std::make_shared<TIntegerType>()},
        {"optional", std::make_shared<TNullable>(
            std::make_shared<TIntegerType>())},
    });
    std::ostringstream diagnostics;
    TKernelCompiler compiler(&diagnostics);
    auto dispatch = compiler.CompileFilter(NKernel::BuildFilterKernelSpec(
        inputType, ParsePredicate("(== plain 1)")));
    ASSERT_TRUE(dispatch);
    EXPECT_EQ(diagnostics.str().find("qdb_sql_bool_"), std::string::npos);
    EXPECT_EQ(diagnostics.str().find("filter_truth_state"), std::string::npos);
}

} // namespace

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    NQumir::NCodeGen::TLLVMInitializer llvmInit;
    return RUN_ALL_TESTS();
}
