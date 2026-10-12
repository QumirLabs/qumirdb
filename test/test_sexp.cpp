#include <gtest/gtest.h>
#include "mock_source.h"

#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/cte_consumer.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/late_materialize.h>
#include <qdb/plan/ops/limit.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/sort.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/ops/union.h>
#include <qdb/plan/ops/window.h>
#include <qdb/plan/plan_print.h>
#include <qdb/plan/clone_operator.h>
#include <qdb/plan/passes/flatten_joins.h>
#include <qdb/sexp/parser.h>
#include <qdb/sexp/printer.h>

#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>
#include <qumir/parser/core/printer.h>
#include <qumir/parser/type.h>

#include <sstream>
#include <stdexcept>

using namespace NQdb;
using namespace NQdb::NSexp;
using namespace NQumir::NAst::NCore;
using namespace NQumir::NAst;

namespace {


TPrintOptions MakePrintOpts() {
    TPrintOptions opts;
    opts.Pretty = false;
    for (auto& [k, v] : MakeRelPrinters()) {
        opts.NodePrinters[k] = std::move(v);
    }
    return opts;
}

TParser MakeParser(TRelParserOptions opts = {}) {
    TParser p;
    for (auto& [k, v] : MakeRelParsers(std::move(opts))) {
        p.NodeParsers[k] = std::move(v);
    }
    return p;
}

TExprPtr Parse(TParser& p, const std::string& src) {
    std::istringstream in(src);
    TTokenStream ts(in);
    auto result = p.Parse(ts);
    EXPECT_TRUE(result.has_value()) << (result ? "" : result.error().ToString());
    return result.value_or(nullptr);
}

} // namespace

// --- Printer ---

TEST(SexpPrinter, Source) {
    NQdb::TMockSource src({"x", "y"});
    auto op = std::make_shared<TSourceOperator>(src);
    EXPECT_EQ(PrintAst(op, MakePrintOpts()), "(rel source)");
}

TEST(SexpParser, SourceRowGroupPredicateRoundtrip) {
    NQdb::TMockSource src({"x"});
    auto sourceOp = std::make_shared<TSourceOperator>(src);
    const std::string input =
        "(rel source \"data.parquet\" \"t\" (> |t.x| 5))";

    TRelParserOptions opts;
    opts.SourceFactory = [&](std::string_view path, NQumir::TLocation)
        -> TOperatorPtr
    {
        sourceOp = std::make_shared<TSourceOperator>(src, std::string(path));
        return sourceOp;
    };
    auto p = MakeParser(std::move(opts));
    auto expr = Parse(p, input);

    ASSERT_NE(expr, nullptr);
    ASSERT_TRUE(sourceOp->RowGroupPredicate());
    EXPECT_EQ(PrintAst(expr, MakePrintOpts()), input);
}

TEST(SexpParser, RuntimeFilterUsesRoundtrip) {
    NQdb::TMockSource src({"x", "y"});
    TRelParserOptions opts;
    opts.SourceFactory = [&](std::string_view path, NQumir::TLocation)
        -> TOperatorPtr {
        return std::make_shared<TSourceOperator>(src, std::string(path));
    };
    auto parser = MakeParser(std::move(opts));
    const std::string input =
        "(rel use-filter (rel source \"data.parquet\") "
        "(7 |t.x|) (9 y x))";
    auto expr = Parse(parser, input);
    ASSERT_NE(expr, nullptr);
    auto op = std::static_pointer_cast<IOperator>(expr);
    ASSERT_EQ(op->RuntimeFilterUses.size(), 2u);
    EXPECT_EQ(op->RuntimeFilterUses[0].Id, 7u);
    EXPECT_EQ(op->RuntimeFilterUses[0].ProbeKeys,
        (std::vector<std::string>{"t.x"}));
    EXPECT_EQ(op->RuntimeFilterUses[1].ProbeKeys,
        (std::vector<std::string>{"y", "x"}));
    EXPECT_EQ(PrintAst(expr, MakePrintOpts()), input);
}

TEST(SexpParser, SharedFilterConsumerPlacementRoundtrip) {
    NQdb::TMockSource left({"lk"});
    NQdb::TMockSource middle({"rk"});
    NQdb::TMockSource third({"ck"});
    TRelParserOptions opts;
    opts.SourceFactory = [&](std::string_view path, NQumir::TLocation)
        -> TOperatorPtr {
        auto& source = path == "L" ? left : path == "R" ? middle : third;
        return std::make_shared<TSourceOperator>(source, std::string(path));
    };
    auto parser = MakeParser(std::move(opts));
    const std::string input =
        "(rel join "
        "(rel join (rel source \"L\") "
        "(rel use-filter (rel source \"R\") (7 rk)) "
        "((lk rk)) (left) (emit-filter 7 left)) "
        "(rel use-filter (rel source \"C\") (7 ck)) "
        "((lk ck)) (left))";
    auto expr = Parse(parser, input);
    ASSERT_NE(expr, nullptr);
    auto& outer = static_cast<TJoinOperator&>(*expr);
    auto& first = static_cast<TJoinOperator&>(*outer.Left());
    ASSERT_TRUE(first.RuntimeFilter());
    ASSERT_EQ(first.Right()->RuntimeFilterUses.size(), 1u);
    ASSERT_EQ(outer.Right()->RuntimeFilterUses.size(), 1u);
    EXPECT_EQ(first.Right()->RuntimeFilterUses[0].Id,
        outer.Right()->RuntimeFilterUses[0].Id);
    EXPECT_EQ(PrintAst(expr, MakePrintOpts()), input);
}

