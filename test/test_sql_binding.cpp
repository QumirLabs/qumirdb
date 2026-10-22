#include "mock_source.h"

#include <qdb/catalog/external_module.h>
#include <qdb/plan/build.h>
#include <qdb/plan/passes/unbound_vars.h>
#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/sort.h>
#include <qdb/plan/pipeline.h>
#include <qdb/sql/parser.h>

#include <qumir/parser/type.h>

#include <gtest/gtest.h>

#include <sstream>

using namespace NQdb;

namespace {

std::expected<TOperatorPtr, NQumir::TError> Build(
    std::string_view text,
    TMockSource& source,
    const NKernel::TAnnotationContext& annotation = {})
{
    std::istringstream input{std::string(text)};
    NSql::TTokenStream tokens(input);
    NSql::TParser parser;
    auto query = parser.Parse(tokens);
    if (!query) {
        return std::unexpected(query.error());
    }
    return BuildPlan(*query, [&](std::string_view name)
        -> std::expected<TOperatorPtr, NQumir::TError>
    {
        return std::make_shared<TSourceOperator>(source, std::string(name));
    }, annotation);
}

std::string Failure(std::string_view sql) {
    auto i64 = std::make_shared<NQumir::NAst::TIntegerType>();
    TMockSource source(TMockColumns{}, {{"x", i64}, {"y", i64}});
    try {
        auto plan = Build(sql, source);
        if (!plan) {
            return plan.error().ToString();
        }
        ApplyPlanPasses(*plan);
    } catch (const NQumir::TError& error) {
        return error.ToString();
    }
    return {};
}

} // namespace

TEST(SqlBinding, DistinctSortsOnDistinctOutputsOnly) {
    // A hidden sort column over SELECT DISTINCT is computed from the distinct rows,
    // so it may read their columns but no other FROM column.
    EXPECT_NE(Failure("SELECT DISTINCT x FROM t ORDER BY y").find("SELECT DISTINCT output"),
        std::string::npos);
    EXPECT_TRUE(Failure("SELECT DISTINCT x, y FROM t ORDER BY x + y").empty());
}

TEST(SqlBinding, ReusesAggregateExpressionBeforeCollectorMutation) {
    EXPECT_TRUE(Failure("SELECT sum(x) + 1 AS s FROM t ORDER BY sum(x) + 1").empty());
    EXPECT_TRUE(Failure("SELECT DISTINCT sum(x) + 1 AS s FROM t ORDER BY sum(x) + 1").empty());
}

TEST(SqlBinding, EquivalentQualifiedExpressionsBindToTheSelectedOutput) {
    TMockSource source(std::vector<std::string>{"x", "y"});
    for (const auto* sql : {
        "SELECT DISTINCT x FROM t ORDER BY t.x",
        "SELECT DISTINCT t.x FROM t ORDER BY x",
        "SELECT DISTINCT sum(x) AS s FROM t ORDER BY sum(t.x)",
        "SELECT DISTINCT sum(t.x) AS s FROM t ORDER BY sum(x)",
        "SELECT sum(x) + 1 AS s FROM t ORDER BY sum(t.x) + 1",
    }) {
        SCOPED_TRACE(sql);
        auto plan = Build(sql, source);
        ASSERT_TRUE(plan) << plan.error().ToString();
        auto sort = TMaybeOp<TSortOperator>(*plan);
        ASSERT_TRUE(sort);
        ASSERT_EQ(sort.Cast()->Keys().size(), 1);
        EXPECT_EQ(sort.Cast()->Keys()[0].Column, (*plan)->OutputColumns()->Fields[0].first);
    }
}

