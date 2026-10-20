#include <gtest/gtest.h>
#include "mock_source.h"

#include <qdb/io/schema.h>
#include "plan_runner.h"
#include <qdb/modules/qumirdb_runtime.h>
#include <qdb/plan/build.h>
#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/late_materialize.h>
#include <qdb/plan/ops/limit.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/ops/sort.h>
#include <qdb/plan/ops/union.h>
#include <qdb/plan/ops/window.h>
#include <qdb/plan/passes/column_pruning.h>
#include <qdb/plan/passes/qualify_columns.h>
#include <qdb/plan/passes/top_sort.h>
#include <qdb/plan/passes/push_limit.h>
#include <qdb/plan/passes/typing.h>
#include <qdb/plan/types/decimal.h>
#include <qdb/plan/types/nullable.h>
#include <qdb/sql/parser.h>
#include <qdb/plan/passes/sql_binding.h>

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/parser/ast.h>
#include <qumir/parser/core/printer.h>
#include <qumir/parser/type.h>

#include <algorithm>
#include <expected>
#include <memory>
#include <numeric>
#include <sstream>
#include <string_view>
#include <vector>

using namespace NQdb;

namespace {



std::expected<TOperatorPtr, NQumir::TError> BuildSqlPlan(std::string_view sql, ISource& source) {
    std::istringstream in{std::string(sql)};
    NSql::TTokenStream ts(in);
    NSql::TParser parser;
    auto parsed = parser.Parse(ts);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }

    auto factory = [&](std::string_view table)
        -> std::expected<TOperatorPtr, NQumir::TError>
    {
        return std::make_shared<TSourceOperator>(source, std::string(table));
    };

    auto plan = BuildPlan(parsed.value(), factory);
    if (plan) {
        ResolveSqlBindings(*plan);
    }
    return plan;
}

TRowSet MakeStringI64Batch(const std::string& names, int64_t* offsets, int64_t* values,
    int64_t rows, std::vector<TColumn>& columns, uint8_t* selection = nullptr)
{
    columns = {
        TColumn{.Data = const_cast<char*>(names.data()), .Offsets = offsets, .OffsetWidth = 8},
        TColumn{.Data = reinterpret_cast<char*>(values)},
    };
    return TRowSet{
        .Columns = columns.data(),
        .ColumnCount = 2,
        .RowCount = rows,
        .Selection = selection,
        .RefCount = 1,
    };
}

TRowSet MakeI64I64Batch(int64_t* left, int64_t* right, int64_t rows,
    std::vector<TColumn>& columns)
{
    columns = {
        TColumn{.Data = reinterpret_cast<char*>(left)},
        TColumn{.Data = reinterpret_cast<char*>(right)},
    };
    return TRowSet{
        .Columns = columns.data(),
        .ColumnCount = 2,
        .RowCount = rows,
        .RefCount = 1,
    };
}

std::string StringCell(const TColumn& column, int64_t row) {
    const auto* offsets = static_cast<const int64_t*>(column.Offsets);
    return std::string(column.Data + offsets[row], column.Data + offsets[row + 1]);
}

bool MaskBit(const TColumn& column, int64_t row) {
    return ((column.Mask[row / 8] >> (row % 8)) & 1) != 0;
}

NQumir::NAst::TExprPtr Ident(std::string name) {
    return std::make_shared<NQumir::NAst::TIdentExpr>(
        NQumir::TLocation{}, std::move(name));
}

std::vector<std::string> FieldNames(const NQumir::NAst::TTypePtr& type) {
    auto* st = static_cast<NQumir::NAst::TStructType*>(type.get());
    std::vector<std::string> names;
    if (!st) {
        return names;
    }
    for (const auto& [name, _] : st->Fields) {
        names.push_back(name);
    }
    return names;
}

} // namespace

TEST(SortPlan, BuildPlanWrapsOrderByAfterProjection) {
    auto i64 = std::make_shared<NQumir::NAst::TIntegerType>();
    NQdb::TMockSource source(TMockColumns{}, {{"a", i64}, {"b", i64}});
    auto plan = BuildSqlPlan("SELECT a AS k FROM t ORDER BY k DESC NULLS LAST", source);
    ASSERT_TRUE(plan.has_value()) << (plan ? "" : plan.error().ToString());

    auto sort = TMaybeOp<TSortOperator>(*plan);
    ASSERT_TRUE(sort);
    ASSERT_EQ(sort.Cast()->Keys().size(), 1u);
    EXPECT_EQ(sort.Cast()->Keys()[0].Column, "k");
    EXPECT_EQ(sort.Cast()->Keys()[0].Direction, ESortDirection::Desc);
    EXPECT_EQ(sort.Cast()->Keys()[0].Nulls, ESortNulls::Last);

    auto project = TMaybeOp<TProjectOperator>(sort.Cast()->Input());
    ASSERT_TRUE(project);
    ASSERT_EQ(project.Cast()->Projections().size(), 1u);
    EXPECT_EQ(project.Cast()->Projections()[0].Name, "k");
}

TEST(SortPlan, BuildPlanKeepsLimitAboveSort) {
    auto i64 = std::make_shared<NQumir::NAst::TIntegerType>();
    NQdb::TMockSource source(TMockColumns{}, {{"a", i64}, {"b", i64}});
    auto plan = BuildSqlPlan("SELECT a FROM t ORDER BY a LIMIT 3", source);
    ASSERT_TRUE(plan.has_value()) << (plan ? "" : plan.error().ToString());

    auto limit = TMaybeOp<TLimitOperator>(*plan);
    ASSERT_TRUE(limit);
    EXPECT_EQ(limit.Cast()->Limit(), 3);
    EXPECT_EQ(limit.Cast()->Offset(), 0);
    auto sort = TMaybeOp<TSortOperator>(limit.Cast()->Input());
    ASSERT_TRUE(sort);
    ASSERT_EQ(sort.Cast()->Keys().size(), 1u);
    EXPECT_EQ(sort.Cast()->Keys()[0].Column, "a");
}

TEST(SortPlan, TopSortPassRewritesLimitOverSort) {
    auto i64 = std::make_shared<NQumir::NAst::TIntegerType>();
    NQdb::TMockSource source(TMockColumns{}, {{"a", i64}, {"b", i64}});
    auto plan = BuildSqlPlan("SELECT a FROM t ORDER BY a DESC LIMIT 3", source);
    ASSERT_TRUE(plan.has_value()) << (plan ? "" : plan.error().ToString());

    auto optimized = ApplyTopSort(*plan);

    auto topSort = TMaybeOp<TTopSortOperator>(optimized);
    ASSERT_TRUE(topSort);
    EXPECT_EQ(topSort.Cast()->Limit(), 3);
    ASSERT_EQ(topSort.Cast()->Keys().size(), 1u);
    EXPECT_EQ(topSort.Cast()->Keys()[0].Column, "a");
    EXPECT_EQ(topSort.Cast()->Keys()[0].Direction, ESortDirection::Desc);
    auto project = TMaybeOp<TProjectOperator>(topSort.Cast()->Input());
    ASSERT_TRUE(project);
}

