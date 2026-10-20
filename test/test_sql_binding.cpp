#include "mock_source.h"

#include <qdb/plan/build.h>
#include <qdb/kernel/spec.h>
#include <qdb/plan/passes/column_pruning.h>
#include <qdb/plan/passes/sql_binding.h>
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

TEST(SqlOrdinals, RejectsInvalidPositionsAndHiddenOrderColumns) {
    for (const auto& sql : {
        "SELECT x FROM t ORDER BY 0",
        "SELECT x FROM t ORDER BY -1",
        "SELECT x FROM t ORDER BY 2",
        "SELECT x FROM t ORDER BY 9223372036854775807",
        "SELECT x FROM t ORDER BY 2, y",
        "SELECT x FROM t ORDER BY y, 2",
        "SELECT x FROM t GROUP BY 0",
        "SELECT x FROM t GROUP BY -1",
        "SELECT x FROM t GROUP BY 2",
    }) {
        SCOPED_TRACE(sql);
        const auto error = Failure(sql);
        EXPECT_NE(error.find("position"), std::string::npos) << error;
    }
}

TEST(SqlOrdinals, RejectsAggregateWindowAndUnresolvedGroupTargets) {
    // A position before an untypeable item reports why the item has no width.
    for (const auto& [sql, expected] : std::vector<std::pair<const char*, const char*>>{
        {"SELECT sum(y) FROM t GROUP BY 1", "GROUP BY key cannot refer to an aggregate"},
        {"SELECT rank() OVER (ORDER BY y) FROM t GROUP BY 1", "GROUP BY position must refer to a scalar input expression"},
        {"SELECT opaque(x), y FROM t GROUP BY 2", "could not infer type of '(call opaque"},
    }) {
        SCOPED_TRACE(sql);
        const auto error = Failure(sql);
        EXPECT_NE(error.find(expected), std::string::npos) << error;
    }
}

TEST(SqlOrdinals, PositionBindingSurvivesPlanRoundtripAndExcludesHiddenColumns) {
    auto i64 = std::make_shared<NQumir::NAst::TIntegerType>();
    TMockSource source(TMockColumns{}, {{"x", i64}, {"y", i64}});
    auto plan = Build("SELECT x AS a FROM t ORDER BY 1 DESC, y", source);
    ASSERT_TRUE(plan) << plan.error().ToString();
    auto visible = TMaybeOp<TProjectOperator>(*plan);
    ASSERT_TRUE(visible);
    auto boundSort = TMaybeOp<TSortOperator>(visible.Cast()->Input());
    ASSERT_TRUE(boundSort);
    EXPECT_EQ(boundSort.Cast()->Keys()[0].Column, "a");
    ASSERT_EQ((*plan)->OutputColumns()->Fields.size(), 1);
    std::ostringstream text;
    NSexp::PrintRelPlan(text, *plan);
    EXPECT_EQ(text.str().find("sql-output"), std::string::npos);
    EXPECT_EQ(text.str().find("(output "), std::string::npos);
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
    ASSERT_TRUE(parsed) << parsed.error().ToString();
    auto restored = std::static_pointer_cast<IOperator>(*parsed);
    ApplyPlanPasses(restored);
    auto project = TMaybeOp<TProjectOperator>(restored);
    ASSERT_TRUE(project);
    auto sort = TMaybeOp<TSortOperator>(project.Cast()->Input());
    ASSERT_TRUE(sort);
    EXPECT_EQ(sort.Cast()->Keys()[0].Column, "a");
}