TEST(SqlBinding, AggregatesBindAfterInputSchemasAreResolved) {
    TMockSource source(std::vector<std::string>{"x", "y", "sum_0", "arg_0", "__group__"});
    for (const auto* sql : {
        "SELECT stddev_samp(t.x) FROM t",
        "SELECT stddev_samp(d.a) FROM (SELECT x + 1 AS a FROM t) d",
        "SELECT sum(d.x) FROM (SELECT DISTINCT x, y AS sum_0 FROM t) d",
        "SELECT sum(t.x) FROM t JOIN u ON t.x = u.x",
        "SELECT x + 1 AS sum_0, sum(y) FROM (SELECT x, y FROM t) d GROUP BY sum_0",
        "SELECT sum(x * y), sum(arg_0), sum(__group__) FROM t",
        "SELECT sum(__group__) FROM (SELECT x AS __group__ FROM t)",
        "SELECT sum(x) + 1 FROM t HAVING sum(x) + 1 > 0 ORDER BY sum(t.x) + 1",
        "WITH c AS (SELECT a.* FROM t AS a) "
        "SELECT t.x, (SELECT max(c.y) FROM c WHERE c.x = t.x) AS m FROM t",
    }) {
        SCOPED_TRACE(sql);
        auto plan = Build(sql, source);
        ASSERT_TRUE(plan) << plan.error().ToString();
        const auto check = [&](const auto& self, const TOperatorPtr& node) -> void {
            const auto schema = node->OutputColumns();
            ASSERT_TRUE(schema);
            std::unordered_set<std::string> names;
            for (const auto& [name, type] : schema->Fields) {
                EXPECT_TRUE(type) << name;
                EXPECT_TRUE(names.insert(name).second) << name;
            }
            if (auto project = TMaybeOp<TProjectOperator>(node)) {
                std::unordered_set<std::string> available;
                for (const auto& [name, _] : project.Cast()->Input()->OutputColumns()->Fields) {
                    available.insert(name);
                }
                for (const auto& spec : project.Cast()->Projections()) {
                    for (const auto& name : FindUnboundVars(spec.Expression)) {
                        EXPECT_TRUE(available.contains(name)) << name;
                    }
                }
            }
            for (const auto& child : node->Inputs()) {
                self(self, child);
            }
        };
        check(check, *plan);
    }
}

TEST(SqlBinding, SubqueryWidthsUseResolvedDistinctOutputs) {
    TMockSource source(std::vector<std::string>{"x", "y"});
    for (const auto* sql : {
        "SELECT x FROM t WHERE x IN (SELECT DISTINCT y FROM u)",
        "SELECT (SELECT DISTINCT y FROM u LIMIT 1) FROM t",
        "SELECT (SELECT max(x) FROM (SELECT x FROM u) d WHERE x > 0) FROM t",
        "SELECT (SELECT max(a) FROM (SELECT x AS a FROM u) d WHERE a > 0) FROM t",
        "SELECT (SELECT max(a) FROM (SELECT x FROM u) d(a) WHERE a > 0) FROM t",
        "SELECT x FROM t WHERE x > 0 OR x IN (SELECT DISTINCT y FROM u)",
    }) {
        SCOPED_TRACE(sql);
        auto plan = Build(sql, source);
        ASSERT_TRUE(plan) << plan.error().ToString();
        EXPECT_EQ((*plan)->OutputColumns()->Fields.size(), 1);
    }
    for (const auto* sql : {
        "SELECT x FROM t WHERE x IN (SELECT DISTINCT x, y FROM u)",
        "SELECT (SELECT DISTINCT x, y FROM u LIMIT 1) FROM t",
    }) {
        SCOPED_TRACE(sql);
        EXPECT_NE(Failure(sql).find("must return exactly one column"), std::string::npos);
    }
}

TEST(SqlBinding, AmbiguousInputsDoNotFallBackToGroupingAliases) {
    for (const auto* sql : {
        "SELECT l.x AS x, count(*) FROM t AS l JOIN t AS r ON l.y = r.y GROUP BY x",
        "SELECT l.a AS a, count(*) FROM "
            "(SELECT x AS a, y FROM t) AS l JOIN "
            "(SELECT x AS a, y FROM t) AS r ON l.y = r.y GROUP BY a",
    }) {
        SCOPED_TRACE(sql);
        EXPECT_NE(Failure(sql).find("ambiguous input column"), std::string::npos);
    }
    for (const auto* sql : {
        "SELECT l.x AS a, count(*) FROM t AS l JOIN t AS r ON l.y = r.y GROUP BY a",
        "SELECT l.x AS x, count(*) FROM t AS l JOIN t AS r ON l.y = r.y GROUP BY l.x",
        "SELECT x AS y, count(*) FROM t GROUP BY y, x",
    }) {
        SCOPED_TRACE(sql);
        EXPECT_TRUE(Failure(sql).empty());
    }
}