TEST(SortPlan, LimitPushdownEnablesTopSortThroughStripProjection) {
    auto i64 = std::make_shared<NQumir::NAst::TIntegerType>();
    NQdb::TMockSource source(TMockColumns{}, {{"a", i64}, {"b", i64}});
    auto plan = BuildSqlPlan("SELECT a FROM t ORDER BY b LIMIT 3", source);
    ASSERT_TRUE(plan.has_value()) << (plan ? "" : plan.error().ToString());

    ASSERT_TRUE(TMaybeOp<TLimitOperator>(*plan));

    auto pushed = PushDownLimits(*plan);
    auto optimized = ApplyTopSort(pushed);

    auto project = TMaybeOp<TProjectOperator>(optimized);
    ASSERT_TRUE(project);
    auto topSort = TMaybeOp<TTopSortOperator>(project.Cast()->Input());
    ASSERT_TRUE(topSort);
    EXPECT_EQ(topSort.Cast()->Limit(), 3);
}

TEST(SortPlan, LimitPushdownStaysAboveOtherOperators) {
    auto i64 = std::make_shared<NQumir::NAst::TIntegerType>();
    NQdb::TMockSource source(TMockColumns{}, {{"a", i64}});
    auto plan = BuildSqlPlan("SELECT a FROM t", source);
    ASSERT_TRUE(plan.has_value());
    auto filter = MakeFilter(*plan, "(> a 0)");
    ASSERT_TRUE(filter.has_value());

    std::vector<TSortKey> sortKeys{{.Column = "a"}};
    std::vector<TOperatorPtr> barriers = {
        *filter,
        std::make_shared<TJoinOperator>(
            *plan, *plan, std::vector<TJoinKey>{{"a", "a"}}, EJoinType::LeftSemi, nullptr),
        std::make_shared<TAggregateOperator>(
            *plan, std::vector<std::string>{"a"}, std::vector<TAggregateSpec>{}),
        std::make_shared<TSortOperator>(*plan, sortKeys),
        std::make_shared<TTopSortOperator>(*plan, sortKeys, 8),
        std::make_shared<TLimitOperator>((*plan)->Inputs().front(), 7, 1),
        std::make_shared<TUnionAllOperator>(std::vector<TOperatorPtr>{*plan, *plan}),
        std::make_shared<TWindowOperator>(
            *plan, std::vector<std::string>{}, sortKeys, std::nullopt,
            std::vector<TWindowFunc>{{.Name = "r", .Func = "rank"}}),
        std::make_shared<TLateMaterializeOperator>(
            *plan, "a", std::vector<TLateMaterializeColumn>{
                {.PhysicalName = "a", .OutputName = "a", .Type = i64},
            }),
    };

    for (const auto& barrier : barriers) {
        SCOPED_TRACE(barrier->RelName());
        auto limit = std::make_shared<TLimitOperator>(barrier, 3, 2);
        auto optimized = PushDownLimits(limit);
        ASSERT_EQ(optimized, limit);
        EXPECT_EQ(limit->Input(), barrier);
        EXPECT_EQ(limit->Limit(), 3);
        EXPECT_EQ(limit->Offset(), 2);
    }
}

TEST(SortPlan, LimitAndTopSortPassesTraverseUnionUnderTopSort) {
    auto i64 = std::make_shared<NQumir::NAst::TIntegerType>();
    NQdb::TMockSource source(TMockColumns{}, {{"a", i64}, {"b", i64}});
    auto first = BuildSqlPlan("SELECT a FROM t ORDER BY b LIMIT 3", source);
    auto second = BuildSqlPlan("SELECT a FROM t ORDER BY b LIMIT 5", source);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());

    auto unionAll = std::make_shared<TUnionAllOperator>(
        std::vector<TOperatorPtr>{*first, *second});
    auto root = std::make_shared<TTopSortOperator>(
        unionAll, std::vector<TSortKey>{{.Column = "a"}}, 2);
    auto optimized = ApplyTopSort(PushDownLimits(root));

    ASSERT_EQ(optimized, root);
    ASSERT_EQ(root->Input(), unionAll);
    ASSERT_EQ(unionAll->Inputs().size(), 2u);
    std::vector<int64_t> limits;
    for (const auto& input : unionAll->Inputs()) {
        auto project = TMaybeOp<TProjectOperator>(input);
        ASSERT_TRUE(project);
        auto topSort = TMaybeOp<TTopSortOperator>(project.Cast()->Input());
        ASSERT_TRUE(topSort);
        ASSERT_EQ(topSort.Cast()->Keys().size(), 1u);
        EXPECT_EQ(topSort.Cast()->Keys()[0].Column, "__sort_0");
        limits.push_back(topSort.Cast()->Limit());
    }
    EXPECT_EQ(limits, (std::vector<int64_t>{3, 5}));
}

TEST(SortPlan, QualifyColumnsUpdatesTopSortKeys) {
    auto i64 = std::make_shared<NQumir::NAst::TIntegerType>();
    NQdb::TMockSource source(TMockColumns{}, {{"s_name", i64}});
    auto sourceOp = std::make_shared<TSourceOperator>(source, "supplier.parquet");
    auto aggregate = std::make_shared<TAggregateOperator>(
        sourceOp,
        std::vector<std::string>{"s_name"},
        std::vector<TAggregateSpec>{{.Name = "numwait", .Func = "count"}});
    auto topSort = std::make_shared<TTopSortOperator>(
        aggregate,
        std::vector<TSortKey>{
            {.Column = "numwait", .Direction = ESortDirection::Desc},
            {.Column = "s_name", .Direction = ESortDirection::Asc},
        },
        100);

    AssignSourceAliases(topSort);
    QualifyColumns(topSort);

    ASSERT_EQ(aggregate->GroupKeys().size(), 1u);
    EXPECT_EQ(aggregate->GroupKeys()[0], "supplier.s_name");
    ASSERT_EQ(topSort->Keys().size(), 2u);
    EXPECT_EQ(topSort->Keys()[0].Column, "numwait");
    EXPECT_EQ(topSort->Keys()[1].Column, "supplier.s_name");
}

TEST(SortPlan, BuildPlanSortsOrderByExpressionViaHiddenColumn) {
    auto i64 = std::make_shared<NQumir::NAst::TIntegerType>();
    NQdb::TMockSource source(TMockColumns{}, {{"a", i64}, {"b", i64}});
    auto plan = BuildSqlPlan("SELECT a FROM t ORDER BY a + b", source);
    ASSERT_TRUE(plan.has_value()) << (plan ? "" : plan.error().ToString());

    // strip projection restores the user-visible output (just "a")
    auto strip = TMaybeOp<TProjectOperator>(*plan);
    ASSERT_TRUE(strip);
    ASSERT_EQ(strip.Cast()->Projections().size(), 1u);
    EXPECT_EQ(strip.Cast()->Projections()[0].Name, "a");

    // sort keys on the synthesized hidden column
    auto sort = TMaybeOp<TSortOperator>(strip.Cast()->Input());
    ASSERT_TRUE(sort);
    ASSERT_EQ(sort.Cast()->Keys().size(), 1u);
    EXPECT_EQ(sort.Cast()->Keys()[0].Column, "__sort_0");

    // the expression lives in the projection below the sort as __sort_0
    auto project = TMaybeOp<TProjectOperator>(sort.Cast()->Input());
    ASSERT_TRUE(project);
    ASSERT_EQ(project.Cast()->Projections().size(), 2u);
    EXPECT_EQ(project.Cast()->Projections()[1].Name, "__sort_0");
}

