#include <gtest/gtest.h>

#include <qdb/kernel/annotate_type.h>
#include <qdb/plan/types/decimal.h>
#include <qdb/plan/types/nullable.h>

#include <qumir/error.h>
#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>
#include <qumir/parser/core/printer.h>
#include <qumir/parser/type.h>

#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace NQdb::NKernel;
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

TStructType Schema() {
    return TStructType({
        {"q", std::make_shared<TFloatType>()},
        {"p", std::make_shared<TFloatType>()},
        {"d", std::make_shared<TFloatType>()},
        {"k", std::make_shared<TIntegerType>(TIntegerType::I32)},
        {"s", std::make_shared<TStringType>()},
    });
}

TStructType NullableSchema() {
    auto i32 = std::make_shared<TIntegerType>(TIntegerType::I32);
    auto boolean = std::make_shared<TBoolType>();
    return TStructType({
        {"a", std::make_shared<NQdb::TNullable>(i32)},
        {"b", std::make_shared<NQdb::TNullable>(i32)},
        {"flag", std::make_shared<NQdb::TNullable>(boolean)},
    });
}

bool IsF64(const TTypePtr& t) { return static_cast<bool>(TMaybeType<TFloatType>(t)); }
bool IsInt(const TTypePtr& t) { return static_cast<bool>(TMaybeType<TIntegerType>(t)); }
bool IsBool(const TTypePtr& t) { return static_cast<bool>(TMaybeType<TBoolType>(t)); }
bool IsNullable(const TTypePtr& t) { return NQdb::IsNullableType(t); }
bool IsDecimal(const TTypePtr& t, int32_t precision, int32_t scale) {
    auto spec = NQdb::DecimalSpecOf(t);
    return spec && spec->Precision == precision && spec->Scale == scale;
}

TTypePtr NullableInner(const TTypePtr& t) {
    auto nullable = TMaybeType<NQdb::TNullable>(t).Cast();
    return nullable ? nullable->UnderlyingType : nullptr;
}

bool IsI32(const TTypePtr& t) {
    auto integer = TMaybeType<TIntegerType>(t).Cast();
    return integer && integer->Kind == TIntegerType::I32;
}

size_t CountOccurrences(std::string_view text, std::string_view needle) {
    size_t count = 0;
    for (size_t pos = 0; (pos = text.find(needle, pos)) != std::string_view::npos;
         pos += needle.size()) {
        ++count;
    }
    return count;
}

} // namespace

TEST(ProjectType, ColumnRef) {
    auto sc = Schema();
    EXPECT_TRUE(IsF64(AnnotateExprType(Parse("p"), sc)));
    EXPECT_TRUE(IsInt(AnnotateExprType(Parse("k"), sc)));
}

TEST(ProjectType, ArithmeticPromotesToF64) {
    auto sc = Schema();
    // p * (1 - d) -> f64 (Q1's disc_price shape).
    EXPECT_TRUE(IsF64(AnnotateExprType(Parse("(* p (- 1 d))"), sc)));
    // p * (1 - d) * (1 + q) -> f64 (charge shape).
    EXPECT_TRUE(IsF64(AnnotateExprType(Parse("(* (* p (- 1 d)) (+ 1 q))"), sc)));
}

TEST(ProjectType, PureIntArithmeticIsI64) {
    EXPECT_TRUE(IsInt(AnnotateExprType(Parse("(+ 1 2)"), Schema())));
    EXPECT_TRUE(IsInt(AnnotateExprType(Parse("(* k 3)"), Schema())));
}

TEST(ProjectType, DivisionForAvgIsF64) {
    // avg = sum(f64) / count(i64) -> f64.
    EXPECT_TRUE(IsF64(AnnotateExprType(Parse("(/ p k)"), Schema())));
}

TEST(ProjectType, ZeroDivisorMakesExactDivisionNullable) {
    TStructType sc({
        {"k", std::make_shared<TIntegerType>(TIntegerType::I32)},
        {"amount", std::make_shared<NQdb::TDecimal>(7, 2)},
        {"qty", std::make_shared<TIntegerType>()},
        {"p", std::make_shared<TFloatType>()},
    });
    for (const char* src : {"(% k k)", "(% k 0)", "(/ amount qty)", "(/ amount amount)"}) {
        EXPECT_TRUE(IsNullable(AnnotateExprType(Parse(src), sc))) << src;
    }
    // A non-zero literal divisor cannot make the result NULL, and float division
    // has NaN and infinity for a zero divisor.
    EXPECT_TRUE(IsI32(AnnotateExprType(Parse("(% k 3)"), sc)));
    EXPECT_TRUE(IsDecimal(AnnotateExprType(Parse("(/ amount 2)"), sc), 7, 2));
    EXPECT_FALSE(IsNullable(AnnotateExprType(Parse("(/ amount 2)"), sc)));
    EXPECT_TRUE(IsF64(AnnotateExprType(Parse("(/ k k)"), sc)));
    EXPECT_TRUE(IsF64(AnnotateExprType(Parse("(% p p)"), sc)));
}