TEST(SexpPrinter, Filter) {
    NQdb::TMockSource src({"x"});
    auto source = std::make_shared<TSourceOperator>(src);
    auto filt = MakeFilter(source, "(> x 5)");
    ASSERT_TRUE(filt.has_value());
    EXPECT_EQ(PrintAst(*filt, MakePrintOpts()), "(rel filter (rel source) (> x 5))");
}

TEST(SexpPrinter, Project) {
    NQdb::TMockSource src({"x", "y"});
    auto source = std::make_shared<TSourceOperator>(src);
    auto proj = MakeProject(source, {{"a", "x"}, {"b", "y"}});
    ASSERT_TRUE(proj.has_value());
    EXPECT_EQ(PrintAst(*proj, MakePrintOpts()), "(rel project (rel source) (a x) (b y))");
}

TEST(SexpPrinter, Aggregate) {
    NQdb::TMockSource src({"x", "y"});
    auto source = std::make_shared<TSourceOperator>(src);
    auto agg = MakeAggregate(source, {"x"}, {{"cnt", "count", ""}, {"s", "sum", "y"}});
    ASSERT_TRUE(agg.has_value());
    EXPECT_EQ(PrintAst(*agg, MakePrintOpts()),
              "(rel aggregate (rel source) (keys x) (agg cnt count) (agg s sum y))");
}

TEST(SexpPrinter, FilterOfProject) {
    NQdb::TMockSource src({"x", "y"});
    auto source = std::make_shared<TSourceOperator>(src);
    auto proj = MakeProject(source, {{"a", "x"}});
    ASSERT_TRUE(proj.has_value());
    auto filt = MakeFilter(*proj, "(> a 0)");
    ASSERT_TRUE(filt.has_value());
    EXPECT_EQ(PrintAst(*filt, MakePrintOpts()),
              "(rel filter (rel project (rel source) (a x)) (> a 0))");
}

// --- Parser ---

TEST(SexpParser, SourceNoFactory) {
    auto p = MakeParser();
    std::istringstream in("(rel source)");
    TTokenStream ts(in);
    EXPECT_FALSE(p.Parse(ts).has_value());
}

TEST(SexpParser, SourceRejectsInvalidTrailingTokenClearly) {
    auto p = MakeParser();
    std::istringstream in("(rel source \"data.parquet\" ])");
    TTokenStream ts(in);
    auto parsed = p.Parse(ts);

    ASSERT_FALSE(parsed.has_value());
    EXPECT_NE(
        parsed.error().ToString().find(
            "(rel source) expects ')' or alias string"),
        std::string::npos);
}

TEST(SexpParser, Filter) {
    NQdb::TMockSource src({"x"});
    auto sourceOp = std::make_shared<TSourceOperator>(src);

    TRelParserOptions opts;
    opts.SourceFactory = [&](std::string_view path, NQumir::TLocation) -> TOperatorPtr {
    sourceOp = std::make_shared<TSourceOperator>(src, std::string(path));
    return sourceOp;
};
    auto p = MakeParser(std::move(opts));

    auto expr = Parse(p, "(rel filter (rel source \"data.parquet\") (> x 5))");
    ASSERT_NE(expr, nullptr);

    EXPECT_EQ(expr->NodeName(), IOperator::NodeId);
    EXPECT_EQ(static_cast<IOperator&>(*expr).RelName(), TFilterOperator::OpId);

    auto& filt = static_cast<TFilterOperator&>(*expr);
    EXPECT_EQ(filt.Input(), sourceOp);
    EXPECT_NE(filt.Predicate(), nullptr);
}

TEST(SexpParser, Project) {
    NQdb::TMockSource src({"x", "y"});
    auto sourceOp = std::make_shared<TSourceOperator>(src);

    TRelParserOptions opts;
    opts.SourceFactory = [&](std::string_view path, NQumir::TLocation) -> TOperatorPtr {
    sourceOp = std::make_shared<TSourceOperator>(src, std::string(path));
    return sourceOp;
};
    auto p = MakeParser(std::move(opts));

    auto expr = Parse(p, "(rel project (rel source \"data.parquet\") (a x) (b y))");
    ASSERT_NE(expr, nullptr);

    auto& proj = static_cast<TProjectOperator&>(*expr);
    ASSERT_EQ(proj.Projections().size(), 2u);
    EXPECT_EQ(proj.Projections()[0].Name, "a");
    EXPECT_EQ(proj.Projections()[1].Name, "b");
}