TEST(SortPlan, QualifyResolvesHiddenSortColumnToGroupKey) {
    // Binding recognizes the same input column behind both the GROUP BY key
    // and the SELECT alias, so ORDER BY can reuse that selected output.
    auto i64 = std::make_shared<NQumir::NAst::TIntegerType>();
    NQdb::TMockSource source(TMockColumns{}, {{"a", i64}, {"b", i64}});
    auto plan = BuildSqlPlan(
        "SELECT d1.a AS x, count(*) AS c FROM t d1 GROUP BY d1.a ORDER BY a", source);
    ASSERT_TRUE(plan.has_value()) << (plan ? "" : plan.error().ToString());

    AssignSourceAliases(*plan);
    QualifyColumns(*plan);

    auto sort = TMaybeOp<TSortOperator>(*plan);
    ASSERT_TRUE(sort);
    ASSERT_EQ(sort.Cast()->Keys().size(), 1);
    EXPECT_EQ(sort.Cast()->Keys()[0].Column, "x");
    auto project = TMaybeOp<TProjectOperator>(sort.Cast()->Input());
    ASSERT_TRUE(project);
    ASSERT_EQ(project.Cast()->Projections().size(), 2);
    EXPECT_EQ(project.Cast()->Projections()[0].Name, "x");
    EXPECT_EQ(NQumir::NAst::NCore::PrintAst(project.Cast()->Projections()[0].Expression), "|d1.a|");
}

TEST(SortPlan, GroupByExpressionMaterializesKeyAndSubstitutesRefs) {
    auto i64 = std::make_shared<NQumir::NAst::TIntegerType>();
    NQdb::TMockSource source(TMockColumns{}, {{"a", i64}, {"b", i64}});
    auto plan = BuildSqlPlan(
        "SELECT a + b AS k, count(*) AS c FROM t GROUP BY a + b HAVING a + b > 5", source);
    ASSERT_TRUE(plan.has_value()) << (plan ? "" : plan.error().ToString());

    // project -> filter(having) -> aggregate -> project(materialized key)
    auto topProject = TMaybeOp<TProjectOperator>(*plan);
    ASSERT_TRUE(topProject);
    EXPECT_EQ(NQumir::NAst::NCore::PrintAst(topProject.Cast()->Projections()[0].Expression), "gb_0");

    auto having = TMaybeOp<TFilterOperator>(topProject.Cast()->Input());
    ASSERT_TRUE(having);
    EXPECT_EQ(NQumir::NAst::NCore::PrintAst(having.Cast()->Predicate()), "(> gb_0 5)");

    auto aggregate = TMaybeOp<TAggregateOperator>(having.Cast()->Input());
    ASSERT_TRUE(aggregate);
    EXPECT_EQ(aggregate.Cast()->GroupKeys(), (std::vector<std::string>{"gb_0"}));

    auto keyProject = TMaybeOp<TProjectOperator>(aggregate.Cast()->Input());
    ASSERT_TRUE(keyProject);
    EXPECT_EQ(keyProject.Cast()->Projections()[0].Name, "gb_0");
    EXPECT_EQ(NQumir::NAst::NCore::PrintAst(keyProject.Cast()->Projections()[0].Expression), "(+ |t.a| |t.b|)");

    // full name/type resolution succeeds: no dangling base columns above the aggregate
    AssignSourceAliases(*plan);
    QualifyColumns(*plan);
    EXPECT_NO_THROW(AnnotateTypes(*plan));
}

TEST(SortPlan, GroupBySelectAliasMaterializesAliasedExpression) {
    auto i64 = std::make_shared<NQumir::NAst::TIntegerType>();
    NQdb::TMockSource source(TMockColumns{}, {{"a", i64}, {"b", i64}});
    auto plan = BuildSqlPlan(
        "SELECT a + b AS k, count(*) AS c FROM t src GROUP BY k", source);
    ASSERT_TRUE(plan.has_value()) << (plan ? "" : plan.error().ToString());

    auto topProject = TMaybeOp<TProjectOperator>(*plan);
    ASSERT_TRUE(topProject);
    EXPECT_EQ(
        NQumir::NAst::NCore::PrintAst(
            topProject.Cast()->Projections()[0].Expression),
        "k");

    auto aggregate = TMaybeOp<TAggregateOperator>(topProject.Cast()->Input());
    ASSERT_TRUE(aggregate);
    EXPECT_EQ(aggregate.Cast()->GroupKeys(), (std::vector<std::string>{"k"}));

    auto keyProject = TMaybeOp<TProjectOperator>(aggregate.Cast()->Input());
    ASSERT_TRUE(keyProject);
    ASSERT_FALSE(keyProject.Cast()->Projections().empty());
    EXPECT_EQ(keyProject.Cast()->Projections()[0].Name, "k");
    EXPECT_EQ(
        NQumir::NAst::NCore::PrintAst(
            keyProject.Cast()->Projections()[0].Expression),
        "(+ |src.a| |src.b|)");

    AssignSourceAliases(*plan);
    QualifyColumns(*plan);
    EXPECT_NO_THROW(AnnotateTypes(*plan));
}

TEST(SortPlan, GroupByColumnAliasUsesInputColumnDirectly) {
    auto i64 = std::make_shared<NQumir::NAst::TIntegerType>();
    NQdb::TMockSource source(TMockColumns{}, {{"a", i64}});
    auto plan = BuildSqlPlan(
        "SELECT a AS k, count(*) AS c FROM t GROUP BY k", source);
    ASSERT_TRUE(plan.has_value()) << (plan ? "" : plan.error().ToString());

    auto topProject = TMaybeOp<TProjectOperator>(*plan);
    ASSERT_TRUE(topProject);
    auto aggregate = TMaybeOp<TAggregateOperator>(topProject.Cast()->Input());
    ASSERT_TRUE(aggregate);
    EXPECT_EQ(aggregate.Cast()->GroupKeys(), (std::vector<std::string>{"t.a"}));
    EXPECT_TRUE(TMaybeOp<TSourceOperator>(aggregate.Cast()->Input()));

    AssignSourceAliases(*plan);
    QualifyColumns(*plan);
    EXPECT_NO_THROW(AnnotateTypes(*plan));
}

