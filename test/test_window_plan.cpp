#include <gtest/gtest.h>
#include "mock_source.h"

#include <qdb/plan/build.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/ops/window.h>
#include <qdb/plan/pipeline.h>
#include <qdb/sql/parser.h>

#include <qumir/parser/ast.h>

#include <algorithm>
#include <expected>
#include <map>
#include <sstream>
#include <string>
#include <string_view>

using namespace NQdb;

namespace {

std::expected<TOperatorPtr, NQumir::TError> BuildSqlPlan(
    std::string_view sql, const std::map<std::string, ISource*>& tables)
{
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
        auto it = tables.find(std::string(table));
        if (it == tables.end()) {
            return std::unexpected(NQumir::TError("unknown table: " + std::string(table)));
        }
        return std::make_shared<TSourceOperator>(*it->second, std::string(table));
    };
    return BuildPlan(parsed.value(), factory);
}

const TWindowOperator* FindWindow(const TOperatorPtr& op) {
    if (auto* window = dynamic_cast<const TWindowOperator*>(op.get())) {
        return window;
    }
    for (const auto& child : op->Children()) {
        if (auto childOp = NQumir::NAst::TMaybeNode<IOperator>(child)) {
            if (auto* found = FindWindow(childOp.Cast())) {
                return found;
            }
        }
    }
    return nullptr;
}

int CountWindows(const TOperatorPtr& op) {
    int count = dynamic_cast<const TWindowOperator*>(op.get()) ? 1 : 0;
    for (const auto& child : op->Children()) {
        if (auto childOp = NQumir::NAst::TMaybeNode<IOperator>(child)) {
            count += CountWindows(childOp.Cast());
        }
    }
    return count;
}

const TJoinOperator* FindJoin(const TOperatorPtr& op) {
    if (auto* join = dynamic_cast<const TJoinOperator*>(op.get())) {
        return join;
    }
    for (const auto& child : op->Children()) {
        if (auto childOp = NQumir::NAst::TMaybeNode<IOperator>(child)) {
            if (auto* found = FindJoin(childOp.Cast())) {
                return found;
            }
        }
    }
    return nullptr;
}

void AssertWindowFuncArgsExistInInput(const TOperatorPtr& op) {
    if (auto* window = dynamic_cast<const TWindowOperator*>(op.get())) {
        auto* input = static_cast<NQumir::NAst::TStructType*>(
            window->Input()->OutputColumns().get());
        ASSERT_NE(input, nullptr);
        auto hasField = [&](const std::string& name) {
            for (const auto& [fieldName, _] : input->Fields) {
                if (fieldName == name) {
                    return true;
                }
            }
            return false;
        };
        for (const auto& func : window->Functions()) {
            if (auto ident = NQumir::NAst::TMaybeNode<NQumir::NAst::TIdentExpr>(func.Arg)) {
                EXPECT_TRUE(hasField(ident.Cast()->Name))
                    << "missing window argument " << ident.Cast()->Name;
            }
        }
    }
    for (const auto& child : op->Children()) {
        if (auto childOp = NQumir::NAst::TMaybeNode<IOperator>(child)) {
            AssertWindowFuncArgsExistInInput(childOp.Cast());
        }
    }
}

} // namespace

TEST(WindowPlan, SingleSpecStructure) {
    NQdb::TMockSource t({"a", "b", "c"});
    std::map<std::string, ISource*> tables = {{"t", &t}};

    auto plan = BuildSqlPlan(
        "SELECT rank() OVER (PARTITION BY a ORDER BY b DESC) AS r FROM t", tables);
    ASSERT_TRUE(plan.has_value()) << (plan ? "" : plan.error().ToString());

    auto* window = FindWindow(*plan);
    ASSERT_NE(window, nullptr);
    ASSERT_EQ(window->PartitionKeys().size(), 1u);
    EXPECT_EQ(window->PartitionKeys()[0], "t.a");
    ASSERT_EQ(window->OrderKeys().size(), 1u);
    EXPECT_EQ(window->OrderKeys()[0].Column, "t.b");
    EXPECT_EQ(window->OrderKeys()[0].Direction, ESortDirection::Desc);
    ASSERT_EQ(window->Functions().size(), 1u);
    EXPECT_EQ(window->Functions()[0].Func, "rank");
    EXPECT_EQ(window->Functions()[0].Arg, nullptr);
    // ORDER BY present with no explicit frame -> RANGE ... CURRENT ROW.
    ASSERT_TRUE(window->Frame().has_value());
    EXPECT_EQ(window->Frame()->Mode, EWindowFrameMode::Range);
    EXPECT_EQ(window->Frame()->Start.Kind, EFrameBoundKind::UnboundedPreceding);
    EXPECT_EQ(window->Frame()->End.Kind, EFrameBoundKind::CurrentRow);
}