TEST(SexpParser, LateMaterializePrintRoundtrip) {
    NQdb::TMockSource src({"x"});
    TRelParserOptions opts;
    opts.SourceFactory = [&](std::string_view path, NQumir::TLocation) -> TOperatorPtr
    {
        return std::make_shared<TSourceOperator>(src, std::string(path));
    };
    auto parser = MakeParser(std::move(opts));
    const std::string input = R"((rel late-materialize (rel source "data.parquet") (locator __row_id__) (column url URL string) (column event_time EventTime i64)))";

    auto expr = Parse(parser, input);
    ASSERT_NE(expr, nullptr);
    auto late = TMaybeOp<TLateMaterializeOperator>(std::static_pointer_cast<IOperator>(expr));
    ASSERT_TRUE(late);
    EXPECT_EQ(late.Cast()->LocatorColumn(), "__row_id__");
    ASSERT_EQ(late.Cast()->Columns().size(), 2u);
    EXPECT_EQ(late.Cast()->Columns()[0].OutputName, "url");
    EXPECT_EQ(late.Cast()->Columns()[0].PhysicalName, "URL");
    EXPECT_EQ(NQumir::NAst::NCore::PrintType(late.Cast()->Columns()[0].Type), "string");
    EXPECT_EQ(late.Cast()->RequiredColumnsForChild(0, {}), (std::unordered_set<std::string>{"__row_id__"}));
    ASSERT_EQ(late.Cast()->OutputColumns()->Fields.size(), 2u);
    EXPECT_EQ(late.Cast()->OutputColumns()->Fields[0].first, "url");
    EXPECT_EQ(late.Cast()->OutputColumns()->Fields[1].first, "event_time");
    EXPECT_EQ(PrintAst(expr, MakePrintOpts()), input);
}

TEST(SexpParser, FilterPrintRoundtrip) {
    NQdb::TMockSource src({"x"});
    auto sourceOp = std::make_shared<TSourceOperator>(src);

    const std::string input = "(rel filter (rel source \"data.parquet\") (> x 5))";

    TRelParserOptions opts;
    opts.SourceFactory = [&](std::string_view path, NQumir::TLocation) -> TOperatorPtr {
    sourceOp = std::make_shared<TSourceOperator>(src, std::string(path));
    return sourceOp;
};
    auto p = MakeParser(std::move(opts));

    auto expr = Parse(p, input);
    ASSERT_NE(expr, nullptr);

    auto printed = PrintAst(std::static_pointer_cast<TExpr>(expr), MakePrintOpts());
    EXPECT_EQ(printed, input);
}

TEST(SexpParser, AggregatePrintRoundtrip) {
    NQdb::TMockSource src({"x", "y"});
    auto sourceOp = std::make_shared<TSourceOperator>(src);

    const std::string input = "(rel aggregate (rel source \"data.parquet\") (keys x) (agg cnt count) (agg s sum y))";

    TRelParserOptions opts;
    opts.SourceFactory = [&](std::string_view path, NQumir::TLocation) -> TOperatorPtr {
    sourceOp = std::make_shared<TSourceOperator>(src, std::string(path));
    return sourceOp;
};
    auto p = MakeParser(std::move(opts));

    auto expr = Parse(p, input);
    ASSERT_NE(expr, nullptr);

    auto& agg = static_cast<TAggregateOperator&>(*expr);
    ASSERT_EQ(agg.GroupKeys().size(), 1u);
    EXPECT_EQ(agg.GroupKeys()[0], "x");
    ASSERT_EQ(agg.Aggs().size(), 2u);
    EXPECT_EQ(agg.Aggs()[0].Name, "cnt");
    EXPECT_EQ(agg.Aggs()[0].Func, "count");
    EXPECT_EQ(agg.Aggs()[0].Arg, nullptr);
    EXPECT_EQ(agg.Aggs()[1].Name, "s");
    EXPECT_EQ(agg.Aggs()[1].Func, "sum");
    EXPECT_NE(agg.Aggs()[1].Arg, nullptr);

    auto printed = PrintAst(std::static_pointer_cast<TExpr>(expr), MakePrintOpts());
    EXPECT_EQ(printed, input);
}

