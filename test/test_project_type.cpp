#include <gtest/gtest.h>

#include <qdb/kernel/annotate_type.h>
#include <qdb/plan/types/decimal.h>
#include <qdb/plan/types/nullable.h>

#include <qumir/error.h>
#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>
#include <qumir/parser/core/printer.h>
#include <qumir/parser/type.h>

#include <functional>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>

#include "test_types.h"

using namespace NQdb::NKernel;
using namespace NQumir::NAst;

namespace {

TExprPtr Parse(const std::string& src) {
    std::istringstream in(src);
    NCore::TTokenStream tokens(in);
    NCore::TParser parser(NQdb::TestTypes());
    auto parsed = parser.Parse(tokens);
    if (!parsed) {
        throw std::runtime_error(parsed.error().ToString());
    }
    return *parsed;
}

const TStructType& Schema() {
    return *NQdb::TestTypes().MakeStruct({
        {"q", NQdb::TestTypes().MakeFloat()},
        {"p", NQdb::TestTypes().MakeFloat()},
        {"d", NQdb::TestTypes().MakeFloat()},
        {"k", NQdb::TestTypes().MakeInteger(TIntegerType::I32)},
        {"s", NQdb::TestTypes().Make<TStringType>()},
    });
}

const TStructType& NullableSchema() {
    auto i32 = NQdb::TestTypes().MakeInteger(TIntegerType::I32);
    auto boolean = NQdb::TestTypes().Make<TBoolType>();
    return *NQdb::TestTypes().MakeStruct({
        {"a", NQdb::MakeNullableType(NQdb::TestTypes(), i32)},
        {"b", NQdb::MakeNullableType(NQdb::TestTypes(), i32)},
        {"flag", NQdb::MakeNullableType(NQdb::TestTypes(), boolean)},
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
    return NQdb::IsNullableType(t) ? NQdb::UnwrapNullableType(t) : nullptr;
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
    const auto& sc = Schema();
    EXPECT_TRUE(IsF64(AnnotateExprType(NQdb::TestTypes(), Parse("p"), sc, NQdb::TestAnnotation())));
    EXPECT_TRUE(IsInt(AnnotateExprType(NQdb::TestTypes(), Parse("k"), sc, NQdb::TestAnnotation())));
}

TEST(ProjectType, ArithmeticPromotesToF64) {
    const auto& sc = Schema();
    // p * (1 - d) -> f64 (Q1's disc_price shape).
    EXPECT_TRUE(IsF64(AnnotateExprType(NQdb::TestTypes(), Parse("(* p (- 1 d))"), sc, NQdb::TestAnnotation())));
    // p * (1 - d) * (1 + q) -> f64 (charge shape).
    EXPECT_TRUE(IsF64(AnnotateExprType(NQdb::TestTypes(), Parse("(* (* p (- 1 d)) (+ 1 q))"), sc, NQdb::TestAnnotation())));
}

TEST(ProjectType, PureIntArithmeticIsI64) {
    EXPECT_TRUE(IsInt(AnnotateExprType(NQdb::TestTypes(), Parse("(+ 1 2)"), Schema(), NQdb::TestAnnotation())));
    EXPECT_TRUE(IsInt(AnnotateExprType(NQdb::TestTypes(), Parse("(* k 3)"), Schema(), NQdb::TestAnnotation())));
}

TEST(ProjectType, DivisionForAvgIsF64) {
    // avg = sum(f64) / count(i64) -> f64.
    EXPECT_TRUE(IsF64(AnnotateExprType(NQdb::TestTypes(), Parse("(/ p k)"), Schema(), NQdb::TestAnnotation())));
}

TEST(ProjectType, ZeroDivisorMakesExactDivisionNullable) {
    const auto& sc = *NQdb::TestTypes().MakeStruct({
        {"k", NQdb::TestTypes().MakeInteger(TIntegerType::I32)},
        {"amount", NQdb::MakeDecimalType(NQdb::TestTypes(), {7, 2})},
        {"qty", NQdb::TestTypes().MakeInteger()},
        {"p", NQdb::TestTypes().MakeFloat()},
    });
    for (const char* src : {"(% k k)", "(% k 0)", "(/ amount qty)", "(/ amount amount)"}) {
        EXPECT_TRUE(IsNullable(AnnotateExprType(NQdb::TestTypes(), Parse(src), sc, NQdb::TestAnnotation()))) << src;
    }
    // A non-zero literal divisor cannot make the result NULL, and float division
    // has NaN and infinity for a zero divisor.
    EXPECT_TRUE(IsI32(AnnotateExprType(NQdb::TestTypes(), Parse("(% k 3)"), sc, NQdb::TestAnnotation())));
    EXPECT_TRUE(IsDecimal(AnnotateExprType(NQdb::TestTypes(), Parse("(/ amount 2)"), sc, NQdb::TestAnnotation()), 7, 2));
    EXPECT_FALSE(IsNullable(AnnotateExprType(NQdb::TestTypes(), Parse("(/ amount 2)"), sc, NQdb::TestAnnotation())));
    EXPECT_TRUE(IsF64(AnnotateExprType(NQdb::TestTypes(), Parse("(/ k k)"), sc, NQdb::TestAnnotation())));
    EXPECT_TRUE(IsF64(AnnotateExprType(NQdb::TestTypes(), Parse("(% p p)"), sc, NQdb::TestAnnotation())));
}

// The fast path returns the expression itself. Only a division that Expand guards
// with NULLIF leaves it; float division by a column has no such rewrite.
TEST(ProjectType, OnlyGuardedDivisionLeavesExpandFastPath) {
    const auto& sc = *NQdb::TestTypes().MakeStruct({
        {"k", NQdb::TestTypes().MakeInteger(TIntegerType::I32)},
        {"p", NQdb::TestTypes().MakeFloat()},
    });
    for (const char* src : {"(/ p p)", "(/ k k)", "(% p p)"}) {
        auto input = Parse(src);
        EXPECT_EQ(ExpandNullable(NQdb::TestTypes(), input, sc, NQdb::TestAnnotation()).first, input) << src;
    }
    auto input = Parse("(% k k)");
    EXPECT_NE(ExpandNullable(NQdb::TestTypes(), input, sc, NQdb::TestAnnotation()).first, input);
}

TEST(ProjectType, ComparisonIsBool) {
    EXPECT_TRUE(IsBool(AnnotateExprType(NQdb::TestTypes(), Parse("(< p 5.0)"), Schema(), NQdb::TestAnnotation())));
}

TEST(ProjectType, CastTargetType) {
    EXPECT_TRUE(IsF64(AnnotateExprType(NQdb::TestTypes(), Parse("(cast k f64)"), Schema(), NQdb::TestAnnotation())));
}

TEST(ProjectType, NullableAdditionAnnotates) {
    auto type = AnnotateExprType(NQdb::TestTypes(), Parse("(+ a b)"), NullableSchema(), NQdb::TestAnnotation());
    ASSERT_TRUE(IsNullable(type));
    EXPECT_TRUE(IsI32(NullableInner(type)));
}

TEST(ProjectType, NullableAdditionWithLiteralAnnotates) {
    auto type = AnnotateExprType(NQdb::TestTypes(), Parse("(+ a 1)"), NullableSchema(), NQdb::TestAnnotation());
    ASSERT_TRUE(IsNullable(type));
    EXPECT_TRUE(IsI32(NullableInner(type)));
}

TEST(ProjectType, RemainderKeepsOperandWidth) {
    for (const char* src : {"(% a b)", "(% a 2)", "(% 7 a)"}) {
        auto type = AnnotateExprType(NQdb::TestTypes(), Parse(src), NullableSchema(), NQdb::TestAnnotation());
        ASSERT_TRUE(IsNullable(type)) << src;
        EXPECT_TRUE(IsI32(NullableInner(type))) << src;
    }
    EXPECT_TRUE(IsI32(AnnotateExprType(NQdb::TestTypes(), Parse("(% k 3)"), Schema(), NQdb::TestAnnotation())));
}

TEST(ProjectType, OutOfRangeLiteralWidensOperation) {
    for (const char* src : {"(* a 10000000000)", "(* 10000000000 a)", "(+ a 3000000000)"}) {
        auto type = AnnotateExprType(NQdb::TestTypes(), Parse(src), NullableSchema(), NQdb::TestAnnotation());
        ASSERT_TRUE(IsNullable(type)) << src;
        auto integer = TMaybeType<TIntegerType>(NullableInner(type)).Cast();
        ASSERT_TRUE(integer) << src;
        EXPECT_EQ(integer->Kind, TIntegerType::I64) << src;
    }
    auto required = TMaybeType<TIntegerType>(
        AnnotateExprType(NQdb::TestTypes(), Parse("(* k 10000000000)"), Schema(), NQdb::TestAnnotation())).Cast();
    ASSERT_TRUE(required);
    EXPECT_EQ(required->Kind, TIntegerType::I64);
    EXPECT_TRUE(IsI32(AnnotateExprType(NQdb::TestTypes(), Parse("(* k 2147483647)"), Schema(), NQdb::TestAnnotation())));
}

TEST(ProjectType, NullableDivisionAnnotates) {
    auto type = AnnotateExprType(NQdb::TestTypes(), Parse("(/ a b)"), NullableSchema(), NQdb::TestAnnotation());
    ASSERT_TRUE(IsNullable(type));
    EXPECT_TRUE(IsF64(NullableInner(type)));
}

TEST(ProjectType, NullableBoolMixedOverloadAnnotates) {
    auto type = AnnotateExprType(NQdb::TestTypes(), Parse("(&& flag #t)"), NullableSchema(), NQdb::TestAnnotation());
    ASSERT_TRUE(IsNullable(type));
    EXPECT_TRUE(IsBool(NullableInner(type)));
}

TEST(ProjectType, NullableIsNullAnnotates) {
    EXPECT_TRUE(IsBool(AnnotateExprType(NQdb::TestTypes(), Parse("(call qdb_is_null a)"), NullableSchema(), NQdb::TestAnnotation())));
}

TEST(ProjectType, NullPredicateIsNullableBool) {
    for (const char* src : {"(call qdb_sql_null)", "(! (call qdb_sql_null))", "(cast (call qdb_sql_null) bool)"}) {
        auto [expr, type] = ExpandKernelPredicate(NQdb::TestTypes(), Parse(src), Schema(), NQdb::TestAnnotation());
        ASSERT_TRUE(IsNullable(type)) << src;
        EXPECT_TRUE(IsBool(NullableInner(type))) << src;
        EXPECT_TRUE(TMaybeNode<TCastExpr>(expr)) << src;
    }
}

TEST(ProjectType, UntypedNullValueIsNullableInteger) {
    for (const char* src : {
        "(call qdb_sql_null)",
        "(if (< k 1) (call qdb_sql_null) (call qdb_sql_null))",
        "(call coalesce (call qdb_sql_null) (call qdb_sql_null))",
        "(+ (call qdb_sql_null) (call qdb_sql_null))",
    }) {
        auto annotated = AnnotateExprType(NQdb::TestTypes(), Parse(src), Schema(), NQdb::TestAnnotation());
        ASSERT_TRUE(IsNullable(annotated)) << src;
        EXPECT_TRUE(IsI32(NullableInner(annotated))) << src;
        auto [expr, type] = ExpandKernelValue(NQdb::TestTypes(), Parse(src), Schema(), NQdb::TestAnnotation());
        ASSERT_TRUE(IsNullable(type)) << src;
        EXPECT_TRUE(IsI32(NullableInner(type))) << src;
    }
    EXPECT_THROW(
        AnnotateExprType(NQdb::TestTypes(), Parse("(call missing_fn (call qdb_sql_null))"), Schema(), NQdb::TestAnnotation()),
        NQumir::TError);
}

TEST(ProjectType, NullOperandTakesOtherOperandType) {
    for (const char* src : {"(+ (call qdb_sql_null) k)", "(+ k (call qdb_sql_null))"}) {
        auto type = AnnotateExprType(NQdb::TestTypes(), Parse(src), Schema(), NQdb::TestAnnotation());
        ASSERT_TRUE(IsNullable(type)) << src;
        EXPECT_TRUE(IsI32(NullableInner(type))) << src;
    }
}

TEST(ProjectType, InListResultTracksOperandNullability) {
    auto nullable = AnnotateExprType(NQdb::TestTypes(),
        Parse("(call qdb_in_list a 1 2 3)"), NullableSchema(), NQdb::TestAnnotation());
    ASSERT_TRUE(IsNullable(nullable));
    EXPECT_TRUE(IsBool(NullableInner(nullable)));

    auto plain = AnnotateExprType(NQdb::TestTypes(),
        Parse("(call qdb_in_list k 1 2 3)"), Schema(), NQdb::TestAnnotation());
    EXPECT_TRUE(IsBool(plain));
    EXPECT_FALSE(IsNullable(plain));
}

TEST(ProjectType, InListExpansionUsesOneNullableLhsGuard) {
    auto [expr, type] = ExpandNullable(NQdb::TestTypes(),
        Parse("(call qdb_in_list a 1 2 3)"), NullableSchema(), NQdb::TestAnnotation());

    ASSERT_TRUE(IsNullable(type));
    EXPECT_TRUE(IsBool(NullableInner(type)));
    const std::string printed = NQumir::NAst::NCore::PrintAst(expr);
    EXPECT_EQ(printed.find("qdb_in_list"), std::string::npos);
    EXPECT_EQ(CountOccurrences(printed, "qdb_sql_null"), 1u);
    EXPECT_EQ(CountOccurrences(printed, "(field __nx0 Valid)"), 1u);
    EXPECT_EQ(CountOccurrences(printed, "(||"), 2u);
}

// The constants of a long list are one lookup whatever their number, so the kernel
// compiles in constant time; a NULL and a column item stay beside it.
TEST(ProjectType, LongInListIsOneTableLookup) {
    auto list = [](int constants) {
        std::string src = "(call qdb_in_list a";
        for (int i = 0; i < constants; ++i) {
            src += " " + std::to_string(i * 3);
        }
        return src + " (call qdb_sql_null) b)";
    };
    auto [expr, type] = ExpandNullable(NQdb::TestTypes(), Parse(list(100000)), NullableSchema(), NQdb::TestAnnotation());
    ASSERT_TRUE(IsNullable(type));
    EXPECT_TRUE(IsBool(NullableInner(type)));
    std::string printed = NQumir::NAst::NCore::PrintAst(expr);
    EXPECT_EQ(CountOccurrences(printed, "qdb_in_sorted_i64"), 1u);
    EXPECT_EQ(CountOccurrences(printed, "(=="), 1u);

    std::tie(expr, type) = ExpandNullable(NQdb::TestTypes(), Parse(list(15)), NullableSchema(), NQdb::TestAnnotation());
    printed = NQumir::NAst::NCore::PrintAst(expr);
    EXPECT_EQ(CountOccurrences(printed, "qdb_in_sorted_i64"), 0u);
    EXPECT_EQ(CountOccurrences(printed, "(=="), 16u);
}

TEST(ProjectType, DecimalArithmeticAnnotates) {
    const auto& sc = *NQdb::TestTypes().MakeStruct({
        {"amount", NQdb::MakeDecimalType(NQdb::TestTypes(), {7, 2})},
        {"tax", NQdb::MakeDecimalType(NQdb::TestTypes(), {6, 3})},
        {"sales", NQdb::MakeDecimalType(NQdb::TestTypes(), {17, 2})},
        {"prev", NQdb::MakeDecimalType(NQdb::TestTypes(), {17, 2})},
        {"wide", NQdb::MakeDecimalType(NQdb::TestTypes(), {38, 10})},
        {"wide2", NQdb::MakeDecimalType(NQdb::TestTypes(), {38, 10})},
        {"price", NQdb::MakeDecimalType(NQdb::TestTypes(), {15, 2})},
        {"disc", NQdb::MakeDecimalType(NQdb::TestTypes(), {15, 2})},
        {"frac", NQdb::MakeDecimalType(NQdb::TestTypes(), {25, 20})},
        {"qty", NQdb::TestTypes().MakeInteger()},
    });

    EXPECT_TRUE(IsDecimal(AnnotateExprType(NQdb::TestTypes(), Parse("(+ amount tax)"), sc, NQdb::TestAnnotation()), 9, 3));
    // An integer counts as DECIMAL(w, 0): BIGINT has 19 digits, a literal its own.
    EXPECT_TRUE(IsDecimal(AnnotateExprType(NQdb::TestTypes(), Parse("(+ amount 1)"), sc, NQdb::TestAnnotation()), 8, 2));
    EXPECT_TRUE(IsDecimal(AnnotateExprType(NQdb::TestTypes(), Parse("(+ amount qty)"), sc, NQdb::TestAnnotation()), 22, 2));
    EXPECT_TRUE(IsDecimal(AnnotateExprType(NQdb::TestTypes(), Parse("(* amount qty)"), sc, NQdb::TestAnnotation()), 26, 2));
    EXPECT_TRUE(IsDecimal(AnnotateExprType(NQdb::TestTypes(), Parse("(* amount 100)"), sc, NQdb::TestAnnotation()), 10, 2));
    EXPECT_TRUE(IsDecimal(AnnotateExprType(NQdb::TestTypes(), Parse("(/ amount qty)"), sc, NQdb::TestAnnotation()), 7, 2));
    EXPECT_TRUE(IsDecimal(AnnotateExprType(NQdb::TestTypes(), Parse("(/ amount tax)"), sc, NQdb::TestAnnotation()), 17, 9));
    EXPECT_TRUE(IsDecimal(AnnotateExprType(NQdb::TestTypes(), Parse("(/ sales prev)"), sc, NQdb::TestAnnotation()), 37, 20));
    EXPECT_TRUE(TMaybeType<TFloatType>(AnnotateExprType(NQdb::TestTypes(), Parse("(/ wide wide2)"), sc, NQdb::TestAnnotation())));
    EXPECT_TRUE(IsBool(AnnotateExprType(NQdb::TestTypes(), Parse("(< amount tax)"), sc, NQdb::TestAnnotation())));

    // A product adds precisions and scales; the precision is capped at 38, a scale
    // past 38 has no type. (15,2) * (15,2) keeps its 30 digits (DuckDB: 18).
    EXPECT_TRUE(IsDecimal(AnnotateExprType(NQdb::TestTypes(), Parse("(* amount tax)"), sc, NQdb::TestAnnotation()), 13, 5));
    EXPECT_TRUE(IsDecimal(AnnotateExprType(NQdb::TestTypes(), Parse("(* amount amount)"), sc, NQdb::TestAnnotation()), 14, 4));
    EXPECT_TRUE(IsDecimal(AnnotateExprType(NQdb::TestTypes(), Parse("(* price disc)"), sc, NQdb::TestAnnotation()), 30, 4));
    EXPECT_TRUE(IsDecimal(AnnotateExprType(NQdb::TestTypes(), Parse("(* price (- 1 disc))"), sc, NQdb::TestAnnotation()), 31, 4));
    EXPECT_TRUE(IsDecimal(AnnotateExprType(NQdb::TestTypes(), Parse("(* (* price (- 1 disc)) (+ 1 disc))"), sc, NQdb::TestAnnotation()), 38, 6));
    EXPECT_TRUE(IsDecimal(AnnotateExprType(NQdb::TestTypes(), Parse("(* sales prev)"), sc, NQdb::TestAnnotation()), 34, 4));
    EXPECT_TRUE(IsDecimal(AnnotateExprType(NQdb::TestTypes(), Parse("(* wide tax)"), sc, NQdb::TestAnnotation()), 38, 13));
    EXPECT_TRUE(IsDecimal(AnnotateExprType(NQdb::TestTypes(), Parse("(* wide wide2)"), sc, NQdb::TestAnnotation()), 38, 20));
    EXPECT_TRUE(IsDecimal(AnnotateExprType(NQdb::TestTypes(), Parse("(* price 0.0001)"), sc, NQdb::TestAnnotation()), 19, 6));
    EXPECT_THROW(AnnotateExprType(NQdb::TestTypes(), Parse("(* frac frac)"), sc, NQdb::TestAnnotation()), NQumir::TError);
}

// The unscaled product is already at the result's scale: no scale alignment call. A
// product of at most 18 digits is the i64 `*`; a wider one widens its operands to i128.
TEST(ProjectType, ExpandKernelExprLowersDecimalProduct) {
    const auto& sc = *NQdb::TestTypes().MakeStruct({
        {"amount", NQdb::MakeDecimalType(NQdb::TestTypes(), {7, 2})},
        {"tax", NQdb::MakeDecimalType(NQdb::TestTypes(), {6, 3})},
        {"price", NQdb::MakeDecimalType(NQdb::TestTypes(), {15, 2})},
    });

    auto [narrow, narrowType] = ExpandKernelExpr(NQdb::TestTypes(), Parse("(* amount tax)"), sc, NQdb::TestAnnotation());
    EXPECT_TRUE(IsDecimal(narrowType, 13, 5));
    const std::string narrowPrinted = NQumir::NAst::NCore::PrintAst(narrow);
    EXPECT_EQ(narrowPrinted.find("qdb_decimal"), std::string::npos);
    EXPECT_EQ(narrowPrinted.find("DECIMAL"), std::string::npos);

    auto [wide, wideType] = ExpandKernelExpr(NQdb::TestTypes(), Parse("(* price price)"), sc, NQdb::TestAnnotation());
    EXPECT_TRUE(IsDecimal(wideType, 30, 4));
    const std::string widePrinted = NQumir::NAst::NCore::PrintAst(wide);
    EXPECT_NE(widePrinted.find("qdb_decimal_mul"), std::string::npos);
    EXPECT_NE(widePrinted.find("qdb_decimal_from_unscaled"), std::string::npos);
    EXPECT_EQ(widePrinted.find("qdb_decimal_scale_up"), std::string::npos);
}

TEST(ProjectType, DecimalRemainderTakesTheWiderIntegerPart) {
    const auto& sc = *NQdb::TestTypes().MakeStruct({
        {"amount", NQdb::MakeDecimalType(NQdb::TestTypes(), {10, 2})},
        {"tax", NQdb::MakeDecimalType(NQdb::TestTypes(), {5, 3})},
        {"wide", NQdb::MakeDecimalType(NQdb::TestTypes(), {38, 2})},
        {"k", NQdb::TestTypes().MakeInteger(TIntegerType::I32)},
        {"qty", NQdb::TestTypes().MakeInteger()},
        {"p", NQdb::TestTypes().MakeFloat()},
    });

    // Common scale; the integer part of the wider operand (DuckDB's rule). INTEGER is
    // DECIMAL(10, 0), BIGINT DECIMAL(19, 0), and an integer literal a decimal of its digits.
    EXPECT_TRUE(IsDecimal(AnnotateExprType(NQdb::TestTypes(), Parse("(% amount k)"), sc, NQdb::TestAnnotation()), 12, 2));
    EXPECT_TRUE(IsDecimal(AnnotateExprType(NQdb::TestTypes(), Parse("(% amount 2)"), sc, NQdb::TestAnnotation()), 10, 2));
    EXPECT_TRUE(IsDecimal(AnnotateExprType(NQdb::TestTypes(), Parse("(% 7 amount)"), sc, NQdb::TestAnnotation()), 10, 2));
    EXPECT_TRUE(IsDecimal(AnnotateExprType(NQdb::TestTypes(), Parse("(% amount qty)"), sc, NQdb::TestAnnotation()), 21, 2));
    EXPECT_TRUE(IsDecimal(AnnotateExprType(NQdb::TestTypes(), Parse("(% amount 3000000000)"), sc, NQdb::TestAnnotation()), 12, 2));
    EXPECT_TRUE(IsDecimal(AnnotateExprType(NQdb::TestTypes(), Parse("(% amount amount)"), sc, NQdb::TestAnnotation()), 10, 2));
    EXPECT_TRUE(IsDecimal(AnnotateExprType(NQdb::TestTypes(), Parse("(% amount tax)"), sc, NQdb::TestAnnotation()), 11, 3));
    EXPECT_TRUE(IsDecimal(AnnotateExprType(NQdb::TestTypes(), Parse("(% wide k)"), sc, NQdb::TestAnnotation()), 38, 2));
    EXPECT_THROW(AnnotateExprType(NQdb::TestTypes(), Parse("(% amount p)"), sc, NQdb::TestAnnotation()), NQumir::TError);
}

TEST(ProjectType, ExpandKernelExprErasesDecimalCasts) {
    const auto& sc = *NQdb::TestTypes().MakeStruct({
        {"amount", NQdb::MakeDecimalType(NQdb::TestTypes(), {7, 2})},
    });

    auto [expr, type] = ExpandKernelExpr(NQdb::TestTypes(), Parse("(+ amount 1)"), sc, NQdb::TestAnnotation());

    EXPECT_TRUE(IsDecimal(type, 8, 2));
    const std::string printed = NQumir::NAst::NCore::PrintAst(expr);
    EXPECT_NE(printed.find("qdb_decimal64_scale_up"), std::string::npos);
    EXPECT_EQ(printed.find("DECIMAL"), std::string::npos);
}

TEST(ProjectType, ExpandKernelExprPreservesNullableDecimalComparisonWithFloat) {
    auto i64 = NQdb::TestTypes().MakeInteger();
    const auto& sc = *NQdb::TestTypes().MakeStruct({
        {"a", NQdb::MakeNullableType(NQdb::TestTypes(), i64)},
        {"b", NQdb::MakeNullableType(NQdb::TestTypes(), i64)},
    });

    auto [expr, type] = ExpandKernelExpr(NQdb::TestTypes(), Parse(
        "(< (/ (cast a <named DECIMAL [17 2]>) "
        "      (cast b <named DECIMAL [17 2]>)) 0.9)"), sc, NQdb::TestAnnotation());

    ASSERT_TRUE(IsNullable(type));
    EXPECT_TRUE(IsBool(NullableInner(type)));
    const std::string printed = NQumir::NAst::NCore::PrintAst(expr);
    EXPECT_NE(printed.find("qdb_decimal_div"), std::string::npos);
    EXPECT_NE(printed.find("qdb_decimal_from_unscaled"), std::string::npos);
    EXPECT_EQ(printed.find("DECIMAL"), std::string::npos);
}

TEST(ProjectType, ExpandNullableWrapsPlainSiblingOfNullableBranch) {
    const auto& sc = *NQdb::TestTypes().MakeStruct({
        {"p", NQdb::TestTypes().MakeFloat()},
        {"maybe_p", NQdb::MakeNullableType(NQdb::TestTypes(), NQdb::TestTypes().MakeFloat())},
    });

    auto [expr, type] = ExpandNullable(NQdb::TestTypes(),
        Parse("(if (< p (: 10.0 f64)) p maybe_p)"), sc, NQdb::TestAnnotation());

    ASSERT_TRUE(IsNullable(type));
    EXPECT_TRUE(IsF64(NullableInner(type)));
}

TEST(ProjectType, CaseMappingCallsTakeTheStringArena) {
    EXPECT_TRUE(TMaybeType<TStringType>(AnnotateExprType(NQdb::TestTypes(), Parse("(call upper s)"), Schema(), NQdb::TestAnnotation())));
    const auto& nullable = *NQdb::TestTypes().MakeStruct({
        {"n", NQdb::MakeNullableType(NQdb::TestTypes(), NQdb::TestTypes().Make<TStringType>())},
    });
    auto [expr, type] = ExpandNullable(NQdb::TestTypes(), Parse("(call lower n)"), nullable, NQdb::TestAnnotation());
    ASSERT_TRUE(IsNullable(type));
    EXPECT_TRUE(TMaybeType<TStringType>(NullableInner(type)));
    const std::string printed = NQumir::NAst::NCore::PrintAst(expr);
    EXPECT_NE(printed.find("(call qdb_string_lower __arena__ (field __nx0 Value))"),
        std::string::npos) << printed;
    EXPECT_THROW(AnnotateExprType(NQdb::TestTypes(), Parse("(call upper k)"), Schema(), NQdb::TestAnnotation()), NQumir::TError);
    EXPECT_THROW(AnnotateExprType(NQdb::TestTypes(), Parse("(call lower s s)"), Schema(), NQdb::TestAnnotation()), NQumir::TError);
}

TEST(ProjectType, RejectsUnknownColumn) {
    EXPECT_THROW(AnnotateExprType(NQdb::TestTypes(), Parse("missing"), Schema(), NQdb::TestAnnotation()), NQumir::TError);
}

// An unknown function is an error at its call, not a bare NULL that turns the
// comparison, COALESCE or CASE around it into NULL.
TEST(ProjectType, RejectsUnknownFunction) {
    auto error = [](const std::function<void()>& typing) {
        try {
            typing();
        } catch (const NQumir::TError& error) {
            return error.ToString();
        }
        return std::string();
    };
    const std::string expected = "Error: unknown function: nosuchfn @ Line: 1";
    EXPECT_TRUE(error([] {
        AnnotateExprType(NQdb::TestTypes(), Parse("(call coalesce (call nosuchfn a) 0)"), NullableSchema(), NQdb::TestAnnotation());
    }).starts_with(expected));
    EXPECT_TRUE(error([] {
        ExpandKernelPredicate(NQdb::TestTypes(), Parse("(== (call nosuchfn a) 1)"), NullableSchema(), NQdb::TestAnnotation());
    }).starts_with(expected));
    EXPECT_TRUE(error([] {
        ExpandKernelPredicate(NQdb::TestTypes(), Parse("(call qdb_is_null (call nosuchfn k))"), Schema(), NQdb::TestAnnotation());
    }).starts_with(expected));
    EXPECT_TRUE(error([] {
        ExpandKernelExpr(NQdb::TestTypes(), Parse("(if (== (call nosuchfn k) 1) 1 0)"), Schema(), NQdb::TestAnnotation());
    }).starts_with(expected));
}

// COALESCE and CASE of decimals with different precision or scale take the common
// decimal type, as in DuckDB.
TEST(ProjectType, CoalesceDecimalBranchesOfDifferentScale) {
    auto i64 = NQdb::TestTypes().MakeInteger();
    const auto& sc = *NQdb::TestTypes().MakeStruct({
        {"a", NQdb::MakeNullableType(NQdb::TestTypes(), i64)},
    });

    auto [wide, wideType] = ExpandKernelExpr(NQdb::TestTypes(), Parse(
        "(call coalesce (cast a <named DECIMAL [12 2]>) (cast -1 <named DECIMAL [10 2]>))"), sc, NQdb::TestAnnotation());
    EXPECT_TRUE(IsDecimal(wideType, 12, 2));

    auto [scaled, scaledType] = ExpandKernelExpr(NQdb::TestTypes(), Parse(
        "(call coalesce (cast a <named DECIMAL [10 2]>) (cast 1 <named DECIMAL [8 4]>))"), sc, NQdb::TestAnnotation());
    EXPECT_TRUE(IsDecimal(scaledType, 12, 4));
    EXPECT_NE(NQumir::NAst::NCore::PrintAst(scaled).find("qdb_decimal64_scale_up"),
        std::string::npos);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
