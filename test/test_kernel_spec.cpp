#include <gtest/gtest.h>

#include <qdb/kernel/annotate_type.h>
#include <qdb/kernel/builder.h>
#include <qdb/kernel/column_value.h>
#include <qdb/kernel/spec.h>
#include <qdb/plan/types/decimal.h>
#include <qdb/plan/types/nullable.h>

#include <qumir/parser/ast.h>
#include <qumir/parser/core/printer.h>
#include <qumir/parser/type.h>

#include <sstream>
#include <string>
#include <utility>

using namespace NQdb;

TEST(KernelSpec, LikeConstantPatternPicksFastPath) {
    using namespace NQumir::NAst;
    TStructType input({{"url", std::make_shared<TStringType>()}});
    auto lowered = [&](const std::string& pattern) {
        auto call = std::make_shared<TCallExpr>(NQumir::TLocation{},
            std::make_shared<TIdentExpr>(NQumir::TLocation{}, "qdb_string_view_sql_like"),
            std::vector<TExprPtr>{
                std::make_shared<TIdentExpr>(NQumir::TLocation{}, "url"),
                std::make_shared<TStringLiteralExpr>(NQumir::TLocation{}, pattern),
            });
        return NCore::PrintAst(
            NKernel::ExpandKernelExpr(call, input, nullptr).first);
    };
    EXPECT_NE(lowered("%google%").find("qdb_like_contains"), std::string::npos);
    EXPECT_NE(lowered("google%").find("qdb_like_prefix"), std::string::npos);
    EXPECT_NE(lowered("%google").find("qdb_like_suffix"), std::string::npos);
    EXPECT_NE(lowered("google").find("qdb_like_equals"), std::string::npos);
    EXPECT_NE(lowered("%goo_le%").find("qdb_string_view_sql_like"), std::string::npos);
    EXPECT_NE(lowered("a%b").find("qdb_string_view_sql_like"), std::string::npos);
}

TEST(KernelSpec, PrintsStableDebugDescription) {
    using namespace NQumir::NAst;

    auto i64 = std::make_shared<TIntegerType>();
    auto input = std::make_shared<TStructType>(std::vector<std::pair<std::string, TTypePtr>>{
        {"a", i64},
        {"b", i64},
    });
    auto output = std::make_shared<TStructType>(std::vector<std::pair<std::string, TTypePtr>>{
        {"a", i64},
    });

    NKernel::TOperatorKernelSpec spec{
        .Kind = NKernel::EOperatorKernelKind::UnaryStreaming,
        .OperatorName = "filter",
        .InputSchemas = {input},
        .OutputSchema = output,
        .ReferencedColumns = {
            {.Name = "b", .Index = 1, .Type = i64},
        },
        .Expressions = {
            std::make_shared<TBinaryExpr>(
                NQumir::TLocation{}, TOperator(">"),
                std::make_shared<TIdentExpr>(NQumir::TLocation{}, "b"),
                std::make_shared<TNumberExpr>(NQumir::TLocation{}, int64_t(0))),
        },
        .Keys = {
            {.Name = "predicate-columns",
             .Columns = {{.Name = "b", .Index = 1, .Type = i64}}},
        },
        .Entrypoints = {
            {.Name = "qdb_filter_0", .Abi = "void(ref TRowSet)"},
        },
        .SourceModules = {"qumirdb"},
    };

    std::ostringstream out;
    NKernel::PrintKernelSpec(out, spec);

    EXPECT_EQ(out.str(),
        "kernel-spec filter\n"
        "  kind: unary-streaming\n"
        "  inputs:\n"
        "    [0] struct { a: Int; b: Int; }\n"
        "  output: struct { a: Int; }\n"
        "  referenced:\n"
        "    b#1:i64\n"
        "  expressions:\n"
        "    [0] (> b 0)\n"
        "  keys:\n"
        "    predicate-columns: b#1:i64\n"
        "  aggregates: []\n"
        "  sort-keys: []\n"
        "  join-keys: []\n"
        "  entrypoints:\n"
        "    qdb_filter_0: void(ref TRowSet)\n"
        "  source-modules:\n"
        "    qumirdb\n");
}