TEST(SqlOrdinals, OrdersFlattenedStructOutputPositionsAfterTyping) {
    TExternalModuleCatalog catalog;
    std::istringstream definitions(
        "CREATE MODULE m LANGUAGE rust AS $$$$;"
        "CREATE FUNCTION pair(DOUBLE) RETURNS (DOUBLE, DOUBLE) "
        "SET MODULE TO m SET SYMBOL TO pair;");
    NSql::TTokenStream tokens(definitions);
    NSql::TParser parser;
    auto statements = parser.ParseAll(tokens);
    ASSERT_TRUE(statements) << statements.error().ToString();
    auto module = NSql::TMaybeNode<NSql::TSqlExternalModule>((*statements)[0]);
    auto function = NSql::TMaybeNode<NSql::TSqlExternalFunction>((*statements)[1]);
    ASSERT_TRUE(module);
    ASSERT_TRUE(function);
    ASSERT_TRUE(catalog.Apply(*module.Cast()));
    ASSERT_TRUE(catalog.Apply(*function.Cast()));
    auto floating = std::make_shared<NQumir::NAst::TFloatType>();
    TMockSource source(TMockColumns{}, {{"x", floating}});
    NKernel::TAnnotationContext annotation{.ExternalCatalog = catalog.Snapshot()};
    auto plan = Build("SELECT pair(x), x + 1.0 AS z FROM t ORDER BY 2 DESC, 3", source, annotation);
    ASSERT_TRUE(plan) << plan.error().ToString();
    AnnotateTypes(*plan, annotation);
    auto sort = TMaybeOp<TSortOperator>(*plan);
    ASSERT_TRUE(sort);
    ASSERT_EQ(sort.Cast()->Input()->OutputColumns()->Fields.size(), 3);
    EXPECT_EQ(sort.Cast()->Keys()[0].Column, "col1");
    EXPECT_EQ(sort.Cast()->Keys()[1].Column, "z");
    for (const auto& sql : {
        "SELECT DISTINCT pair(x) AS (p, q), x + 1.0 FROM t ORDER BY 3 DESC",
        "SELECT DISTINCT pair(x), x + 1.0 FROM t ORDER BY 3 DESC",
    }) {
        SCOPED_TRACE(sql);
        auto built = Build(sql, source, annotation);
        ASSERT_TRUE(built) << built.error().ToString();
        auto unboundRoundtrip = Roundtrip(*built, source);
        ASSERT_TRUE(unboundRoundtrip) << unboundRoundtrip.error().ToString();
        for (auto candidate : {*built, *unboundRoundtrip}) {
            AnnotateTypes(candidate, annotation);
            auto distinctSort = TMaybeOp<TSortOperator>(candidate);
            ASSERT_TRUE(distinctSort);
            EXPECT_EQ(distinctSort.Cast()->Keys()[0].Column, "col2");
            auto aggregate = TMaybeOp<TAggregateOperator>(distinctSort.Cast()->Input());
            ASSERT_TRUE(aggregate);
            ASSERT_EQ(aggregate.Cast()->GroupKeys().size(), 3);
            EXPECT_EQ(aggregate.Cast()->GroupKeys()[2], "col2");
            auto boundRoundtrip = Roundtrip(candidate, source);
            ASSERT_TRUE(boundRoundtrip) << boundRoundtrip.error().ToString();
            AnnotateTypes(*boundRoundtrip, annotation);
            auto restoredSort = TMaybeOp<TSortOperator>(*boundRoundtrip);
            ASSERT_TRUE(restoredSort);
            EXPECT_EQ(restoredSort.Cast()->Keys()[0].Column, "col2");
            EXPECT_EQ((*boundRoundtrip)->OutputColumns()->Fields.size(), 3);
        }
    }
    auto pruned = Build(
        "SELECT x + 1.0, pair(x), x + 2.0 FROM t ORDER BY 3", source, annotation);
    ASSERT_TRUE(pruned) << pruned.error().ToString();
    AnnotateTypes(*pruned, annotation);
    TColumnSet demand{"col3"};
    ApplyColumnPruning(*pruned, &demand, nullptr);
    AnnotateTypes(*pruned, annotation);
    auto restored = Roundtrip(*pruned, source);
    ASSERT_TRUE(restored) << restored.error().ToString();
    for (auto candidate : {*pruned, *restored}) {
        AnnotateTypes(candidate, annotation);
        auto prunedSort = TMaybeOp<TSortOperator>(candidate);
        ASSERT_TRUE(prunedSort);
        auto project = TMaybeOp<TProjectOperator>(prunedSort.Cast()->Input());
        ASSERT_TRUE(project);
        ASSERT_EQ(project.Cast()->Projections().size(), 2);
        EXPECT_FALSE(project.Cast()->Projections()[0].ImplicitName);
        EXPECT_EQ(project.Cast()->Projections()[0].ColumnAliases,
            (std::vector<std::string>{"col1", "col2"}));
        EXPECT_EQ(project.Cast()->Projections()[1].Name, "col3");
        const auto schema = project.Cast()->OutputColumns();
        ASSERT_EQ(schema->Fields.size(), 3);
        EXPECT_EQ(schema->Fields[0].first, "col1");
        EXPECT_EQ(schema->Fields[1].first, "col2");
        EXPECT_EQ(schema->Fields[2].first, "col3");
        auto spec = NKernel::BuildSortKernelSpec(*schema, prunedSort.Cast()->Keys());
        EXPECT_EQ(spec.SortKeys[0].Column.Index, 1);
    }
    auto invalid = Build("SELECT pair(x), x + 1.0 AS z FROM t ORDER BY 4", source, annotation);
    ASSERT_FALSE(invalid);
    EXPECT_NE(invalid.error().ToString().find("position"), std::string::npos);
}