// The fast path returns the expression itself. Only a division that Expand guards
// with NULLIF leaves it; float division by a column has no such rewrite.
TEST(ProjectType, OnlyGuardedDivisionLeavesExpandFastPath) {
    TStructType sc({
        {"k", std::make_shared<TIntegerType>(TIntegerType::I32)},
        {"p", std::make_shared<TFloatType>()},
    });
    for (const char* src : {"(/ p p)", "(/ k k)", "(% p p)"}) {
        auto input = Parse(src);
        EXPECT_EQ(ExpandNullable(input, sc).first, input) << src;
    }
    auto input = Parse("(% k k)");
    EXPECT_NE(ExpandNullable(input, sc).first, input);
}

TEST(ProjectType, ComparisonIsBool) {
    EXPECT_TRUE(IsBool(AnnotateExprType(Parse("(< p 5.0)"), Schema())));
}

TEST(ProjectType, CastTargetType) {
    EXPECT_TRUE(IsF64(AnnotateExprType(Parse("(cast k f64)"), Schema())));
}

TEST(ProjectType, NullableAdditionAnnotates) {
    auto type = AnnotateExprType(Parse("(+ a b)"), NullableSchema());
    ASSERT_TRUE(IsNullable(type));
    EXPECT_TRUE(IsI32(NullableInner(type)));
}

TEST(ProjectType, NullableAdditionWithLiteralAnnotates) {
    auto type = AnnotateExprType(Parse("(+ a 1)"), NullableSchema());
    ASSERT_TRUE(IsNullable(type));
    EXPECT_TRUE(IsI32(NullableInner(type)));
}

TEST(ProjectType, RemainderKeepsOperandWidth) {
    for (const char* src : {"(% a b)", "(% a 2)", "(% 7 a)"}) {
        auto type = AnnotateExprType(Parse(src), NullableSchema());
        ASSERT_TRUE(IsNullable(type)) << src;
        EXPECT_TRUE(IsI32(NullableInner(type))) << src;
    }
    EXPECT_TRUE(IsI32(AnnotateExprType(Parse("(% k 3)"), Schema())));
}

TEST(ProjectType, NullableDivisionAnnotates) {
    auto type = AnnotateExprType(Parse("(/ a b)"), NullableSchema());
    ASSERT_TRUE(IsNullable(type));
    EXPECT_TRUE(IsF64(NullableInner(type)));
}

TEST(ProjectType, NullableBoolMixedOverloadAnnotates) {
    auto type = AnnotateExprType(Parse("(&& flag #t)"), NullableSchema());
    ASSERT_TRUE(IsNullable(type));
    EXPECT_TRUE(IsBool(NullableInner(type)));
}

TEST(ProjectType, NullableIsNullAnnotates) {
    EXPECT_TRUE(IsBool(AnnotateExprType(Parse("(call qdb_is_null a)"), NullableSchema())));
}

TEST(ProjectType, NullPredicateIsNullableBool) {
    for (const char* src : {"(call qdb_sql_null)", "(! (call qdb_sql_null))", "(cast (call qdb_sql_null) bool)"}) {
        auto [expr, type] = ExpandKernelPredicate(Parse(src), Schema());
        ASSERT_TRUE(IsNullable(type)) << src;
        EXPECT_TRUE(IsBool(NullableInner(type))) << src;
        EXPECT_TRUE(TMaybeNode<TCastExpr>(expr)) << src;
    }
}

TEST(ProjectType, InListResultTracksOperandNullability) {
    auto nullable = AnnotateExprType(
        Parse("(call qdb_in_list a 1 2 3)"), NullableSchema());
    ASSERT_TRUE(IsNullable(nullable));
    EXPECT_TRUE(IsBool(NullableInner(nullable)));

    auto plain = AnnotateExprType(
        Parse("(call qdb_in_list k 1 2 3)"), Schema());
    EXPECT_TRUE(IsBool(plain));
    EXPECT_FALSE(IsNullable(plain));
}