TEST(SqlBinding, InternalFieldsAreNotSqlInputNames) {
    for (const auto* sql : {
        "SELECT 1 FROM (SELECT x AS a, y AS a FROM t) WHERE __scope_0 = 1",
        "SELECT count(*) FROM (SELECT x AS a, y AS a FROM t) GROUP BY __scope_0",
        "SELECT * FROM (SELECT x AS a, y AS a FROM t) ORDER BY __scope_0",
    }) {
        SCOPED_TRACE(sql);
        EXPECT_NE(Failure(sql).find("unknown input column"), std::string::npos);
    }
    EXPECT_TRUE(Failure("SELECT 1 AS __scope_0 FROM "
        "(SELECT x AS a, y AS a FROM t) GROUP BY __scope_0").empty());
}

TEST(SqlBinding, UngroupedColumnsAreReportedByTheirSqlNames) {
    for (const auto* sql : {
        "SELECT x * 2, count(*) FROM t GROUP BY x % 2",
        "SELECT y, count(*) FROM t GROUP BY x",
        "SELECT count(*) FROM t GROUP BY x HAVING y > 0",
    }) {
        SCOPED_TRACE(sql);
        EXPECT_NE(Failure(sql).find("must appear in GROUP BY or be used in an aggregate function"),
            std::string::npos) << Failure(sql);
    }
    EXPECT_NE(Failure("SELECT y, count(*) FROM t AS a GROUP BY x").find("column a.y"), std::string::npos);
}

TEST(SqlBinding, RepeatedLabelsSortByInputExpression) {
    auto i64 = std::make_shared<NQumir::NAst::TIntegerType>();
    TMockSource source(TMockColumns{}, {{"x", i64}, {"y", i64}});
    auto plan = Build("SELECT x AS a, y AS a FROM t ORDER BY y", source);
    ASSERT_TRUE(plan) << plan.error().ToString();
    ApplyPlanPasses(*plan);
    auto output = TMaybeOp<TProjectOperator>(*plan);
    ASSERT_TRUE(output);
    auto sorted = output.Cast()->Input();
    while (auto project = TMaybeOp<TProjectOperator>(sorted)) {
        sorted = project.Cast()->Input();
    }
    auto sort = TMaybeOp<TSortOperator>(sorted);
    ASSERT_TRUE(sort);
    std::unordered_set<std::string> names;
    for (const auto& [name, _] : sort.Cast()->Input()->OutputColumns()->Fields) {
        EXPECT_TRUE(names.insert(name).second) << name;
    }
    ASSERT_EQ(sort.Cast()->Keys().size(), 1);
    EXPECT_TRUE(names.contains(sort.Cast()->Keys()[0].Column)) << sort.Cast()->Keys()[0].Column;
    ASSERT_EQ(output.Cast()->Projections().size(), 2);
    EXPECT_EQ(output.Cast()->Projections()[0].Name, "a");
    EXPECT_EQ(output.Cast()->Projections()[1].Name, "a");
    EXPECT_NE(Failure("SELECT x AS a, y AS a FROM t ORDER BY a").find("ambiguous"),
        std::string::npos);
}

