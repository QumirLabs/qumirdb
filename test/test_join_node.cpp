#include <gtest/gtest.h>

#include <qdb/plan/ops/join.h>
#include <qdb/plan/types/nullable.h>

#include <qumir/parser/type.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace NQdb;
using namespace NQumir::NAst;

// Minimal operator carrying a fixed output schema, used to wire join inputs
// without depending on real sources.
class TFakeSource : public IOperator {
public:
    static constexpr const char* OpId = "fake_source";

    explicit TFakeSource(std::vector<std::pair<std::string, TTypePtr>> fields) {
        auto schema = std::make_shared<TStructType>(std::move(fields));
        Type = std::make_shared<TFunctionType>(std::vector<TTypePtr>{}, schema);
    }

    std::string_view RelName() const override { return OpId; }
    std::vector<TColumnReference> ColumnReferences() const override { return {}; }
    std::vector<TExprPtr> Children() const override { return {}; }
    const std::string ToString() const override { return "(rel fake_source)"; }
};

TOperatorPtr Source(std::vector<std::pair<std::string, TTypePtr>> fields) {
    return std::make_shared<TFakeSource>(std::move(fields));
}

TTypePtr I64() { return std::make_shared<TIntegerType>(); }
TTypePtr Str() { return std::make_shared<TStringType>(); }

const TStructType* AsStruct(const TTypePtr& t) {
    return static_cast<const TStructType*>(t.get());
}

TOperatorPtr LeftAB() { return Source({{"a", I64()}, {"b", Str()}}); }
TOperatorPtr RightCD() { return Source({{"c", I64()}, {"d", Str()}}); }

} // namespace

TEST(JoinNode, InnerOutputConcatenatesBothSides) {
    auto join = MakeJoin(LeftAB(), RightCD(), {{"a", "c"}}, EJoinType::Inner);
    ASSERT_TRUE(join) << (join ? "" : join.error().ToString());

    auto* out = AsStruct((*join)->OutputColumns());
    ASSERT_NE(out, nullptr);
    ASSERT_EQ(out->Fields.size(), 4u);
    EXPECT_EQ(out->Fields[0].first, "a");
    EXPECT_EQ(out->Fields[1].first, "b");
    EXPECT_EQ(out->Fields[2].first, "c");
    EXPECT_EQ(out->Fields[3].first, "d");
    for (const auto& [name, type] : out->Fields) {
        EXPECT_FALSE(IsNullableType(type)) << name << " must not be nullable for INNER";
    }
}

TEST(JoinNode, LeftOuterMakesRightSideNullable) {
    auto join = MakeJoin(LeftAB(), RightCD(), {{"a", "c"}}, EJoinType::Left);
    ASSERT_TRUE(join) << (join ? "" : join.error().ToString());

    auto* out = AsStruct((*join)->OutputColumns());
    ASSERT_EQ(out->Fields.size(), 4u);
    EXPECT_FALSE(IsNullableType(out->Fields[0].second)); // a
    EXPECT_FALSE(IsNullableType(out->Fields[1].second)); // b
    EXPECT_TRUE(IsNullableType(out->Fields[2].second));  // c (right, padded)
    EXPECT_TRUE(IsNullableType(out->Fields[3].second));  // d
}

TEST(JoinNode, RightOuterMakesLeftSideNullable) {
    auto join = MakeJoin(LeftAB(), RightCD(), {{"a", "c"}}, EJoinType::Right);
    ASSERT_TRUE(join);
    auto* out = AsStruct((*join)->OutputColumns());
    EXPECT_TRUE(IsNullableType(out->Fields[0].second));  // a (left, padded)
    EXPECT_TRUE(IsNullableType(out->Fields[1].second));  // b
    EXPECT_FALSE(IsNullableType(out->Fields[2].second)); // c
    EXPECT_FALSE(IsNullableType(out->Fields[3].second)); // d
}

TEST(JoinNode, FullOuterMakesBothSidesNullable) {
    auto join = MakeJoin(LeftAB(), RightCD(), {{"a", "c"}}, EJoinType::Full);
    ASSERT_TRUE(join);
    auto* out = AsStruct((*join)->OutputColumns());
    for (const auto& [name, type] : out->Fields) {
        EXPECT_TRUE(IsNullableType(type)) << name;
    }
}