TEST(SexpParser, WindowPrintRoundtrip) {
    NQdb::TMockSource src({"a", "b", "c", "y"});
    auto sourceOp = std::make_shared<TSourceOperator>(src);

    const std::string input =
        "(rel window (rel source \"data.parquet\") (partition a b) "
        "(order (c desc nulls-default)) "
        "(frame range (start unbounded-preceding) (end current-row)) "
        "(fn w_0 rank) (fn w_1 sum y))";

    TRelParserOptions opts;
    opts.SourceFactory = [&](std::string_view path, NQumir::TLocation) -> TOperatorPtr {
    sourceOp = std::make_shared<TSourceOperator>(src, std::string(path));
    return sourceOp;
};
    auto p = MakeParser(std::move(opts));

    auto expr = Parse(p, input);
    ASSERT_NE(expr, nullptr);
    ASSERT_EQ(static_cast<IOperator&>(*expr).RelName(), TWindowOperator::OpId);

    auto& window = static_cast<TWindowOperator&>(*expr);
    ASSERT_EQ(window.PartitionKeys().size(), 2u);
    EXPECT_EQ(window.PartitionKeys()[0], "a");
    EXPECT_EQ(window.PartitionKeys()[1], "b");
    ASSERT_EQ(window.OrderKeys().size(), 1u);
    EXPECT_EQ(window.OrderKeys()[0].Column, "c");
    EXPECT_EQ(window.OrderKeys()[0].Direction, ESortDirection::Desc);
    ASSERT_TRUE(window.Frame().has_value());
    EXPECT_EQ(window.Frame()->Mode, EWindowFrameMode::Range);
    EXPECT_EQ(window.Frame()->Start.Kind, EFrameBoundKind::UnboundedPreceding);
    EXPECT_EQ(window.Frame()->End.Kind, EFrameBoundKind::CurrentRow);
    ASSERT_EQ(window.Functions().size(), 2u);
    EXPECT_EQ(window.Functions()[0].Func, "rank");
    EXPECT_EQ(window.Functions()[0].Arg, nullptr);
    EXPECT_EQ(window.Functions()[1].Func, "sum");
    EXPECT_NE(window.Functions()[1].Arg, nullptr);

    auto printed = PrintAst(std::static_pointer_cast<TExpr>(expr), MakePrintOpts());
    EXPECT_EQ(printed, input);
}

TEST(SexpParser, WindowNoFrameRoundtrip) {
    NQdb::TMockSource src({"a"});
    auto sourceOp = std::make_shared<TSourceOperator>(src);

    const std::string input = "(rel window (rel source \"data.parquet\") (fn w_0 rank))";

    TRelParserOptions opts;
    opts.SourceFactory = [&](std::string_view path, NQumir::TLocation) -> TOperatorPtr {
    sourceOp = std::make_shared<TSourceOperator>(src, std::string(path));
    return sourceOp;
};
    auto p = MakeParser(std::move(opts));

    auto expr = Parse(p, input);
    ASSERT_NE(expr, nullptr);
    auto& window = static_cast<TWindowOperator&>(*expr);
    EXPECT_TRUE(window.PartitionKeys().empty());
    EXPECT_TRUE(window.OrderKeys().empty());
    EXPECT_FALSE(window.Frame().has_value());

    auto printed = PrintAst(std::static_pointer_cast<TExpr>(expr), MakePrintOpts());
    EXPECT_EQ(printed, input);
}

TEST(SexpParser, SortPrintRoundtrip) {
    NQdb::TMockSource src({"x", "y"});
    const std::string input =
        "(rel sort (rel source \"data.parquet\") (x desc nulls-default) (y asc nulls-last))";

    TRelParserOptions opts;
    opts.SourceFactory = [&](std::string_view path, NQumir::TLocation) -> TOperatorPtr {
        return std::make_shared<TSourceOperator>(src, std::string(path));
    };
    auto p = MakeParser(std::move(opts));

    auto expr = Parse(p, input);
    ASSERT_NE(expr, nullptr);

    auto& sort = static_cast<TSortOperator&>(*expr);
    ASSERT_EQ(sort.Keys().size(), 2u);
    EXPECT_EQ(sort.Keys()[0].Column, "x");
    EXPECT_EQ(sort.Keys()[0].Direction, ESortDirection::Desc);
    EXPECT_EQ(sort.Keys()[1].Nulls, ESortNulls::Last);

    auto printed = PrintAst(std::static_pointer_cast<TExpr>(expr), MakePrintOpts());
    EXPECT_EQ(printed, input);
}

TEST(SexpParser, TopSortPrintRoundtrip) {
    NQdb::TMockSource src({"x"});
    const std::string input =
        "(rel top-sort (rel source \"data.parquet\") (x desc nulls-default) (limit 10))";

    TRelParserOptions opts;
    opts.SourceFactory = [&](std::string_view path, NQumir::TLocation) -> TOperatorPtr {
        return std::make_shared<TSourceOperator>(src, std::string(path));
    };
    auto p = MakeParser(std::move(opts));

    auto expr = Parse(p, input);
    ASSERT_NE(expr, nullptr);

    auto& sort = static_cast<TTopSortOperator&>(*expr);
    ASSERT_EQ(sort.Keys().size(), 1u);
    EXPECT_EQ(sort.Keys()[0].Column, "x");
    EXPECT_EQ(sort.Limit(), 10);

    auto printed = PrintAst(std::static_pointer_cast<TExpr>(expr), MakePrintOpts());
    EXPECT_EQ(printed, input);
}