TEST(SqlOrdinals, UniqueComputedNamesSurvivePruningAndRetyping) {
    auto i64 = std::make_shared<NQumir::NAst::TIntegerType>();
    TMockSource source(TMockColumns{}, {{"x", i64}, {"y", i64}});
    // A column alias list renames the SELECT outputs in place.
    for (const auto& [sql, name] : std::vector<std::pair<const char*, const char*>>{
        {"SELECT d.b FROM (SELECT x + 1, y + 1 FROM t) AS d(a, b)", "b"},
        {"SELECT q.col1 FROM (SELECT x + 1, y + 1 FROM t ORDER BY 2) AS q", "col1"},
    }) {
        SCOPED_TRACE(sql);
        auto plan = Build(sql, source);
        ASSERT_TRUE(plan) << plan.error().ToString();
        AnnotateTypes(*plan);
        auto current = *plan;
        while (current->Inputs().size() == 1
            && !TMaybeOp<TSourceOperator>(current->Inputs().front()))
        {
            current = current->Inputs().front();
        }
        auto producer = TMaybeOp<TProjectOperator>(current);
        ASSERT_TRUE(producer);
        ASSERT_EQ(producer.Cast()->Projections().size(), 2);
        EXPECT_FALSE(producer.Cast()->Projections()[1].ImplicitName);
        EXPECT_EQ(producer.Cast()->OutputColumns()->Fields[1].first, name);

        EXPECT_FALSE(producer.Cast()->Projections()[1].ImplicitName);
        AnnotateTypes(*plan);
        ApplyColumnPruning(*plan);
        AnnotateTypes(*plan);
        ASSERT_EQ(producer.Cast()->Projections().size(), 1);
        EXPECT_EQ(producer.Cast()->Projections()[0].Name, name);
        ASSERT_EQ(producer.Cast()->OutputColumns()->Fields.size(), 1);
        EXPECT_EQ(producer.Cast()->OutputColumns()->Fields[0].first, name);
        auto restored = Roundtrip(*plan, source);
        ASSERT_TRUE(restored) << restored.error().ToString();
        for (auto candidate : {*plan, *restored}) {
            AnnotateTypes(candidate);
            ASSERT_EQ(candidate->OutputColumns()->Fields.size(), 1);
            EXPECT_TRUE(NQumir::NAst::TMaybeType<NQumir::NAst::TIntegerType>(
                candidate->OutputColumns()->Fields[0].second));
            for (auto node = candidate; node->Inputs().size() == 1;
                node = node->Inputs().front())
            {
                if (auto sort = TMaybeOp<TSortOperator>(node)) {
                    EXPECT_EQ(sort.Cast()->Keys()[0].Column, "col1");
                    auto spec = NKernel::BuildSortKernelSpec(
                        *sort.Cast()->Input()->OutputColumns(), sort.Cast()->Keys());
                    EXPECT_EQ(spec.SortKeys[0].Column.Index, 0);
                }
            }
        }
    }
}

