#include <qdb/plan/passes/const_fold.h>
#include <qdb/kernel/annotate_type.h>
#include <qdb/kernel/vm_function.h>
#include <qdb/modules/qumirdb.h>
#include <qdb/plan/types/decimal.h>
#include <qdb/utils/module_path.h>

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

// TConstFolder takes untyped expressions (it types closed ones itself), so no
// annotation here - parse and hand the raw AST straight to it.
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

TExprPtr Fold(TExprPtr expr) {
    NQdb::TConstFolder folder;
    return folder.Fold(std::move(expr));
}

int64_t FoldToInt(const std::string& src) {
    auto folded = Fold(Parse(src));
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
        auto folded = Fold(cmp);
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

TEST(ConstFold, LeavesNamedTypesOfTheArgumentUnresolved) {
    auto expr = Parse("(call strlen (cast \"hello\" StringView))");
    const std::string before = NCore::PrintAst(expr);
    Fold(expr);
    EXPECT_EQ(NCore::PrintAst(expr), before);
}

TEST(ConstFold, LeavesExpressionItCannotTypeUnfolded) {
    // The kernel expander rejects a decimal product.
    NQumir::TLocation loc{};
    auto decLit = [&](int64_t v) -> TExprPtr {
        return std::make_shared<TCastExpr>(
            loc, std::make_shared<TNumberExpr>(loc, v),
            std::make_shared<NQdb::TDecimal>(10, 2));
    };
    auto product = std::make_shared<TBinaryExpr>(loc, "*"_op, decLit(5), decLit(2));
    EXPECT_EQ(Fold(product), product);
}

// Arithmetic over Qumir's own types compiles without the qumirdb module; the
// fold must give what the module's compilation gives.
TEST(ConstFold, BuiltinArithmeticFoldsAsWithTheModule) {
    NQdb::TVmFrontendContext module({NQdb::NUtils::ModuleFile("qumirdb.oz")});
    for (const char* src : {
        "(+ 1212 11)", "(// 7 2)", "(% -7 2)", "(* 9223372036854775807 2)",
        "(cast -3.7 i32)", "(- (cast 5 u8) (cast 6 u8))", "(cast (cast 300 i32) i8)",
        "(+ (cast 9131 i32) (cast 90 i32))", "(/ 7 2)", "(+ 0.1 0.2)", "(< 3 4.5)"})
    {
        auto [expanded, type] = NQdb::NKernel::ExpandKernelExpr(Parse(src), TStructType({}));
        NQumir::TLocation loc{};
        auto compiled = NQdb::TVmFunction::Compile(module, {
            .NamePrefix = "__module_eval__",
            .Params = {},
            .Body = std::make_shared<TBlockExpr>(loc, std::vector<TExprPtr>{expanded}),
            .ReturnType = type,
            .Imports = {"qumirdb"},
        });
        ASSERT_TRUE(compiled) << src << ": " << compiled.error();
        auto expected = (*compiled)->Eval({});
        ASSERT_TRUE(expected) << src;
        auto folded = TMaybeNode<TNumberExpr>(Fold(Parse(src)));
        ASSERT_TRUE(folded) << src;
        const auto& number = *folded.Cast();
        if (number.IsFloat()) {
            EXPECT_DOUBLE_EQ(number.FloatValue, std::stod(*expected)) << src;
        } else if (*expected == "true" || *expected == "false") {
            EXPECT_EQ(number.IntValue != 0, *expected == "true") << src;
        } else {
            EXPECT_EQ(number.IntValue, std::stoll(*expected)) << src;
        }
    }
}

// SQL `%` on doubles is the qumirdb module's operator; Qumir takes integers
// only, so the fold compiles it with the module.
TEST(ConstFold, FoldsDoubleRemainderWithTheModule) {
    auto folded = TMaybeNode<TNumberExpr>(Fold(Parse("(% 7.5 2.0)")));
    ASSERT_TRUE(folded);
    EXPECT_DOUBLE_EQ(folded.Cast()->FloatValue, 1.5);
}

// A REAL result stays a REAL literal with its exact value: the text form would
// make it a DOUBLE rounded to 15 decimals.
TEST(ConstFold, KeepsRealResultAsRealLiteral) {
    auto folded = TMaybeNode<TNumberExpr>(Fold(Parse("(+ (cast 0.1 f32) (cast 0.2 f32))")));
    ASSERT_TRUE(folded);
    auto type = TMaybeType<TFloatType>(folded.Cast()->Type);
    ASSERT_TRUE(type);
    EXPECT_EQ(type.Cast()->Kind, TFloatType::F32);
    EXPECT_EQ(folded.Cast()->FloatValue, static_cast<double>(0.1f + 0.2f));
}

// The estimates of a query read the same constants for every plan the join
// order search considers; the query's folder folds each of them once.
TEST(ConstFold, FoldsEachNumberOnce) {
    NQdb::TConstFolder folder;
    auto first = folder.FoldNumber(Parse("(+ 1212 11)"));
    ASSERT_TRUE(first);
    EXPECT_EQ(first->IntValue, 1223);
    EXPECT_EQ(folder.FoldNumber(Parse("(+ 1212 11)")), first);
    EXPECT_EQ(folder.FoldNumber(Parse("(+ a 1)")), nullptr);
}

TEST(ConstFold, LeavesOpenExpressionUntouched) {
    auto expr = Parse("(+ a 1)"); // free variable `a` -> not closed, left as-is
    auto folded = Fold(expr);
    EXPECT_EQ(folded, expr);
    EXPECT_TRUE(TMaybeNode<TBinaryExpr>(folded));
}

int main(int argc, char** argv) {
    NQumir::NRegistry::EnsureQumirDbRuntimeSymbolsLinked();
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