TEST(SexpParser, LimitPrintRoundtrip) {
    NQdb::TMockSource src({"x"});
    const std::string input =
        "(rel limit (rel source \"data.parquet\") (limit 10) (offset 2))";

    TRelParserOptions opts;
    opts.SourceFactory = [&](std::string_view path, NQumir::TLocation) -> TOperatorPtr {
        return std::make_shared<TSourceOperator>(src, std::string(path));
    };
    auto p = MakeParser(std::move(opts));

    auto expr = Parse(p, input);
    ASSERT_NE(expr, nullptr);

    auto& limit = static_cast<TLimitOperator&>(*expr);
    EXPECT_EQ(limit.Limit(), 10);
    EXPECT_EQ(limit.Offset(), 2);

    auto printed = PrintAst(std::static_pointer_cast<TExpr>(expr), MakePrintOpts());
    EXPECT_EQ(printed, input);
}

TEST(SexpPrinter, Join) {
    NQdb::TMockSource leftSrc({"a", "b"});
    NQdb::TMockSource rightSrc({"c", "d"});
    auto l = std::make_shared<TSourceOperator>(leftSrc);
    auto r = std::make_shared<TSourceOperator>(rightSrc);
    auto join = MakeJoin(l, r, {{"a", "c"}}, EJoinType::Inner);
    ASSERT_TRUE(join.has_value()) << (join ? "" : join.error().ToString());
    EXPECT_EQ(PrintAst(*join, MakePrintOpts()),
              "(rel join (rel source) (rel source) ((a c)) (inner))");
}

TEST(SexpPrinter, JoinWithFilterAndMultipleKeys) {
    NQdb::TMockSource leftSrc({"a", "b"});
    NQdb::TMockSource rightSrc({"c", "d"});
    auto l = std::make_shared<TSourceOperator>(leftSrc);
    auto r = std::make_shared<TSourceOperator>(rightSrc);
    auto join = MakeJoin(l, r, {{"a", "c"}, {"b", "d"}}, EJoinType::Left, "(< b d)");
    ASSERT_TRUE(join.has_value()) << (join ? "" : join.error().ToString());
    EXPECT_EQ(PrintAst(*join, MakePrintOpts()),
              "(rel join (rel source) (rel source) ((a c) (b d)) (left) (residual (< b d)))");
}

TEST(SexpParser, JoinPrintRoundtrip) {
    NQdb::TMockSource leftSrc({"a", "b"});
    NQdb::TMockSource rightSrc({"c", "d"});

    const std::string input =
        "(rel join (rel source \"left.parquet\") (rel source \"right.parquet\") "
        "((a c nulls_equal) (b d)) (left) (residual (< b d)))";

    TRelParserOptions opts;
    opts.SourceFactory = [&](std::string_view path, NQumir::TLocation) -> TOperatorPtr {
        if (path == "left.parquet") {
            return std::make_shared<TSourceOperator>(leftSrc, std::string(path));
        }
        return std::make_shared<TSourceOperator>(rightSrc, std::string(path));
    };
    auto p = MakeParser(std::move(opts));

    auto expr = Parse(p, input);
    ASSERT_NE(expr, nullptr);

    auto& join = static_cast<TJoinOperator&>(*expr);
    ASSERT_EQ(join.Keys().size(), 2u);
    EXPECT_EQ(join.Keys()[0].Left, "a");
    EXPECT_EQ(join.Keys()[0].Right, "c");
    EXPECT_TRUE(join.Keys()[0].NullsEqual);
    EXPECT_FALSE(join.Keys()[1].NullsEqual);
    EXPECT_EQ(join.JoinType(), EJoinType::Left);
    EXPECT_NE(join.Filter(), nullptr);

    auto printed = PrintAst(std::static_pointer_cast<TExpr>(expr), MakePrintOpts());
    EXPECT_EQ(printed, input);

    const std::string legacy =
        "(rel join (rel source \"left.parquet\") (rel source \"right.parquet\") "
        "((a c nulls_equal) (b d)) (left) (< b d))";
    auto legacyExpr = Parse(p, legacy);
    ASSERT_NE(legacyExpr, nullptr);
    EXPECT_EQ(PrintAst(std::static_pointer_cast<TExpr>(legacyExpr), MakePrintOpts()),
              input);
}