TEST(KernelSpec, BuildsFilterSpecFromPredicate) {
    using namespace NQumir::NAst;

    auto i64 = std::make_shared<TIntegerType>();
    TStructType input({
        {"a", i64},
        {"b", i64},
        {"c", i64},
    });
    auto predicate = std::make_shared<TBinaryExpr>(
        NQumir::TLocation{}, TOperator("<"),
        std::make_shared<TIdentExpr>(NQumir::TLocation{}, "c"),
        std::make_shared<TIdentExpr>(NQumir::TLocation{}, "a"));

    auto spec = NKernel::BuildFilterKernelSpec(input, predicate, "qdb_filter_test");

    EXPECT_EQ(spec.Kind, NKernel::EOperatorKernelKind::UnaryStreaming);
    EXPECT_EQ(spec.OperatorName, "filter");
    ASSERT_EQ(spec.InputSchemas.size(), 1u);
    ASSERT_EQ(spec.ReferencedColumns.size(), 2u);
    EXPECT_EQ(spec.ReferencedColumns[0].Name, "a");
    EXPECT_EQ(spec.ReferencedColumns[0].Index, 0);
    EXPECT_EQ(spec.ReferencedColumns[1].Name, "c");
    EXPECT_EQ(spec.ReferencedColumns[1].Index, 2);
    ASSERT_EQ(spec.Expressions.size(), 1u);
    EXPECT_EQ(spec.Expressions[0], predicate);
    ASSERT_EQ(spec.Entrypoints.size(), 1u);
    EXPECT_EQ(spec.Entrypoints[0].Name, "qdb_filter_test");
    EXPECT_EQ(spec.Entrypoints[0].Abi, "void(ref TRowSet)");
    ASSERT_EQ(spec.SourceModules.size(), 1u);
    EXPECT_EQ(spec.SourceModules[0], "qumirdb");
}

TEST(KernelSpec, BuildsProjectSpecFromComputedExpressions) {
    using namespace NQumir::NAst;

    auto i64 = std::make_shared<TIntegerType>();
    auto f64 = std::make_shared<TFloatType>();
    TStructType input({
        {"a", i64},
        {"b", f64},
        {"c", i64},
    });
    std::vector<TExprPtr> exprs = {
        std::make_shared<TBinaryExpr>(
            NQumir::TLocation{}, TOperator("+"),
            std::make_shared<TIdentExpr>(NQumir::TLocation{}, "b"),
            std::make_shared<TIdentExpr>(NQumir::TLocation{}, "a")),
    };
    std::vector<TTypePtr> types = {f64};

    auto spec = NKernel::BuildProjectKernelSpec(
        input, exprs, types, "qdb_project_test");

    EXPECT_EQ(spec.Kind, NKernel::EOperatorKernelKind::UnaryStreaming);
    EXPECT_EQ(spec.OperatorName, "project-compute");
    ASSERT_EQ(spec.InputSchemas.size(), 1u);
    ASSERT_EQ(spec.ReferencedColumns.size(), 2u);
    EXPECT_EQ(spec.ReferencedColumns[0].Name, "a");
    EXPECT_EQ(spec.ReferencedColumns[0].Index, 0);
    EXPECT_EQ(spec.ReferencedColumns[1].Name, "b");
    EXPECT_EQ(spec.ReferencedColumns[1].Index, 1);
    ASSERT_EQ(spec.Expressions.size(), 1u);
    EXPECT_EQ(spec.Expressions[0], exprs[0]);
    auto* output = static_cast<TStructType*>(spec.OutputSchema.get());
    ASSERT_NE(output, nullptr);
    ASSERT_EQ(output->Fields.size(), 1u);
    EXPECT_EQ(output->Fields[0].first, "computed_0");
    EXPECT_EQ(output->Fields[0].second, f64);
    ASSERT_EQ(spec.Entrypoints.size(), 1u);
    EXPECT_EQ(spec.Entrypoints[0].Name, "qdb_project_test");
    EXPECT_EQ(spec.Entrypoints[0].Abi, "void(ref TRowSet, ptr ptr i8)");
    ASSERT_EQ(spec.SourceModules.size(), 1u);
    EXPECT_EQ(spec.SourceModules[0], "qumirdb");
}

