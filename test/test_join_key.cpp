#include <gtest/gtest.h>

#include <qdb/kernel/join_key.h>
#include <qdb/plan/types/decimal.h>
#include <qdb/plan/types/nullable.h>

#include <qumir/error.h>
#include <qumir/parser/type.h>

#include <memory>
#include <string>
#include <vector>

using namespace NQdb::NKernel;
using namespace NQumir::NAst;

namespace {

TTypePtr I(TIntegerType::EKind k) { return std::make_shared<TIntegerType>(k); }
TTypePtr F64() { return std::make_shared<TFloatType>(); }
TTypePtr Str() { return std::make_shared<TStringType>(); }
TTypePtr Dec() { return std::make_shared<NQdb::TDecimal>(38, 6); }
TTypePtr Nullable(TTypePtr type) { return std::make_shared<NQdb::TNullable>(std::move(type)); }

TStructType Schema(std::vector<std::pair<std::string, TTypePtr>> fields) {
    return TStructType(std::move(fields));
}

} // namespace

TEST(JoinKeyDescriptor, ScalarI64) {
    auto left = Schema({{"lk", I(TIntegerType::I64)}});
    auto right = Schema({{"rk", I(TIntegerType::I64)}});
    auto d = BuildJoinKeyDescriptor(left, right, {{"lk", "rk"}});
    ASSERT_EQ(d.Fields.size(), 1u);
    EXPECT_EQ(d.Fields[0].LeftColumnIndex, 0);
    EXPECT_EQ(d.Fields[0].RightColumnIndex, 0);
    EXPECT_EQ(d.Fields[0].Size, 8u);
    EXPECT_FALSE(d.Fields[0].IsNullable);
    EXPECT_EQ(d.Size, 8u);
    EXPECT_FALSE(d.HasDistinctLookupType());
    EXPECT_TRUE(d.IsScalar());
}

TEST(JoinKeyDescriptor, ScalarI32Works) {
    // The whole point of Stage 4: int32 keys (TPC-H *_key).
    auto left = Schema({{"o_custkey", I(TIntegerType::I32)}, {"o_total", F64()}});
    auto right = Schema({{"c_custkey", I(TIntegerType::I32)}, {"c_name", Str()}});
    auto d = BuildJoinKeyDescriptor(left, right, {{"o_custkey", "c_custkey"}});
    ASSERT_EQ(d.Fields.size(), 1u);
    EXPECT_EQ(d.Fields[0].LeftColumnIndex, 0);
    EXPECT_EQ(d.Fields[0].RightColumnIndex, 0);
    EXPECT_EQ(d.Fields[0].Size, 4u);
    EXPECT_EQ(d.Fields[0].Alignment, 4u);
}

TEST(JoinKeyDescriptor, DifferentColumnNamesSameKeyType) {
    // Different names, same type -> one column-name-independent Key type.
    auto a = BuildJoinKeyDescriptor(
        Schema({{"o_custkey", I(TIntegerType::I32)}}),
        Schema({{"c_custkey", I(TIntegerType::I32)}}), {{"o_custkey", "c_custkey"}});
    auto b = BuildJoinKeyDescriptor(
        Schema({{"x", I(TIntegerType::I32)}}),
        Schema({{"y", I(TIntegerType::I32)}}), {{"x", "y"}});
    EXPECT_EQ(a.TypeName, b.TypeName); // depends on type only, not column names
    EXPECT_EQ(a.TypeName.find("custkey"), std::string::npos);
}

TEST(JoinKeyDescriptor, NullableTypeNameUsesNullableTypeToken) {
    auto d = BuildJoinKeyDescriptor(
        Schema({{"lk", Nullable(I(TIntegerType::I64))}}),
        Schema({{"rk", I(TIntegerType::I64)}}), {{"lk", "rk"}});
    EXPECT_EQ(d.TypeName, "AggKey_ni64");
}

TEST(JoinKeyDescriptor, ReusesAggregatePhysicalTypeName) {
    auto agg = BuildAggregateKeyDescriptor(
        Schema({{"id", Nullable(I(TIntegerType::I64))}, {"name", Nullable(Str())}}),
        {"id", "name"});
    auto join = BuildJoinKeyDescriptor(
        Schema({{"left_id", Nullable(I(TIntegerType::I64))}, {"left_name", Nullable(Str())}}),
        Schema({{"right_id", I(TIntegerType::I64)}, {"right_name", Nullable(Str())}}),
        {{"left_id", "right_id"}, {"left_name", "right_name"}});

    EXPECT_EQ(join.TypeName, agg.TypeName);
    EXPECT_EQ(join.LookupTypeName, agg.LookupTypeName);
    EXPECT_EQ(join.StoredTypeName, agg.StoredTypeName);
}

