#include <gtest/gtest.h>

#include <qdb/plan/types/decimal.h>
#include <qdb/plan/types/nullable.h>
#include <qdb/plan/types/type_text.h>

#include <qumir/parser/type.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace NQdb;
using namespace NQumir::NAst;

namespace {

// TypeKey does not look inside TNullable, so descend into it here.
std::string Key(const TTypePtr& type) {
    if (auto nullable = TMaybeType<TNullable>(type)) {
        return "Nullable<" + Key(nullable.Cast()->UnderlyingType) + ">";
    }
    if (auto structure = TMaybeType<TStructType>(type)) {
        std::string key = "Struct{";
        for (const auto& [name, field] : structure.Cast()->Fields) {
            key += name + ":" + Key(field) + ";";
        }
        return key + "}";
    }
    return TypeKey(type);
}

TTypePtr Int(TIntegerType::EKind kind) {
    return std::make_shared<TIntegerType>(kind);
}

TTypePtr Nullable(TTypePtr type) {
    return std::make_shared<TNullable>(std::move(type));
}

TTypePtr Struct(std::vector<std::pair<std::string, TTypePtr>> fields) {
    return std::make_shared<TStructType>(std::move(fields));
}

void ExpectRoundTrip(const TTypePtr& type) {
    auto text = PrintTypeText(type);
    auto parsed = ParseTypeText(text);
    EXPECT_EQ(Key(parsed), Key(type)) << text;
    EXPECT_EQ(PrintTypeText(parsed), text);
}

std::vector<TTypePtr> Scalars() {
    return {
        Int(TIntegerType::I32),
        Int(TIntegerType::I64),
        std::make_shared<TFloatType>(TFloatType::F32),
        std::make_shared<TFloatType>(TFloatType::F64),
        std::make_shared<TBoolType>(),
        std::make_shared<TStringType>(),
        std::make_shared<TDecimal>(18, 2),
        std::make_shared<TDecimal>(38, 10),
        BinIntStorageType(),
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
        EXPECT_TRUE(IsNullableType(ParseTypeText(PrintTypeText(nullable))));
    }
}

TEST(TypeText, ParsesQdbClasses) {
    EXPECT_EQ(PrintTypeText(Nullable(Int(TIntegerType::I64))), "<named Nullable [i64]>");
    auto decimal = ParseTypeText(PrintTypeText(std::make_shared<TDecimal>(12, 3)));
    auto typed = std::dynamic_pointer_cast<TDecimal>(decimal);
    ASSERT_TRUE(typed);
    EXPECT_EQ(typed->Spec.Precision, 12);
    EXPECT_EQ(typed->Spec.Scale, 3);

    auto nested = ParseTypeText(PrintTypeText(Nullable(std::make_shared<TDecimal>(18, 2))));
    auto nullable = std::dynamic_pointer_cast<TNullable>(nested);
    ASSERT_TRUE(nullable);
    EXPECT_TRUE(std::dynamic_pointer_cast<TDecimal>(nullable->UnderlyingType));
}

TEST(TypeText, StructsRoundTrip) {
    auto flat = Struct({
        {"a", Int(TIntegerType::I32)},
        {"b", Nullable(Int(TIntegerType::I64))},
        {"c", std::make_shared<TStringType>()},
        {"d", Nullable(std::make_shared<TDecimal>(18, 2))},
        {"e", BinIntStorageType()},
    });
    ExpectRoundTrip(flat);
    ExpectRoundTrip(Struct({
        {"x", flat},
        {"y", Nullable(std::make_shared<TBoolType>())},
        {"z", Struct({{"w", std::make_shared<TDecimal>(38, 0)}})},
    }));
}

TEST(TypeText, AcceptsSchemaSpellings) {
    auto key = Key(ParseTypeText("<named Nullable [<named Decimal [18 2]>]>"));
    EXPECT_EQ(key, Key(Nullable(std::make_shared<TDecimal>(18, 2))));
    EXPECT_TRUE(std::dynamic_pointer_cast<TDecimal>(ParseTypeText("<named Decimal [18 2]>")));
}

TEST(TypeText, BareDecimalIsDefault) {
    auto decimal = std::dynamic_pointer_cast<TDecimal>(ParseTypeText("DECIMAL"));
    ASSERT_TRUE(decimal);
    EXPECT_EQ(decimal->Spec.Precision, DefaultDecimalPrecision);
    EXPECT_EQ(decimal->Spec.Scale, DefaultDecimalScale);
}

TEST(TypeText, RejectsBadInput) {
    EXPECT_THROW(ParseTypeText("<named Nullable [1 2]>"), std::runtime_error);
    EXPECT_THROW(ParseTypeText("<named DECIMAL [18]>"), std::runtime_error);
    EXPECT_THROW(ParseTypeText("<named DECIMAL [39 0]>"), std::runtime_error);
    EXPECT_THROW(ParseTypeText("i64 i64"), std::runtime_error);
    EXPECT_THROW(ParseTypeText("Nullable<DECIMAL(18,2)>"), std::runtime_error);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