TEST(SexpParser, JoinAnnotationRoundtrip) {
    NQdb::TMockSource leftSrc({"a", "b"});
    NQdb::TMockSource rightSrc({"c", "d"});

    auto parseJoin = [&](const std::string& input) {
        TRelParserOptions opts;
        opts.SourceFactory =
            [&](std::string_view path, NQumir::TLocation) -> TOperatorPtr {
            if (path == "left.parquet") {
                return std::make_shared<TSourceOperator>(leftSrc, std::string(path));
            }
            return std::make_shared<TSourceOperator>(rightSrc, std::string(path));
        };
        auto p = MakeParser(std::move(opts));
        return Parse(p, input);
    };
    auto roundtrip = [&](const std::string& input) {
        auto expr = parseJoin(input);
        EXPECT_NE(expr, nullptr) << input;
        if (expr) {
            EXPECT_EQ(PrintAst(std::static_pointer_cast<TExpr>(expr), MakePrintOpts()),
                      input);
        }
        return expr;
    };

    const std::string head =
        "(rel join (rel source \"left.parquet\") (rel source \"right.parquet\") "
        "((a c)) ";

    auto none = roundtrip(head + "(inner))");
    ASSERT_NE(none, nullptr);
    EXPECT_FALSE(static_cast<TJoinOperator&>(*none).RuntimeFilter().has_value());
    EXPECT_EQ(static_cast<TJoinOperator&>(*none).Filter(), nullptr);

    auto emitOnly = roundtrip(head + "(inner) (emit-filter 7 right))");
    ASSERT_NE(emitOnly, nullptr);
    const auto& emitted = static_cast<TJoinOperator&>(*emitOnly).RuntimeFilter();
    ASSERT_TRUE(emitted.has_value());
    EXPECT_EQ(emitted->Id, 7u);
    EXPECT_EQ(emitted->BuildSide, EJoinFilterSide::Right);
    EXPECT_EQ(static_cast<TJoinOperator&>(*emitOnly).Filter(), nullptr);

    auto residualOnly = roundtrip(head + "(left) (residual (< b d)))");
    ASSERT_NE(residualOnly, nullptr);
    EXPECT_NE(static_cast<TJoinOperator&>(*residualOnly).Filter(), nullptr);
    EXPECT_FALSE(
        static_cast<TJoinOperator&>(*residualOnly).RuntimeFilter().has_value());

    auto both = roundtrip(
        head + "(left) (residual (< b d)) (emit-filter 42 left))");
    ASSERT_NE(both, nullptr);
    const auto& bothFilter = static_cast<TJoinOperator&>(*both).RuntimeFilter();
    ASSERT_TRUE(bothFilter.has_value());
    EXPECT_EQ(bothFilter->Id, 42u);
    EXPECT_EQ(bothFilter->BuildSide, EJoinFilterSide::Left);
    EXPECT_NE(static_cast<TJoinOperator&>(*both).Filter(), nullptr);

    auto rejects = [&](const std::string& input) {
        TRelParserOptions opts;
        opts.SourceFactory =
            [&](std::string_view path, NQumir::TLocation) -> TOperatorPtr {
            if (path == "left.parquet") {
                return std::make_shared<TSourceOperator>(leftSrc, std::string(path));
            }
            return std::make_shared<TSourceOperator>(rightSrc, std::string(path));
        };
        auto p = MakeParser(std::move(opts));
        std::istringstream in(input);
        TTokenStream ts(in);
        EXPECT_FALSE(p.Parse(ts).has_value()) << input;
    };
    rejects(head + "(inner) (emit-filter 7 middle))");
    rejects(head + "(inner) (emit-filter 0 right))");
    rejects(head + "(inner) (bogus 1))");
    rejects(head + "(inner) (emit-filter 1 left) (emit-filter 2 right))");
    rejects(head + "(inner) (emit-filter 4294967296 right))");
}

TEST(SexpParser, CloneKeepsJoinRuntimeFilter) {
    NQdb::TMockSource leftSrc({"a", "b"});
    NQdb::TMockSource rightSrc({"c", "d"});

    TRelParserOptions opts;
    opts.SourceFactory = [&](std::string_view path, NQumir::TLocation) -> TOperatorPtr {
        if (path == "left.parquet") {
            return std::make_shared<TSourceOperator>(leftSrc, std::string(path));
        }
        return std::make_shared<TSourceOperator>(rightSrc, std::string(path));
    };
    auto p = MakeParser(std::move(opts));
    auto expr = Parse(p,
        "(rel join (rel source \"left.parquet\") (rel source \"right.parquet\") "
        "((a c)) (inner) (emit-filter 3 left))");
    ASSERT_NE(expr, nullptr);

    auto clone = CloneOperator(std::static_pointer_cast<IOperator>(expr));
    ASSERT_NE(clone, nullptr);
    auto* cloned = dynamic_cast<TJoinOperator*>(clone.get());
    ASSERT_NE(cloned, nullptr);
    ASSERT_TRUE(cloned->RuntimeFilter().has_value());
    EXPECT_EQ(cloned->RuntimeFilter()->Id, 3u);
    EXPECT_EQ(cloned->RuntimeFilter()->BuildSide, EJoinFilterSide::Left);
}

TEST(SexpParser, JoinToStringUsesTaggedForms) {
    NQdb::TMockSource leftSrc({"a", "b"});
    NQdb::TMockSource rightSrc({"c", "d"});
    auto factory = [&](std::string_view path, NQumir::TLocation) -> TOperatorPtr {
        if (path == "left.parquet") {
            return std::make_shared<TSourceOperator>(leftSrc, std::string(path));
        }
        return std::make_shared<TSourceOperator>(rightSrc, std::string(path));
    };
    TRelParserOptions opts;
    opts.SourceFactory = factory;
    auto p = MakeParser(std::move(opts));
    auto expr = Parse(p,
        "(rel join (rel source \"left.parquet\") (rel source \"right.parquet\") "
        "((a c)) (left) (residual (< b d)) (emit-filter 7 right))");
    ASSERT_NE(expr, nullptr);

    const std::string printed = expr->ToString();
    EXPECT_NE(printed.find("(residual "), std::string::npos) << printed;
    EXPECT_NE(printed.find("(emit-filter 7 right)"), std::string::npos) << printed;

    EXPECT_EQ(printed.find("(left) (< b d)"), std::string::npos) << printed;
}

