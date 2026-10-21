#include <gtest/gtest.h>
#include "mock_source.h"

#include <qdb/plan/build.h>
#include <qdb/plan/ops/cte_ref.h>
#include <qdb/plan/ops/source.h>
#include <qdb/sql/parser.h>

#include <expected>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>

using namespace NQdb;

namespace {

// Plans `sql` over `t (x, y)` and `opaque`, a source whose columns the planner
// cannot list before building it. Returns the planning error, if any.
std::string PlanningError(std::string_view sql) {
    TMockSource t({"x", "y"});
    std::istringstream in{std::string(sql)};
    NSql::TTokenStream ts(in);
    NSql::TParser parser;
    auto parsed = parser.Parse(ts);
    if (!parsed) {
        return parsed.error().ToString();
    }
    auto plan = BuildPlan(parsed.value(), [&](std::string_view table)
        -> std::expected<TOperatorPtr, NQumir::TError>
    {
        if (table == "t") {
            return std::make_shared<TSourceOperator>(t, std::string(table));
        }
        if (table == "opaque") {
            return std::make_shared<TCteRef>(std::make_shared<TCteDefinition>());
        }
        return std::unexpected(NQumir::TError("unknown table: " + std::string(table)));
    });
    return plan
        ? std::string()
        : plan.error().ToString();
}

} // namespace

// A bare name of undecidable scope may be an outer column, which the standalone
// subquery plan would leave unbound, so it is refused rather than planned as
// uncorrelated.
TEST(SubqueryScope, RefusesBareNameOfUnknownScope) {
    EXPECT_NE(
        PlanningError("SELECT x FROM t WHERE x < (SELECT max(k) FROM opaque WHERE k > 0)")
            .find("unsupported correlated scalar subquery"),
        std::string::npos);
    EXPECT_NE(
        PlanningError("SELECT x FROM t WHERE x IN (SELECT k FROM opaque WHERE k > 0) OR y = 0")
            .find("correlated IN subquery in a boolean expression is not supported"),
        std::string::npos);
}

TEST(SubqueryScope, ReportsUnknownTable) {
    for (const auto sql : {
        "SELECT x FROM t WHERE x < (SELECT max(k) FROM missing WHERE k > 0)",
        "SELECT x FROM t WHERE x < (SELECT max(k) FROM (SELECT * FROM missing) d WHERE k > 0)",
    }) {
        EXPECT_NE(PlanningError(sql).find("unknown table: missing"), std::string::npos) << sql;
    }
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