TEST(SortPlan, GroupByDropsConstantFromCompositeKey) {
    auto i64 = std::make_shared<NQumir::NAst::TIntegerType>();
    NQdb::TMockSource source(TMockColumns{}, {{"url", i64}});
    auto plan = BuildSqlPlan(
        "SELECT 1, url, count(*) AS c FROM t GROUP BY 1, url", source);
    ASSERT_TRUE(plan.has_value()) << (plan ? "" : plan.error().ToString());

    auto topProject = TMaybeOp<TProjectOperator>(*plan);
    ASSERT_TRUE(topProject);
    EXPECT_EQ(
        NQumir::NAst::NCore::PrintAst(
            topProject.Cast()->Projections()[0].Expression),
        "1");

    auto aggregate = TMaybeOp<TAggregateOperator>(topProject.Cast()->Input());
    ASSERT_TRUE(aggregate);
    EXPECT_EQ(aggregate.Cast()->GroupKeys(), (std::vector<std::string>{"t.url"}));
    EXPECT_TRUE(TMaybeOp<TSourceOperator>(aggregate.Cast()->Input()));

    AssignSourceAliases(*plan);
    QualifyColumns(*plan);
    EXPECT_NO_THROW(AnnotateTypes(*plan));
}

TEST(SortPlan, GroupByDropsAffineKeysDeterminedByRetainedColumn) {
    auto i64 = std::make_shared<NQumir::NAst::TIntegerType>();
    NQdb::TMockSource source(TMockColumns{}, {{"client_ip", i64}});
    auto plan = BuildSqlPlan(
        "SELECT client_ip, client_ip - 1, client_ip - 2, client_ip - 3, "
        "count(*) AS c FROM t GROUP BY client_ip, client_ip - 1, "
        "client_ip - 2, client_ip - 3",
        source);
    ASSERT_TRUE(plan.has_value()) << (plan ? "" : plan.error().ToString());

    auto topProject = TMaybeOp<TProjectOperator>(*plan);
    ASSERT_TRUE(topProject);
    EXPECT_EQ(
        NQumir::NAst::NCore::PrintAst(
            topProject.Cast()->Projections()[1].Expression),
        "(- |t.client_ip| 1)");

    auto aggregate = TMaybeOp<TAggregateOperator>(topProject.Cast()->Input());
    ASSERT_TRUE(aggregate);
    EXPECT_EQ(
        aggregate.Cast()->GroupKeys(),
        (std::vector<std::string>{"t.client_ip"}));
    EXPECT_TRUE(TMaybeOp<TSourceOperator>(aggregate.Cast()->Input()));

    AssignSourceAliases(*plan);
    QualifyColumns(*plan);
    EXPECT_NO_THROW(AnnotateTypes(*plan));
}

TEST(SortPlan, GroupByKeepsAffineKeyWithoutItsBaseKey) {
    auto i64 = std::make_shared<NQumir::NAst::TIntegerType>();
    NQdb::TMockSource source(TMockColumns{}, {{"client_ip", i64}});
    auto plan = BuildSqlPlan(
        "SELECT client_ip - 1, count(*) AS c FROM t GROUP BY client_ip - 1",
        source);
    ASSERT_TRUE(plan.has_value()) << (plan ? "" : plan.error().ToString());

    auto topProject = TMaybeOp<TProjectOperator>(*plan);
    ASSERT_TRUE(topProject);
    auto aggregate = TMaybeOp<TAggregateOperator>(topProject.Cast()->Input());
    ASSERT_TRUE(aggregate);
    EXPECT_EQ(aggregate.Cast()->GroupKeys(), (std::vector<std::string>{"gb_0"}));
    EXPECT_TRUE(TMaybeOp<TProjectOperator>(aggregate.Cast()->Input()));
}

TEST(SortPlan, ColumnPruningKeepsSortKeyColumns) {
    using namespace NQumir::NAst;

    auto i64 = std::make_shared<TIntegerType>();
    NQdb::TMockSource source(TMockColumns{}, {{"a", i64}, {"b", i64}, {"c", i64}});
    auto sourceOp = std::make_shared<TSourceOperator>(source, "t");
    auto sort = std::make_shared<TSortOperator>(sourceOp, std::vector<TSortKey>{
        {.Column = "b", .Direction = ESortDirection::Asc},
    });
    auto project = std::make_shared<TProjectOperator>(sort, std::vector<TProjectionSpec>{
        {.Name = "a", .Expression = Ident("a")},
    });

    ApplyColumnPruning(project);

    EXPECT_EQ(FieldNames(sourceOp->OutputColumns()), (std::vector<std::string>{"a", "b"}));
    EXPECT_EQ(FieldNames(sort->RequiredColumns()), (std::vector<std::string>{"a", "b"}));
}

TEST(SortExec, SortsStringAndNumericKeysAcrossBatches) {
    using namespace NQumir::NAst;

    std::string data1 = "bobalice";
    int64_t offsets1[] = {0, 3, 8};
    int64_t values1[] = {2, 1};
    std::vector<TColumn> columns1;
    TRowSet batch1 = MakeStringI64Batch(data1, offsets1, values1, 2, columns1);

    std::string data2 = "carolalice";
    int64_t offsets2[] = {0, 5, 10};
    int64_t values2[] = {3, 4};
    std::vector<TColumn> columns2;
    TRowSet batch2 = MakeStringI64Batch(data2, offsets2, values2, 2, columns2);

    auto str = std::make_shared<TStringType>();
    auto i64 = std::make_shared<TIntegerType>();
    NQdb::TMockSource source(
        TMockColumns{},
        {{"name", str}, {"score", i64}},
        {batch1, batch2});

    auto sourceOp = std::make_shared<TSourceOperator>(source, "t");
    auto root = std::make_shared<TSortOperator>(sourceOp, std::vector<TSortKey>{
        {.Column = "name", .Direction = ESortDirection::Asc},
        {.Column = "score", .Direction = ESortDirection::Desc},
    });

    auto runtime = RunPlan(root);

    TRowSet out{};
    ASSERT_TRUE(runtime->Next(out));
    ASSERT_EQ(out.RowCount, 4);
    EXPECT_EQ(StringCell(out.Columns[0], 0), "alice");
    EXPECT_EQ(reinterpret_cast<int64_t*>(out.Columns[1].Data)[0], 4);
    EXPECT_EQ(StringCell(out.Columns[0], 1), "alice");
    EXPECT_EQ(reinterpret_cast<int64_t*>(out.Columns[1].Data)[1], 1);
    EXPECT_EQ(StringCell(out.Columns[0], 2), "bob");
    EXPECT_EQ(StringCell(out.Columns[0], 3), "carol");
    Release(&out);

    TRowSet second{};
    EXPECT_FALSE(runtime->Next(second));
}

