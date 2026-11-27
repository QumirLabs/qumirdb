#include <gtest/gtest.h>
#include "mock_source.h"

#include <qdb/io/schema.h>
#include "plan_runner.h"
#include "test_types.h"
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

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/parser/ast.h>
#include <qumir/parser/core/printer.h>
#include <qumir/parser/type.h>

#include <algorithm>
#include <expected>
#include <limits>
#include <memory>
#include <numeric>
#include <sstream>
#include <string_view>
#include <tuple>
#include <vector>

using namespace NQdb;

namespace {



std::expected<TOperatorPtr, NQumir::TError> BuildSqlPlan(std::string_view sql, ITable& source) {
    std::istringstream in{std::string(sql)};
    NSql::TTokenStream ts(in);
    NSql::TParser parser(TestTypes());
    auto parsed = parser.Parse(ts);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }

    auto factory = [&](NQumir::NAst::TTypeFactory& types, std::string_view table)
        -> std::expected<TOperatorPtr, NQumir::TError>
    {
        return NQumir::MakeIntrusive<TSourceOperator>(types, Borrow(source), std::string(table));
    };

    return BuildPlan(TestTypes(), TestAnnotation(), parsed.value(), factory);
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
    return NQumir::MakeIntrusive<NQumir::NAst::TIdentExpr>(
        NQumir::TLocation{}, std::move(name));
}

std::vector<std::string> FieldNames(const NQumir::NAst::TTypePtr& type) {
    auto* st = static_cast<const NQumir::NAst::TStructType*>(type);
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
    auto i64 = TestTypes().MakeInteger();
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
    auto i64 = TestTypes().MakeInteger();
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
    auto i64 = TestTypes().MakeInteger();
    NQdb::TMockSource source(TMockColumns{}, {{"a", i64}, {"b", i64}});
    auto plan = BuildSqlPlan("SELECT a FROM t ORDER BY a DESC LIMIT 3", source);
    ASSERT_TRUE(plan.has_value()) << (plan ? "" : plan.error().ToString());

    auto optimized = ApplyTopSort(TestTypes(), *plan);

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
    auto i64 = TestTypes().MakeInteger();
    NQdb::TMockSource source(TMockColumns{}, {{"a", i64}, {"b", i64}});
    auto plan = BuildSqlPlan("SELECT a FROM t ORDER BY b LIMIT 3", source);
    ASSERT_TRUE(plan.has_value()) << (plan ? "" : plan.error().ToString());

    ASSERT_TRUE(TMaybeOp<TLimitOperator>(*plan));

    auto pushed = PushDownLimits(TestTypes(), *plan);
    auto optimized = ApplyTopSort(TestTypes(), pushed);

    auto project = TMaybeOp<TProjectOperator>(optimized);
    ASSERT_TRUE(project);
    auto topSort = TMaybeOp<TTopSortOperator>(project.Cast()->Input());
    ASSERT_TRUE(topSort);
    EXPECT_EQ(topSort.Cast()->Limit(), 3);
}