TEST(KernelSpec, BuildsAggregateSpecFromGroupKeysAndAggregates) {
    using namespace NQumir::NAst;

    auto i64 = std::make_shared<TIntegerType>();
    TStructType input({
        {"k", i64},
        {"v", i64},
    });
    std::vector<TAggregateSpec> aggs = {{
        .Name = "sum_v",
        .Func = "sum",
        .Arg = std::make_shared<TIdentExpr>(NQumir::TLocation{}, "v"),
    }};

    auto spec = NKernel::BuildAggregateKernelSpec(input, {"k"}, aggs);

    EXPECT_EQ(spec.Kind, NKernel::EOperatorKernelKind::UnaryBlocking);
    EXPECT_EQ(spec.OperatorName, "aggregate");
    ASSERT_EQ(spec.InputSchemas.size(), 1u);
    ASSERT_EQ(spec.Keys.size(), 1u);
    EXPECT_EQ(spec.Keys[0].Name, "group-keys");
    ASSERT_EQ(spec.Keys[0].Columns.size(), 1u);
    EXPECT_EQ(spec.Keys[0].Columns[0].Name, "k");
    ASSERT_EQ(spec.Aggregates.size(), 1u);
    EXPECT_EQ(spec.Aggregates[0].Name, "sum_v");
    EXPECT_EQ(spec.Aggregates[0].Func, "sum");
    EXPECT_TRUE(spec.Aggregates[0].HasArg);
    EXPECT_EQ(spec.Aggregates[0].Arg.Name, "v");
    ASSERT_EQ(spec.Entrypoints.size(), 2u);
    EXPECT_EQ(spec.Entrypoints[0].Name, "agg_dispatch");
    EXPECT_EQ(spec.Entrypoints[1].Name, "agg_finish_rowset");
    ASSERT_EQ(spec.SourceModules.size(), 1u);
    EXPECT_EQ(spec.SourceModules[0], "qumirdb");
}

TEST(KernelSpec, BuildsSortSpecFromSortKeys) {
    using namespace NQumir::NAst;

    auto i64 = std::make_shared<TIntegerType>();
    auto str = std::make_shared<TStringType>();
    TStructType input({
        {"id", i64},
        {"name", str},
    });

    auto spec = NKernel::BuildSortKernelSpec(
        input,
        {{.Column = "name",
          .Direction = ESortDirection::Desc,
          .Nulls = ESortNulls::First}});

    EXPECT_EQ(spec.Kind, NKernel::EOperatorKernelKind::UnaryBlocking);
    EXPECT_EQ(spec.OperatorName, "sort");
    ASSERT_EQ(spec.InputSchemas.size(), 1u);
    ASSERT_EQ(spec.ReferencedColumns.size(), 1u);
    EXPECT_EQ(spec.ReferencedColumns[0].Name, "name");
    EXPECT_EQ(spec.ReferencedColumns[0].Index, 1);
    ASSERT_EQ(spec.SortKeys.size(), 1u);
    EXPECT_EQ(spec.SortKeys[0].Column.Name, "name");
    EXPECT_EQ(spec.SortKeys[0].Direction, ESortDirection::Desc);
    EXPECT_EQ(spec.SortKeys[0].Nulls, ESortNulls::First);
    ASSERT_EQ(spec.SourceModules.size(), 1u);
    EXPECT_EQ(spec.SourceModules[0], "qumirdb");
}

TEST(KernelSpec, BuildsJoinSpecFromEquiKeys) {
    using namespace NQumir::NAst;

    auto i64 = std::make_shared<TIntegerType>();
    TStructType left({
        {"lk", i64},
        {"lv", i64},
    });
    TStructType right({
        {"rk", i64},
        {"rv", i64},
    });

    auto spec = NKernel::BuildJoinKernelSpec(
        left, right, {{.Left = "lk", .Right = "rk"}}, EJoinType::Inner);

    EXPECT_EQ(spec.Kind, NKernel::EOperatorKernelKind::Binary);
    EXPECT_EQ(spec.OperatorName, "join");
    EXPECT_EQ(spec.JoinType, EJoinType::Inner);
    ASSERT_EQ(spec.InputSchemas.size(), 2u);
    ASSERT_EQ(spec.JoinKeys.size(), 1u);
    EXPECT_EQ(spec.JoinKeys[0].Left.Name, "lk");
    EXPECT_EQ(spec.JoinKeys[0].Left.Index, 0);
    EXPECT_EQ(spec.JoinKeys[0].Right.Name, "rk");
    EXPECT_EQ(spec.JoinKeys[0].Right.Index, 0);
    ASSERT_EQ(spec.Entrypoints.size(), 2u);
    EXPECT_EQ(spec.Entrypoints[0].Name, "jt_dispatch");
    EXPECT_EQ(spec.Entrypoints[1].Name, "jt_materialize");
    ASSERT_EQ(spec.SourceModules.size(), 1u);
    EXPECT_EQ(spec.SourceModules[0], "qumirdb");
}

