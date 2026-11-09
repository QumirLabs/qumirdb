#include <qdb/scheduler/connections/local.h>
#include <gtest/gtest.h>

#include "mock_source.h"

#include <qdb/exec/plan_builder.h>
#include <qdb/exec/runtime_filter.h>
#include <qdb/io/parquet/source.h>
#include <qdb/io/text/sink.h>
#include <qdb/kernel/finalize.h>
#include <qdb/plan/build.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/cte_consumer.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/pipeline.h>
#include <qdb/plan/plan_print.h>
#include <qdb/plan/passes/runtime_filters.h>
#include <qdb/scheduler/impl/executor.h>
#include <qdb/scheduler/lowering/plan_lowerer.h>
#include <qdb/scheduler/tasks/runtime_adapter.h>
#include <qdb/scheduler/lowering/scan_split.h>
#include <qdb/sql/parser.h>

#include <qumir/codegen/llvm/llvm_initializer.h>

#include <algorithm>
#include <expected>
#include <functional>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

using namespace NQdb;

namespace {

class TSplitMockSource final
    : public TMockSource
    , public NScheduler::IScanMetadataSource
{
public:
    using TMockSource::TMockSource;

    std::vector<NScheduler::TScanRowGroup> ScanRowGroups() const override {
        return {
            {.RowGroup = 0, .RowCount = 100, .ByteSize = 800},
            {.RowGroup = 1, .RowCount = 100, .ByteSize = 800},
            {.RowGroup = 2, .RowCount = 100, .ByteSize = 800},
            {.RowGroup = 3, .RowCount = 100, .ByteSize = 800},
        };
    }
};

class TStatsSplitMockSource final
    : public TMockSource
    , public NScheduler::IScanMetadataSource
{
public:
    TStatsSplitMockSource(uint64_t rows, uint64_t ndv)
        : TMockSource({"k"})
        , Stats_(std::make_shared<TStats>())
    {
        Stats_->RowCount = rows;
        auto column = std::make_shared<TStats::TColumnStats>();
        column->Ndv = ndv;
        Stats_->ColumnStats["k"] = std::move(column);
    }

    const TStatsPtr Stats() const override {
        return Stats_;
    }

    std::vector<NScheduler::TScanRowGroup> ScanRowGroups() const override {
        return {
            {.RowGroup = 0, .RowCount = 25'000'000, .ByteSize = 200'000'000},
            {.RowGroup = 1, .RowCount = 25'000'000, .ByteSize = 200'000'000},
            {.RowGroup = 2, .RowCount = 25'000'000, .ByteSize = 200'000'000},
            {.RowGroup = 3, .RowCount = 25'000'000, .ByteSize = 200'000'000},
        };
    }

private:
    TStatsPtr Stats_;
};

class TRecordingFilterBindingFactory final : public IRuntimeFilterBindingFactory {
public:
    TRuntimeFilterBinding Create(
        uint32_t id, size_t producerCount, size_t bloomBytes,
        std::shared_ptr<TRuntimeContext> runtimeContext) override {
        Created.emplace_back(id, producerCount);
        return Local_.Create(id, producerCount, bloomBytes, std::move(runtimeContext));
    }

