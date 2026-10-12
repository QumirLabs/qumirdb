#include <qdb/plan/passes/const_fold.h>
#include <qdb/modules/qumirdb.h>
#include <qdb/plan/types/decimal.h>

#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>
#include <qumir/parser/core/printer.h>
#include <qumir/parser/ast.h>

#include <gtest/gtest.h>

#include <sstream>
#include <stdexcept>
#include <string>

using namespace NQumir::NAst;
using namespace NQumir::NAst::NLiterals;

namespace {

// ConstFold takes untyped expressions (it types closed ones itself), so no
// annotation here - parse and hand the raw AST straight to ConstFold.
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

int64_t FoldToInt(const std::string& src) {
    auto folded = NQdb::ConstFold(Parse(src));
    auto number = TMaybeNode<TNumberExpr>(folded);
    EXPECT_TRUE(number) << "not folded to a number: " << NCore::PrintAst(folded);
    return number ? number.Cast()->IntValue : -999;
}

} // namespace

TEST(ConstFold, FoldsArithmetic) {
    EXPECT_EQ(FoldToInt("(+ 1 (* 2 3))"), 7);
    EXPECT_EQ(FoldToInt("(- (* 4 5) 2)"), 18);
}

TEST(ConstFold, FoldsUnary) {
    EXPECT_EQ(FoldToInt("(- (+ 2 3))"), -5);
    EXPECT_NE(FoldToInt("(! (== 2 3))"), 0); // not false -> true
    EXPECT_EQ(FoldToInt("(! (== 2 2))"), 0); // not true -> false
}

TEST(ConstFold, FoldsNumericComparison) {
    EXPECT_NE(FoldToInt("(== 2 2)"), 0);
    EXPECT_EQ(FoldToInt("(== 2 3)"), 0);
    EXPECT_NE(FoldToInt("(> 5 3)"), 0);
    EXPECT_EQ(FoldToInt("(< 5 3)"), 0);
}

TEST(ConstFold, FoldsStringViewComparison) {
    EXPECT_NE(FoldToInt("(== (cast \"s\" StringView) (cast \"s\" StringView))"), 0);
    EXPECT_EQ(FoldToInt("(== (cast \"s\" StringView) (cast \"c\" StringView))"), 0);
}

TEST(ConstFold, FoldsStringViewLength) {
    EXPECT_EQ(FoldToInt("(call strlen (cast \"hello\" StringView))"), 5);
}

TEST(ConstFold, FoldsDecimalComparison) {
    NQumir::TLocation loc{};
    auto decLit = [&](int64_t v) -> TExprPtr {
        return std::make_shared<TCastExpr>(
            loc, std::make_shared<TNumberExpr>(loc, v),
            std::make_shared<NQdb::TDecimal>(10, 2));
    };
    auto eq = [&](int64_t l, int64_t r) {
        auto cmp = std::make_shared<TBinaryExpr>(loc, "=="_op, decLit(l), decLit(r));
        auto folded = NQdb::ConstFold(cmp);
        auto number = TMaybeNode<TNumberExpr>(folded);
        EXPECT_TRUE(number) << "not folded: " << NCore::PrintAst(folded);
        return number ? number.Cast()->IntValue : -999;
    };
    EXPECT_NE(eq(5, 5), 0);
    EXPECT_EQ(eq(5, 7), 0);
}

TEST(ConstFold, FoldsNullableIsNull) {
    EXPECT_EQ(FoldToInt("(call qdb_is_null (call nullable_from_value 5))"), 0);
}

TEST(ConstFold, LeavesOpenExpressionUntouched) {
    auto expr = Parse("(+ a 1)"); // free variable `a` -> not closed, left as-is
    auto folded = NQdb::ConstFold(expr);
    EXPECT_EQ(folded, expr);
    EXPECT_TRUE(TMaybeNode<TBinaryExpr>(folded));
}

int main(int argc, char** argv) {
    NQumir::NRegistry::EnsureQumirDbRuntimeSymbolsLinked();
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