TEST(JoinNode, SemiAndAntiKeepOnlyPreservedSide) {
    auto semi = MakeJoin(LeftAB(), RightCD(), {{"a", "c"}}, EJoinType::LeftSemi);
    ASSERT_TRUE(semi);
    auto* semiOut = AsStruct((*semi)->OutputColumns());
    ASSERT_EQ(semiOut->Fields.size(), 2u);
    EXPECT_EQ(semiOut->Fields[0].first, "a");
    EXPECT_EQ(semiOut->Fields[1].first, "b");

    auto anti = MakeJoin(LeftAB(), RightCD(), {{"a", "c"}}, EJoinType::RightAnti);
    ASSERT_TRUE(anti);
    auto* antiOut = AsStruct((*anti)->OutputColumns());
    ASSERT_EQ(antiOut->Fields.size(), 2u);
    EXPECT_EQ(antiOut->Fields[0].first, "c");
    EXPECT_EQ(antiOut->Fields[1].first, "d");
}

// The plan has no marker column to name yet; only the join kernel runs it.
TEST(JoinNode, MarkJoinIsNotTypedInPlansYet) {
    auto join = MakeJoin(LeftAB(), RightCD(), {{"a", "c"}}, EJoinType::LeftMark);
    ASSERT_FALSE(join);
    EXPECT_NE(join.error().ToString().find("left_mark"), std::string::npos);
}

TEST(JoinNode, OverlappingColumnNamesAreRejected) {
    auto l = Source({{"a", I64()}, {"shared", Str()}});
    auto r = Source({{"c", I64()}, {"shared", Str()}});
    auto join = MakeJoin(l, r, {{"a", "c"}}, EJoinType::Inner);
    ASSERT_FALSE(join);
    EXPECT_NE(join.error().ToString().find("shared"), std::string::npos);
}

TEST(JoinNode, EmptyKeyListAcceptedForInner) {
    auto join = MakeJoin(LeftAB(), RightCD(), {}, EJoinType::Inner);
    EXPECT_TRUE(join);
}

TEST(JoinNode, EmptyKeyListRejectedForNonInner) {
    auto join = MakeJoin(LeftAB(), RightCD(), {}, EJoinType::LeftSemi);
    EXPECT_FALSE(join);
}

TEST(JoinNode, FilterIsParsedAndReferencedColumnsCoverKeysAndFilter) {
    auto join = MakeJoin(LeftAB(), RightCD(), {{"a", "c"}}, EJoinType::Inner, "(< b d)");
    ASSERT_TRUE(join) << (join ? "" : join.error().ToString());

    auto j = TMaybeOp<TJoinOperator>(*join);
    ASSERT_TRUE(j);
    EXPECT_NE(j.Cast()->Filter(), nullptr);
    EXPECT_EQ(j.Cast()->JoinType(), EJoinType::Inner);

    auto refs = j.Cast()->ComputeReferencedColumns();
    EXPECT_TRUE(refs.contains("a")); // left key
    EXPECT_TRUE(refs.contains("c")); // right key
    EXPECT_TRUE(refs.contains("b")); // filter
    EXPECT_TRUE(refs.contains("d")); // filter
}

TEST(JoinNode, ToStringRoundTripsTypeAndFilter) {
    auto join = MakeJoin(LeftAB(), RightCD(), {{"a", "c"}}, EJoinType::Left, "(< b d)");
    ASSERT_TRUE(join);
    auto s = (*join)->ToString();
    EXPECT_NE(s.find("(rel join"), std::string::npos);
    EXPECT_NE(s.find("((a c))"), std::string::npos);
    EXPECT_NE(s.find("(left)"), std::string::npos);
    EXPECT_NE(s.find("(< b d)"), std::string::npos);
}

TEST(JoinNode, JoinTypeNameParseRoundTrip) {
    for (auto type : {EJoinType::Inner, EJoinType::Left, EJoinType::Right,
                      EJoinType::Full, EJoinType::LeftSemi, EJoinType::RightSemi,
                      EJoinType::LeftAnti, EJoinType::RightAnti,
                      EJoinType::LeftMark}) {
        auto parsed = ParseJoinType(JoinTypeName(type));
        ASSERT_TRUE(parsed);
        EXPECT_EQ(*parsed, type);
    }
    EXPECT_FALSE(ParseJoinType("not_a_join"));
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
