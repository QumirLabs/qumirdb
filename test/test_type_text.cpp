#include "test_types.h"

#include <gtest/gtest.h>

#include <qdb/kernel/query_modules.h>
#include <qdb/kernel/vm_function.h>
#include <qdb/modules/qumirdb.h>
#include <qdb/plan/types/date.h>
#include <qdb/plan/types/decimal.h>
#include <qdb/plan/types/nullable.h>
#include <qdb/plan/types/type_text.h>
#include <qdb/utils/module_path.h>

#include <qumir/parser/ast.h>
#include <qumir/parser/type.h>
#include <qumir/parser/type_factory.h>

#include <stdexcept>
#include <string>
#include <vector>

#include "test_types.h"

using namespace NQdb;
using namespace NQumir::NAst;

namespace {

TTypePtr Int(TIntegerType::EKind kind) {
    return TestTypes().MakeInteger(kind);
}

TTypePtr Nullable(TTypePtr type) {
    return MakeNullableType(TestTypes(), type);
}

TTypePtr Struct(std::vector<std::pair<std::string, TTypePtr>> fields) {
    return TestTypes().MakeStruct(std::move(fields));
}

TTypePtr Parse(std::string_view text) {
    return ParseTypeText(TestTypes(), text);
}

// Types are interned, so a round trip gives back the same object.
void ExpectRoundTrip(const TTypePtr& type) {
    auto text = PrintTypeText(type);
    EXPECT_EQ(Parse(text), type) << text;
}

std::vector<TTypePtr> Scalars() {
    return {
        Int(TIntegerType::I32),
        Int(TIntegerType::I64),
        TestTypes().MakeFloat(TFloatType::F32),
        TestTypes().MakeFloat(TFloatType::F64),
        TestTypes().Make<TBoolType>(),
        TestTypes().Make<TStringType>(),
        MakeDecimalType(TestTypes(), {18, 2}),
        MakeDecimalType(TestTypes(), {38, 10}),
        MakeDateType(TestTypes()),
        MakeDateType(TestTypes(), TIntegerType::I64),
        BinIntStorageType(TestTypes()),
    };
}

} // namespace

TEST(TypeText, ScalarsRoundTrip) {
    for (const auto& type : Scalars()) {
        ExpectRoundTrip(type);
    }
}

TEST(TypeText, NullableRoundTrip) {
    for (const auto& type : Scalars()) {
        auto nullable = Nullable(type);
        ExpectRoundTrip(nullable);
        EXPECT_TRUE(IsNullableType(Parse(PrintTypeText(nullable))));
        EXPECT_EQ(UnwrapNullableType(nullable), type);
    }
}

TEST(TypeText, ParsesQdbTypes) {
    EXPECT_EQ(PrintTypeText(Nullable(Int(TIntegerType::I64))), "<named Nullable [i64]>");
    EXPECT_EQ(PrintTypeText(MakeDateType(TestTypes())), "<named DATE [i32]>");
    EXPECT_EQ(PrintTypeText(MakeDecimalType(TestTypes(), {12, 3})), "<named DECIMAL [12 3]>");

    auto decimal = DecimalSpecOfValueType(Parse("<named DECIMAL [12 3]>"));
    ASSERT_TRUE(decimal);
    EXPECT_EQ(decimal->Precision, 12);
    EXPECT_EQ(decimal->Scale, 3);

    auto nested = Parse(PrintTypeText(Nullable(MakeDecimalType(TestTypes(), {18, 2}))));
    ASSERT_TRUE(IsNullableType(nested));
    EXPECT_EQ(UnwrapNullableType(nested), MakeDecimalType(TestTypes(), {18, 2}));

    const auto* storage = DateStorageType(Parse("<named DATE [i64]>"));
    ASSERT_TRUE(storage);
    EXPECT_EQ(storage->Width, TIntegerType::I64);
}

// A kernel compilation completes Nullable [T] with its qumirdb.oz struct; plan text
// stays the plan's.
TEST(TypeText, CompletedNullablePrintsWithoutItsStruct) {
    const auto value = Int(TIntegerType::I16);
    const auto layout = TestTypes().MakeStruct({{"Value", value}, {"Valid", TestTypes().Make<TBoolType>()}});
    const auto completed = TestTypes().MakeNamed("Nullable", layout, {TGenericArg::TypeArg(value)});
    EXPECT_EQ(completed, Nullable(value));
    EXPECT_EQ(PrintTypeText(completed), "<named Nullable [i16]>");
    EXPECT_EQ(UnwrapNullableType(completed), value);
}