TEST(WindowPlan, DistinctSpecsStack) {
    NQdb::TMockSource t({"a", "b", "x"});
    std::map<std::string, ISource*> tables = {{"t", &t}};

    // Two functions share the (partition a) spec -> one node; a different spec
    // (partition b) -> a second, stacked node.
    auto plan = BuildSqlPlan(
        "SELECT rank() OVER (PARTITION BY a) AS r1, "
        "sum(x) OVER (PARTITION BY a) AS s1, "
        "rank() OVER (PARTITION BY b) AS r2 FROM t", tables);
    ASSERT_TRUE(plan.has_value()) << (plan ? "" : plan.error().ToString());

    EXPECT_EQ(CountWindows(*plan), 2);
}

TEST(WindowPlan, FullPipelineOverJoin) {
    NQdb::TMockSource orders({"o_orderkey", "o_custkey", "o_total"});
    NQdb::TMockSource customer({"c_custkey", "c_name"});
    std::map<std::string, ISource*> tables = {{"orders", &orders}, {"customer", &customer}};

    auto plan = BuildSqlPlan(
        "SELECT c_name, "
        "rank() OVER (PARTITION BY c_custkey ORDER BY o_total DESC) AS r "
        "FROM orders, customer WHERE o_custkey = c_custkey", tables);
    ASSERT_TRUE(plan.has_value()) << (plan ? "" : plan.error().ToString());

    // Full pipeline must run through the window (join reorder + equi-key
    // extraction + column pruning all recurse into the window's child).
    ApplyPlanPasses(*plan);

    EXPECT_EQ(CountWindows(*plan), 1);
    // The join equality below the window was lifted into a join key.
    auto* join = FindJoin(*plan);
    ASSERT_NE(join, nullptr);
    EXPECT_FALSE(join->Keys().empty());
}

TEST(WindowPlan, CteWindowAggregateArgsAreIndependentPerInline) {
    NQdb::TMockSource t({"k", "v"});
    std::map<std::string, ISource*> tables = {{"t", &t}};

    auto plan = BuildSqlPlan(
        "WITH w AS ("
        "  SELECT k, sum(v) AS s, avg(sum(v)) OVER (PARTITION BY k) AS a "
        "  FROM t GROUP BY k"
        ") "
        "SELECT l.a, r.a FROM w l, w r WHERE l.k = r.k",
        tables);
    ASSERT_TRUE(plan.has_value()) << (plan ? "" : plan.error().ToString());

    AssertWindowFuncArgsExistInInput(*plan);
}

TEST(WindowPlan, HavingScalarSubqueryIgnoresFunctionCalleeForCorrelation) {
    NQdb::TMockSource storeSales({
        "ss_item_sk",
        "ss_net_profit",
        "ss_store_sk",
        "ss_hdemo_sk",
    });
    std::map<std::string, ISource*> tables = {{"store_sales", &storeSales}};

    auto plan = BuildSqlPlan(
        "SELECT item_sk, rank() OVER (ORDER BY rank_col ASC) AS r "
        "FROM ("
        "  SELECT ss_item_sk AS item_sk, avg(ss_net_profit) AS rank_col "
        "  FROM store_sales ss1 "
        "  WHERE ss_store_sk = 2 "
        "  GROUP BY ss_item_sk "
        "  HAVING avg(ss_net_profit) > 0.9 * ("
        "    SELECT avg(ss_net_profit) AS rank_col "
        "    FROM store_sales "
        "    WHERE ss_store_sk = 2 AND ss_hdemo_sk IS NULL "
        "    GROUP BY ss_store_sk"
        "  )"
        ") v",
        tables);
    ASSERT_TRUE(plan.has_value()) << (plan ? "" : plan.error().ToString());
    ApplyPlanPasses(*plan);

    auto* window = FindWindow(*plan);
    ASSERT_NE(window, nullptr);
    auto* scalarJoin = FindJoin(*plan);
    ASSERT_NE(scalarJoin, nullptr);
    EXPECT_EQ(scalarJoin->JoinType(), EJoinType::Inner);
    EXPECT_TRUE(scalarJoin->Keys().empty());
    auto* output = static_cast<NQumir::NAst::TStructType*>(
        scalarJoin->OutputColumns().get());
    ASSERT_NE(output, nullptr);
    EXPECT_TRUE(std::ranges::any_of(output->Fields, [](const auto& field) {
        return field.first == "__scalar_0__";
    }));
}