TEST(SqlOrdinals, BoundNamesSurvivePruningAndRoundtrip) {
    auto i64 = std::make_shared<NQumir::NAst::TIntegerType>();
    TMockSource source(TMockColumns{}, {{"x", i64}, {"y", i64}});
    auto plan = Build("SELECT x AS unused, x AS a, y AS a FROM t ORDER BY 3", source);
    ASSERT_TRUE(plan) << plan.error().ToString();
    ApplyPlanPasses(*plan);
    TColumnSet demand{"a"};
    ApplyColumnPruning(*plan, &demand, nullptr);
    auto output = TMaybeOp<TProjectOperator>(*plan);
    ASSERT_TRUE(output);
    auto sort = TMaybeOp<TSortOperator>(output.Cast()->Input());
    ASSERT_TRUE(sort);
    auto schema = sort.Cast()->Input()->OutputColumns();
    ASSERT_EQ(schema->Fields.size(), 2);
    EXPECT_NE(schema->Fields[0].first, schema->Fields[1].first);
    EXPECT_EQ(sort.Cast()->Keys()[0].Column, schema->Fields[1].first);
    auto spec = NKernel::BuildSortKernelSpec(*schema, sort.Cast()->Keys());
    ASSERT_EQ(spec.SortKeys.size(), 1);
    EXPECT_EQ(spec.SortKeys[0].Column.Index, 1);

    std::ostringstream text;
    NSexp::PrintRelPlan(text, *plan);
    EXPECT_EQ(text.str().find("(field "), std::string::npos);
    EXPECT_EQ(text.str().find("sql-output"), std::string::npos);
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
    ASSERT_TRUE(parsed) << parsed.error().ToString();
    auto restored = std::static_pointer_cast<IOperator>(*parsed);
    ApplyPlanPasses(restored);
    auto restoredOutput = TMaybeOp<TProjectOperator>(restored);
    ASSERT_TRUE(restoredOutput);
    auto restoredSort = TMaybeOp<TSortOperator>(restoredOutput.Cast()->Input());
    ASSERT_TRUE(restoredSort);
    auto restoredSpec = NKernel::BuildSortKernelSpec(
        *restoredSort.Cast()->Input()->OutputColumns(), restoredSort.Cast()->Keys());
    EXPECT_EQ(restoredSpec.SortKeys[0].Column.Index, 1);
}

TEST(SqlOrdinals, DuplicateLabelsAndHiddenExpressionsUseDistinctNames) {
    auto i64 = std::make_shared<NQumir::NAst::TIntegerType>();
    TMockSource source(TMockColumns{}, {{"x", i64}, {"y", i64}});
    for (const auto& sql : {
        "SELECT x AS a, y AS a FROM t ORDER BY 2, x + y",
        "SELECT x AS __sort_0, x AS a, y AS a FROM t ORDER BY 3, x + y",
        "SELECT DISTINCT x AS a, y AS a FROM t ORDER BY 2",
        "SELECT x AS a, y AS a FROM t UNION ALL SELECT y AS b, x AS c FROM t ORDER BY 2",
        "SELECT x AS a, y AS a FROM t UNION SELECT y AS b, x AS c FROM t ORDER BY 2",
        "SELECT x AS a, y AS a FROM t UNION ALL "
            "(SELECT y AS b, x AS c FROM t ORDER BY 1) ORDER BY 2",
        "(SELECT x AS a, y AS a FROM t ORDER BY 2) UNION "
            "SELECT y AS b, x AS c FROM t ORDER BY 2",
    }) {
        SCOPED_TRACE(sql);
        auto plan = Build(sql, source);
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
        const auto input = sort.Cast()->Input()->OutputColumns();
        std::unordered_set<std::string> names;
        for (const auto& [name, _] : input->Fields) {
            EXPECT_TRUE(names.insert(name).second) << name;
        }
        for (const auto& key : sort.Cast()->Keys()) {
            EXPECT_TRUE(names.contains(key.Column)) << key.Column;
        }
        EXPECT_EQ(output.Cast()->Projections()[output.Cast()->Projections().size() - 2].Name, "a");
        EXPECT_EQ(output.Cast()->Projections().back().Name, "a");
    }
}

