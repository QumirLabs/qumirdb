#include <gtest/gtest.h>
#include "mock_source.h"

#include <qdb/plan/build.h>
#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/cte_ref.h>
#include <qdb/plan/ops/cte_consumer.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/passes/column_pruning.h>
#include <qdb/plan/passes/cte_reuse.h>
#include <qdb/plan/passes/qualify_columns.h>
#include <qdb/plan/passes/typing.h>
#include <qdb/plan/pipeline.h>
#include <qdb/sql/parser.h>
#include <qdb/plan/passes/sql_binding.h>

#include <qumir/parser/core/printer.h>

#include <expected>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <string_view>

using namespace NQdb;

namespace {

TOperatorPtr BuildSqlPlan(std::string_view sql, const std::map<std::string, ISource*>& tables) {
    std::istringstream in{std::string(sql)};
    NSql::TTokenStream ts(in);
    NSql::TParser parser;
    auto parsed = parser.Parse(ts);
    EXPECT_TRUE(parsed.has_value()) << (parsed ? "" : parsed.error().ToString());
    auto factory = [&](std::string_view table)
        -> std::expected<TOperatorPtr, NQumir::TError>
    {
        auto it = tables.find(std::string(table));
        if (it == tables.end()) {
            return std::unexpected(NQumir::TError("unknown table: " + std::string(table)));
        }
        return std::make_shared<TSourceOperator>(*it->second, std::string(table));
    };
    auto plan = BuildPlan(parsed.value(), factory);
    EXPECT_TRUE(plan.has_value()) << (plan ? "" : plan.error().ToString());
    ResolveSqlBindings(*plan);
    return plan.value();
}

void Annotate(const TOperatorPtr& plan) {
    AssignSourceAliases(plan);
    QualifyColumns(plan);
    AnnotateTypes(plan);
}

// Builds and annotates main + every definition, then propagates demand (as the
// pipeline does up to the reuse decision).
TCteUsageMap Analyze(const TOperatorPtr& plan) {
    for (const auto& def : CollectCteDefinitions(plan)) {
        Annotate(def->Plan);
    }
    Annotate(plan);
    return PropagateCteDemands(plan);
}

std::multiset<size_t> RefCounts(const TCteUsageMap& usage) {
    std::multiset<size_t> counts;
    for (const auto& [_, info] : usage) {
        counts.insert(info.StaticRefCount);
    }
    return counts;
}

void CollectConsumers(
    const TOperatorPtr& op, std::vector<std::shared_ptr<TCteConsumer>>& out) {
    if (!op) {
        return;
    }
    if (auto consumer = TMaybeOp<TCteConsumer>(op)) {
        out.push_back(consumer.Cast());
        return;
    }
    for (const auto& child : op->Children()) {
        if (auto childOp = NQumir::NAst::TMaybeNode<IOperator>(child)) {
            CollectConsumers(childOp.Cast(), out);
        }
    }
}

std::vector<std::shared_ptr<TCteConsumer>> FindConsumers(const TOperatorPtr& op) {
    std::vector<std::shared_ptr<TCteConsumer>> out;
    CollectConsumers(op, out);
    return out;
}

TOperatorPtr FindSource(const TOperatorPtr& op) {
    if (!op) {
        return nullptr;
    }
    if (TMaybeOp<TSourceOperator>(op)) {
        return op;
    }
    for (const auto& child : op->Children()) {
        if (auto childOp = NQumir::NAst::TMaybeNode<IOperator>(child)) {
            if (auto found = FindSource(childOp.Cast())) {
                return found;
            }
        }
    }
    return nullptr;
}

std::shared_ptr<TFilterOperator> FindFilter(const TOperatorPtr& op) {
    if (!op) {
        return nullptr;
    }
    if (auto filter = TMaybeOp<TFilterOperator>(op)) {
        return filter.Cast();
    }
    for (const auto& child : op->Children()) {
        if (auto childOp = NQumir::NAst::TMaybeNode<IOperator>(child)) {
            if (auto found = FindFilter(childOp.Cast())) {
                return found;
            }
        }
    }
    return nullptr;
}

TOperatorPtr FindAggregate(const TOperatorPtr& op) {
    if (!op) {
        return nullptr;
    }
    if (TMaybeOp<TAggregateOperator>(op)) {
        return op;
    }
    for (const auto& child : op->Children()) {
        if (auto childOp = NQumir::NAst::TMaybeNode<IOperator>(child)) {
            if (auto found = FindAggregate(childOp.Cast())) {
                return found;
            }
        }
    }
    return nullptr;
}

std::vector<std::string> FieldNames(const TOperatorPtr& op) {
    std::vector<std::string> names;
    auto* schema = static_cast<NQumir::NAst::TStructType*>(op->OutputColumns().get());
    if (schema) {
        for (const auto& [name, _] : schema->Fields) {
            names.push_back(name);
        }
    }
    return names;
}

bool ContainsCteRef(const TOperatorPtr& op) {
    if (!op) {
        return false;
    }
    if (TMaybeOp<TCteRef>(op)) {
        return true;
    }
    for (const auto& child : op->Children()) {
        if (auto childOp = NQumir::NAst::TMaybeNode<IOperator>(child)) {
            if (ContainsCteRef(childOp.Cast())) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

TEST(CteReuse, RefcountDecidesMode) {
    auto single = std::make_shared<TCteDefinition>();
    auto shared = std::make_shared<TCteDefinition>();

    TCteUsageMap usage;
    usage[single.get()].StaticRefCount = 1;
    usage[shared.get()].StaticRefCount = 3;

    auto decisions = ChooseCteReuse(usage);
    EXPECT_EQ(decisions.at(single.get()).Mode, ECteReuseMode::Inline);
    EXPECT_EQ(decisions.at(shared.get()).Mode, ECteReuseMode::Materialize);
}

TEST(CteReuse, SingleReferenceInlines) {
    NQdb::TMockSource t({"a"});
    std::map<std::string, ISource*> tables = {{"t", &t}};

    auto usage = Analyze(BuildSqlPlan("WITH x AS (SELECT a FROM t) SELECT a FROM x", tables));
    EXPECT_EQ(RefCounts(usage), (std::multiset<size_t>{1}));
}

TEST(CteReuse, TwoReferencesMaterialize) {
    NQdb::TMockSource t({"a"});
    std::map<std::string, ISource*> tables = {{"t", &t}};

    auto usage = Analyze(BuildSqlPlan(
        "WITH x AS (SELECT a FROM t) SELECT p.a FROM x p JOIN x q ON p.a = q.a", tables));
    EXPECT_EQ(RefCounts(usage), (std::multiset<size_t>{2}));
}

// Multiplicity through inline/materialized parents is not modeled yet: b is
// referenced twice, a once (inside b), regardless of b's own reuse.
TEST(CteReuse, NestedRefcountsAreSyntactic) {
    NQdb::TMockSource t({"a"});
    std::map<std::string, ISource*> tables = {{"t", &t}};

    auto usage = Analyze(BuildSqlPlan(
        "WITH a AS (SELECT a FROM t), b AS (SELECT a FROM a) "
        "SELECT p.a FROM b p JOIN b q ON p.a = q.a", tables));
    EXPECT_EQ(RefCounts(usage), (std::multiset<size_t>{1, 2}));
}

TEST(CteReuse, ApplyRemovesEveryCteRef) {
    NQdb::TMockSource t({"a"});
    std::map<std::string, ISource*> tables = {{"t", &t}};

    auto plan = BuildSqlPlan(
        "WITH x AS (SELECT a FROM t) SELECT p.a FROM x p JOIN x q ON p.a = q.a", tables);
    auto usage = Analyze(plan);
    auto resolved = ApplyCteReuse(std::move(plan), ChooseCteReuse(usage));
    EXPECT_FALSE(ContainsCteRef(resolved));
}

// Relational-output pruning narrows the reference wrappers to the demanded column,
// so both references share one materialization whose spool (and source scan) drops
// the unused b, keeping only a.
TEST(CteReuse, MaterializeNarrowsSpoolAndSourceToDemand) {
    NQdb::TMockSource t({"a", "b"});
    std::map<std::string, ISource*> tables = {{"t", &t}};

    auto plan = BuildSqlPlan(
        "WITH x AS (SELECT a, b FROM t) SELECT p.a FROM x p JOIN x q ON p.a = q.a", tables);
    auto usage = Analyze(plan);
    auto resolved = ApplyCteReuse(std::move(plan), ChooseCteReuse(usage));

    auto consumers = FindConsumers(resolved);
    ASSERT_EQ(consumers.size(), 2u);
    EXPECT_EQ(
        consumers[0]->Materialization().get(), consumers[1]->Materialization().get());
    EXPECT_EQ(consumers[0]->OutputColumns(), consumers[1]->OutputColumns());

    EXPECT_EQ(FieldNames(consumers[0]), (std::vector<std::string>{"a"}));

    auto source = FindSource(consumers[0]->Materialization()->Plan);
    ASSERT_TRUE(source != nullptr);
    EXPECT_EQ(FieldNames(source), (std::vector<std::string>{"t.a"}));
}

// The definition's top is a UNION ALL, whose outputs relational pruning cannot
// narrow (branches stay aligned), so the boundary projection is what drops b.
TEST(CteReuse, BoundaryProjectionNarrowsUnionSpool) {
    NQdb::TMockSource t({"a", "b"});
    std::map<std::string, ISource*> tables = {{"t", &t}};

    auto plan = BuildSqlPlan(
        "WITH x AS (SELECT a, b FROM t UNION ALL SELECT a, b FROM t) "
        "SELECT p.a FROM x p JOIN x q ON p.a = q.a", tables);
    auto usage = Analyze(plan);
    auto resolved = ApplyCteReuse(std::move(plan), ChooseCteReuse(usage));

    auto consumers = FindConsumers(resolved);
    ASSERT_EQ(consumers.size(), 2u);
    EXPECT_EQ(FieldNames(consumers[0]), (std::vector<std::string>{"a"}));
}

// Partial reduction: the outer query needs the group key a and the count c but
// not the sum s, so only sum(b) is dropped from the aggregate.
TEST(CteReuse, AggregateOutputPruningDropsOnlyUnusedAggregate) {
    NQdb::TMockSource t({"a", "b"});
    std::map<std::string, ISource*> tables = {{"t", &t}};

    auto plan = BuildSqlPlan(
        "WITH x AS (SELECT a, count(*) AS c, sum(b) AS s FROM t GROUP BY a) "
        "SELECT p.a, p.c FROM x p JOIN x q ON p.a = q.a", tables);
    auto usage = Analyze(plan);
    auto resolved = ApplyCteReuse(std::move(plan), ChooseCteReuse(usage));

    auto consumers = FindConsumers(resolved);
    ASSERT_FALSE(consumers.empty());
    auto aggregate = FindAggregate(consumers[0]->Materialization()->Plan);
    ASSERT_TRUE(aggregate != nullptr);
    auto* agg = static_cast<TAggregateOperator*>(aggregate.get());
    ASSERT_EQ(agg->Aggs().size(), 1u);
    EXPECT_EQ(agg->Aggs()[0].Func, "count");
    EXPECT_EQ(agg->GroupKeys().size(), 1u);
}

// An aggregate consumed only by HAVING must survive pruning: the definition's
// Filter adds count(*) to the demand even though the outer query selects only a.
TEST(CteReuse, AggregateOutputPruningKeepsHavingAggregate) {
    NQdb::TMockSource t({"a", "b"});
    std::map<std::string, ISource*> tables = {{"t", &t}};

    auto plan = BuildSqlPlan(
        "WITH x AS (SELECT a, count(*) AS c, sum(b) AS s FROM t "
        "GROUP BY a HAVING count(*) > 1) "
        "SELECT p.a FROM x p JOIN x q ON p.a = q.a", tables);
    auto usage = Analyze(plan);
    auto resolved = ApplyCteReuse(std::move(plan), ChooseCteReuse(usage));

    auto consumers = FindConsumers(resolved);
    ASSERT_FALSE(consumers.empty());
    auto aggregate = FindAggregate(consumers[0]->Materialization()->Plan);
    ASSERT_TRUE(aggregate != nullptr);
    auto* agg = static_cast<TAggregateOperator*>(aggregate.get());
    ASSERT_EQ(agg->Aggs().size(), 1u);
    EXPECT_EQ(agg->Aggs()[0].Func, "count");
    EXPECT_EQ(agg->GroupKeys().size(), 1u);
}

// With real statistics the reuse decision is cost-based: a cheap definition is
// re-executed (inline), an expensive one is materialized once and streamed.
TEST(CteReuse, CostBasedReuseDecision) {
    NQdb::TMockSource t({"a"});
    std::map<std::string, ISource*> tables = {{"t", &t}};

    auto plan = BuildSqlPlan(
        "WITH x AS (SELECT a FROM t) SELECT p.a FROM x p JOIN x q ON p.a = q.a", tables);
    auto usage = Analyze(plan);
    auto defs = CollectCteDefinitions(plan);
    ASSERT_EQ(defs.size(), 1u);

    auto setCost = [&](double cost) {
        defs[0]->Plan->Stats_ = std::make_shared<TStats>();
        defs[0]->Plan->Stats_->RowCount = 1000; // one i64 column -> ~8000 output bytes
        defs[0]->Plan->Stats_->Cost = cost;
    };

    setCost(100.0);
    EXPECT_EQ(
        ChooseCteReuse(usage).at(defs[0].get()).Mode, ECteReuseMode::Inline);

    setCost(10'000'000.0);
    EXPECT_EQ(
        ChooseCteReuse(usage).at(defs[0].get()).Mode, ECteReuseMode::Materialize);
}

// The union keeps the definition's four-column output (branch pruning can't
// narrow it), but only a is demanded, so the spool is one column. The cost is
// tuned between the one-column and four-column materialize thresholds, so the
// decision is Materialize only if the spool is costed over RequiredOutputs.
TEST(CteReuse, CostBasedUsesNarrowedSpoolBytes) {
    NQdb::TMockSource t({"a", "b", "c", "d"});
    std::map<std::string, ISource*> tables = {{"t", &t}};

    auto plan = BuildSqlPlan(
        "WITH x AS (SELECT a, b, c, d FROM t UNION ALL SELECT a, b, c, d FROM t) "
        "SELECT p.a FROM x p JOIN x q ON p.a = q.a", tables);
    auto usage = Analyze(plan);
    auto defs = CollectCteDefinitions(plan);
    ASSERT_EQ(defs.size(), 1u);
    ASSERT_EQ(FieldNames(defs[0]->Plan).size(), 4u);

    defs[0]->Plan->Stats_ = std::make_shared<TStats>();
    defs[0]->Plan->Stats_->RowCount = 1000;
    // One-column threshold ~ 3*8*1000 = 24000; four-column ~ 3*32*1000 = 96000.
    defs[0]->Plan->Stats_->Cost = 50'000.0;

    EXPECT_EQ(
        ChooseCteReuse(usage).at(defs[0].get()).Mode, ECteReuseMode::Materialize);
}

// Every reference filters b, so their disjunction (deduped to b = 1) is pushed
// into the shared definition and lands on the source, shrinking the producer.
TEST(CteReuse, ConsumerPredicatePushedToDefinitionSource) {
    NQdb::TMockSource t({"a", "b"});
    std::map<std::string, ISource*> tables = {{"t", &t}};

    auto plan = BuildSqlPlan(
        "WITH x AS (SELECT a, b FROM t) "
        "SELECT p.a FROM x p JOIN x q ON p.a = q.a WHERE p.b = 1 AND q.b = 1", tables);
    ApplyPlanPasses(plan);

    auto consumers = FindConsumers(plan);
    ASSERT_EQ(consumers.size(), 2u);
    auto filter = FindFilter(consumers[0]->Materialization()->Plan);
    ASSERT_TRUE(filter != nullptr);
    EXPECT_TRUE(TMaybeOp<TSourceOperator>(filter->Input()));
}

TEST(CteReuse, MultiColumnConsumerPredicatesUseSharedSupersetBuilder) {
    NQdb::TMockSource t({"a", "b"});
    std::map<std::string, ISource*> tables = {{"t", &t}};

    auto plan = BuildSqlPlan(
        "WITH x AS (SELECT a, b FROM t) "
        "SELECT p.a FROM x p JOIN x q ON p.a = q.a "
        "WHERE p.a + p.b < 10 AND q.a + q.b < 20",
        tables);
    ApplyPlanPasses(plan);

    auto consumers = FindConsumers(plan);
    ASSERT_EQ(consumers.size(), 2u);
    auto filter = FindFilter(consumers[0]->Materialization()->Plan);
    ASSERT_TRUE(filter != nullptr);
    const auto predicate = NQumir::NAst::NCore::PrintAst(
        filter->Predicate());
    EXPECT_NE(predicate.find("||"), std::string::npos);
    EXPECT_NE(predicate.find("+"), std::string::npos);
}

// One reference is unconstrained (q has no predicate), so it needs every row;
// nothing is safely shareable and the definition source stays unfiltered.
TEST(CteReuse, AsymmetricPredicateNotPushed) {
    NQdb::TMockSource t({"a", "b"});
    std::map<std::string, ISource*> tables = {{"t", &t}};

    auto plan = BuildSqlPlan(
        "WITH x AS (SELECT a, b FROM t) "
        "SELECT p.a FROM x p JOIN x q ON p.a = q.a WHERE p.b = 1", tables);
    ApplyPlanPasses(plan);

    auto consumers = FindConsumers(plan);
    ASSERT_EQ(consumers.size(), 2u);
    EXPECT_TRUE(FindFilter(consumers[0]->Materialization()->Plan) == nullptr);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