TEST(WindowPlan, ColumnListAliasesSurviveTyping) {
    NQdb::TMockSource t({"x"});
    std::map<std::string, ISource*> tables = {{"t", &t}};

    for (const auto sql : {
        "SELECT q.renamed FROM (SELECT x + 1 FROM t) AS q(renamed)",
        "WITH q(renamed) AS (SELECT x + 1 FROM t) SELECT renamed FROM q",
    }) {
        auto plan = BuildSqlPlan(sql, tables);
        ASSERT_TRUE(plan.has_value()) << (plan ? "" : plan.error().ToString());
        ApplyPlanPasses(*plan);
        auto* output = static_cast<NQumir::NAst::TStructType*>(
            (*plan)->OutputColumns().get());
        ASSERT_NE(output, nullptr);
        ASSERT_EQ(output->Fields.size(), 1u);
        EXPECT_NE(output->Fields[0].second, nullptr);
    }
}

TEST(WindowPlan, ScalarSubqueryIgnoresInListTemporaryForCorrelation) {
    NQdb::TMockSource customer({
        "c_phone",
        "c_acctbal",
        "c_custkey",
    });
    NQdb::TMockSource orders({"o_custkey"});
    std::map<std::string, ISource*> tables = {
        {"customer", &customer},
        {"orders", &orders},
    };

    auto plan = BuildSqlPlan(
        "SELECT cntrycode, count(*) AS numcust, sum(c_acctbal) AS totacctbal "
        "FROM ("
        "  SELECT substring(c_phone FROM 1 FOR 2) AS cntrycode, c_acctbal "
        "  FROM customer "
        "  WHERE substring(c_phone FROM 1 FOR 2) IN ('27', '20') "
        "    AND c_acctbal > ("
        "      SELECT avg(c_acctbal) "
        "      FROM customer "
        "      WHERE c_acctbal > 0.00 "
        "        AND substring(c_phone FROM 1 FOR 2) IN ('27', '20')"
        "    ) "
        "    AND NOT EXISTS ("
        "      SELECT * FROM orders WHERE o_custkey = c_custkey"
        "    )"
        ") AS custsale "
        "GROUP BY cntrycode "
        "ORDER BY cntrycode",
        tables);
    ASSERT_TRUE(plan.has_value()) << (plan ? "" : plan.error().ToString());
}

TEST(WindowPlan, WindowOutsideSelectListRejected) {
    NQdb::TMockSource t({"a", "b"});
    std::map<std::string, ISource*> tables = {{"t", &t}};

    // Windows are only valid in the SELECT list / ORDER BY; elsewhere they must
    // yield a clean error rather than crashing in later passes.
    auto inWhere = BuildSqlPlan(
        "SELECT a FROM t WHERE rank() OVER (ORDER BY b) > 1", tables);
    EXPECT_FALSE(inWhere.has_value());

    auto inHaving = BuildSqlPlan(
        "SELECT a FROM t GROUP BY a HAVING rank() OVER (ORDER BY a) > 1", tables);
    EXPECT_FALSE(inHaving.has_value());

    // A window inside an independent subquery in WHERE is that subquery's own,
    // and must NOT be rejected here.
    auto inSubqueryOk = BuildSqlPlan(
        "SELECT a FROM t WHERE a IN "
        "(SELECT b FROM (SELECT b, rank() OVER (ORDER BY b) rk FROM t) s WHERE rk = 1)",
        tables);
    EXPECT_TRUE(inSubqueryOk.has_value())
        << (inSubqueryOk ? "" : inSubqueryOk.error().ToString());
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