TEST(SqlOrdinals, BooleanLiteralsRemainScalarKeys) {
    for (const auto& sql : {
        "SELECT count(*) FROM t GROUP BY TRUE",
        "SELECT count(*) FROM t GROUP BY FALSE",
        "SELECT x FROM t ORDER BY TRUE, x DESC",
        "SELECT x FROM t ORDER BY FALSE, x DESC",
    }) {
        SCOPED_TRACE(sql);
        const auto error = Failure(sql);
        EXPECT_TRUE(error.empty()) << error;
    }
}

TEST(SqlOrdinals, BuilderLeavesPositionsForTheLogicalBindingPass) {
    TMockSource source(std::vector<std::string>{"x", "y"});
    std::istringstream input("SELECT sum(y) AS s, x FROM t GROUP BY 2 ORDER BY 1 DESC");
    NSql::TTokenStream tokens(input);
    NSql::TParser parser;
    auto query = parser.Parse(tokens);
    ASSERT_TRUE(query) << query.error().ToString();
    auto built = BuildPlan(*query, [&](std::string_view name)
        -> std::expected<TOperatorPtr, NQumir::TError>
    {
        return std::make_shared<TSourceOperator>(source, std::string(name));
    });
    ASSERT_TRUE(built) << built.error().ToString();
    auto sort = TMaybeOp<TSortOperator>(*built);
    ASSERT_TRUE(sort);
    EXPECT_TRUE(sort.Cast()->Keys().empty());
    ASSERT_EQ(sort.Cast()->UnresolvedKeys().size(), 1);
    auto orderPosition = NQumir::NAst::TMaybeNode<NQumir::NAst::TNumberExpr>(
        sort.Cast()->UnresolvedKeys()[0].Expression);
    ASSERT_TRUE(orderPosition);
    EXPECT_EQ(orderPosition.Cast()->IntValue, 1);
    auto select = TMaybeOp<TProjectOperator>(sort.Cast()->Input());
    ASSERT_TRUE(select);
    ASSERT_TRUE(select.Cast()->SelectBinding());
    auto aggregate = TMaybeOp<TAggregateOperator>(select.Cast()->Input());
    ASSERT_TRUE(aggregate);
    ASSERT_TRUE(aggregate.Cast()->Binding());
    ASSERT_EQ(aggregate.Cast()->Binding()->Keys.size(), 1);
    auto groupPosition = NQumir::NAst::TMaybeNode<NQumir::NAst::TNumberExpr>(
        aggregate.Cast()->Binding()->Keys[0].Expression);
    ASSERT_TRUE(groupPosition);
    EXPECT_EQ(groupPosition.Cast()->IntValue, 2);
    auto scan = TMaybeOp<TSourceOperator>(aggregate.Cast()->Input());
    ASSERT_TRUE(scan);
    EXPECT_TRUE(scan.Cast()->GetAlias().empty());
    EXPECT_TRUE(HasUnresolvedSqlBindings(*built));
    std::ostringstream serialized;
    EXPECT_THROW(NSexp::PrintRelPlan(serialized, *built), std::runtime_error);

    ResolveSqlBindings(*built);
    EXPECT_FALSE(HasUnresolvedSqlBindings(*built));
    sort = TMaybeOp<TSortOperator>(*built);
    ASSERT_TRUE(sort);
    ASSERT_EQ(sort.Cast()->Keys().size(), 1);
    EXPECT_EQ(sort.Cast()->Keys()[0].Column, "s");
    select = TMaybeOp<TProjectOperator>(sort.Cast()->Input());
    ASSERT_TRUE(select);
    aggregate = TMaybeOp<TAggregateOperator>(select.Cast()->Input());
    ASSERT_TRUE(aggregate);
    EXPECT_EQ(aggregate.Cast()->GroupKeys(), (std::vector<std::string>{"t.x"}));
    EXPECT_NO_THROW(NSexp::PrintRelPlan(serialized, *built));
}