    std::vector<std::pair<uint32_t, size_t>> Created;

private:
    TLocalRuntimeFilterBindingFactory Local_;
};

TJoinOperator* FindJoin(const TOperatorPtr& node) {
    if (auto join = TMaybeOp<TJoinOperator>(node)) {
        return join.Cast().get();
    }
    for (const auto& child : node->Children()) {
        if (auto op = NQumir::NAst::TMaybeNode<IOperator>(child)) {
            if (auto* join = FindJoin(op.Cast())) {
                return join;
            }
        }
    }
    return nullptr;
}

bool HasRuntimeFilterUse(const TOperatorPtr& node) {
    if (!node->RuntimeFilterUses.empty()) {
        return true;
    }
    for (const auto& input : node->Inputs()) {
        if (HasRuntimeFilterUse(input)) {
            return true;
        }
    }
    return false;
}

TOperatorPtr BuildSqlPlan(
    std::string_view sql,
    const std::unordered_map<std::string, ISource*>& sources)
{
    std::istringstream input{std::string(sql)};
    NSql::TTokenStream tokens(input);
    NSql::TParser parser;
    auto parsed = parser.Parse(tokens);
    if (!parsed) {
        throw std::runtime_error(parsed.error().ToString());
    }
    auto plan = BuildPlan(*parsed, [&](std::string_view table)
        -> std::expected<TOperatorPtr, NQumir::TError>
    {
        auto it = sources.find(std::string(table));
        if (it == sources.end()) {
            return std::unexpected(
                NQumir::TError("unknown table: " + std::string(table)));
        }
        return std::make_shared<TSourceOperator>(
            *it->second, std::string(table));
    });
    if (!plan) {
        throw std::runtime_error(plan.error().ToString());
    }
    auto root = *plan;
    ApplyPlanPasses(root, {.EnableCbo = false});
    return root;
}

NScheduler::TSettings SingleSettings() {
    NScheduler::TSettings settings;
    settings.Scheduler.Mode =
        NScheduler::EExecutionMode::SingleThreadedScheduler;
    settings.Scheduler.WorkerCount = 1;
    settings.ScanSplit.Strategy = NScheduler::EScanSplitStrategy::SerialRead;
    settings.ScanSplit.MaxScanTasks = 1;
    settings.HashShuffle.PartitionCount = 1;
    settings.HashShuffle.MaxPartitionCount = 1;
    return settings;
}

NScheduler::TSettings MultiSettings() {
    NScheduler::TSettings settings;
    settings.Scheduler.Mode =
        NScheduler::EExecutionMode::ThreadedScheduler;
    settings.Scheduler.WorkerCount = 4;
    settings.ScanSplit.Strategy =
        NScheduler::EScanSplitStrategy::RowGroupRange;
    settings.ScanSplit.MaxScanTasks = 4;
    settings.HashShuffle.PartitionCount = 2;
    settings.HashShuffle.MaxPartitionCount = 2;
    return settings;
}

struct TCollectSink : ISink {
    void Write(const TRowSet&) override
    {}
};

struct TBuiltPlan {
    TOperatorPtr Logical;
    TExecPlan Exec;
    std::vector<TGeneratedKernel> Kernels;
    size_t PhysicalTasks = 0;
    std::unordered_map<std::string, size_t> SourceLanes;
    struct TRepartition {
        std::vector<std::string> Keys;
        size_t Sources = 0;
        size_t Destinations = 0;
    };
    std::vector<TRepartition> Repartitions;
};

TBuiltPlan Lower(
    std::string_view sql,
    const std::unordered_map<std::string, ISource*>& sources,
    NScheduler::TSettings settings)
{
    auto logical = BuildSqlPlan(sql, sources);
    auto lowered = NScheduler::LowerPlanToGraph(logical, settings, nullptr);
    auto exec = BuildExecPlan(lowered);
    if (!exec) {
        throw std::runtime_error(exec.error());
    }
    std::vector<TBuiltPlan::TRepartition> repartitions;
    std::unordered_map<std::string, size_t> sourceLanes;
    for (const auto& node : lowered.Graph->Nodes()) {
        if (node->DebugKind == "source") {
            ++sourceLanes[node->DebugLabel];
        }
    }
    std::unordered_set<const NScheduler::IConnection*> seenConnections;
    for (const auto& edge : lowered.Graph->Edges()) {
        const auto* connection = edge->Connection;
        if (!connection ||
            connection->Kind() != NScheduler::EConnectionKind::HashShuffle ||
            !seenConnections.insert(connection).second)
        {
            continue;
        }
        const auto* exchange = static_cast<
            const NScheduler::THashShuffleConnection*>(connection);
        repartitions.push_back({
            .Keys = exchange->Repartition().Keys,
            .Sources = exchange->SrcCount(),
            .Destinations = exchange->DstCount(),
        });
    }
    return {
        .Logical = std::move(logical),
        .Exec = std::move(*exec),
        .Kernels = std::move(lowered.Kernels),
        .PhysicalTasks = lowered.Graph->Nodes().size(),
        .SourceLanes = std::move(sourceLanes),
        .Repartitions = std::move(repartitions),
    };
}

void ExpectSameSemanticPlan(const TExecPlan& left, const TExecPlan& right) {
    ASSERT_EQ(left.Root, right.Root);
    ASSERT_EQ(left.Nodes.size(), right.Nodes.size());
    ASSERT_EQ(left.RootLimit.has_value(), right.RootLimit.has_value());
    for (size_t i = 0; i < left.Nodes.size(); ++i) {
        const auto& l = left.Nodes[i];
        const auto& r = right.Nodes[i];
        EXPECT_EQ(l.Id, r.Id) << i;
        EXPECT_EQ(l.StageId, r.StageId) << i;
        EXPECT_EQ(l.Kind, r.Kind) << i;
        EXPECT_EQ(l.AggregatePhase, r.AggregatePhase) << i;
        ASSERT_EQ(l.RuntimeFilterUse.has_value(),
            r.RuntimeFilterUse.has_value()) << i;
        if (l.RuntimeFilterUse) {
            EXPECT_EQ(l.RuntimeFilterUse->Id, r.RuntimeFilterUse->Id) << i;
            EXPECT_EQ(l.RuntimeFilterUse->ProbeKeys,
                r.RuntimeFilterUse->ProbeKeys) << i;
        }
        EXPECT_EQ(l.Inputs, r.Inputs) << i;
        ASSERT_TRUE(l.OutputType) << i;
        ASSERT_TRUE(r.OutputType) << i;
        EXPECT_EQ(l.OutputType->ToString(), r.OutputType->ToString()) << i;
    }
}

void ExpectKernelBindings(const TBuiltPlan& built) {
    std::vector<bool> seen(built.Kernels.size(), false);
    for (const auto& node : built.Exec.Nodes) {
        for (const auto kernelIndex : node.KernelIndexes) {
            ASSERT_LT(kernelIndex, built.Kernels.size());
            EXPECT_EQ(built.Kernels[kernelIndex].ExecStageId, node.StageId);
            seen[kernelIndex] = true;
        }
    }
    EXPECT_TRUE(std::ranges::all_of(seen, [](bool value) { return value; }));
}

size_t CountKind(const TExecPlan& plan, EExecPlanNodeKind kind) {
    return std::ranges::count_if(plan.Nodes, [&](const auto& node) {
        return node.Kind == kind;
    });
}

TEST(ExecPlanBuilder, SingleAndMultiCollapseJoinToSameSemanticDag) {
    TSplitMockSource left({"k", "lv"});
    TSplitMockSource right({"k", "rv"});
    const std::unordered_map<std::string, ISource*> sources{
        {"l", &left},
        {"r", &right},
    };
    constexpr std::string_view sql =
        "SELECT l.k FROM l JOIN r ON l.k = r.k";

    auto single = Lower(sql, sources, SingleSettings());
    auto multi = Lower(sql, sources, MultiSettings());

    EXPECT_GT(multi.PhysicalTasks, single.PhysicalTasks);
    ExpectSameSemanticPlan(single.Exec, multi.Exec);
    ExpectKernelBindings(single);
    ExpectKernelBindings(multi);
    EXPECT_EQ(CountKind(single.Exec, EExecPlanNodeKind::Join), 1u);
    EXPECT_EQ(CountKind(multi.Exec, EExecPlanNodeKind::Join), 1u);
}

TEST(ExecPlanBuilder, CostedRuntimeFilterIsPlacedBeforeLowering) {
    TStatsSplitMockSource large(1'000'000, 1'000'000);
    TStatsSplitMockSource small(100'000, 10'000);
    const std::unordered_map<std::string, ISource*> sources{
        {"l", &large}, {"r", &small},
    };
    auto plan = BuildSqlPlan(
        "SELECT l.k FROM l JOIN r ON l.k = r.k", sources);
    auto* join = FindJoin(plan);
    ASSERT_NE(join, nullptr);
    ASSERT_TRUE(join->RuntimeFilter());
    EXPECT_EQ(join->RuntimeFilter()->BuildSide, EJoinFilterSide::Right);
    auto probe = TMaybeOp<TSourceOperator>(join->Left());
    ASSERT_TRUE(probe);
    ASSERT_EQ(probe.Cast()->RuntimeFilterUses.size(), 1u);
    EXPECT_EQ(probe.Cast()->RuntimeFilterUses[0].Id,
        join->RuntimeFilter()->Id);
}

TEST(ExecPlanBuilder, LowererUsesInjectedRuntimeFilterBindings) {
    TSplitMockSource left({"k"});
    TSplitMockSource right({"k"});
    const std::unordered_map<std::string, ISource*> sources{
        {"l", &left},
        {"r", &right},
    };
    auto plan = BuildSqlPlan(
        "SELECT l.k FROM l JOIN r ON l.k = r.k", sources);
    auto* join = FindJoin(plan);
    ASSERT_NE(join, nullptr);
    join->MutableRuntimeFilter() = TRuntimeFilterSpec{
        .Id = 17, .BuildSide = EJoinFilterSide::Right};

    PlaceRuntimeFilterConsumers(plan);
    std::stringstream printed;
    PrintPlanTree(printed, plan);
    EXPECT_NE(printed.str().find("source l AS l use-filter 17 keys=[l.k]"),
        std::string::npos);

    auto bindings = std::make_shared<TRecordingFilterBindingFactory>();
    auto lowered = NScheduler::LowerPlanToGraph(
        plan, MultiSettings(), nullptr, nullptr, bindings);
    ASSERT_TRUE(lowered.Graph);
    EXPECT_EQ(bindings->Created,
        (std::vector<std::pair<uint32_t, size_t>>{{17, 4}}));
    size_t filterTasks = 0;
    for (const auto& node : lowered.Graph->Nodes()) {
        if (node->DebugKind == "runtime-filter") {
            ++filterTasks;
            EXPECT_NE(dynamic_cast<NScheduler::TRuntimeFilterTask*>(node->Task),
                nullptr);
        }
    }
    EXPECT_EQ(filterTasks, 4u);
    size_t dependencies = 0;
    for (const auto& edge : lowered.Graph->Edges()) {
        if (edge->Kind == NScheduler::ETaskEdgeKind::Dependency) {
            ++dependencies;
            EXPECT_EQ(edge->Dst->DebugKind, "runtime-filter");
        }
    }
    EXPECT_EQ(dependencies, 16u);
    // Runtime filters have no records yet, so the build shuffles that publish
    // one have none either; the join tasks do.
    const auto& nodes = lowered.Graph->Nodes();
    size_t buildShuffles = 0;
    for (size_t i = 0; i < nodes.size(); ++i) {
        const bool recorded = i < lowered.NodeSpecs.size() && lowered.NodeSpecs[i];
        if (nodes[i]->DebugLabel == "join-right-repartition") {
            ++buildShuffles;
            EXPECT_FALSE(recorded);
        } else if (nodes[i]->DebugKind == "join") {
            EXPECT_TRUE(recorded);
        }
    }
    EXPECT_EQ(buildShuffles, 4u);
    auto exec = BuildExecPlan(lowered);
    ASSERT_TRUE(exec.has_value()) << exec.error();
    ASSERT_EQ(CountKind(*exec, EExecPlanNodeKind::RuntimeFilter), 1u);
    const auto filterNode = std::ranges::find_if(exec->Nodes,
        [](const auto& node) {
            return node.Kind == EExecPlanNodeKind::RuntimeFilter;
        });
    ASSERT_TRUE(filterNode->RuntimeFilterUse);
    EXPECT_EQ(filterNode->RuntimeFilterUse->Id, 17u);
    EXPECT_EQ(filterNode->RuntimeFilterUse->ProbeKeys,
        (std::vector<std::string>{"l.k"}));
    ASSERT_EQ(filterNode->Inputs.size(), 1u);
    EXPECT_EQ(exec->Nodes[filterNode->Inputs[0]].Kind,
        EExecPlanNodeKind::Source);
    EXPECT_EQ(filterNode->OutputType->ToString(),
        exec->Nodes[filterNode->Inputs[0]].OutputType->ToString());
}

TEST(ExecPlanBuilder, RuntimeFilterStaysAfterOneSharedCteReader) {
    TSplitMockSource source({"k", "v"});
    const std::unordered_map<std::string, ISource*> sources{{"t", &source}};
    constexpr std::string_view sql =
        "WITH x AS (SELECT k, v FROM t) "
        "SELECT p.k FROM x p JOIN x q ON p.k = q.k";
    auto plan = BuildSqlPlan(sql, sources);
    auto* join = FindJoin(plan);
    ASSERT_NE(join, nullptr);
    join->MutableRuntimeFilter() = TRuntimeFilterSpec{
        .Id = 23, .BuildSide = EJoinFilterSide::Right};
    PlaceRuntimeFilterConsumers(plan);
    ASSERT_EQ(join->Left()->Inputs().size(), 1u);
    ASSERT_EQ(join->Right()->Inputs().size(), 1u);
    auto left = TMaybeOp<TCteConsumer>(join->Left()->Inputs()[0]);
    auto right = TMaybeOp<TCteConsumer>(join->Right()->Inputs()[0]);
    ASSERT_TRUE(left);
    ASSERT_TRUE(right);
    ASSERT_EQ(left.Cast()->Materialization(), right.Cast()->Materialization());
    ASSERT_EQ(left.Cast()->RuntimeFilterUses.size(), 1u);
    EXPECT_TRUE(right.Cast()->RuntimeFilterUses.empty());
    EXPECT_FALSE(HasRuntimeFilterUse(left.Cast()->Materialization()->Plan));

    auto lowered = NScheduler::LowerPlanToGraph(
        plan, MultiSettings(), nullptr);
    size_t dependencyCount = 0;
    for (const auto& edge : lowered.Graph->Edges()) {
        if (edge->Kind == NScheduler::ETaskEdgeKind::Dependency) {
            ++dependencyCount;
            EXPECT_EQ(edge->Dst->DebugKind, "runtime-filter");
        }
    }
    EXPECT_GT(dependencyCount, 0u);

    auto exec = BuildExecPlan(lowered);
    ASSERT_TRUE(exec.has_value()) << exec.error();
    EXPECT_EQ(CountKind(*exec, EExecPlanNodeKind::CteProducer), 1u);
    EXPECT_EQ(CountKind(*exec, EExecPlanNodeKind::CteConsumer), 2u);
    ASSERT_EQ(CountKind(*exec, EExecPlanNodeKind::RuntimeFilter), 1u);
    const auto filterNode = std::ranges::find_if(exec->Nodes,
        [](const auto& node) {
            return node.Kind == EExecPlanNodeKind::RuntimeFilter;
        });
    ASSERT_TRUE(filterNode->RuntimeFilterUse);
    EXPECT_EQ(filterNode->RuntimeFilterUse->Id, 23u);
    EXPECT_EQ(filterNode->RuntimeFilterUse->ProbeKeys,
        (std::vector<std::string>{"k"}));
    ASSERT_EQ(filterNode->Inputs.size(), 1u);
    EXPECT_EQ(exec->Nodes[filterNode->Inputs[0]].Kind,
        EExecPlanNodeKind::CteConsumer);
}

TEST(ExecPlanBuilder, RuntimeFiltersShareTheQueryJit) {
    TSplitMockSource left({"k"}), right({"k"}), third({"k"});
    const std::unordered_map<std::string, ISource*> sources{
        {"l", &left}, {"r", &right}, {"t", &third},
    };
    auto plan = BuildSqlPlan(
        "SELECT l.k FROM l JOIN r ON l.k = r.k JOIN t ON l.k = t.k", sources);
    auto* outer = FindJoin(plan);
    ASSERT_NE(outer, nullptr);
    auto* inner = FindJoin(outer->Left());
    ASSERT_NE(inner, nullptr);
    inner->MutableRuntimeFilter() = TRuntimeFilterSpec{
        .Id = 1, .BuildSide = EJoinFilterSide::Right};
    outer->MutableRuntimeFilter() = TRuntimeFilterSpec{
        .Id = 2, .BuildSide = EJoinFilterSide::Right};

    auto first = NScheduler::LowerPlanToGraph(plan, MultiSettings(), nullptr);
    auto second = NScheduler::LowerPlanToGraph(plan, MultiSettings(), nullptr);
    ASSERT_EQ(first.RuntimeFilters.size(), 2u);
    EXPECT_EQ(std::ranges::count(first.Kernels, "runtime-filter",
        &TGeneratedKernel::Name), 1);
    auto filter = std::ranges::find(first.Kernels, "runtime-filter",
        &TGeneratedKernel::Name);
    auto secondFilter = std::ranges::find(second.Kernels, "runtime-filter",
        &TGeneratedKernel::Name);
    ASSERT_NE(filter, first.Kernels.end());
    ASSERT_NE(secondFilter, second.Kernels.end());
    ASSERT_TRUE(filter->RuntimeContext);
    EXPECT_NE(filter->RuntimeContext, secondFilter->RuntimeContext);
    EXPECT_FALSE(filter->Slot->Runner);

    FinalizeKernels(first.Kernels, EKernelBackend::Llvm);
    ASSERT_TRUE(filter->Slot->Runner);
    for (const auto& kernel : first.Kernels) {
        EXPECT_EQ(kernel.Slot->Runner, filter->Slot->Runner);
    }
}

TEST(ExecPlanBuilder, RejectsDuplicateRuntimeFilterProducerIds) {
    TSplitMockSource left({"k"}), right({"k"}), third({"k"});
    const std::unordered_map<std::string, ISource*> sources{
        {"l", &left}, {"r", &right}, {"t", &third},
    };
    auto plan = BuildSqlPlan(
        "SELECT l.k FROM l JOIN r ON l.k = r.k JOIN t ON l.k = t.k", sources);
    auto* outer = FindJoin(plan);
    ASSERT_NE(outer, nullptr);
    auto* inner = FindJoin(outer->Left());
    ASSERT_NE(inner, nullptr);
    inner->MutableRuntimeFilter() = TRuntimeFilterSpec{
        .Id = 7, .BuildSide = EJoinFilterSide::Right};
    outer->MutableRuntimeFilter() = TRuntimeFilterSpec{
        .Id = 7, .BuildSide = EJoinFilterSide::Right};
    EXPECT_THROW(NScheduler::LowerPlanToGraph(plan, SingleSettings(), nullptr),
        std::runtime_error);
}

TEST(ExecPlanBuilder, SingleAndMultiCollapseUnionLanes) {
    TSplitMockSource left({"k"});
    TSplitMockSource right({"k"});
    const std::unordered_map<std::string, ISource*> sources{
        {"l", &left},
        {"r", &right},
    };
    constexpr std::string_view sql =
        "SELECT k FROM l UNION ALL SELECT k FROM r";

    auto single = Lower(sql, sources, SingleSettings());
    auto multi = Lower(sql, sources, MultiSettings());

    EXPECT_GT(multi.PhysicalTasks, single.PhysicalTasks);
    ExpectSameSemanticPlan(single.Exec, multi.Exec);
    ExpectKernelBindings(single);
    ExpectKernelBindings(multi);
    ASSERT_EQ(CountKind(single.Exec, EExecPlanNodeKind::UnionAll), 1u);
    const auto& root = single.Exec.Nodes[single.Exec.Root];
    EXPECT_EQ(root.Kind, EExecPlanNodeKind::UnionAll);
    EXPECT_EQ(root.StageId, InvalidExecStageId);
    EXPECT_EQ(root.Inputs.size(), 2u);
}

TEST(ExecPlanBuilder, CrossJoinResidualPreservesBothInputSchemas) {
    TSplitMockSource left({"lv"});
    TSplitMockSource right({"rv"});
    const std::unordered_map<std::string, ISource*> sources{
        {"l", &left}, {"r", &right},
    };
    constexpr std::string_view sql =
        "SELECT r.rv, count(*) AS n, sum(l.lv) AS total "
        "FROM l, r WHERE r.rv > l.lv GROUP BY r.rv";

    auto single = Lower(sql, sources, SingleSettings());
    auto multi = Lower(sql, sources, MultiSettings());

    EXPECT_GT(multi.PhysicalTasks, single.PhysicalTasks);
    ExpectKernelBindings(single);
    ExpectKernelBindings(multi);
    for (const auto* built : {&single, &multi}) {
        EXPECT_EQ(CountKind(built->Exec, EExecPlanNodeKind::CrossJoin), 1u);
        EXPECT_EQ(CountKind(built->Exec, EExecPlanNodeKind::CrossResidualFilter), 1u);
        const auto residual = std::ranges::find_if(
            built->Exec.Nodes, [](const auto& node) {
                return node.Kind == EExecPlanNodeKind::CrossResidualFilter;
            });
        ASSERT_NE(residual, built->Exec.Nodes.end());
        auto output = NQumir::NAst::TMaybeType<NQumir::NAst::TStructType>(
            residual->OutputType);
        ASSERT_TRUE(output);
        EXPECT_TRUE(FieldType(output.Cast().get(), "l.lv"));
        EXPECT_TRUE(FieldType(output.Cast().get(), "r.rv"));
    }
}

TEST(ExecPlanBuilder, LoweringRecordsSpecsForSupportedNodeKinds) {
    TSplitMockSource left({"lv"});
    TSplitMockSource right({"rv"});
    const std::unordered_map<std::string, ISource*> sources{
        {"l", &left}, {"r", &right},
    };
    auto plan = BuildSqlPlan(
        "SELECT r.rv, count(*) AS n FROM l, r WHERE r.rv > l.lv GROUP BY r.rv", sources);
    auto lowered = NScheduler::LowerPlanToGraph(plan, MultiSettings(), nullptr);
    TCollectSink sink;
    NScheduler::AttachSink(lowered, sink);

    const auto& nodes = lowered.Graph->Nodes();
    ASSERT_EQ(lowered.NodeSpecs.size(), nodes.size());
    size_t forwards = 0;
    std::vector<uint32_t> expectedMissing;
    for (size_t i = 0; i < nodes.size(); ++i) {
        const auto& record = lowered.NodeSpecs[i];
        const auto& kind = nodes[i]->DebugKind;
        const bool isForward = kind == "broadcast";
        // Mock sources are not parquet scans, so they keep no record.
        const bool recorded = isForward || kind == "sink" || kind == "filter" || kind == "project"
            || kind == "aggregate" || kind == "hash-shuffle" || kind == "join";
        ASSERT_EQ(record.has_value(), recorded) << kind;
        if (!record) {
            expectedMissing.push_back(static_cast<uint32_t>(i));
            continue;
        }
        EXPECT_EQ(record->DebugKind, nodes[i]->DebugKind);
        EXPECT_EQ(record->TaskGroupId, nodes[i]->TaskGroupId);
        EXPECT_EQ(
            isForward,
            std::holds_alternative<NScheduler::TForwardNode>(record->Body));
        forwards += isForward;
    }
    EXPECT_GT(forwards, 0u);

    std::vector<uint32_t> missing;
    EXPECT_FALSE(NScheduler::ExtractLoweredGraphSpec(lowered, &missing));
    EXPECT_EQ(missing, expectedMissing);
}

// Runs a graph into a CSV sink and returns the rows, sorted unless `ordered`.
std::vector<std::string> RunCsv(
    NScheduler::TTaskGraph& graph, const std::ostringstream& out, const NScheduler::TSettings& settings,
    bool ordered)
{
    std::string error;
    auto sealed = graph.Seal(&error);
    if (!sealed) {
        throw std::runtime_error(error);
    }
    NScheduler::TSchedulerExecutor executor(sealed, settings);
    if (!executor.Run(&error)) {
        throw std::runtime_error(error);
    }
    std::vector<std::string> rows;
    std::istringstream lines(out.str());
    for (std::string line; std::getline(lines, line);) {
        rows.push_back(std::move(line));
    }
    if (!ordered) {
        std::ranges::sort(rows);
    }
    return rows;
}

// Lowers `sql` over decimals.parquet, serializes the graph spec, runs the
// original graph, then destroys the plan, the source and the lowered graph.
// Only the bytes and the kernel slots survive, and the rebuilt graph must give
// the same rows on both schedulers (in the same order when `ordered`).
std::vector<std::string> ExpectQueryFromSpec(
    const std::string& sql, const NScheduler::TSettings& settings, bool ordered,
    const std::function<void(const NScheduler::TGraphSpec&)>& checkSpec)
{
    auto source = std::make_unique<TParquetSource>(
        QDB_TEST_DATA_DIR "/regtest/cases/decimal_parquet/tables/decimals.parquet");
    const std::unordered_map<std::string, ISource*> sources{{"t", source.get()}};
    auto plan = BuildSqlPlan(sql, sources);
    auto lowered = std::make_unique<NScheduler::TLoweredPlan>(
        NScheduler::LowerPlanToGraph(plan, settings, nullptr));

    // The schema's column names point into the output type.
    const auto outputType = lowered->OutputType;
    const auto& fields = static_cast<NQumir::NAst::TStructType&>(*outputType).Fields;
    std::vector<TColumnSchema> columns;
    for (const auto& [name, type] : fields) {
        columns.push_back({.Name = name, .Type = type});
    }
    const TSchema schema{columns};
    std::ostringstream originalOut;
    TCsvSink original(schema, originalOut);
    NScheduler::AttachSink(*lowered, original);

    std::vector<uint32_t> missing;
    const auto spec = NScheduler::ExtractLoweredGraphSpec(*lowered, &missing);
    EXPECT_TRUE(spec) << missing.size() << " nodes have no record";
    if (!spec) {
        return {};
    }
    checkSpec(*spec);
    const auto bytes = NScheduler::SerializeGraphSpec(*spec);
    const auto decoded = NScheduler::DeserializeGraphSpec(bytes);
    EXPECT_EQ(NScheduler::SerializeGraphSpec(decoded), bytes);

    FinalizeKernels(lowered->Kernels, settings.Kernel.Backend);
    std::vector<std::shared_ptr<TKernelSlot>> slots;
    for (const auto& kernel : lowered->Kernels) {
        slots.push_back(kernel.Slot);
    }
    const auto expected = RunCsv(*lowered->Graph, originalOut, settings, ordered);

    lowered.reset();
    plan.reset();
    source.reset();

    for (const auto& runSettings : {MultiSettings(), SingleSettings()}) {
        std::ostringstream rebuiltOut;
        TCsvSink rebuilt(schema, rebuiltOut);
        auto graph = NScheduler::MaterializeGraph(decoded, &rebuilt, slots);
        const auto got = RunCsv(*graph, rebuiltOut, runSettings, ordered);
        EXPECT_EQ(got, expected);
    }
    return expected;
}

template <class TNode>
size_t CountNodes(const NScheduler::TGraphSpec& spec)
{
    return std::ranges::count_if(spec.Nodes, [](const auto& node) {
        return std::holds_alternative<TNode>(node.Body);
    });
}

std::vector<std::string> ExpectScanFilterProjectFromSpec(std::string_view where)
{
    return ExpectQueryFromSpec(
        "SELECT id, d15 + d15 AS twice, REGEXP_REPLACE(s, '-[a-z]+$', '') AS item "
        "FROM t WHERE " + std::string(where),
        MultiSettings(), false, [](const NScheduler::TGraphSpec& spec) {
            EXPECT_GT(CountNodes<NScheduler::TParquetScanNode>(spec), 0u);
            EXPECT_EQ(spec.Regexes.size(), 1u);
        });
}

TEST(ExecPlanBuilder, ScanFilterProjectGraphRunsFromItsSerializedSpec) {
    const auto rows = ExpectScanFilterProjectFromSpec("d9 > 100 AND id % 3 <> 0");
    EXPECT_GT(rows.size(), 50u);
}

TEST(ExecPlanBuilder, ProvenEmptyScanGraphRunsFromItsSerializedSpec) {
    // Every row group is pruned, so the scans carry an empty row group list.
    const auto rows = ExpectScanFilterProjectFromSpec("id > 1000");
    EXPECT_LE(rows.size(), 1u);
}

TEST(ExecPlanBuilder, SortGraphRunsFromItsSerializedSpec) {
    const auto rows = ExpectQueryFromSpec(
        "SELECT id, d9 FROM t ORDER BY id DESC", SingleSettings(), true,
        [](const NScheduler::TGraphSpec& spec) {
            EXPECT_EQ(CountNodes<NScheduler::TSortNode>(spec), 1u);
            EXPECT_EQ(CountNodes<NScheduler::TSortMergeNode>(spec), 0u);
        });
    EXPECT_GT(rows.size(), 100u);
}

TEST(ExecPlanBuilder, TopSortGraphRunsFromItsSerializedSpec) {
    const auto rows = ExpectQueryFromSpec(
        "SELECT id, d9 FROM t ORDER BY id LIMIT 7", SingleSettings(), true,
        [](const NScheduler::TGraphSpec& spec) {
            ASSERT_EQ(CountNodes<NScheduler::TSortNode>(spec), 1u);
            for (const auto& node : spec.Nodes) {
                if (const auto* sort = std::get_if<NScheduler::TSortNode>(&node.Body)) {
                    ASSERT_TRUE(sort->Plan.Top);
                    EXPECT_EQ(sort->Plan.Top->Limit, 7);
                }
            }
        });
    EXPECT_EQ(rows.size(), 8u);
}

TEST(ExecPlanBuilder, ParallelSortMergeGraphRunsFromItsSerializedSpec) {
    const auto rows = ExpectQueryFromSpec(
        "SELECT id, d9 FROM t ORDER BY d9, id", MultiSettings(), true,
        [](const NScheduler::TGraphSpec& spec) {
            EXPECT_GT(CountNodes<NScheduler::TSortNode>(spec), 1u);
            ASSERT_EQ(CountNodes<NScheduler::TSortMergeNode>(spec), 1u);
            for (const auto& node : spec.Nodes) {
                if (const auto* merge = std::get_if<NScheduler::TSortMergeNode>(&node.Body)) {
                    EXPECT_EQ(merge->In.size(), CountNodes<NScheduler::TSortNode>(spec));
                }
            }
        });
    EXPECT_GT(rows.size(), 100u);
}

TEST(ExecPlanBuilder, WindowGraphRunsFromItsSerializedSpec) {
    const auto rows = ExpectQueryFromSpec(
        "SELECT id, rank() OVER (ORDER BY id DESC) AS rn FROM t", MultiSettings(), false,
        [](const NScheduler::TGraphSpec& spec) {
            EXPECT_EQ(CountNodes<NScheduler::TWindowNode>(spec), 1u);
        });
    EXPECT_GT(rows.size(), 100u);
}

TEST(ExecPlanBuilder, ParallelTopSortLimitGraphRunsFromItsSerializedSpec) {
    // Local top-sorts, a sort merge, then the final limit.
    const auto rows = ExpectQueryFromSpec(
        "SELECT id, d9 FROM t ORDER BY d9, id LIMIT 7", MultiSettings(), true,
        [](const NScheduler::TGraphSpec& spec) {
            EXPECT_GT(CountNodes<NScheduler::TSortNode>(spec), 1u);
            EXPECT_EQ(CountNodes<NScheduler::TSortMergeNode>(spec), 1u);
            ASSERT_EQ(CountNodes<NScheduler::TLimitNode>(spec), 1u);
            for (const auto& node : spec.Nodes) {
                if (const auto* limit = std::get_if<NScheduler::TLimitNode>(&node.Body)) {
                    EXPECT_EQ(limit->Limit, 7);
                    EXPECT_EQ(limit->Offset, 0);
                }
            }
        });
    EXPECT_EQ(rows.size(), 8u);
}

TEST(ExecPlanBuilder, LimitOffsetGraphRunsFromItsSerializedSpec) {
    // One scan lane, so the rows the limit keeps are the same on every run.
    const auto rows = ExpectQueryFromSpec(
        "SELECT id FROM t LIMIT 5 OFFSET 3", SingleSettings(), true,
        [](const NScheduler::TGraphSpec& spec) {
            ASSERT_EQ(CountNodes<NScheduler::TLimitNode>(spec), 1u);
            for (const auto& node : spec.Nodes) {
                if (const auto* limit = std::get_if<NScheduler::TLimitNode>(&node.Body)) {
                    EXPECT_EQ(limit->Limit, 5);
                    EXPECT_EQ(limit->Offset, 3);
                }
            }
        });
    EXPECT_EQ(rows.size(), 6u);
}

TEST(ExecPlanBuilder, LimitZeroGraphRunsFromItsSerializedSpec) {
    const auto rows = ExpectQueryFromSpec(
        "SELECT id FROM t LIMIT 0", SingleSettings(), true,
        [](const NScheduler::TGraphSpec& spec) {
            EXPECT_EQ(CountNodes<NScheduler::TLimitNode>(spec), 1u);
        });
    EXPECT_LE(rows.size(), 1u);
}

TEST(ExecPlanBuilder, OffsetPastEndGraphRunsFromItsSerializedSpec) {
    const auto rows = ExpectQueryFromSpec(
        "SELECT id FROM t LIMIT 5 OFFSET 1000000", SingleSettings(), true,
        [](const NScheduler::TGraphSpec& spec) {
            EXPECT_EQ(CountNodes<NScheduler::TLimitNode>(spec), 1u);
        });
    EXPECT_LE(rows.size(), 1u);
}

TEST(ExecPlanBuilder, PartitionedWindowGraphRunsFromItsSerializedSpec) {
    const auto rows = ExpectQueryFromSpec(
        "SELECT id, rank() OVER (PARTITION BY id % 4 ORDER BY id DESC) AS rn FROM t",
        MultiSettings(), false, [](const NScheduler::TGraphSpec& spec) {
            EXPECT_GT(CountNodes<NScheduler::THashShuffleNode>(spec), 0u);
            EXPECT_GT(CountNodes<NScheduler::TWindowNode>(spec), 1u);
        });
    EXPECT_GT(rows.size(), 100u);
}

TEST(ExecPlanBuilder, LateMaterializeGraphRunsFromItsSerializedSpec) {
    // The wide top-3 fetches its columns by locator after the sort.
    const auto rows = ExpectQueryFromSpec(
        "SELECT id, d9, d15, d18, d38, s FROM t ORDER BY d9, id LIMIT 3", MultiSettings(), true,
        [](const NScheduler::TGraphSpec& spec) {
            ASSERT_EQ(CountNodes<NScheduler::TLateMergeNode>(spec), 1u);
            EXPECT_GT(CountNodes<NScheduler::TLateLookupNode>(spec), 1u);
            for (const auto& node : spec.Nodes) {
                if (const auto* merge = std::get_if<NScheduler::TLateMergeNode>(&node.Body)) {
                    EXPECT_EQ(merge->In.size(), CountNodes<NScheduler::TLateLookupNode>(spec));
                }
            }
        });
    EXPECT_EQ(rows.size(), 4u);
}

TEST(ExecPlanBuilder, GroupedAggregateGraphRunsFromItsSerializedSpec) {
    auto settings = MultiSettings();
    // The partial aggregate cascade has no record yet.
    settings.Aggregate.EnablePartial = false;
    const auto rows = ExpectQueryFromSpec(
        "SELECT id % 7 AS k, COUNT(*) AS n, SUM(d15) AS total, MIN(s) AS lo "
        "FROM t GROUP BY id % 7", settings, false,
        [](const NScheduler::TGraphSpec& spec) {
            // Four scan lanes shuffle into two partition aggregates.
            EXPECT_EQ(CountNodes<NScheduler::THashShuffleNode>(spec), 4u);
            EXPECT_EQ(CountNodes<NScheduler::TAggregateNode>(spec), 2u);
        });
    // The header and one row per group.
    EXPECT_EQ(rows.size(), 1u + 7u);
}

TEST(ExecPlanBuilder, UngroupedAggregateGraphRunsFromItsSerializedSpec) {
    for (const auto cascade : {NScheduler::EGlobalCascadeMode::Off, NScheduler::EGlobalCascadeMode::On}) {
        auto settings = MultiSettings();
        settings.Aggregate.CascadeGlobal = cascade;
        const bool cascaded = cascade == NScheduler::EGlobalCascadeMode::On;
        const auto rows = ExpectQueryFromSpec(
            "SELECT COUNT(*) AS n, SUM(d15) AS total, MIN(s) AS lo FROM t WHERE id % 3 <> 0",
            settings, false, [cascaded](const NScheduler::TGraphSpec& spec) {
                // The cascade adds one partial aggregate per scan lane.
                EXPECT_EQ(CountNodes<NScheduler::TAggregateNode>(spec), cascaded ? 5u : 1u);
            });
        EXPECT_EQ(rows.size(), 1u + 1u);
    }
}

// The join hashes of the shuffles in `spec`: entry 0 (left) or 1 (right).
std::vector<uint32_t> JoinHashEntries(const NScheduler::TGraphSpec& spec)
{
    std::vector<uint32_t> entries;
    for (const auto& node : spec.Nodes) {
        if (const auto* shuffle = std::get_if<NScheduler::THashShuffleNode>(&node.Body)) {
            entries.push_back(std::get<TJoinHashPlanData>(shuffle->Hash).Entry);
        }
    }
    std::ranges::sort(entries);
    return entries;
}

// These join queries place no runtime filter on a 256-row table, so every
// node has a record.
TEST(ExecPlanBuilder, HashJoinGraphRunsFromItsSerializedSpec) {
    auto settings = MultiSettings();
    // The symmetric table; the row table would take this inner join.
    settings.Join.Table = NScheduler::EJoinTableKind::Legacy;
    const auto rows = ExpectQueryFromSpec(
        "SELECT a.id, b.d9, a.s FROM t a JOIN t b ON a.id = b.id WHERE a.id % 3 <> 0",
        settings, false, [](const NScheduler::TGraphSpec& spec) {
            // Four scan lanes per side shuffle into two partition joins.
            EXPECT_EQ(CountNodes<NScheduler::THashJoinNode>(spec), 2u);
            EXPECT_EQ(JoinHashEntries(spec), (std::vector<uint32_t>{0, 0, 0, 0, 1, 1, 1, 1}));
        });
    EXPECT_EQ(rows.size(), 1u + 170u);
}

TEST(ExecPlanBuilder, RowJoinGraphRunsFromItsSerializedSpec) {
    // The smaller right input is the build side, so the row table serves the
    // LEFT join.
    const auto rows = ExpectQueryFromSpec(
        "SELECT a.id, a.d9, b.s FROM t a LEFT JOIN (SELECT id, s FROM t WHERE id % 4 = 0) b "
        "ON a.id = b.id",
        MultiSettings(), false, [](const NScheduler::TGraphSpec& spec) {
            ASSERT_EQ(CountNodes<NScheduler::TRowJoinNode>(spec), 2u);
            for (const auto& node : spec.Nodes) {
                if (const auto* join = std::get_if<NScheduler::TRowJoinNode>(&node.Body)) {
                    EXPECT_EQ(join->BuildSide, EJoinBuildSide::Right);
                }
            }
            EXPECT_EQ(JoinHashEntries(spec), (std::vector<uint32_t>{0, 0, 0, 0, 1, 1, 1, 1}));
        });
    EXPECT_EQ(rows.size(), 1u + 256u);
}

TEST(ExecPlanBuilder, CrossJoinGraphRunsFromItsSerializedSpec) {
    const auto rows = ExpectQueryFromSpec(
        "SELECT a.id, b.id AS bid FROM t a, (SELECT id FROM t WHERE id < 3) b "
        "WHERE a.id % 50 = 0 AND a.id > b.id",
        MultiSettings(), false, [](const NScheduler::TGraphSpec& spec) {
            EXPECT_GT(CountNodes<NScheduler::TCrossJoinNode>(spec), 0u);
        });
    EXPECT_EQ(rows.size(), 1u + 15u);
}

TEST(ExecPlanBuilder, MultiKeepsRealPartialAndCombineStages) {
    TSplitMockSource source({"k"});
    const std::unordered_map<std::string, ISource*> sources{{"t", &source}};
    constexpr std::string_view sql = "SELECT count(*) AS c FROM t";

    auto singleSettings = SingleSettings();
    singleSettings.Aggregate.CascadeGlobal = NScheduler::EGlobalCascadeMode::On;
    auto multiSettings = MultiSettings();
    multiSettings.Aggregate.CascadeGlobal = NScheduler::EGlobalCascadeMode::On;

    auto single = Lower(sql, sources, singleSettings);
    auto multi = Lower(sql, sources, multiSettings);

    EXPECT_EQ(CountKind(single.Exec, EExecPlanNodeKind::Aggregate), 1u);
    EXPECT_EQ(CountKind(multi.Exec, EExecPlanNodeKind::Aggregate), 2u);
    std::vector<EAggregatePhase> phases;
    for (const auto& node : multi.Exec.Nodes) {
        if (node.Kind == EExecPlanNodeKind::Aggregate) {
            phases.push_back(node.AggregatePhase);
        }
    }
    EXPECT_EQ(phases,
        (std::vector<EAggregatePhase>{
            EAggregatePhase::Partial, EAggregatePhase::Final}));
    EXPECT_GT(multi.PhysicalTasks, single.PhysicalTasks);
    ExpectKernelBindings(single);
    ExpectKernelBindings(multi);
}

TEST(ExecPlanBuilder, AutoGlobalCascadeFollowsTheInputEstimate) {
    constexpr std::string_view sql = "SELECT count(*) AS c FROM t";
    auto phases = [](const TBuiltPlan& built) {
        std::vector<EAggregatePhase> result;
        for (const auto& node : built.Exec.Nodes) {
            if (node.Kind == EExecPlanNodeKind::Aggregate) {
                result.push_back(node.AggregatePhase);
            }
        }
        return result;
    };
    const std::vector<EAggregatePhase> cascade{
        EAggregatePhase::Partial, EAggregatePhase::Final};
    const std::vector<EAggregatePhase> gather{EAggregatePhase::Single};

    // Auto: a large estimated input cascades, a small one gathers.
    TStatsSplitMockSource large(100'000'000, 1'000);
    TStatsSplitMockSource small(1'000, 10);
    auto settings = MultiSettings();
    ASSERT_EQ(settings.Aggregate.CascadeGlobal, NScheduler::EGlobalCascadeMode::Auto);
    EXPECT_EQ(phases(Lower(sql, {{"t", &large}}, settings)), cascade);
    EXPECT_EQ(phases(Lower(sql, {{"t", &small}}, settings)), gather);

    // The bound is the grouped partial aggregate's one.
    settings.Aggregate.PartialMinInputRows = 1'000;
    EXPECT_EQ(phases(Lower(sql, {{"t", &small}}, settings)), cascade);
    settings.Aggregate.PartialMinInputRows = 1'001;
    EXPECT_EQ(phases(Lower(sql, {{"t", &small}}, settings)), gather);

    // Without an estimate auto gathers.
    TSplitMockSource unknown({"k"});
    settings = MultiSettings();
    EXPECT_EQ(phases(Lower(sql, {{"t", &unknown}}, settings)), gather);

    // true and false override the estimate; one lane never cascades.
    settings.Aggregate.CascadeGlobal = NScheduler::EGlobalCascadeMode::On;
    EXPECT_EQ(phases(Lower(sql, {{"t", &small}}, settings)), cascade);
    EXPECT_EQ(phases(Lower(sql, {{"t", &large}}, SingleSettings())), gather);
    settings.Aggregate.CascadeGlobal = NScheduler::EGlobalCascadeMode::Off;
    EXPECT_EQ(phases(Lower(sql, {{"t", &large}}, settings)), gather);
}

TEST(ExecPlanBuilder, GroupedPartialAggregateHasDistributedPhaseBoundary) {
    TStatsSplitMockSource source(100'000'000, 5'000'000);
    const std::unordered_map<std::string, ISource*> sources{{"t", &source}};
    constexpr std::string_view sql =
        "SELECT k, count(*) AS c FROM t GROUP BY k";

    auto settings = MultiSettings();
    auto built = Lower(sql, sources, settings);

    std::vector<const TExecPlanNode*> aggregates;
    for (const auto& node : built.Exec.Nodes) {
        if (node.Kind == EExecPlanNodeKind::Aggregate) {
            aggregates.push_back(&node);
        }
    }
    ASSERT_EQ(aggregates.size(), 2u);
    EXPECT_EQ(aggregates[0]->AggregatePhase, EAggregatePhase::Partial);
    EXPECT_EQ(aggregates[1]->AggregatePhase, EAggregatePhase::Final);
    ASSERT_EQ(aggregates[1]->Inputs.size(), 1u);
    EXPECT_EQ(aggregates[1]->Inputs[0], aggregates[0]->Id);

    ASSERT_EQ(built.Repartitions.size(), 1u);
    EXPECT_EQ(built.Repartitions[0].Sources, 4u);
    EXPECT_EQ(built.Repartitions[0].Destinations, 2u);
    EXPECT_EQ(built.Repartitions[0].Keys,
        (std::vector<std::string>{"t.k"}));
    ExpectKernelBindings(built);

    settings.Aggregate.EnablePartial = false;
    auto disabled = Lower(sql, sources, settings);
    EXPECT_EQ(CountKind(disabled.Exec, EExecPlanNodeKind::Aggregate), 1u);
    const auto aggregate = std::ranges::find_if(
        disabled.Exec.Nodes,
        [](const auto& node) {
            return node.Kind == EExecPlanNodeKind::Aggregate;
        });
    ASSERT_NE(aggregate, disabled.Exec.Nodes.end());
    EXPECT_EQ(aggregate->AggregatePhase, EAggregatePhase::Single);
}

TEST(ExecPlanBuilder, CteConsumersShareTypedMaterialization) {
    TSplitMockSource source({"k", "v"});
    const std::unordered_map<std::string, ISource*> sources{{"t", &source}};
    constexpr std::string_view sql =
        "WITH x AS (SELECT k, v FROM t) "
        "SELECT p.k, q.v FROM x p JOIN x q ON p.k = q.k";

    auto built = Lower(sql, sources, MultiSettings());

    ASSERT_EQ(CountKind(built.Exec, EExecPlanNodeKind::CteProducer), 1u);
    ASSERT_EQ(CountKind(built.Exec, EExecPlanNodeKind::CteConsumer), 2u);
    std::optional<size_t> materialization;
    for (const auto& node : built.Exec.Nodes) {
        if (node.Kind != EExecPlanNodeKind::CteProducer &&
            node.Kind != EExecPlanNodeKind::CteConsumer)
        {
            continue;
        }
        ASSERT_TRUE(node.MaterializationId);
        if (!materialization) {
            materialization = node.MaterializationId;
        }
        EXPECT_EQ(node.MaterializationId, materialization);
    }
    ExpectKernelBindings(built);
}

TEST(ExecPlanBuilder, AutoScanTasksAreChosenPerSourceBeforeJoinShuffle) {
    TStatsSplitMockSource large(100'000'000, 5'000'000);
    TSplitMockSource small({"k"});
    const std::unordered_map<std::string, ISource*> sources{
        {"l", &large}, {"r", &small},
    };
    auto settings = MultiSettings();
    settings.Scheduler.WorkerCount = 2;
    settings.ScanSplit.MaxScanTasks = 0;
    auto standalone = Lower("SELECT k FROM l", sources, settings);
    EXPECT_EQ(standalone.SourceLanes.at("source l"), 4u);

    constexpr std::string_view sql =
        "SELECT l.k FROM l JOIN r ON l.k = r.k";
    auto joined = Lower(sql, sources, settings);
    EXPECT_EQ(joined.SourceLanes.at("source l"), 2u);
    EXPECT_EQ(joined.SourceLanes.at("source r"), 1u);
    ASSERT_EQ(joined.Repartitions.size(), 2u);
    EXPECT_EQ(joined.Repartitions[0].Sources, 2u);
    EXPECT_EQ(joined.Repartitions[1].Sources, 1u);
    EXPECT_EQ(joined.Repartitions[0].Destinations, 2u);
    EXPECT_EQ(joined.Repartitions[1].Destinations, 2u);

    settings.ScanSplit.MaxScanTasks = 4;
    auto explicitTasks = Lower(sql, sources, settings);
    EXPECT_EQ(explicitTasks.SourceLanes.at("source l"), 4u);
    EXPECT_EQ(explicitTasks.SourceLanes.at("source r"), 4u);
    ExpectSameSemanticPlan(joined.Exec, explicitTasks.Exec);
}

TEST(ExecPlanBuilder, AutoScanJoinBudgetReachesSharedCteMaterialization) {
    TSplitMockSource source({"k"});
    const std::unordered_map<std::string, ISource*> sources{{"t", &source}};
    auto settings = MultiSettings();
    settings.Scheduler.WorkerCount = 2;
    settings.ScanSplit.MaxScanTasks = 0;
    settings.ScanSplit.TargetRowsPerTask = 1;
    // The producer is first reached by a consumer without a downstream join.
    constexpr std::string_view sql =
        "WITH x AS (SELECT k FROM t) "
        "SELECT k FROM x UNION ALL "
        "SELECT p.k FROM x p JOIN x q ON p.k = q.k";
    auto built = Lower(sql, sources, settings);
    EXPECT_EQ(built.SourceLanes.at("source t"), 2u);
    EXPECT_EQ(CountKind(built.Exec, EExecPlanNodeKind::CteProducer), 1u);
    ExpectKernelBindings(built);
}

} // namespace

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    NQumir::NCodeGen::TLLVMInitializer initializer;
    return RUN_ALL_TESTS();
}