TEST(TypeText, StructsRoundTrip) {
    auto flat = Struct({
        {"a", Int(TIntegerType::I32)},
        {"b", Nullable(Int(TIntegerType::I64))},
        {"c", TestTypes().Make<TStringType>()},
        {"d", Nullable(MakeDecimalType(TestTypes(), {18, 2}))},
        {"e", BinIntStorageType(TestTypes())},
        {"f", Nullable(MakeDateType(TestTypes()))},
    });
    ExpectRoundTrip(flat);
    ExpectRoundTrip(Struct({
        {"x", flat},
        {"y", Nullable(TestTypes().Make<TBoolType>())},
        {"z", Struct({{"w", MakeDecimalType(TestTypes(), {38, 0})}})},
    }));
}

TEST(TypeText, AcceptsSchemaSpellings) {
    EXPECT_EQ(
        Parse("<named Nullable [<named Decimal [18 2]>]>"),
        Nullable(MakeDecimalType(TestTypes(), {18, 2})));
    EXPECT_EQ(Parse("<named Decimal [18 2]>"), MakeDecimalType(TestTypes(), {18, 2}));
}

// DECIMAL arguments are interned as text, so every DECIMAL is built with explicit ones.
TEST(TypeText, BareDecimalIsDefault) {
    EXPECT_EQ(
        Parse("DECIMAL"),
        MakeDecimalType(TestTypes(), {DefaultDecimalPrecision, DefaultDecimalScale}));
    EXPECT_EQ(Parse("<named DECIMAL [018 2]>"), MakeDecimalType(TestTypes(), {18, 2}));
}

TEST(TypeText, SqlTypeNames) {
    EXPECT_EQ(SqlTypeName(Nullable(MakeDecimalType(TestTypes(), {15, 2}))), "DECIMAL(15,2)");
    EXPECT_EQ(SqlTypeName(MakeDateType(TestTypes())), "DATE");
    EXPECT_EQ(SqlTypeName(Int(TIntegerType::I64)), "BIGINT");
    EXPECT_EQ(SqlTypeName(Int(TIntegerType::I32)), "INTEGER");
    EXPECT_EQ(SqlTypeName(TestTypes().MakeFloat(TFloatType::F32)), "REAL");
    EXPECT_EQ(SqlTypeName(TestTypes().MakeFloat(TFloatType::F64)), "DOUBLE");
}

TEST(TypeText, RejectsBadInput) {
    EXPECT_THROW(Parse("<named Nullable [1 2]>"), std::runtime_error);
    EXPECT_THROW(Parse("<named DECIMAL [18]>"), std::runtime_error);
    EXPECT_THROW(Parse("<named DECIMAL [39 0]>"), std::runtime_error);
    EXPECT_THROW(Parse("<named DATE [f64]>"), std::runtime_error);
    EXPECT_THROW(Parse("i64 i64"), std::runtime_error);
    EXPECT_THROW(Parse("Nullable<DECIMAL(18,2)>"), std::runtime_error);
}

// QumirDB builds the qumirdb.oz types itself. If one differs from its declaration,
// MakeNamed makes a second object when the module is composed and the type splits in two.
// DECIMAL and DATE[i32] are plan types: kernels see them erased to BinInt and i32.
TEST(TypeText, QumirDbTypesAreTheOnesQumirdbOzDeclares) {
    const auto queryTypes = NKernel::MakeQueryTypes(*TestModuleBase());
    auto& types = *queryTypes;
    auto build = [&] {
        std::vector<TTypePtr> built;
        for (const auto& type : NQumir::NRegistry::QumirDbExternalTypes(types)) {
            built.push_back(NQumir::NRegistry::QumirDbNamedType(types, type.Name));
        }
        const auto i64 = types.MakeInteger();
        built.push_back(MakeNullableType(types, i64));
        built.push_back(MakeNullableType(types, types.Make<TStringType>()));
        built.push_back(DateCastTarget(types));
        built.push_back(BinIntStorageType(types));
        return built;
    };
    const auto before = build();

    NKernel::TQueryModules modules(types, TestModuleBase());
    TVmFrontendContext frontend(modules, {NUtils::ModuleFile("qumirdb.oz")});
    std::vector<TParam> params;
    for (const auto& type : before) {
        params.push_back(NQumir::MakeIntrusive<TVarStmt>(
            NQumir::TLocation{}, "p" + std::to_string(params.size()), type));
    }
    NQumir::TLocation loc{};
    auto compiled = TVmFunction::Compile(frontend, {
        .NamePrefix = "__type_check__",
        .Params = std::move(params),
        .Body = NQumir::MakeIntrusive<TBlockExpr>(loc, std::vector<TExprPtr>{
            NQumir::MakeIntrusive<TReturnExpr>(
                loc, NQumir::MakeIntrusive<TNumberExpr>(loc, int64_t{0}, types)),
        }),
        .ReturnType = types.MakeInteger(),
        .Imports = {"qumirdb"},
    });
    ASSERT_TRUE(compiled) << compiled.error();

    const auto after = build();
    ASSERT_EQ(before.size(), after.size());
    for (size_t i = 0; i < before.size(); ++i) {
        EXPECT_EQ(before[i], after[i]) << PrintTypeText(before[i]);
    }
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