TEST(SortExec, SortsCompositeNumericKeysWithFusedRadixKernel) {
    using namespace NQumir::NAst;

    int64_t a1[] = {2, 1, 2};
    int64_t b1[] = {10, 5, 30};
    std::vector<TColumn> columns1;
    TRowSet batch1 = MakeI64I64Batch(a1, b1, 3, columns1);

    int64_t a2[] = {1, 2};
    int64_t b2[] = {7, 20};
    std::vector<TColumn> columns2;
    TRowSet batch2 = MakeI64I64Batch(a2, b2, 2, columns2);

    auto i64 = std::make_shared<TIntegerType>();
    NQdb::TMockSource source(
        TMockColumns{},
        {{"a", i64}, {"b", i64}},
        {batch1, batch2});

    auto sourceOp = std::make_shared<TSourceOperator>(source, "t");
    auto root = std::make_shared<TSortOperator>(sourceOp, std::vector<TSortKey>{
        {.Column = "a", .Direction = ESortDirection::Asc},
        {.Column = "b", .Direction = ESortDirection::Desc},
    });

    auto runtime = RunPlan(root);

    TRowSet out{};
    ASSERT_TRUE(runtime->Next(out));
    ASSERT_EQ(out.RowCount, 5);
    auto* outA = reinterpret_cast<int64_t*>(out.Columns[0].Data);
    auto* outB = reinterpret_cast<int64_t*>(out.Columns[1].Data);
    EXPECT_EQ(std::vector<int64_t>(outA, outA + out.RowCount),
        (std::vector<int64_t>{1, 1, 2, 2, 2}));
    EXPECT_EQ(std::vector<int64_t>(outB, outB + out.RowCount),
        (std::vector<int64_t>{7, 5, 30, 20, 10}));
    Release(&out);
}

TEST(SortExec, SchedulerRuntimeRunsSortTail) {
    using namespace NQumir::NAst;

    int64_t a1[] = {2, 1, 2};
    int64_t b1[] = {10, 5, 30};
    std::vector<TColumn> columns1;
    TRowSet batch1 = MakeI64I64Batch(a1, b1, 3, columns1);

    int64_t a2[] = {1, 2};
    int64_t b2[] = {7, 20};
    std::vector<TColumn> columns2;
    TRowSet batch2 = MakeI64I64Batch(a2, b2, 2, columns2);

    auto i64 = std::make_shared<TIntegerType>();
    NQdb::TMockSource source(
        TMockColumns{},
        {{"a", i64}, {"b", i64}},
        {batch1, batch2});

    auto sourceOp = std::make_shared<TSourceOperator>(source, "t");
    auto root = std::make_shared<TSortOperator>(sourceOp, std::vector<TSortKey>{
        {.Column = "a", .Direction = ESortDirection::Asc},
        {.Column = "b", .Direction = ESortDirection::Desc},
    });
    NScheduler::TSettings settings;
    settings.Scheduler.Mode = NScheduler::EExecutionMode::ThreadedScheduler;
    settings.Scheduler.WorkerCount = 2;

    auto runtime = RunPlan(root, settings);

    TRowSet out{};
    ASSERT_TRUE(runtime->Next(out));
    ASSERT_EQ(out.RowCount, 5);
    auto* outA = reinterpret_cast<int64_t*>(out.Columns[0].Data);
    auto* outB = reinterpret_cast<int64_t*>(out.Columns[1].Data);
    EXPECT_EQ(std::vector<int64_t>(outA, outA + out.RowCount),
        (std::vector<int64_t>{1, 1, 2, 2, 2}));
    EXPECT_EQ(std::vector<int64_t>(outB, outB + out.RowCount),
        (std::vector<int64_t>{7, 5, 30, 20, 10}));
    Release(&out);
}

TEST(SortExec, TopSortReturnsLimitFromOneBatch) {
    using namespace NQumir::NAst;

    int64_t keys[] = {5, 1, 3, 2};
    int64_t payload[] = {50, 10, 30, 20};
    std::vector<TColumn> columns;
    TRowSet batch = MakeI64I64Batch(keys, payload, 4, columns);

    auto i64 = std::make_shared<TIntegerType>();
    NQdb::TMockSource source(
        TMockColumns{},
        {{"k", i64}, {"payload", i64}},
        {batch});
    auto plan = BuildSqlPlan("SELECT k, payload FROM t ORDER BY k LIMIT 2", source);
    ASSERT_TRUE(plan.has_value()) << (plan ? "" : plan.error().ToString());
    auto optimized = ApplyTopSort(*plan);
    ASSERT_TRUE(TMaybeOp<TTopSortOperator>(optimized));

    auto runtime = RunPlan(optimized);

    TRowSet out{};
    ASSERT_TRUE(runtime->Next(out));
    ASSERT_EQ(out.RowCount, 2);
    auto* outK = reinterpret_cast<int64_t*>(out.Columns[0].Data);
    auto* outPayload = reinterpret_cast<int64_t*>(out.Columns[1].Data);
    EXPECT_EQ(std::vector<int64_t>(outK, outK + out.RowCount),
        (std::vector<int64_t>{1, 2}));
    EXPECT_EQ(std::vector<int64_t>(outPayload, outPayload + out.RowCount),
        (std::vector<int64_t>{10, 20}));
    Release(&out);
    TRowSet second{};
    EXPECT_FALSE(runtime->Next(second));
}

TEST(SortExec, SchedulerRuntimeRunsTopSortTail) {
    using namespace NQumir::NAst;

    int64_t keys[] = {5, 1, 3, 2};
    int64_t payload[] = {50, 10, 30, 20};
    std::vector<TColumn> columns;
    TRowSet batch = MakeI64I64Batch(keys, payload, 4, columns);

    auto i64 = std::make_shared<TIntegerType>();
    NQdb::TMockSource source(
        TMockColumns{},
        {{"k", i64}, {"payload", i64}},
        {batch});
    auto sourceOp = std::make_shared<TSourceOperator>(source, "t");
    auto root = std::make_shared<TTopSortOperator>(
        sourceOp,
        std::vector<TSortKey>{
            {.Column = "k", .Direction = ESortDirection::Asc},
        },
        2);
    NScheduler::TSettings settings;
    settings.Scheduler.Mode = NScheduler::EExecutionMode::ThreadedScheduler;
    settings.Scheduler.WorkerCount = 2;

    auto runtime = RunPlan(root, settings);

    TRowSet out{};
    ASSERT_TRUE(runtime->Next(out));
    ASSERT_EQ(out.RowCount, 2);
    auto* outK = reinterpret_cast<int64_t*>(out.Columns[0].Data);
    auto* outPayload = reinterpret_cast<int64_t*>(out.Columns[1].Data);
    EXPECT_EQ(std::vector<int64_t>(outK, outK + out.RowCount),
        (std::vector<int64_t>{1, 2}));
    EXPECT_EQ(std::vector<int64_t>(outPayload, outPayload + out.RowCount),
        (std::vector<int64_t>{10, 20}));
    Release(&out);
}

