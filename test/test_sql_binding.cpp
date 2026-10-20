#include "mock_source.h"

#include <qdb/plan/build.h>
#include <qdb/kernel/spec.h>
#include <qdb/plan/passes/column_pruning.h>
#include <qdb/plan/passes/unbound_vars.h>
#include <qdb/catalog/external_module.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/limit.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/sort.h>
#include <qdb/plan/pipeline.h>
#include <qdb/sexp/parser.h>
#include <qdb/sexp/printer.h>
#include <qdb/sql/parser.h>
#include <qdb/plan/passes/sql_binding.h>

#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>
#include <qumir/parser/type.h>

#include <gtest/gtest.h>

#include <sstream>

using namespace NQdb;

namespace {

std::expected<TOperatorPtr, NQumir::TError> Build(
    std::string_view text, TMockSource& source, const NKernel::TAnnotationContext& context = {})
{
    std::istringstream input{std::string(text)};
    NSql::TTokenStream tokens(input);
    NSql::TParser parser;
    auto query = parser.Parse(tokens);
    if (!query) {
        return std::unexpected(query.error());
    }
    auto plan = BuildPlan(*query, [&](std::string_view name)
        -> std::expected<TOperatorPtr, NQumir::TError>
    {
        return std::make_shared<TSourceOperator>(source, std::string(name));
    });
    if (plan) {
        try {
            ResolveSqlBindings(*plan, context);
        } catch (const NQumir::TError& error) {
            return std::unexpected(error);
        }
    }
    return plan;
}

std::expected<TOperatorPtr, NQumir::TError> Roundtrip(
    const TOperatorPtr& plan, TMockSource& source)
{
    std::ostringstream text;
    NSexp::PrintRelPlan(text, plan);
    NSexp::TRelParserOptions options;
    options.SourceFactory = [&](std::string_view name, NQumir::TLocation) -> TOperatorPtr {
        return std::make_shared<TSourceOperator>(source, std::string(name));
    };
    NQumir::NAst::NCore::TParser parser;
    for (auto& [name, callback] : NSexp::MakeRelParsers(std::move(options))) {
        parser.NodeParsers[name] = std::move(callback);
    }
    std::istringstream input(text.str());
    NQumir::NAst::NCore::TTokenStream tokens(input);
    auto parsed = parser.Parse(tokens);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    return std::static_pointer_cast<IOperator>(*parsed);
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

TEST(SqlBinding, NamedStructProjectionFreezesItsFlattenedLayout) {
    using namespace NQumir::NAst;
    auto i64 = std::make_shared<TIntegerType>();
    auto structure = std::make_shared<TNamedType>("Pair", std::make_shared<TStructType>(
        std::vector<std::pair<std::string, TTypePtr>>{{"first", i64}, {"second", i64}}));
    TMockSource source(TMockColumns{}, {{"x", i64}, {"pair", structure}});
    auto input = std::make_shared<TSourceOperator>(source, "t");
    TOperatorPtr plan = std::make_shared<TProjectOperator>(input, std::vector<TProjectionSpec>{
        {.Name = "unused", .Expression = std::make_shared<TIdentExpr>(NQumir::TLocation{}, "x")},
        {.Expression = std::make_shared<TIdentExpr>(NQumir::TLocation{}, "pair"),
            .ImplicitName = true},
    });
    AnnotateTypes(plan);
    ASSERT_EQ(plan->OutputColumns()->Fields.size(), 3);

    auto project = TMaybeOp<TProjectOperator>(plan);
    ASSERT_TRUE(project);
    EXPECT_EQ(project.Cast()->Projections()[1].ColumnAliases,
        (std::vector<std::string>{"col1", "col2"}));
    TColumnSet demand{"col2"};
    ApplyColumnPruning(plan, &demand, nullptr);
    AnnotateTypes(plan);
    ASSERT_EQ(project.Cast()->Projections().size(), 1);
    EXPECT_FALSE(project.Cast()->Projections()[0].ImplicitName);
    ASSERT_EQ(plan->OutputColumns()->Fields.size(), 2);
    EXPECT_EQ(plan->OutputColumns()->Fields[0].first, "col1");
    EXPECT_EQ(plan->OutputColumns()->Fields[1].first, "col2");
}

TEST(SqlBinding, DistinctRejectsHiddenSortExpressions) {
    for (const auto& sql : {
        "SELECT DISTINCT x FROM t ORDER BY y",
        "SELECT DISTINCT x, y FROM t ORDER BY x + y",
    }) {
        SCOPED_TRACE(sql);
        EXPECT_NE(Failure(sql).find("SELECT DISTINCT output"), std::string::npos);
    }
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
        EXPECT_FALSE(HasUnresolvedSqlBindings(*plan));
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
        EXPECT_FALSE(HasUnresolvedSqlBindings(*plan));
        EXPECT_EQ((*plan)->OutputColumns()->Fields.size(), 1);
    }
    for (const auto* sql : {
        "SELECT x FROM t WHERE x IN (SELECT DISTINCT x, y FROM u)",
        "SELECT (SELECT DISTINCT x, y FROM u LIMIT 1) FROM t",
    }) {
        SCOPED_TRACE(sql);
        EXPECT_NE(Failure(sql).find("column alias count"), std::string::npos);
    }
}

TEST(SqlBinding, AmbiguousInputsDoNotFallBackToGroupingAliases) {
    for (const auto* sql : {
        "SELECT l.x AS x, count(*) FROM t AS l JOIN t AS r ON l.y = r.y GROUP BY x",
        "SELECT l.x AS a, count(*) FROM "
            "(SELECT x AS a, y FROM t) AS l JOIN "
            "(SELECT x AS a, y FROM t) AS r ON l.y = r.y GROUP BY a",
    }) {
        SCOPED_TRACE(sql);
        EXPECT_NE(Failure(sql).find("input column is ambiguous"), std::string::npos);
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
        "SELECT 1 FROM (SELECT x AS a, y AS a FROM t) WHERE __output_0 = 1",
        "SELECT count(*) FROM (SELECT x AS a, y AS a FROM t) GROUP BY __output_0",
        "SELECT * FROM (SELECT x AS a, y AS a FROM t) ORDER BY __output_0",
    }) {
        SCOPED_TRACE(sql);
        EXPECT_NE(Failure(sql).find("unknown input column"), std::string::npos);
    }
    EXPECT_TRUE(Failure("SELECT 1 AS __output_0 FROM "
        "(SELECT x AS a, y AS a FROM t) GROUP BY __output_0").empty());
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