TEST(SexpParser, FlattenKeepsAnnotatedJoin) {
    NQdb::TMockSource leftSrc({"a", "b"});
    NQdb::TMockSource rightSrc({"c", "d"});
    NQdb::TMockSource thirdSrc({"e", "f"});

    TRelParserOptions opts;
    opts.SourceFactory = [&](std::string_view path, NQumir::TLocation) -> TOperatorPtr {
        if (path == "left.parquet") {
            return std::make_shared<TSourceOperator>(leftSrc, std::string(path));
        }
        if (path == "right.parquet") {
            return std::make_shared<TSourceOperator>(rightSrc, std::string(path));
        }
        return std::make_shared<TSourceOperator>(thirdSrc, std::string(path));
    };
    auto p = MakeParser(std::move(opts));
    auto expr = Parse(p,
        "(rel join"
        " (rel join (rel source \"left.parquet\") (rel source \"right.parquet\")"
        " ((a c)) (inner) (emit-filter 7 right))"
        " (rel source \"third.parquet\") ((a e)) (inner))");
    ASSERT_NE(expr, nullptr);

    auto flattened = FlattenInnerJoins(std::static_pointer_cast<IOperator>(expr));
    ASSERT_NE(flattened, nullptr);

    std::function<const TJoinOperator*(const TOperatorPtr&)> findAnnotated =
        [&](const TOperatorPtr& node) -> const TJoinOperator* {
        if (!node) {
            return nullptr;
        }
        if (auto* join = dynamic_cast<TJoinOperator*>(node.get());
            join && join->RuntimeFilter()) {
            return join;
        }
        for (const auto& child : node->Children()) {
            if (auto childOp = TMaybeNode<IOperator>(child)) {
                if (auto* found = findAnnotated(childOp.Cast())) {
                    return found;
                }
            }
        }
        return nullptr;
    };

    const auto* annotated = findAnnotated(flattened);
    ASSERT_NE(annotated, nullptr) << "flattening dropped the runtime-filter join";
    EXPECT_EQ(annotated->RuntimeFilter()->Id, 7u);
    EXPECT_EQ(annotated->RuntimeFilter()->BuildSide, EJoinFilterSide::Right);
    EXPECT_EQ(annotated->Keys().size(), 1u);
}

TEST(SexpParser, CrossJoinAcceptsEmptyKeyList) {
    NQdb::TMockSource leftSrc({"a", "b"});
    NQdb::TMockSource rightSrc({"c", "d"});

    TRelParserOptions opts;
    opts.SourceFactory = [&](std::string_view path, NQumir::TLocation) -> TOperatorPtr {
        if (path == "left.parquet") {
            return std::make_shared<TSourceOperator>(leftSrc, std::string(path));
        }
        return std::make_shared<TSourceOperator>(rightSrc, std::string(path));
    };
    auto p = MakeParser(std::move(opts));

    // Empty key list + inner → cross join, must parse successfully.
    std::istringstream in(
        "(rel join (rel source \"left.parquet\") (rel source \"right.parquet\") () (inner))");
    TTokenStream ts(in);
    EXPECT_TRUE(p.Parse(ts).has_value());
}

TEST(SexpParser, JoinRejectsEmptyKeysWithNonInnerType) {
    NQdb::TMockSource leftSrc({"a", "b"});
    NQdb::TMockSource rightSrc({"c", "d"});

    TRelParserOptions opts;
    opts.SourceFactory = [&](std::string_view path, NQumir::TLocation) -> TOperatorPtr {
        if (path == "left.parquet") {
            return std::make_shared<TSourceOperator>(leftSrc, std::string(path));
        }
        return std::make_shared<TSourceOperator>(rightSrc, std::string(path));
    };
    auto p = MakeParser(std::move(opts));

    std::istringstream in(
        "(rel join (rel source \"left.parquet\") (rel source \"right.parquet\") () (left_semi))");
    TTokenStream ts(in);
    EXPECT_FALSE(p.Parse(ts).has_value());
}

// --- CTE envelope round-trip ---