TEST(SortExec, TopSortHonorsDescDirection) {
    using namespace NQumir::NAst;

    int64_t keys[] = {5, 1, 3, 2};
    int64_t payload[] = {50, 10, 30, 20};
    std::vector<TColumn> columns;
    TRowSet batch = MakeI64I64Batch(keys, payload, 4, columns);

    auto i64 = std::make_shared<TIntegerType>();
    NQdb::TMockSource source(
        TMockColumns{},
        {{"k", i64}, {"payload", i64}},
        {batch});
    auto plan = BuildSqlPlan("SELECT k, payload FROM t ORDER BY k DESC LIMIT 2", source);
    ASSERT_TRUE(plan.has_value()) << (plan ? "" : plan.error().ToString());
    auto optimized = ApplyTopSort(*plan);
    ASSERT_TRUE(TMaybeOp<TTopSortOperator>(optimized));

    auto runtime = RunPlan(optimized);

    TRowSet out{};
    ASSERT_TRUE(runtime->Next(out));
    ASSERT_EQ(out.RowCount, 2);
    auto* outK = reinterpret_cast<int64_t*>(out.Columns[0].Data);
    auto* outPayload = reinterpret_cast<int64_t*>(out.Columns[1].Data);
    EXPECT_EQ(std::vector<int64_t>(outK, outK + out.RowCount),
        (std::vector<int64_t>{5, 3}));
    EXPECT_EQ(std::vector<int64_t>(outPayload, outPayload + out.RowCount),
        (std::vector<int64_t>{50, 30}));
    Release(&out);
}

TEST(SortExec, TopSortMergesBatchesStably) {
    using namespace NQumir::NAst;

    int64_t k1[] = {1, 2, 2};
    int64_t p1[] = {10, 20, 21};
    std::vector<TColumn> columns1;
    TRowSet batch1 = MakeI64I64Batch(k1, p1, 3, columns1);

    int64_t k2[] = {0, 2, 1};
    int64_t p2[] = {0, 22, 11};
    std::vector<TColumn> columns2;
    TRowSet batch2 = MakeI64I64Batch(k2, p2, 3, columns2);

    auto i64 = std::make_shared<TIntegerType>();
    NQdb::TMockSource source(
        TMockColumns{},
        {{"k", i64}, {"payload", i64}},
        {batch1, batch2});
    auto plan = BuildSqlPlan("SELECT k, payload FROM t ORDER BY k LIMIT 5", source);
    ASSERT_TRUE(plan.has_value()) << (plan ? "" : plan.error().ToString());
    auto optimized = ApplyTopSort(*plan);
    ASSERT_TRUE(TMaybeOp<TTopSortOperator>(optimized));

    auto runtime = RunPlan(optimized);

    TRowSet out{};
    ASSERT_TRUE(runtime->Next(out));
    ASSERT_EQ(out.RowCount, 5);
    auto* outK = reinterpret_cast<int64_t*>(out.Columns[0].Data);
    auto* outPayload = reinterpret_cast<int64_t*>(out.Columns[1].Data);
    EXPECT_EQ(std::vector<int64_t>(outK, outK + out.RowCount),
        (std::vector<int64_t>{0, 1, 1, 2, 2}));
    EXPECT_EQ(std::vector<int64_t>(outPayload, outPayload + out.RowCount),
        (std::vector<int64_t>{0, 10, 11, 20, 21}));
    Release(&out);
}

TEST(SortExec, HeapTopSortMergesReducedBatchesStably) {
    using namespace NQumir::NAst;

    constexpr int64_t batchRows = 128;
    constexpr int64_t limit = 7;
    std::vector<int64_t> keys1(batchRows);
    std::vector<int64_t> payload1(batchRows);
    std::vector<int64_t> keys2(batchRows);
    std::vector<int64_t> payload2(batchRows);
    for (int64_t row = 0; row < batchRows; ++row) {
        keys1[row] = (row * 37) % 23;
        payload1[row] = row;
        keys2[row] = ((row + 11) * 29) % 23;
        payload2[row] = batchRows + row;
    }

    std::vector<TColumn> columns1;
    TRowSet batch1 = MakeI64I64Batch(
        keys1.data(), payload1.data(), batchRows, columns1);
    std::vector<TColumn> columns2;
    TRowSet batch2 = MakeI64I64Batch(
        keys2.data(), payload2.data(), batchRows, columns2);

    auto i64 = std::make_shared<TIntegerType>();
    NQdb::TMockSource source(
        TMockColumns{},
        {{"k", i64}, {"payload", i64}},
        {batch1, batch2});
    auto sourceOp = std::make_shared<TSourceOperator>(source, "t");
    auto root = std::make_shared<TTopSortOperator>(
        sourceOp,
        std::vector<TSortKey>{
            {.Column = "k", .Direction = ESortDirection::Asc},
        },
        limit);

    auto runtime = RunPlan(root);

    std::vector<int64_t> expected(batchRows * 2);
    std::iota(expected.begin(), expected.end(), 0);
    auto keyAt = [&](int64_t row) {
        return row < batchRows
            ? keys1[row]
            : keys2[row - batchRows];
    };
    std::stable_sort(expected.begin(), expected.end(),
        [&](int64_t left, int64_t right) {
            return keyAt(left) < keyAt(right);
        });
    expected.resize(limit);

    TRowSet out{};
    ASSERT_TRUE(runtime->Next(out));
    ASSERT_EQ(out.RowCount, limit);
    auto* outPayload = reinterpret_cast<int64_t*>(out.Columns[1].Data);
    EXPECT_EQ(
        std::vector<int64_t>(outPayload, outPayload + out.RowCount), expected);
    Release(&out);
}

TEST(SortExec, TopSortHandlesNullableNumericKeys) {
    using namespace NQumir::NAst;

    int64_t keys[] = {30, 1000, 10, -999, 20};
    int64_t payload[] = {1, 2, 3, 4, 5};
    uint8_t keyMask[] = {0b00010101}; // rows 1 and 3 are NULL.
    std::vector<TColumn> columns = {
        TColumn{.Data = reinterpret_cast<char*>(keys), .Mask = keyMask},
        TColumn{.Data = reinterpret_cast<char*>(payload)},
    };
    TRowSet batch{
        .Columns = columns.data(),
        .ColumnCount = 2,
        .RowCount = 5,
        .RefCount = 1,
    };

    auto i64 = std::make_shared<TIntegerType>();
    NQdb::TMockSource source(
        TMockColumns{},
        {{"k", std::make_shared<TNullable>(i64)}, {"payload", i64}},
        {batch});
    auto plan = BuildSqlPlan(
        "SELECT k, payload FROM t ORDER BY k NULLS FIRST LIMIT 3", source);
    ASSERT_TRUE(plan.has_value()) << (plan ? "" : plan.error().ToString());
    auto optimized = ApplyTopSort(*plan);
    ASSERT_TRUE(TMaybeOp<TTopSortOperator>(optimized));

    auto runtime = RunPlan(optimized);

    TRowSet out{};
    ASSERT_TRUE(runtime->Next(out));
    ASSERT_EQ(out.RowCount, 3);
    ASSERT_NE(out.Columns[0].Mask, nullptr);
    auto* outPayload = reinterpret_cast<int64_t*>(out.Columns[1].Data);
    EXPECT_EQ(std::vector<int64_t>(outPayload, outPayload + out.RowCount),
        (std::vector<int64_t>{2, 4, 3}));
    EXPECT_FALSE(MaskBit(out.Columns[0], 0));
    EXPECT_FALSE(MaskBit(out.Columns[0], 1));
    EXPECT_TRUE(MaskBit(out.Columns[0], 2));
    Release(&out);
}