TEST(JoinKeyDescriptor, ScalarF64) {
    auto d = BuildJoinKeyDescriptor(
        Schema({{"a", F64()}}), Schema({{"b", F64()}}), {{"a", "b"}});
    EXPECT_EQ(d.Fields[0].Size, 8u);
    EXPECT_FALSE(d.HasDistinctLookupType());
}

TEST(JoinKeyDescriptor, StringKeyHasDistinctLookupType) {
    auto d = BuildJoinKeyDescriptor(
        Schema({{"a", Str()}}), Schema({{"b", Str()}}), {{"a", "b"}});
    ASSERT_EQ(d.Fields.size(), 1u);
    EXPECT_TRUE(d.HasDistinctLookupType()); // StringView (lookup) vs OwnedString (stored)
}

TEST(JoinKeyDescriptor, CompositeI32String) {
    auto left = Schema({{"l1", I(TIntegerType::I32)}, {"l2", Str()}});
    auto right = Schema({{"r1", I(TIntegerType::I32)}, {"r2", Str()}});
    auto d = BuildJoinKeyDescriptor(left, right, {{"l1", "r1"}, {"l2", "r2"}});
    ASSERT_EQ(d.Fields.size(), 2u);
    EXPECT_EQ(d.Fields[0].LeftColumnIndex, 0);
    EXPECT_EQ(d.Fields[1].LeftColumnIndex, 1);
    EXPECT_EQ(d.Fields[1].RightColumnIndex, 1);
    EXPECT_TRUE(d.HasDistinctLookupType()); // contains a string component
}

// A decimal key is an i128 inside the BinInt struct; the kernel places it at
// 16 after a validity flag or a narrower key, and so must the host (#163).
TEST(JoinKeyDescriptor, NullableDecimalKeyIsAlignedTo16) {
    auto d = BuildJoinKeyDescriptor(
        Schema({{"lk", Nullable(Dec())}}), Schema({{"rk", Nullable(Dec())}}), {{"lk", "rk"}});
    ASSERT_EQ(d.Fields.size(), 1u);
    EXPECT_TRUE(d.Fields[0].IsNullable);
    EXPECT_EQ(d.Fields[0].Offset, 16u);
    EXPECT_EQ(d.Fields[0].Size, 16u);
    EXPECT_EQ(d.Fields[0].Alignment, 16u);
    EXPECT_EQ(d.Alignment, 16u);
    EXPECT_EQ(d.Size, 32u);
}

TEST(AggregateKeyDescriptor, DecimalAfterNarrowerKeysIsAlignedTo16) {
    auto afterI32 = BuildAggregateKeyDescriptor(
        Schema({{"k", I(TIntegerType::I32)}, {"d", Dec()}}), {"k", "d"});
    ASSERT_EQ(afterI32.Fields.size(), 2u);
    EXPECT_EQ(afterI32.Fields[1].Offset, 16u);
    EXPECT_EQ(afterI32.Size, 32u);
    EXPECT_EQ(afterI32.Alignment, 16u);

    auto afterString = BuildAggregateKeyDescriptor(
        Schema({{"s", Nullable(Str())}, {"d", Nullable(Dec())}}), {"s", "d"});
    ASSERT_EQ(afterString.Fields.size(), 2u);
    EXPECT_EQ(afterString.Fields[0].Offset, 8u);
    EXPECT_EQ(afterString.Fields[1].Offset, 32u); // flag at 24, key at the next 16
    EXPECT_EQ(afterString.Size, 48u);

    auto first = BuildAggregateKeyDescriptor(Schema({{"d", Dec()}}), {"d"});
    EXPECT_EQ(first.Fields[0].Offset, 0u);
    EXPECT_EQ(first.Size, 16u);
}

TEST(JoinKeyDescriptor, RejectsIncompatibleTypes) {
    EXPECT_THROW(BuildJoinKeyDescriptor(
        Schema({{"a", I(TIntegerType::I32)}}), Schema({{"b", Str()}}), {{"a", "b"}}),
        NQumir::TError);
}

TEST(JoinKeyDescriptor, RejectsMissingColumn) {
    EXPECT_THROW(BuildJoinKeyDescriptor(
        Schema({{"a", I(TIntegerType::I64)}}), Schema({{"b", I(TIntegerType::I64)}}),
        {{"missing", "b"}}), NQumir::TError);
    EXPECT_THROW(BuildJoinKeyDescriptor(
        Schema({{"a", I(TIntegerType::I64)}}), Schema({{"b", I(TIntegerType::I64)}}),
        {}), NQumir::TError);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
