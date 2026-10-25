#include <qdb/plan/passes/simplify_outer_joins.h>
#include <qdb/sql/parser.h>

#include <gtest/gtest.h>

#include <sstream>
#include <stdexcept>
#include <string>

using namespace NQdb;

namespace {

NQumir::NAst::TExprPtr Predicate(const std::string& where) {
    std::istringstream input("SELECT 1 FROM s, r WHERE " + where);
    NSql::TTokenStream tokens(input);
    auto parsed = NSql::TParser().Parse(tokens);
    if (!parsed) {
        throw std::runtime_error(parsed.error().ToString());
    }
    auto maybeQuery = NSql::TMaybeNode<NSql::TSqlQuery>(*parsed);
    if (!maybeQuery) {
        throw std::runtime_error("not a query");
    }
    auto query = maybeQuery.Cast();
    auto maybeSelect = NSql::TMaybeNode<NSql::TSqlSelect>(query->Body);
    if (!maybeSelect) {
        throw std::runtime_error("not a select");
    }
    return maybeSelect.Cast()->Where;
}

// The columns of r are the null-extended side, as in `s LEFT JOIN r`.
bool RejectsNullsOfR(const std::string& where) {
    return RejectsNulls(Predicate(where), {"r.a", "r.b"});
}

TEST(RejectsNulls, StrictOperatorsAndCalls) {
    EXPECT_TRUE(RejectsNullsOfR("r.a > 1"));
    EXPECT_TRUE(RejectsNullsOfR("r.a = s.a"));
    EXPECT_TRUE(RejectsNullsOfR("r.a + s.b * 2 < 10"));
    EXPECT_TRUE(RejectsNullsOfR("-r.a < 0"));
    EXPECT_TRUE(RejectsNullsOfR("r.a BETWEEN 1 AND 2"));
    EXPECT_TRUE(RejectsNullsOfR("CAST(r.a AS VARCHAR) = '1'"));
    EXPECT_TRUE(RejectsNullsOfR("abs(r.a) > 1"));
    EXPECT_TRUE(RejectsNullsOfR("upper(r.b) = 'X'"));
    EXPECT_TRUE(RejectsNullsOfR("r.b || 'x' = 'yx'"));
    EXPECT_TRUE(RejectsNullsOfR("r.b LIKE 'x%'"));
    EXPECT_TRUE(RejectsNullsOfR("r.b NOT LIKE 'x%'"));
    EXPECT_TRUE(RejectsNullsOfR("r.a IN (1, 2)"));
    EXPECT_TRUE(RejectsNullsOfR("r.a NOT IN (1, 2)"));
}

TEST(RejectsNulls, NullTests) {
    EXPECT_TRUE(RejectsNullsOfR("r.a IS NOT NULL"));
    EXPECT_TRUE(RejectsNullsOfR("NOT (r.a IS NULL)"));
    EXPECT_TRUE(RejectsNullsOfR("(r.a + s.a) IS NOT NULL"));
    EXPECT_FALSE(RejectsNullsOfR("r.a IS NULL"));
    EXPECT_FALSE(RejectsNullsOfR("NOT (r.a IS NOT NULL)"));
    EXPECT_FALSE(RejectsNullsOfR("coalesce(r.a, 0) IS NOT NULL"));
}

TEST(RejectsNulls, NonStrictExpressions) {
    EXPECT_FALSE(RejectsNullsOfR("coalesce(r.a, 0) = 0"));
    EXPECT_FALSE(RejectsNullsOfR("ifnull(r.a, 0) = 0"));
    EXPECT_FALSE(RejectsNullsOfR("nullif(r.a, 0) = 1"));
    EXPECT_FALSE(RejectsNullsOfR("CASE WHEN r.a IS NULL THEN 1 ELSE r.a END = 1"));
    EXPECT_FALSE(RejectsNullsOfR("NOT (coalesce(r.a, 0) = 1)"));
    // A NULL item does not decide IN: `1 IN (r.a, 1)` is TRUE.
    EXPECT_FALSE(RejectsNullsOfR("s.a IN (r.a, 1)"));
    EXPECT_FALSE(RejectsNullsOfR("s.a > 1"));
}

TEST(RejectsNulls, Connectives) {
    EXPECT_TRUE(RejectsNullsOfR("s.a > 1 AND r.a > 1"));
    EXPECT_FALSE(RejectsNullsOfR("s.a > 1 AND r.a IS NULL"));
    EXPECT_TRUE(RejectsNullsOfR("r.a > 1 OR r.b = 'x'"));
    EXPECT_FALSE(RejectsNullsOfR("r.a > 1 OR s.a > 1"));
    EXPECT_FALSE(RejectsNullsOfR("r.a > 1 OR r.a IS NULL"));
    // NOT is NULL only for a NULL operand, and NULL AND FALSE is FALSE.
    EXPECT_TRUE(RejectsNullsOfR("NOT (r.a > 1 AND r.b = 'x')"));
    EXPECT_FALSE(RejectsNullsOfR("NOT (r.a > 1 AND s.a = 1)"));
}

} // namespace

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