TEST(SortExec, EmptyInputProducesNoRows) {
    using namespace NQumir::NAst;

    auto i64 = std::make_shared<TIntegerType>();
    NQdb::TMockSource source(TMockColumns{}, {{"a", i64}}, {});

    auto sourceOp = std::make_shared<TSourceOperator>(source, "t");
    auto root = std::make_shared<TSortOperator>(sourceOp, std::vector<TSortKey>{
        {.Column = "a", .Direction = ESortDirection::Asc},
    });

    auto runtime = RunPlan(root);

    TRowSet out{};
    EXPECT_FALSE(runtime->Next(out));
}

TEST(SortExec, AllEqualNumericKeysKeepInputOrderWithRadixKernel) {
    using namespace NQumir::NAst;

    int64_t keys[] = {7, 7, 7, 7};
    int64_t payload[] = {10, 20, 30, 40};
    std::vector<TColumn> columns;
    TRowSet batch = MakeI64I64Batch(keys, payload, 4, columns);

    auto i64 = std::make_shared<TIntegerType>();
    NQdb::TMockSource source(
        TMockColumns{},
        {{"k", i64}, {"payload", i64}},
        {batch});

    auto sourceOp = std::make_shared<TSourceOperator>(source, "t");
    auto root = std::make_shared<TSortOperator>(sourceOp, std::vector<TSortKey>{
        {.Column = "k", .Direction = ESortDirection::Desc},
    });

    auto runtime = RunPlan(root);

    TRowSet out{};
    ASSERT_TRUE(runtime->Next(out));
    ASSERT_EQ(out.RowCount, 4);
    auto* outPayload = reinterpret_cast<int64_t*>(out.Columns[1].Data);
    EXPECT_EQ(std::vector<int64_t>(outPayload, outPayload + out.RowCount),
        (std::vector<int64_t>{10, 20, 30, 40}));
    Release(&out);
}

TEST(SortExec, ProjectionAfterSortSeesSortedRows) {
    using namespace NQumir::NAst;

    int64_t a[] = {100, 200, 300, 400};
    int64_t b[] = {3, 1, 4, 2};
    std::vector<TColumn> columns;
    TRowSet batch = MakeI64I64Batch(a, b, 4, columns);

    auto i64 = std::make_shared<TIntegerType>();
    NQdb::TMockSource source(
        TMockColumns{},
        {{"a", i64}, {"b", i64}},
        {batch});

    auto sourceOp = std::make_shared<TSourceOperator>(source, "t");
    auto sort = std::make_shared<TSortOperator>(sourceOp, std::vector<TSortKey>{
        {.Column = "b", .Direction = ESortDirection::Asc},
    });
    auto project = std::make_shared<TProjectOperator>(sort, std::vector<TProjectionSpec>{
        {.Name = "a", .Expression = Ident("a")},
    });

    auto runtime = RunPlan(project);

    TRowSet out{};
    ASSERT_TRUE(runtime->Next(out));
    ASSERT_EQ(out.ColumnCount, 1);
    ASSERT_EQ(out.RowCount, 4);
    auto* outA = reinterpret_cast<int64_t*>(out.Columns[0].Data);
    EXPECT_EQ(std::vector<int64_t>(outA, outA + out.RowCount),
        (std::vector<int64_t>{200, 400, 100, 300}));
    Release(&out);
}

TEST(SortExec, SortAfterAggregate) {
    using namespace NQumir::NAst;

    int64_t keys[] = {2, 1, 2, 3, 1};
    int64_t values[] = {10, 5, 7, 4, 6};
    std::vector<TColumn> columns;
    TRowSet batch = MakeI64I64Batch(keys, values, 5, columns);

    auto i64 = std::make_shared<TIntegerType>();
    NQdb::TMockSource source(
        TMockColumns{},
        {{"k", i64}, {"v", i64}},
        {batch});

    auto sourceOp = std::make_shared<TSourceOperator>(source, "t");
    auto aggregate = std::make_shared<TAggregateOperator>(
        sourceOp,
        std::vector<std::string>{"k"},
        std::vector<TAggregateSpec>{{
            .Name = "s",
            .Func = "sum",
            .Arg = Ident("v"),
        }});
    auto sort = std::make_shared<TSortOperator>(aggregate, std::vector<TSortKey>{
        {.Column = "s", .Direction = ESortDirection::Desc},
    });

    auto runtime = RunPlan(sort);

    TRowSet out{};
    ASSERT_TRUE(runtime->Next(out));
    ASSERT_EQ(out.ColumnCount, 2);
    ASSERT_EQ(out.RowCount, 3);
    auto* outK = reinterpret_cast<int64_t*>(out.Columns[0].Data);
    auto* outS = reinterpret_cast<int64_t*>(out.Columns[1].Data);
    EXPECT_EQ(std::vector<int64_t>(outK, outK + out.RowCount),
        (std::vector<int64_t>{2, 1, 3}));
    EXPECT_EQ(std::vector<int64_t>(outS, outS + out.RowCount),
        (std::vector<int64_t>{17, 11, 4}));
    Release(&out);
}

TEST(SortExec, MaterializesNullableDecimalPayload) {
    using namespace NQumir::NAst;

    int64_t keys[] = {2, 1, 3};
    qdb_bin_int amounts[] = {
        {.Lo = 2000, .Hi = 0},
        {.Lo = 9999, .Hi = 9999}, // NULL by mask; output value is zeroed.
        {.Lo = 3000, .Hi = 0},
    };
    uint8_t amountMask[] = {0b00000101};
    std::vector<TColumn> columns = {
        TColumn{.Data = reinterpret_cast<char*>(keys)},
        TColumn{.Data = reinterpret_cast<char*>(amounts), .Mask = amountMask},
    };
    TRowSet batch{
        .Columns = columns.data(),
        .ColumnCount = 2,
        .RowCount = 3,
        .RefCount = 1,
    };

    auto i64 = std::make_shared<TIntegerType>();
    auto decimal = std::make_shared<NQdb::TNullable>(
        std::make_shared<NQdb::TDecimal>(7, 2));
    NQdb::TMockSource source(
        TMockColumns{},
        {{"k", i64}, {"amount", decimal}},
        {batch});

    auto sourceOp = std::make_shared<TSourceOperator>(source, "t");
    auto sort = std::make_shared<TSortOperator>(sourceOp, std::vector<TSortKey>{
        {.Column = "k", .Direction = ESortDirection::Asc},
    });

    auto runtime = RunPlan(sort);

    TRowSet out{};
    ASSERT_TRUE(runtime->Next(out));
    ASSERT_EQ(out.ColumnCount, 2);
    ASSERT_EQ(out.RowCount, 3);
    ASSERT_NE(out.Columns[1].Mask, nullptr);
    auto* outKeys = reinterpret_cast<int64_t*>(out.Columns[0].Data);
    auto* outAmounts = reinterpret_cast<qdb_bin_int*>(out.Columns[1].Data);
    EXPECT_EQ(std::vector<int64_t>(outKeys, outKeys + out.RowCount),
        (std::vector<int64_t>{1, 2, 3}));
    EXPECT_FALSE(MaskBit(out.Columns[1], 0));
    EXPECT_TRUE(MaskBit(out.Columns[1], 1));
    EXPECT_TRUE(MaskBit(out.Columns[1], 2));
    EXPECT_EQ(outAmounts[0].Lo, 0u);
    EXPECT_EQ(outAmounts[0].Hi, 0u);
    EXPECT_EQ(outAmounts[1].Lo, 2000u);
    EXPECT_EQ(outAmounts[2].Lo, 3000u);
    Release(&out);
}

