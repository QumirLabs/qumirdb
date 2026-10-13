#include <gtest/gtest.h>

#include <qdb/io/io.h>
#include <qdb/exec/filter_exec.h>
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
#include <limits>
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
    dispatch(rowSet, nullptr);
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
    dispatch(rowSet, nullptr);
    return selection;
}

// A filter over one i64 column "value", run the way the filter operator runs
// it: the batch brings its own selection and leaves with a narrowed one.
class FilterSelection : public testing::Test {
protected:
    // Returns the rows the filter keeps. An empty selection means the batch has
    // none; a non-empty validity bitmap makes the column nullable.
    std::vector<bool> Keep(
        const std::string& predicate,
        std::vector<int64_t> values,
        std::vector<uint8_t> selection,
        std::vector<uint8_t> validity = {})
    {
        TColumn column{
            .Data = reinterpret_cast<char*>(values.data()),
            .Mask = validity.empty() ? nullptr : validity.data(),
        };
        TTypePtr valueType = std::make_shared<TIntegerType>(TIntegerType::I64);
        if (!validity.empty()) {
            valueType = std::make_shared<TNullable>(valueType);
        }
        TStructType inputType({{"value", valueType}});
        auto process = MakeFilterProcess(TKernelCompiler().CompileFilter(
            NKernel::BuildFilterKernelSpec(inputType, ParsePredicate(predicate))));
        TRowSet rowSet{
            .Columns = &column,
            .ColumnCount = 1,
            .RowCount = static_cast<int64_t>(values.size()),
            .Selection = selection.empty() ? nullptr : selection.data(),
            .RefCount = 1,
        };
        TUnaryStreamingKernelState state;
        process(rowSet, state);
        std::vector<bool> kept;
        for (int64_t row = 0; row < rowSet.RowCount; ++row) {
            kept.push_back(rowSet.Selection[row] != 0);
        }
        Release(&rowSet);
        return kept;
    }
};

TEST_F(FilterSelection, SparseSelectionNarrowsToRowsThatPass) {
    EXPECT_EQ(Keep("(> value 0)", {5, -1, 7, 9, -3, 2}, {1, 1, 0, 0xff, 0, 2}),
        (std::vector<bool>{true, false, false, true, false, true}));
}

TEST_F(FilterSelection, EmptySelectionKeepsNothing) {
    EXPECT_EQ(Keep("(> value 0)", {5, -1, 7}, {0, 0, 0}),
        (std::vector<bool>{false, false, false}));
}

TEST_F(FilterSelection, FullSelectionMatchesNoSelection) {
    const std::vector<bool> expected = {true, false, true, false};
    EXPECT_EQ(Keep("(> value 0)", {5, -1, 7, 0}, {}), expected);
    EXPECT_EQ(Keep("(> value 0)", {5, -1, 7, 0}, {1, 0xff, 2, 1}), expected);
}

TEST_F(FilterSelection, NullValuesStayRejectedUnderSelection) {
    // Rows 1 and 4 are NULL; rows 2 and 4 are not selected.
    EXPECT_EQ(Keep("(> value 0)", {5, 6, 7, -1, 8, 9},
            {1, 1, 0, 1, 0, 1}, {0b00101101}),
        (std::vector<bool>{true, false, false, false, false, true}));
    EXPECT_EQ(Keep("(call qdb_is_null value)", {5, 6, 7, -1, 8, 9},
            {1, 1, 0, 1, 0, 1}, {0b00101101}),
        (std::vector<bool>{false, true, false, false, false, false}));
}

// abs fails on the minimum i64. An earlier operator rejected those rows, so
// the filter must not evaluate them.
TEST_F(FilterSelection, DoesNotEvaluateRejectedRows) {
    const int64_t min = std::numeric_limits<int64_t>::min();
    EXPECT_EQ(Keep("(> (call abs value) 2)", {5, min, -7, 1, min},
            {1, 0, 1, 1, 0}),
        (std::vector<bool>{true, false, true, false, false}));
}

// The kernel owns every byte of its output: a rejected row gets 0 whatever
// the buffer held before.
TEST(FilterKernel, ClearsOutputOfRejectedRows) {
    const std::string data = std::string("a") + "same" + "z" + "same";
    const std::array<int32_t, 5> offsets = {0, 1, 5, 6, 10};
    TColumn column{.Data = const_cast<char*>(data.data()), .Mask = nullptr,
        .Offsets = const_cast<int32_t*>(offsets.data()), .OffsetWidth = 4};
    TStructType inputType({
        {"value", std::make_shared<TStringType>()},
    });
    auto dispatch = TKernelCompiler().CompileFilter(
        NKernel::BuildFilterKernelSpec(
            inputType, ParsePredicate("(== value \"same\")")));
    const std::array<uint8_t, 4> inputSelection = {0, 1, 0xff, 0};
    std::array<uint8_t, 4> selection = {7, 7, 7, 7};
    TRowSet rowSet{
        .Columns = &column,
        .ColumnCount = 1,
        .RowCount = static_cast<int64_t>(selection.size()),
        .Selection = selection.data(),
        .RefCount = 1,
    };
    dispatch(rowSet, inputSelection.data());

    EXPECT_EQ(selection[0], 0);
    EXPECT_NE(selection[1], 0);
    EXPECT_EQ(selection[2], 0);
    EXPECT_EQ(selection[3], 0);
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
        dispatch(rowSet, nullptr);
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
    dispatch(rowSet, nullptr);

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

TEST(FilterKernel, PreservesInputSelectionAndPrecomputedHashes) {
    std::array<int64_t, 3> values = {1, 2, -1};
    std::array<uint8_t, 3> inputSelection = {2, 0, 0xff};
    std::array<uint64_t, 3> hashes = {11, 22, 33};
    TColumn column{.Data = reinterpret_cast<char*>(values.data())};
    TStructType inputType({
        {"value", std::make_shared<TIntegerType>(TIntegerType::I64)},
    });
    auto dispatch = TKernelCompiler().CompileFilter(
        NKernel::BuildFilterKernelSpec(inputType, ParsePredicate("(> value 0)")));
    auto process = MakeFilterProcess(std::move(dispatch));
    TRowSet rowSet{
        .Columns = &column,
        .ColumnCount = 1,
        .RowCount = 3,
        .Selection = inputSelection.data(),
        .Hash = hashes.data(),
        .RefCount = 1,
    };
    TUnaryStreamingKernelState state;
    process(rowSet, state);
    EXPECT_EQ((std::array<uint8_t, 3>{
        rowSet.Selection[0], rowSet.Selection[1], rowSet.Selection[2]}),
        (std::array<uint8_t, 3>{0xff, 0, 0}));
    EXPECT_EQ(rowSet.Hash, hashes.data());
    Release(&rowSet);
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

    dispatch(rowSet, nullptr);

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
    ASSERT_NE(dispatch.Slot, nullptr);
    EXPECT_EQ(diagnostics.str().find("qdb_sql_bool_"), std::string::npos);
    EXPECT_EQ(diagnostics.str().find("filter_truth_state"), std::string::npos);
}

} // namespace

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    NQumir::NCodeGen::TLLVMInitializer llvmInit;
    return RUN_ALL_TESTS();
}