TEST(ProjectType, InListExpansionUsesOneNullableLhsGuard) {
    auto [expr, type] = ExpandNullable(
        Parse("(call qdb_in_list a 1 2 3)"), NullableSchema());

    ASSERT_TRUE(IsNullable(type));
    EXPECT_TRUE(IsBool(NullableInner(type)));
    const std::string printed = NQumir::NAst::NCore::PrintAst(expr);
    EXPECT_EQ(printed.find("qdb_in_list"), std::string::npos);
    EXPECT_EQ(CountOccurrences(printed, "qdb_sql_null"), 1u);
    EXPECT_EQ(CountOccurrences(printed, "(field __nx0 Valid)"), 1u);
    EXPECT_EQ(CountOccurrences(printed, "(||"), 2u);
}

TEST(ProjectType, DecimalArithmeticAnnotates) {
    TStructType sc({
        {"amount", std::make_shared<NQdb::TDecimal>(7, 2)},
        {"tax", std::make_shared<NQdb::TDecimal>(6, 3)},
        {"sales", std::make_shared<NQdb::TDecimal>(17, 2)},
        {"prev", std::make_shared<NQdb::TDecimal>(17, 2)},
        {"wide", std::make_shared<NQdb::TDecimal>(38, 10)},
        {"wide2", std::make_shared<NQdb::TDecimal>(38, 10)},
        {"qty", std::make_shared<TIntegerType>()},
    });

    EXPECT_TRUE(IsDecimal(AnnotateExprType(Parse("(+ amount tax)"), sc), 9, 3));
    EXPECT_TRUE(IsDecimal(AnnotateExprType(Parse("(+ amount 1)"), sc), 8, 2));
    EXPECT_TRUE(IsDecimal(AnnotateExprType(Parse("(* amount qty)"), sc), 8, 2));
    EXPECT_TRUE(IsDecimal(AnnotateExprType(Parse("(/ amount qty)"), sc), 7, 2));
    EXPECT_TRUE(IsDecimal(AnnotateExprType(Parse("(/ amount tax)"), sc), 17, 9));
    EXPECT_TRUE(IsDecimal(AnnotateExprType(Parse("(/ sales prev)"), sc), 37, 20));
    EXPECT_TRUE(IsDecimal(AnnotateExprType(Parse("(/ wide wide2)"), sc), 38, 0));
    EXPECT_TRUE(IsBool(AnnotateExprType(Parse("(< amount tax)"), sc)));
    EXPECT_THROW(AnnotateExprType(Parse("(* amount tax)"), sc), NQumir::TError);
}

TEST(ProjectType, ExpandKernelExprErasesDecimalCasts) {
    TStructType sc({
        {"amount", std::make_shared<NQdb::TDecimal>(7, 2)},
    });

    auto [expr, type] = ExpandKernelExpr(Parse("(+ amount 1)"), sc);

    EXPECT_TRUE(IsDecimal(type, 8, 2));
    const std::string printed = NQumir::NAst::NCore::PrintAst(expr);
    EXPECT_NE(printed.find("qdb_decimal_from_i64"), std::string::npos);
    EXPECT_EQ(printed.find("DECIMAL"), std::string::npos);
}

TEST(ProjectType, ExpandKernelExprPreservesNullableDecimalComparisonWithFloat) {
    auto i64 = std::make_shared<TIntegerType>();
    TStructType sc({
        {"a", std::make_shared<NQdb::TNullable>(i64)},
        {"b", std::make_shared<NQdb::TNullable>(i64)},
    });

    auto [expr, type] = ExpandKernelExpr(Parse(
        "(< (/ (cast a <named DECIMAL [17 2]>) "
        "      (cast b <named DECIMAL [17 2]>)) 0.9)"), sc);

    ASSERT_TRUE(IsNullable(type));
    EXPECT_TRUE(IsBool(NullableInner(type)));
    const std::string printed = NQumir::NAst::NCore::PrintAst(expr);
    EXPECT_NE(printed.find("qdb_decimal_div"), std::string::npos);
    EXPECT_NE(printed.find("qdb_decimal_from_f64"), std::string::npos);
    EXPECT_EQ(printed.find("DECIMAL"), std::string::npos);
}

TEST(ProjectType, ExpandNullableWrapsPlainSiblingOfNullableBranch) {
    TStructType sc({
        {"p", std::make_shared<TFloatType>()},
        {"maybe_p", std::make_shared<NQdb::TNullable>(
            std::make_shared<TFloatType>())},
    });

    auto [expr, type] = ExpandNullable(
        Parse("(if (< p (: 10.0 f64)) p maybe_p)"), sc);

    ASSERT_TRUE(IsNullable(type));
    EXPECT_TRUE(IsF64(NullableInner(type)));
}

TEST(ProjectType, RejectsUnknownColumn) {
    EXPECT_THROW(AnnotateExprType(Parse("missing"), Schema()), NQumir::TError);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