TEST(KernelSpec, BuildsMarkJoinSpec) {
    using namespace NQumir::NAst;

    auto i64 = std::make_shared<TIntegerType>();
    TStructType left({
        {"lk", i64},
        {"lv", i64},
    });
    TStructType right({
        {"rk", i64},
        {"rv", i64},
    });

    auto spec = NKernel::BuildJoinKernelSpec(
        left, right, {{.Left = "lk", .Right = "rk"}}, EJoinType::LeftMark,
        nullptr, "marker");

    EXPECT_EQ(spec.Kind, NKernel::EOperatorKernelKind::Binary);
    EXPECT_EQ(spec.OperatorName, "join");
    EXPECT_EQ(spec.JoinType, EJoinType::LeftMark);
    ASSERT_EQ(spec.InputSchemas.size(), 2u);
    ASSERT_EQ(spec.JoinKeys.size(), 1u);
    EXPECT_EQ(spec.JoinKeys[0].Left.Name, "lk");
    EXPECT_EQ(spec.JoinKeys[0].Right.Name, "rk");
    auto output = TMaybeType<TStructType>(spec.OutputSchema);
    ASSERT_TRUE(output);
    ASSERT_EQ(output.Cast()->Fields.size(), 3u);
    EXPECT_EQ(output.Cast()->Fields[0].first, "lk");
    EXPECT_EQ(output.Cast()->Fields[1].first, "lv");
    EXPECT_EQ(output.Cast()->Fields[2].first, "marker");
    EXPECT_TRUE(TMaybeType<TBoolType>(output.Cast()->Fields[2].second));
    ASSERT_EQ(spec.Entrypoints.size(), 2u);
    EXPECT_EQ(spec.Entrypoints[0].Name, "jt_dispatch");
    EXPECT_EQ(spec.Entrypoints[1].Name, "jt_materialize");

    // A NULL key on either side can make the marker NULL.
    TStructType nullableRight({
        {"rk", std::make_shared<TNullable>(i64)},
    });
    auto nullableSpec = NKernel::BuildJoinKernelSpec(
        left, nullableRight, {{.Left = "lk", .Right = "rk"}},
        EJoinType::LeftMark, nullptr, "marker");
    auto nullableOutput = TMaybeType<TStructType>(nullableSpec.OutputSchema);
    ASSERT_TRUE(nullableOutput);
    ASSERT_EQ(nullableOutput.Cast()->Fields.size(), 3u);
    const auto& marker = nullableOutput.Cast()->Fields[2].second;
    EXPECT_TRUE(IsNullableType(marker));
    EXPECT_TRUE(TMaybeType<TBoolType>(UnwrapNullableType(marker)));
}

TEST(KernelSpec, BuildsCrossJoinSpec) {
    using namespace NQumir::NAst;

    auto i64 = std::make_shared<TIntegerType>();
    TStructType left({
        {"lk", i64},
    });
    TStructType right({
        {"rv", i64},
    });

    auto spec = NKernel::BuildCrossJoinKernelSpec(left, right);

    EXPECT_EQ(spec.Kind, NKernel::EOperatorKernelKind::Binary);
    EXPECT_EQ(spec.OperatorName, "cross-join");
    EXPECT_EQ(spec.JoinType, EJoinType::Inner);
    ASSERT_EQ(spec.InputSchemas.size(), 2u);
    EXPECT_TRUE(spec.JoinKeys.empty());
    ASSERT_EQ(spec.Entrypoints.size(), 2u);
    EXPECT_EQ(spec.Entrypoints[0].Name, "xj_dispatch");
    EXPECT_EQ(spec.Entrypoints[1].Name, "jt_materialize");
    ASSERT_EQ(spec.SourceModules.size(), 1u);
    EXPECT_EQ(spec.SourceModules[0], "qumirdb");
}