TEST(SqlPositions, StructItemsCountTheirFields) {
    TExternalModuleCatalog catalog;
    std::istringstream definitions(
        "CREATE MODULE m LANGUAGE rust AS $$$$;"
        "CREATE FUNCTION pair(DOUBLE) RETURNS (DOUBLE, DOUBLE) "
        "SET MODULE TO m SET SYMBOL TO pair;");
    NSql::TTokenStream tokens(definitions);
    NSql::TParser parser;
    auto statements = parser.ParseAll(tokens);
    ASSERT_TRUE(statements) << statements.error().ToString();
    ASSERT_TRUE(catalog.Apply(*NSql::TMaybeNode<NSql::TSqlExternalModule>((*statements)[0]).Cast()));
    ASSERT_TRUE(catalog.Apply(*NSql::TMaybeNode<NSql::TSqlExternalFunction>((*statements)[1]).Cast()));
    const NKernel::TAnnotationContext annotation{.ExternalCatalog = catalog.Snapshot()};
    auto f64 = std::make_shared<NQumir::NAst::TFloatType>();
    TMockSource source(TMockColumns{}, {{"x", f64}, {"y", f64}});

    // ORDER BY reads the typed outputs, a struct item's fields included.
    for (const auto& [sql, expected] : std::vector<std::pair<const char*, std::vector<std::string>>>{
        {"SELECT pair(x), x + 1.0 AS z FROM t ORDER BY 2 DESC, 3", {"col1", "z"}},
        {"SELECT DISTINCT pair(x) AS (p, q), x + 1.0 FROM t ORDER BY 3 DESC", {"col2"}},
    }) {
        SCOPED_TRACE(sql);
        auto plan = Build(sql, source, annotation);
        ASSERT_TRUE(plan) << plan.error().ToString();
        auto sort = TMaybeOp<TSortOperator>(*plan);
        ASSERT_TRUE(sort);
        std::vector<std::string> keys;
        for (const auto& key : sort.Cast()->Keys()) {
            keys.push_back(key.Column);
        }
        EXPECT_EQ(keys, expected);
    }

    // GROUP BY counts positions before the aggregate the projection reads exists,
    // so it types each item over the input columns and the aggregate columns.
    for (const auto& [sql, keys] : std::vector<std::pair<const char*, std::vector<std::string>>>{
        {"SELECT pair(x), y, count(*) FROM t GROUP BY x, 3", {"t.x", "t.y"}},
        {"SELECT pair(x) AS (p, q), x + 1.0 AS z, count(*) FROM t GROUP BY x, 3", {"t.x", "gb_0"}},
        {"SELECT pair(sum(x)), y, count(*) FROM t GROUP BY 3", {"t.y"}},
        {"SELECT CASE WHEN x > 0.0 THEN pair(x) ELSE pair(y) END, y FROM t GROUP BY x, 3", {"t.x", "t.y"}},
        {"SELECT x, pair(x), count(*) FROM t GROUP BY 1", {"t.x"}},
    }) {
        SCOPED_TRACE(sql);
        auto plan = Build(sql, source, annotation);
        ASSERT_TRUE(plan) << plan.error().ToString();
        auto project = TMaybeOp<TProjectOperator>(*plan);
        ASSERT_TRUE(project);
        auto aggregate = TMaybeOp<TAggregateOperator>(project.Cast()->Input());
        ASSERT_TRUE(aggregate);
        EXPECT_EQ(aggregate.Cast()->GroupKeys(), keys);
    }
    // A position into a struct item or past the outputs is an error. A struct over
    // a window result is typed only above the aggregate, so it needs AS (...).
    for (const auto& [sql, expected] : std::vector<std::pair<const char*, const char*>>{
        {"SELECT pair(x), y FROM t GROUP BY 2", "GROUP BY position 2 refers to a field of a struct item"},
        {"SELECT pair(x) AS (p, q), count(*) FROM t GROUP BY 2",
            "GROUP BY position 2 refers to a field of a struct item"},
        {"SELECT pair(x), y FROM t GROUP BY 4", "GROUP BY position 4 is not in the select list"},
        {"SELECT pair(sum(sum(x)) OVER (ORDER BY y)), y FROM t GROUP BY 2",
            "GROUP BY position 2 requires AS (...) on a struct-return projection typed above the aggregate"},
    }) {
        SCOPED_TRACE(sql);
        auto plan = Build(sql, source, annotation);
        ASSERT_FALSE(plan);
        EXPECT_NE(plan.error().ToString().find(expected), std::string::npos) << plan.error().ToString();
    }
    auto aliased = Build(
        "SELECT pair(sum(sum(x)) OVER (ORDER BY y)) AS (p, q), y FROM t GROUP BY 3", source, annotation);
    EXPECT_TRUE(aliased) << aliased.error().ToString();
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