TEST(SqlOrdinals, DuplicateLabelsKeepTheirIdentityAcrossLimit) {
    TMockSource source(std::vector<std::string>{"x", "y"});
    auto plan = Build("(SELECT x AS a, y AS a FROM t LIMIT 10) ORDER BY 2", source);
    ASSERT_TRUE(plan) << plan.error().ToString();
    auto output = TMaybeOp<TProjectOperator>(*plan);
    ASSERT_TRUE(output);
    auto sort = TMaybeOp<TSortOperator>(output.Cast()->Input());
    ASSERT_TRUE(sort);
    auto limit = TMaybeOp<TLimitOperator>(sort.Cast()->Input());
    ASSERT_TRUE(limit);
    const auto fields = limit.Cast()->OutputColumns()->Fields;
    ASSERT_EQ(fields.size(), 2);
    EXPECT_NE(fields[0].first, fields[1].first);
    EXPECT_EQ(sort.Cast()->Keys()[0].Column, fields[1].first);
    EXPECT_EQ((*plan)->OutputColumns()->Fields[0].first, "a");
    EXPECT_EQ((*plan)->OutputColumns()->Fields[1].first, "a");
}

TEST(SqlOrdinals, StarsExcludeScalarSubqueryHelperColumns) {
    TMockSource source(std::vector<std::string>{"x", "y"});
    for (const auto* sql : {
        "SELECT *, (SELECT max(x) FROM t) AS m FROM t ORDER BY 3",
        "SELECT * FROM t WHERE x > (SELECT min(x) FROM t) ORDER BY 2",
    }) {
        SCOPED_TRACE(sql);
        auto plan = Build(sql, source);
        ASSERT_TRUE(plan) << plan.error().ToString();
        auto sort = TMaybeOp<TSortOperator>(*plan);
        ASSERT_TRUE(sort);
        const auto columns = (*plan)->OutputColumns();
        EXPECT_EQ(columns->Fields.size(), sql == std::string_view(
            "SELECT *, (SELECT max(x) FROM t) AS m FROM t ORDER BY 3") ? 3 : 2);
        EXPECT_EQ(columns->Fields[0].first, "x");
        EXPECT_EQ(columns->Fields[1].first, "y");
        EXPECT_EQ(sort.Cast()->Keys()[0].Column, columns->Fields.back().first);
    }
}

TEST(SqlOrdinals, DerivedStarsPreserveRepeatedFieldIdentity) {
    TMockSource source(std::vector<std::string>{"x", "y"});
    auto plan = Build("SELECT d.* FROM (SELECT x AS a, y AS a FROM t) AS d ORDER BY 2", source);
    ASSERT_TRUE(plan) << plan.error().ToString();
    auto output = TMaybeOp<TProjectOperator>(*plan);
    ASSERT_TRUE(output);
    auto sort = TMaybeOp<TSortOperator>(output.Cast()->Input());
    ASSERT_TRUE(sort);
    auto selected = TMaybeOp<TProjectOperator>(sort.Cast()->Input());
    ASSERT_TRUE(selected);
    ASSERT_EQ(selected.Cast()->Projections().size(), 2);
    auto first = NQumir::NAst::TMaybeNode<NQumir::NAst::TIdentExpr>(
        selected.Cast()->Projections()[0].Expression);
    auto second = NQumir::NAst::TMaybeNode<NQumir::NAst::TIdentExpr>(
        selected.Cast()->Projections()[1].Expression);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_NE(first.Cast()->Name, second.Cast()->Name);
    EXPECT_EQ(sort.Cast()->Keys()[0].Column, selected.Cast()->Projections()[1].Name);
    EXPECT_EQ((*plan)->OutputColumns()->Fields[0].first, "a");
    EXPECT_EQ((*plan)->OutputColumns()->Fields[1].first, "a");
    EXPECT_NE(Failure("SELECT d.a FROM (SELECT x AS a, y AS a FROM t) AS d").find("ambiguous"),
        std::string::npos);
}

TEST(SqlOrdinals, CorrelationOutputsDoNotShiftUserPositions) {
    const auto error = Failure(
        "SELECT t.x, (SELECT max(u.y) FROM u WHERE u.x = t.x GROUP BY 1) FROM t");
    EXPECT_NE(error.find("GROUP BY key cannot refer to an aggregate"), std::string::npos) << error;
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