TEST(SortExec, NullableNumericKeysUseRadixNullOrdering) {
    using namespace NQumir::NAst;

    int64_t keys[] = {30, 1000, 10, -999, 20};
    int64_t payload[] = {1, 2, 3, 4, 5};
    uint8_t keyMask[] = {0b00010101}; // rows 1 and 3 are NULL.
    std::vector<TColumn> columns = {
        TColumn{.Data = reinterpret_cast<char*>(keys), .Mask = keyMask},
        TColumn{.Data = reinterpret_cast<char*>(payload)},
    };
    TRowSet batch{
        .Columns = columns.data(),
        .ColumnCount = 2,
        .RowCount = 5,
        .RefCount = 1,
    };

    auto i64 = std::make_shared<TIntegerType>();
    NQdb::TMockSource source(
        TMockColumns{},
        {{"k", std::make_shared<TNullable>(i64)}, {"payload", i64}},
        {batch});

    auto sourceOp = std::make_shared<TSourceOperator>(source, "t");
    auto sort = std::make_shared<TSortOperator>(sourceOp, std::vector<TSortKey>{
        {.Column = "k", .Direction = ESortDirection::Asc, .Nulls = ESortNulls::First},
    });

    auto runtime = RunPlan(sort);

    TRowSet out{};
    ASSERT_TRUE(runtime->Next(out));
    ASSERT_EQ(out.RowCount, 5);
    ASSERT_NE(out.Columns[0].Mask, nullptr);
    auto* outPayload = reinterpret_cast<int64_t*>(out.Columns[1].Data);
    EXPECT_EQ(std::vector<int64_t>(outPayload, outPayload + out.RowCount),
        (std::vector<int64_t>{2, 4, 3, 5, 1}));
    EXPECT_FALSE(MaskBit(out.Columns[0], 0));
    EXPECT_FALSE(MaskBit(out.Columns[0], 1));
    EXPECT_TRUE(MaskBit(out.Columns[0], 2));
    EXPECT_TRUE(MaskBit(out.Columns[0], 3));
    EXPECT_TRUE(MaskBit(out.Columns[0], 4));
    Release(&out);
}

TEST(SortExec, NullableNumericKeysRespectDescNullsLast) {
    using namespace NQumir::NAst;

    int64_t keys[] = {30, 1000, 10, -999, 20};
    int64_t payload[] = {1, 2, 3, 4, 5};
    uint8_t keyMask[] = {0b00010101}; // rows 1 and 3 are NULL.
    std::vector<TColumn> columns = {
        TColumn{.Data = reinterpret_cast<char*>(keys), .Mask = keyMask},
        TColumn{.Data = reinterpret_cast<char*>(payload)},
    };
    TRowSet batch{
        .Columns = columns.data(),
        .ColumnCount = 2,
        .RowCount = 5,
        .RefCount = 1,
    };

    auto i64 = std::make_shared<TIntegerType>();
    NQdb::TMockSource source(
        TMockColumns{},
        {{"k", std::make_shared<TNullable>(i64)}, {"payload", i64}},
        {batch});

    auto sourceOp = std::make_shared<TSourceOperator>(source, "t");
    auto sort = std::make_shared<TSortOperator>(sourceOp, std::vector<TSortKey>{
        {.Column = "k", .Direction = ESortDirection::Desc, .Nulls = ESortNulls::Last},
    });

    auto runtime = RunPlan(sort);

    TRowSet out{};
    ASSERT_TRUE(runtime->Next(out));
    ASSERT_EQ(out.RowCount, 5);
    ASSERT_NE(out.Columns[0].Mask, nullptr);
    auto* outPayload = reinterpret_cast<int64_t*>(out.Columns[1].Data);
    EXPECT_EQ(std::vector<int64_t>(outPayload, outPayload + out.RowCount),
        (std::vector<int64_t>{1, 5, 3, 2, 4}));
    EXPECT_TRUE(MaskBit(out.Columns[0], 0));
    EXPECT_TRUE(MaskBit(out.Columns[0], 1));
    EXPECT_TRUE(MaskBit(out.Columns[0], 2));
    EXPECT_FALSE(MaskBit(out.Columns[0], 3));
    EXPECT_FALSE(MaskBit(out.Columns[0], 4));
    Release(&out);
}

TEST(SortExec, RespectsInputSelection) {
    using namespace NQumir::NAst;

    std::string data = "cab";
    int64_t offsets[] = {0, 1, 2, 3};
    int64_t values[] = {3, 1, 2};
    uint8_t selection[] = {1, 0, 1};
    std::vector<TColumn> columns;
    TRowSet batch = MakeStringI64Batch(data, offsets, values, 3, columns, selection);

    auto str = std::make_shared<TStringType>();
    auto i64 = std::make_shared<TIntegerType>();
    NQdb::TMockSource source(
        TMockColumns{},
        {{"name", str}, {"score", i64}},
        {batch});

    auto sourceOp = std::make_shared<TSourceOperator>(source, "t");
    auto root = std::make_shared<TSortOperator>(sourceOp, std::vector<TSortKey>{
        {.Column = "score", .Direction = ESortDirection::Asc},
    });

    auto runtime = RunPlan(root);

    TRowSet out{};
    ASSERT_TRUE(runtime->Next(out));
    ASSERT_EQ(out.RowCount, 2);
    EXPECT_EQ(reinterpret_cast<int64_t*>(out.Columns[1].Data)[0], 2);
    EXPECT_EQ(reinterpret_cast<int64_t*>(out.Columns[1].Data)[1], 3);
    EXPECT_EQ(StringCell(out.Columns[0], 0), "b");
    EXPECT_EQ(StringCell(out.Columns[0], 1), "c");
    Release(&out);
}

int main(int argc, char** argv) {
    NQumir::NCodeGen::TLLVMInitializer initializer;
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