TEST(SortPlan, LimitPushdownStaysAboveOtherOperators) {
    auto i64 = TestTypes().MakeInteger();
    NQdb::TMockSource source(TMockColumns{}, {{"a", i64}});
    auto plan = BuildSqlPlan("SELECT a FROM t", source);
    ASSERT_TRUE(plan.has_value());
    auto filter = MakeFilter(TestTypes(), *plan, "(> a 0)");
    ASSERT_TRUE(filter.has_value());

    std::vector<TSortKey> sortKeys{{.Column = "a"}};
    std::vector<TOperatorPtr> barriers = {
        *filter,
        NQumir::MakeIntrusive<TJoinOperator>(TestTypes(),
            *plan, *plan, std::vector<TJoinKey>{{"a", "a"}}, EJoinType::LeftSemi, nullptr),
        NQumir::MakeIntrusive<TAggregateOperator>(TestTypes(),
            *plan, std::vector<std::string>{"a"}, std::vector<TAggregateSpec>{}),
        NQumir::MakeIntrusive<TSortOperator>(TestTypes(), *plan, sortKeys),
        NQumir::MakeIntrusive<TTopSortOperator>(TestTypes(), *plan, sortKeys, 8),
        NQumir::MakeIntrusive<TLimitOperator>(TestTypes(), (*plan)->Inputs().front(), 7, 1),
        NQumir::MakeIntrusive<TUnionAllOperator>(TestTypes(), std::vector<TOperatorPtr>{*plan, *plan}),
        NQumir::MakeIntrusive<TWindowOperator>(TestTypes(),
            *plan, std::vector<std::string>{}, sortKeys, std::nullopt,
            std::vector<TWindowFunc>{{.Name = "r", .Func = "rank"}}),
        NQumir::MakeIntrusive<TLateMaterializeOperator>(TestTypes(),
            *plan, "a", std::vector<TLateMaterializeColumn>{
                {.PhysicalName = "a", .OutputName = "a", .Type = i64},
            }),
    };

    for (const auto& barrier : barriers) {
        SCOPED_TRACE(barrier->RelName());
        auto limit = NQumir::MakeIntrusive<TLimitOperator>(TestTypes(), barrier, 3, 2);
        auto optimized = PushDownLimits(TestTypes(), limit);
        ASSERT_EQ(optimized, limit);
        EXPECT_EQ(limit->Input(), barrier);
        EXPECT_EQ(limit->Limit(), 3);
        EXPECT_EQ(limit->Offset(), 2);
    }
}

TEST(SortPlan, LimitAndTopSortPassesTraverseUnionUnderTopSort) {
    auto i64 = TestTypes().MakeInteger();
    NQdb::TMockSource source(TMockColumns{}, {{"a", i64}, {"b", i64}});
    auto first = BuildSqlPlan("SELECT a FROM t ORDER BY b LIMIT 3", source);
    auto second = BuildSqlPlan("SELECT a FROM t ORDER BY b LIMIT 5", source);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());

    auto unionAll = NQumir::MakeIntrusive<TUnionAllOperator>(TestTypes(),
        std::vector<TOperatorPtr>{*first, *second});
    auto root = NQumir::MakeIntrusive<TTopSortOperator>(TestTypes(),
        unionAll, std::vector<TSortKey>{{.Column = "a"}}, 2);
    auto optimized = ApplyTopSort(TestTypes(), PushDownLimits(TestTypes(), root));

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
    auto i64 = TestTypes().MakeInteger();
    NQdb::TMockSource source(TMockColumns{}, {{"s_name", i64}});
    auto sourceOp = NQumir::MakeIntrusive<TSourceOperator>(TestTypes(), Borrow(source), "supplier.parquet");
    auto aggregate = NQumir::MakeIntrusive<TAggregateOperator>(TestTypes(),
        sourceOp,
        std::vector<std::string>{"s_name"},
        std::vector<TAggregateSpec>{{.Name = "numwait", .Func = "count"}});
    auto topSort = NQumir::MakeIntrusive<TTopSortOperator>(TestTypes(),
        aggregate,
        std::vector<TSortKey>{
            {.Column = "numwait", .Direction = ESortDirection::Desc},
            {.Column = "s_name", .Direction = ESortDirection::Asc},
        },
        100);

    AssignSourceAliases(TestTypes(), topSort);
    QualifyColumns(TestTypes(), topSort);

    ASSERT_EQ(aggregate->GroupKeys().size(), 1u);
    EXPECT_EQ(aggregate->GroupKeys()[0], "supplier.s_name");
    ASSERT_EQ(topSort->Keys().size(), 2u);
    EXPECT_EQ(topSort->Keys()[0].Column, "numwait");
    EXPECT_EQ(topSort->Keys()[1].Column, "supplier.s_name");
}