TEST(SexpCte, EnvelopeRoundTrips) {
    NQdb::TMockSource src({"a"});

    auto makeOpts = [&] {
        TRelParserOptions opts;
        opts.CteRegistry =
            std::make_shared<std::unordered_map<uint32_t, TCteDefinitionPtr>>();
        opts.SourceFactory = [&](std::string_view path, NQumir::TLocation) -> TOperatorPtr {
            return std::make_shared<TSourceOperator>(src, std::string(path));
        };
        return opts;
    };

    auto p = MakeParser(makeOpts());
    auto expr = Parse(p,
        "(query"
        "  (cte 0 (rel source \"t\"))"
        "  (main (rel join (rel cte-ref 0) (rel cte-ref 0) () (inner))))");
    ASSERT_NE(expr, nullptr);

    // Both references resolve to the one shared definition.
    auto join = TMaybeOp<TJoinOperator>(std::static_pointer_cast<IOperator>(expr));
    ASSERT_TRUE(join);
    auto left = TMaybeOp<TCteRef>(join.Cast()->Left());
    auto right = TMaybeOp<TCteRef>(join.Cast()->Right());
    ASSERT_TRUE(left);
    ASSERT_TRUE(right);
    EXPECT_EQ(left.Cast()->Def().get(), right.Cast()->Def().get());
    EXPECT_NE(left.Cast()->Def()->OutputType, nullptr);

    std::ostringstream out1;
    PrintRelPlan(out1, std::static_pointer_cast<IOperator>(expr));
    EXPECT_NE(out1.str().find("(query"), std::string::npos);
    EXPECT_NE(out1.str().find("(cte 0"), std::string::npos);

    // Re-parse the printed form (fresh registry) and print again: idempotent.
    auto p2 = MakeParser(makeOpts());
    auto expr2 = Parse(p2, out1.str());
    ASSERT_NE(expr2, nullptr);
    std::ostringstream out2;
    PrintRelPlan(out2, std::static_pointer_cast<IOperator>(expr2));
    EXPECT_EQ(out2.str(), out1.str());
}

TEST(SexpCte, NestedDefinitionsRoundTrip) {
    NQdb::TMockSource src({"a"});

    auto makeOpts = [&] {
        TRelParserOptions opts;
        opts.CteRegistry =
            std::make_shared<std::unordered_map<uint32_t, TCteDefinitionPtr>>();
        opts.SourceFactory = [&](std::string_view path, NQumir::TLocation) -> TOperatorPtr {
            return std::make_shared<TSourceOperator>(src, std::string(path));
        };
        return opts;
    };

    auto p = MakeParser(makeOpts());
    auto expr = Parse(p,
        "(query"
        "  (cte 0 (rel source \"base\"))"
        "  (cte 1 (rel project (rel cte-ref 0) (a a)))"
        "  (main (rel cte-ref 1)))");
    ASSERT_NE(expr, nullptr);

    auto defs = CollectCteDefinitions(std::static_pointer_cast<IOperator>(expr));
    ASSERT_EQ(defs.size(), 2u);
    EXPECT_EQ(defs[0]->Id, 0u);
    EXPECT_EQ(defs[1]->Id, 1u);
    EXPECT_NE(defs[0]->OutputType, nullptr);
    EXPECT_NE(defs[1]->OutputType, nullptr);

    std::ostringstream out1;
    PrintRelPlan(out1, std::static_pointer_cast<IOperator>(expr));

    auto p2 = MakeParser(makeOpts());
    auto expr2 = Parse(p2, out1.str());
    ASSERT_NE(expr2, nullptr);
    std::ostringstream out2;
    PrintRelPlan(out2, std::static_pointer_cast<IOperator>(expr2));
    EXPECT_EQ(out2.str(), out1.str());
}

TEST(SexpCte, DuplicateDefinitionIdFails) {
    NQdb::TMockSource src({"a"});

    TRelParserOptions opts;
    opts.CteRegistry =
        std::make_shared<std::unordered_map<uint32_t, TCteDefinitionPtr>>();
    opts.SourceFactory = [&](std::string_view path, NQumir::TLocation) -> TOperatorPtr {
        return std::make_shared<TSourceOperator>(src, std::string(path));
    };
    auto p = MakeParser(std::move(opts));

    std::istringstream in(
        "(query"
        "  (cte 0 (rel source \"a\"))"
        "  (cte 0 (rel source \"b\"))"
        "  (main (rel cte-ref 0)))");
    TTokenStream ts(in);
    auto result = p.Parse(ts);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().ToString().find("duplicate CTE id"), std::string::npos);
}

TEST(SexpCte, MaterializedPlanIsNotRelPlanSerializable) {
    NQdb::TMockSource src({"a"});
    auto producer = std::make_shared<TSourceOperator>(src, "base");

    auto def = std::make_shared<TCteDefinition>();
    def->Id = 0;
    def->Plan = producer;
    def->OutputType = producer->OutputColumns();

    auto materialization = std::make_shared<TCteMaterialization>();
    materialization->Plan = producer;
    materialization->RefCount = 2;

    std::vector<TOperatorPtr> inputs;
    inputs.push_back(std::make_shared<TCteConsumer>(def, materialization));
    inputs.push_back(std::make_shared<TCteConsumer>(def, materialization));
    auto plan = std::make_shared<TUnionAllOperator>(std::move(inputs));

    std::ostringstream sexp;
    EXPECT_THROW(PrintRelPlan(sexp, plan), std::runtime_error);

    std::ostringstream tree;
    PrintPlanTreeWithCtes(tree, plan);
    EXPECT_NE(tree.str().find("cte #0 (materialized, ×2):"), std::string::npos);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