TEST(KernelSpec, RejectsRegexpReplaceInDirectAggregateAndJoinKernels) {
    using namespace NQumir::NAst;

    auto i64 = std::make_shared<TIntegerType>();
    TStructType input({{"value", i64}});
    auto regex = std::make_shared<TCallExpr>(
        NQumir::TLocation{},
        std::make_shared<TIdentExpr>(NQumir::TLocation{}, "regexp_replace"),
        std::vector<TExprPtr>{
            std::make_shared<TIdentExpr>(NQumir::TLocation{}, "value"),
            std::make_shared<TStringLiteralExpr>(NQumir::TLocation{}, "x"),
            std::make_shared<TStringLiteralExpr>(NQumir::TLocation{}, "y"),
        });

    auto expectClearError = [](auto&& build) {
        try {
            build();
            FAIL() << "expected REGEXP_REPLACE rejection";
        } catch (const std::runtime_error& error) {
            EXPECT_NE(
                std::string(error.what()).find("materialize it in a project first"),
                std::string::npos);
        }
    };
    expectClearError([&] {
        NKernel::BuildAggregateKernelSpec(input, {}, {{
            .Name = "bad",
            .Func = "count",
            .Arg = regex,
        }});
    });
    expectClearError([&] {
        NKernel::BuildJoinKernelSpec(
            input, input, {}, EJoinType::Inner, regex);
    });
}

TEST(OzFunBuilder, BuildsFunctionDeclaration) {
    using namespace NQumir::NAst;

    auto i64 = std::make_shared<TIntegerType>();
    auto builder = NKernel::NOz::TFunBuilder("identity")
        .Param("value", i64)
        .Return(i64)
        .Stmt(NKernel::NOz::Return(NKernel::NOz::Ident("value")));
    auto ast = std::move(builder).Build();

    auto fun = TMaybeNode<TFunDecl>(ast);
    ASSERT_TRUE(fun);
    EXPECT_EQ(fun.Cast()->Name, "identity");
    ASSERT_EQ(fun.Cast()->Params.size(), 1u);
    EXPECT_EQ(fun.Cast()->Params[0]->Name, "value");
    EXPECT_EQ(fun.Cast()->Params[0]->Type, i64);
    EXPECT_EQ(fun.Cast()->RetType, i64);
    ASSERT_NE(fun.Cast()->Body, nullptr);
    ASSERT_EQ(fun.Cast()->Body->Stmts.size(), 1u);

    auto ret = TMaybeNode<TReturnExpr>(fun.Cast()->Body->Stmts[0]);
    ASSERT_TRUE(ret);
    auto ident = TMaybeNode<TIdentExpr>(ret.Cast()->Value);
    ASSERT_TRUE(ident);
    EXPECT_EQ(ident.Cast()->Name, "value");
}

TEST(KernelSpec, FixedValueWidthIsTheByteWidthOfOneValue) {
    using namespace NQumir::NAst;
    auto width = [](const TTypePtr& type) { return NKernel::FixedValueWidth(type); };
    EXPECT_EQ(width(std::make_shared<TIntegerType>(TIntegerType::I8)), 1);
    EXPECT_EQ(width(std::make_shared<TIntegerType>(TIntegerType::U32)), 4);
    EXPECT_EQ(width(std::make_shared<TIntegerType>(TIntegerType::I64)), 8);
    EXPECT_EQ(width(std::make_shared<TIntegerType>(TIntegerType::I128)), 16);
    EXPECT_EQ(width(std::make_shared<TFloatType>(TFloatType::F32)), 4);
    EXPECT_EQ(width(std::make_shared<TFloatType>(TFloatType::F64)), 8);
    EXPECT_EQ(width(std::make_shared<TBoolType>()), 1);
    EXPECT_EQ(width(std::make_shared<TStringType>()), 0);
    EXPECT_EQ(width(MakeDecimalType({.Precision = 18, .Scale = 2})), 8);
    EXPECT_EQ(width(MakeDecimalType({.Precision = 38, .Scale = 2})), 16);
    EXPECT_EQ(width(BinIntStorageType()), 16);
    // Nullability does not change the width.
    EXPECT_EQ(width(std::make_shared<TNullable>(std::make_shared<TFloatType>(TFloatType::F32))), 4);
    EXPECT_EQ(width(std::make_shared<TNullable>(std::make_shared<TFloatType>())), 8);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