TEST(SortPlan, BuildPlanSortsOrderByExpressionViaHiddenColumn) {
    auto i64 = TestTypes().MakeInteger();
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

TEST(SortPlan, BuildPlanRejectsOrderByAggregateMissingFromSelectList) {
    auto i64 = TestTypes().MakeInteger();
    NQdb::TMockSource source(TMockColumns{}, {{"a", i64}, {"b", i64}});
    // The item is rejected without lowering its aggregate, so a malformed one
    // reports the same error.
    for (const char* orderBy : {"sum(b) + 1", "sum(DISTINCT b)"}) {
        auto plan = BuildSqlPlan(std::string("SELECT a FROM t GROUP BY a ORDER BY ") + orderBy, source);
        ASSERT_FALSE(plan.has_value()) << orderBy;
        EXPECT_NE(plan.error().ToString().find(
            "ORDER BY on an aggregate not in the select list is not supported"),
            std::string::npos) << plan.error().ToString();
    }
}

TEST(SortPlan, OrderByInputNameReusesSelectedGroupKey) {
    // ORDER BY on an unqualified name of a qualified group key binds as the SELECT
    // list does, so it sorts on the selected output instead of a hidden column.
    auto i64 = TestTypes().MakeInteger();
    NQdb::TMockSource source(TMockColumns{}, {{"a", i64}, {"b", i64}});
    auto plan = BuildSqlPlan(
        "SELECT d1.a AS x, count(*) AS c FROM t d1 GROUP BY d1.a ORDER BY a", source);
    ASSERT_TRUE(plan.has_value()) << (plan ? "" : plan.error().ToString());

    auto sort = TMaybeOp<TSortOperator>(*plan);
    ASSERT_TRUE(sort);
    ASSERT_EQ(sort.Cast()->Keys().size(), 1u);
    EXPECT_EQ(sort.Cast()->Keys()[0].Column, "x");
}

TEST(SortPlan, GroupByExpressionMaterializesKeyAndSubstitutesRefs) {
    auto i64 = TestTypes().MakeInteger();
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
    AssignSourceAliases(TestTypes(), *plan);
    QualifyColumns(TestTypes(), *plan);
    EXPECT_NO_THROW(AnnotateTypes(TestTypes(), *plan, TestAnnotation()));
}

TEST(SortPlan, GroupBySelectAliasMaterializesAliasedExpression) {
    auto i64 = TestTypes().MakeInteger();
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

    AssignSourceAliases(TestTypes(), *plan);
    QualifyColumns(TestTypes(), *plan);
    EXPECT_NO_THROW(AnnotateTypes(TestTypes(), *plan, TestAnnotation()));
}

TEST(SortPlan, GroupByColumnAliasUsesInputColumnDirectly) {
    auto i64 = TestTypes().MakeInteger();
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

    AssignSourceAliases(TestTypes(), *plan);
    QualifyColumns(TestTypes(), *plan);
    EXPECT_NO_THROW(AnnotateTypes(TestTypes(), *plan, TestAnnotation()));
}

TEST(SortPlan, GroupByDropsConstantFromCompositeKey) {
    auto i64 = TestTypes().MakeInteger();
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

    AssignSourceAliases(TestTypes(), *plan);
    QualifyColumns(TestTypes(), *plan);
    EXPECT_NO_THROW(AnnotateTypes(TestTypes(), *plan, TestAnnotation()));
}

TEST(SortPlan, GroupByDropsAffineKeysDeterminedByRetainedColumn) {
    auto i64 = TestTypes().MakeInteger();
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

    AssignSourceAliases(TestTypes(), *plan);
    QualifyColumns(TestTypes(), *plan);
    EXPECT_NO_THROW(AnnotateTypes(TestTypes(), *plan, TestAnnotation()));
}

TEST(SortPlan, GroupByKeepsAffineKeyWithoutItsBaseKey) {
    auto i64 = TestTypes().MakeInteger();
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

    auto i64 = TestTypes().MakeInteger();
    NQdb::TMockSource source(TMockColumns{}, {{"a", i64}, {"b", i64}, {"c", i64}});
    auto sourceOp = NQumir::MakeIntrusive<TSourceOperator>(TestTypes(), Borrow(source), "t");
    auto sort = NQumir::MakeIntrusive<TSortOperator>(TestTypes(), sourceOp, std::vector<TSortKey>{
        {.Column = "b", .Direction = ESortDirection::Asc},
    });
    auto project = NQumir::MakeIntrusive<TProjectOperator>(TestTypes(), sort, std::vector<TProjectionSpec>{
        {.Name = "a", .Expression = Ident("a")},
    });

    ApplyColumnPruning(TestTypes(), project);

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

    auto str = TestTypes().Make<TStringType>();
    auto i64 = TestTypes().MakeInteger();
    NQdb::TMockSource source(
        TMockColumns{},
        {{"name", str}, {"score", i64}},
        {batch1, batch2});

    auto sourceOp = NQumir::MakeIntrusive<TSourceOperator>(TestTypes(), Borrow(source), "t");
    auto root = NQumir::MakeIntrusive<TSortOperator>(TestTypes(), sourceOp, std::vector<TSortKey>{
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

    auto i64 = TestTypes().MakeInteger();
    NQdb::TMockSource source(
        TMockColumns{},
        {{"a", i64}, {"b", i64}},
        {batch1, batch2});

    auto sourceOp = NQumir::MakeIntrusive<TSourceOperator>(TestTypes(), Borrow(source), "t");
    auto root = NQumir::MakeIntrusive<TSortOperator>(TestTypes(), sourceOp, std::vector<TSortKey>{
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

// Sorts a nullable float key holding NaN, +-0, +-inf and NULL across two batches
// and returns the payload column in output order.
template<typename TFloat>
std::vector<int64_t> SortFloatKeyPayload(const char* sql, bool topSort) {
    using namespace NQumir::NAst;
    const TFloat inf = std::numeric_limits<TFloat>::infinity();
    const TFloat nan = std::numeric_limits<TFloat>::quiet_NaN();
    TFloat keys1[] = {TFloat(0.1), nan, TFloat(-0.0), -inf, TFloat(7)};
    int64_t payload1[] = {1, 2, 3, 4, 5};
    uint8_t mask1[] = {0b00011111};
    TFloat keys2[] = {TFloat(0.0), inf, TFloat(-1.5), TFloat(3)};
    int64_t payload2[] = {6, 7, 8, 9};
    uint8_t mask2[] = {0b00001011}; // row 2 is NULL
    std::vector<TColumn> columns1 = {
        TColumn{.Data = reinterpret_cast<char*>(keys1), .Mask = mask1},
        TColumn{.Data = reinterpret_cast<char*>(payload1)},
    };
    std::vector<TColumn> columns2 = {
        TColumn{.Data = reinterpret_cast<char*>(keys2), .Mask = mask2},
        TColumn{.Data = reinterpret_cast<char*>(payload2)},
    };
    TRowSet batch1{.Columns = columns1.data(), .ColumnCount = 2, .RowCount = 5, .RefCount = 1};
    TRowSet batch2{.Columns = columns2.data(), .ColumnCount = 2, .RowCount = 4, .RefCount = 1};

    auto floatType = TestTypes().MakeFloat(
        sizeof(TFloat) == 4 ? TFloatType::F32 : TFloatType::F64);
    NQdb::TMockSource source(
        TMockColumns{},
        {{"k", MakeNullableType(TestTypes(), floatType)},
         {"payload", TestTypes().MakeInteger()}},
        {batch1, batch2});
    auto plan = BuildSqlPlan(sql, source);
    EXPECT_TRUE(plan.has_value()) << (plan ? "" : plan.error().ToString());
    if (!plan) {
        return {};
    }
    auto root = topSort ? ApplyTopSort(TestTypes(), *plan) : *plan;
    auto runtime = RunPlan(root);

    std::vector<int64_t> result;
    TRowSet out{};
    while (runtime->Next(out)) {
        auto* payload = reinterpret_cast<int64_t*>(out.Columns[1].Data);
        result.insert(result.end(), payload, payload + out.RowCount);
        Release(&out);
    }
    return result;
}

TEST(SortExec, F32KeyOrdersLikeF64) {
    for (const char* sql : {
             "SELECT k, payload FROM t ORDER BY k, payload",
             "SELECT k, payload FROM t ORDER BY k DESC, payload",
             "SELECT k, payload FROM t ORDER BY k NULLS FIRST, payload"})
    {
        auto f32Rows = SortFloatKeyPayload<float>(sql, false);
        EXPECT_EQ(f32Rows.size(), 9u) << sql;
        EXPECT_EQ(f32Rows, SortFloatKeyPayload<double>(sql, false)) << sql;
    }
    // -inf, the zeros, the finite values, +inf, then NaN; the NULL row (payload 8) is last.
    auto ascending = SortFloatKeyPayload<float>(
        "SELECT k, payload FROM t ORDER BY k, payload", false);
    ASSERT_EQ(ascending.size(), 9u);
    EXPECT_EQ(ascending, (std::vector<int64_t>{4, 3, 6, 1, 9, 5, 7, 2, 8}));
}

TEST(SortExec, F32TopSortOrdersLikeF64) {
    for (const char* sql : {
             "SELECT k, payload FROM t ORDER BY k, payload LIMIT 4",
             "SELECT k, payload FROM t ORDER BY k DESC, payload LIMIT 4"})
    {
        auto f32Rows = SortFloatKeyPayload<float>(sql, true);
        EXPECT_EQ(f32Rows.size(), 4u) << sql;
        EXPECT_EQ(f32Rows, SortFloatKeyPayload<double>(sql, true)) << sql;
    }
}

// Sorts a nullable integer key over two batches by `order` and returns the
// payload (the row's input position) in output order.
template<typename TInt>
std::vector<int64_t> SortIntKeyPayload(const std::vector<TInt>& keys, uint8_t nullRow, const char* order) {
    using namespace NQumir::NAst;
    const size_t half = keys.size() / 2;
    std::vector<TInt> keys1(keys.begin(), keys.begin() + half);
    std::vector<TInt> keys2(keys.begin() + half, keys.end());
    std::vector<int64_t> payload(keys.size());
    std::iota(payload.begin(), payload.end(), 0);
    std::vector<int64_t> payload1(payload.begin(), payload.begin() + half);
    std::vector<int64_t> payload2(payload.begin() + half, payload.end());
    uint8_t mask1[] = {0xff, 0xff};
    uint8_t mask2[] = {0xff, 0xff};
    mask1[nullRow / 8] &= ~(uint8_t(1) << (nullRow % 8));
    mask2[nullRow / 8] &= ~(uint8_t(1) << (nullRow % 8));
    std::vector<TColumn> columns1 = {
        TColumn{.Data = reinterpret_cast<char*>(keys1.data()), .Mask = mask1},
        TColumn{.Data = reinterpret_cast<char*>(payload1.data())},
    };
    std::vector<TColumn> columns2 = {
        TColumn{.Data = reinterpret_cast<char*>(keys2.data()), .Mask = mask2},
        TColumn{.Data = reinterpret_cast<char*>(payload2.data())},
    };
    TRowSet batch1{.Columns = columns1.data(), .ColumnCount = 2, .RowCount = static_cast<int64_t>(half), .RefCount = 1};
    TRowSet batch2{.Columns = columns2.data(), .ColumnCount = 2, .RowCount = static_cast<int64_t>(half), .RefCount = 1};

    auto intType = TestTypes().MakeInteger(sizeof(TInt) == 4 ? TIntegerType::I32 : TIntegerType::I64);
    NQdb::TMockSource source(
        TMockColumns{},
        {{"k", MakeNullableType(TestTypes(), intType)},
         {"payload", TestTypes().MakeInteger()}},
        {batch1, batch2});
    auto plan = BuildSqlPlan(std::string("SELECT k, payload FROM t ORDER BY ") + order, source);
    EXPECT_TRUE(plan.has_value()) << (plan ? "" : plan.error().ToString());
    if (!plan) {
        return {};
    }
    auto runtime = RunPlan(*plan);
    std::vector<int64_t> result;
    TRowSet out{};
    while (runtime->Next(out)) {
        auto* rows = reinterpret_cast<int64_t*>(out.Columns[1].Data);
        result.insert(result.end(), rows, rows + out.RowCount);
        Release(&out);
    }
    return result;
}

// The expected payload of SortIntKeyPayload: a stable sort of the input positions.
template<typename TInt>
std::vector<int64_t> StableIntKeyOrder(const std::vector<TInt>& keys, uint8_t nullRow, bool desc, bool nullsFirst) {
    const size_t half = keys.size() / 2;
    auto isNull = [&](int64_t row) {
        return static_cast<size_t>(row) % half == nullRow;
    };
    std::vector<int64_t> rows(keys.size());
    std::iota(rows.begin(), rows.end(), 0);
    std::stable_sort(rows.begin(), rows.end(), [&](int64_t a, int64_t b) {
        if (isNull(a) || isNull(b)) {
            return isNull(a) != isNull(b) && isNull(a) == nullsFirst;
        }
        return desc ? keys[b] < keys[a] : keys[a] < keys[b];
    });
    return rows;
}

// The radix sort skips a pass in which every key has the same byte, and the
// row ids end in the caller's array after an odd number of moving passes.
TEST(SortExec, IntegerKeysSkipPassesThatMoveNothing) {
    // Keys that differ in 1, 2 or 3 bytes, with repeats so that ties show stability.
    const std::vector<std::vector<int64_t>> keys64 = {
        {0x0102030405060700 + 7, 0x0102030405060700 + 3, 0x0102030405060700 + 9, 0x0102030405060700 + 3,
         0x0102030405060700 + 0, 0x0102030405060700 + 3, 0x0102030405060700 + 255, 0x0102030405060700 + 9,
         0x0102030405060700 + 1, 0x0102030405060700 + 7},
        {0x1200 + 7, 0x3400 + 3, 0x1200 + 9, 0x3400 + 3, 0x0000 + 0, 0x3400 + 7, 0x1200 + 3, 0x0000 + 9, 0x3400 + 3, 0x1200 + 7},
        {(int64_t(3) << 40) | (1 << 16) | 5, (int64_t(1) << 40) | (2 << 16) | 9, (int64_t(3) << 40) | (1 << 16) | 2,
         (int64_t(2) << 40) | 5, (int64_t(3) << 40) | (1 << 16) | 5, (int64_t(1) << 40) | (7 << 16) | 1,
         (int64_t(2) << 40) | 5, (int64_t(3) << 40) | 2, (int64_t(1) << 40) | (2 << 16) | 9,
         (int64_t(2) << 40) | (1 << 16)},
    };
    for (const auto& keys : keys64) {
        for (const auto& [order, desc, nullsFirst] : {
                 std::tuple{"k DESC NULLS FIRST", true, true},
                 std::tuple{"k ASC NULLS LAST", false, false}})
        {
            EXPECT_EQ(SortIntKeyPayload(keys, 2, order), StableIntKeyOrder(keys, 2, desc, nullsFirst)) << order;
            std::vector<int32_t> narrow;
            for (int64_t key : keys) {
                narrow.push_back(static_cast<int32_t>(key ^ (key >> 32)));
            }
            EXPECT_EQ(SortIntKeyPayload(narrow, 2, order), StableIntKeyOrder(narrow, 2, desc, nullsFirst)) << order;
        }
    }
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

    auto i64 = TestTypes().MakeInteger();
    NQdb::TMockSource source(
        TMockColumns{},
        {{"a", i64}, {"b", i64}},
        {batch1, batch2});

    auto sourceOp = NQumir::MakeIntrusive<TSourceOperator>(TestTypes(), Borrow(source), "t");
    auto root = NQumir::MakeIntrusive<TSortOperator>(TestTypes(), sourceOp, std::vector<TSortKey>{
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

    auto i64 = TestTypes().MakeInteger();
    NQdb::TMockSource source(
        TMockColumns{},
        {{"k", i64}, {"payload", i64}},
        {batch});
    auto plan = BuildSqlPlan("SELECT k, payload FROM t ORDER BY k LIMIT 2", source);
    ASSERT_TRUE(plan.has_value()) << (plan ? "" : plan.error().ToString());
    auto optimized = ApplyTopSort(TestTypes(), *plan);
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

    auto i64 = TestTypes().MakeInteger();
    NQdb::TMockSource source(
        TMockColumns{},
        {{"k", i64}, {"payload", i64}},
        {batch});
    auto sourceOp = NQumir::MakeIntrusive<TSourceOperator>(TestTypes(), Borrow(source), "t");
    auto root = NQumir::MakeIntrusive<TTopSortOperator>(TestTypes(),
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

    auto i64 = TestTypes().MakeInteger();
    NQdb::TMockSource source(
        TMockColumns{},
        {{"k", i64}, {"payload", i64}},
        {batch});
    auto plan = BuildSqlPlan("SELECT k, payload FROM t ORDER BY k DESC LIMIT 2", source);
    ASSERT_TRUE(plan.has_value()) << (plan ? "" : plan.error().ToString());
    auto optimized = ApplyTopSort(TestTypes(), *plan);
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

    auto i64 = TestTypes().MakeInteger();
    NQdb::TMockSource source(
        TMockColumns{},
        {{"k", i64}, {"payload", i64}},
        {batch1, batch2});
    auto plan = BuildSqlPlan("SELECT k, payload FROM t ORDER BY k LIMIT 5", source);
    ASSERT_TRUE(plan.has_value()) << (plan ? "" : plan.error().ToString());
    auto optimized = ApplyTopSort(TestTypes(), *plan);
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

    auto i64 = TestTypes().MakeInteger();
    NQdb::TMockSource source(
        TMockColumns{},
        {{"k", i64}, {"payload", i64}},
        {batch1, batch2});
    auto sourceOp = NQumir::MakeIntrusive<TSourceOperator>(TestTypes(), Borrow(source), "t");
    auto root = NQumir::MakeIntrusive<TTopSortOperator>(TestTypes(),
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

    auto i64 = TestTypes().MakeInteger();
    NQdb::TMockSource source(
        TMockColumns{},
        {{"k", MakeNullableType(TestTypes(), i64)}, {"payload", i64}},
        {batch});
    auto plan = BuildSqlPlan(
        "SELECT k, payload FROM t ORDER BY k NULLS FIRST LIMIT 3", source);
    ASSERT_TRUE(plan.has_value()) << (plan ? "" : plan.error().ToString());
    auto optimized = ApplyTopSort(TestTypes(), *plan);
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

    auto i64 = TestTypes().MakeInteger();
    NQdb::TMockSource source(TMockColumns{}, {{"a", i64}}, {});

    auto sourceOp = NQumir::MakeIntrusive<TSourceOperator>(TestTypes(), Borrow(source), "t");
    auto root = NQumir::MakeIntrusive<TSortOperator>(TestTypes(), sourceOp, std::vector<TSortKey>{
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

    auto i64 = TestTypes().MakeInteger();
    NQdb::TMockSource source(
        TMockColumns{},
        {{"k", i64}, {"payload", i64}},
        {batch});

    auto sourceOp = NQumir::MakeIntrusive<TSourceOperator>(TestTypes(), Borrow(source), "t");
    auto root = NQumir::MakeIntrusive<TSortOperator>(TestTypes(), sourceOp, std::vector<TSortKey>{
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

    auto i64 = TestTypes().MakeInteger();
    NQdb::TMockSource source(
        TMockColumns{},
        {{"a", i64}, {"b", i64}},
        {batch});

    auto sourceOp = NQumir::MakeIntrusive<TSourceOperator>(TestTypes(), Borrow(source), "t");
    auto sort = NQumir::MakeIntrusive<TSortOperator>(TestTypes(), sourceOp, std::vector<TSortKey>{
        {.Column = "b", .Direction = ESortDirection::Asc},
    });
    auto project = NQumir::MakeIntrusive<TProjectOperator>(TestTypes(), sort, std::vector<TProjectionSpec>{
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

    auto i64 = TestTypes().MakeInteger();
    NQdb::TMockSource source(
        TMockColumns{},
        {{"k", i64}, {"v", i64}},
        {batch});

    auto sourceOp = NQumir::MakeIntrusive<TSourceOperator>(TestTypes(), Borrow(source), "t");
    auto aggregate = NQumir::MakeIntrusive<TAggregateOperator>(TestTypes(),
        sourceOp,
        std::vector<std::string>{"k"},
        std::vector<TAggregateSpec>{{
            .Name = "s",
            .Func = "sum",
            .Arg = Ident("v"),
        }});
    auto sort = NQumir::MakeIntrusive<TSortOperator>(TestTypes(), aggregate, std::vector<TSortKey>{
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

    auto i64 = TestTypes().MakeInteger();
    // DECIMAL(20,2) is stored in i128 (BinInt); one of p <= 18 would be a plain i64.
    auto decimal = MakeNullableType(TestTypes(), MakeDecimalType(TestTypes(), {20, 2}));
    NQdb::TMockSource source(
        TMockColumns{},
        {{"k", i64}, {"amount", decimal}},
        {batch});

    auto sourceOp = NQumir::MakeIntrusive<TSourceOperator>(TestTypes(), Borrow(source), "t");
    auto sort = NQumir::MakeIntrusive<TSortOperator>(TestTypes(), sourceOp, std::vector<TSortKey>{
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

    auto i64 = TestTypes().MakeInteger();
    NQdb::TMockSource source(
        TMockColumns{},
        {{"k", MakeNullableType(TestTypes(), i64)}, {"payload", i64}},
        {batch});

    auto sourceOp = NQumir::MakeIntrusive<TSourceOperator>(TestTypes(), Borrow(source), "t");
    auto sort = NQumir::MakeIntrusive<TSortOperator>(TestTypes(), sourceOp, std::vector<TSortKey>{
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

    auto i64 = TestTypes().MakeInteger();
    NQdb::TMockSource source(
        TMockColumns{},
        {{"k", MakeNullableType(TestTypes(), i64)}, {"payload", i64}},
        {batch});

    auto sourceOp = NQumir::MakeIntrusive<TSourceOperator>(TestTypes(), Borrow(source), "t");
    auto sort = NQumir::MakeIntrusive<TSortOperator>(TestTypes(), sourceOp, std::vector<TSortKey>{
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

    auto str = TestTypes().Make<TStringType>();
    auto i64 = TestTypes().MakeInteger();
    NQdb::TMockSource source(
        TMockColumns{},
        {{"name", str}, {"score", i64}},
        {batch});

    auto sourceOp = NQumir::MakeIntrusive<TSourceOperator>(TestTypes(), Borrow(source), "t");
    auto root = NQumir::MakeIntrusive<TSortOperator>(TestTypes(), sourceOp, std::vector<TSortKey>{
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
