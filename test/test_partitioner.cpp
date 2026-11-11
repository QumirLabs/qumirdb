#include <qdb/scheduler/connections/local.h>
#include <qdb/scheduler/graph.h>
#include <qdb/scheduler/port.h>
#include <qdb/scheduler/tasks/runtime_adapter.h>
#include <qdbd/partitioner/partitioner.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <deque>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace NQdb::NScheduler;

// Holds the ports of its edges for Apply to rebind; never runs.
class TNoopTask final : public ITask {
public:
    ETaskResult Execute() override {
        ADD_FAILURE() << "partitioning must not execute tasks";
        return ETaskResult::FINISHED;
    }

    bool RebindPort(const TPortRebind& rebind) override {
        return std::ranges::any_of(Inputs, [&](auto& port) { return rebind.Apply(port); })
            || std::ranges::any_of(Outputs, [&](auto& port) { return rebind.Apply(port); });
    }

    std::vector<TInputPort> Inputs;
    std::vector<TOutputPort> Outputs;
};

// One applied cut. Apply destroys the cut edge, so Edge is a copy of it.
struct TCut {
    TTaskEdge Edge;
    EReboundPort ReboundPort = EReboundPort::DestinationInput;
    int SinkColor = 0;
    IConnection* NewConnection = nullptr;
    TTaskNode* Sink = nullptr;
    TTaskNode* Source = nullptr;
    TTaskEdge* SinkEdge = nullptr;
    TTaskEdge* SourceEdge = nullptr;
};

class TPartitionerTest : public testing::Test {
protected:
    TTaskNode& AddNode() {
        return Graph_.AddOwnedNode(std::make_unique<TNoopTask>());
    }

    TTaskEdge& Connect(TTaskNode& src, TTaskNode& dst) {
        return AddEdge(src, dst, Graph_.AddConnection(std::make_unique<TOneToOneConnection>()));
    }

    TTaskEdge& AddEdge(
        TTaskNode& src,
        TTaskNode& dst,
        IConnection& connection,
        size_t srcLane = 0,
        size_t dstLane = 0,
        size_t execInput = 0)
    {
        Task(src).Outputs.push_back({.Connection = &connection, .Lane = srcLane});
        Task(dst).Inputs.push_back({.Connection = &connection, .Lane = dstLane});
        return Graph_.AddEdge(src, dst, connection, srcLane, dstLane, execInput);
    }

    static TNoopTask& Task(TTaskNode& node) {
        return static_cast<TNoopTask&>(*node.Task);
    }

    void CheckResult(const TPartitioner& partitioner, int partitions) {
        const auto& colors = partitioner.Colors();
        const auto& splits = partitioner.SplitEdges();
        ASSERT_EQ(colors.size(), Graph_.Nodes().size());
        std::set<int> used;
        for (const auto& node : Graph_.Nodes()) {
            const auto it = colors.find(node.get());
            ASSERT_NE(it, colors.end()) << node->ExecutionSlot;
            ASSERT_GE(it->second, 0);
            ASSERT_LT(it->second, partitions);
            used.insert(it->second);
        }
        // Apply takes colors 0..K-1 without gaps.
        EXPECT_EQ(*used.rbegin() + 1, static_cast<int>(used.size()));
        size_t expectedSplits = 0;
        for (const auto& edge : Graph_.Edges()) {
            const bool crosses = colors.at(edge->Src) != colors.at(edge->Dst);
            EXPECT_EQ(splits.contains(edge.get()), crosses)
                << edge->Src->ExecutionSlot << " -> " << edge->Dst->ExecutionSlot;
            expectedSplits += crosses;
        }
        EXPECT_EQ(splits.size(), expectedSplits);
    }

    // Applies the plan to Graph_, fills Cuts_ and checks the remote nodes and
    // that Graphs()[c] holds the roots of color c.
    void ApplyPlan(TPartitioner& partitioner) {
        const auto& colors = partitioner.Colors();
        const auto& splits = partitioner.SplitEdges();
        const auto rowsetSplits = std::ranges::count_if(splits, [](const TTaskEdge* edge) {
            return edge->Kind == ETaskEdgeKind::Rowset;
        });
        auto plan = partitioner.TakePlan();
        ASSERT_EQ(plan.Splits.size(), static_cast<size_t>(rowsetSplits));
        Cuts_.clear();
        std::vector<std::pair<const ITask*, const ITask*>> tasks;
        for (const auto& split : plan.Splits) {
            ASSERT_TRUE(splits.contains(split.Edge));
            const bool gather = split.Edge->Connection->Kind() == EConnectionKind::Gather;
            EXPECT_EQ(
                split.ReboundPort,
                gather ? EReboundPort::SourceOutput : EReboundPort::DestinationInput);
            EXPECT_EQ(split.SinkColor, colors.at(split.Edge->Src));
            EXPECT_EQ(split.SourceColor, colors.at(split.Edge->Dst));
            Cuts_.push_back({
                .Edge = *split.Edge,
                .ReboundPort = split.ReboundPort,
                .SinkColor = split.SinkColor,
                .NewConnection = split.NewConnection.get(),
            });
            tasks.emplace_back(split.SinkTask.get(), split.SourceTask.get());
        }

        const auto sealed = Graph_.Apply(std::move(plan));
        ASSERT_TRUE(sealed);
        auto nodeOf = [&](const ITask* task) -> TTaskNode* {
            for (const auto& node : Graph_.Nodes()) {
                if (node->Task == task) {
                    return node.get();
                }
            }
            return nullptr;
        };
        for (size_t i = 0; i < Cuts_.size(); ++i) {
            auto& cut = Cuts_[i];
            cut.Sink = nodeOf(tasks[i].first);
            cut.Source = nodeOf(tasks[i].second);
            ASSERT_NE(cut.Sink, nullptr);
            ASSERT_NE(cut.Source, nullptr);
            EXPECT_EQ(cut.Sink->DebugKind, "remote-sink");
            EXPECT_EQ(cut.Source->DebugKind, "remote-source");
            ASSERT_EQ(cut.Sink->Inbound.size(), 1u);
            EXPECT_TRUE(cut.Sink->Outbound.empty());
            EXPECT_TRUE(cut.Source->Inbound.empty());
            ASSERT_EQ(cut.Source->Outbound.size(), 1u);
            cut.SinkEdge = cut.Sink->Inbound.front();
            cut.SourceEdge = cut.Source->Outbound.front();
            EXPECT_EQ(cut.SinkEdge->Src, cut.Edge.Src);
            EXPECT_EQ(cut.SourceEdge->Dst, cut.Edge.Dst);
            EXPECT_EQ(cut.SinkEdge->ExecInput, 0u);
            EXPECT_EQ(cut.SourceEdge->ExecInput, cut.Edge.ExecInput);
        }

        std::set<int> used;
        for (const auto& [node, color] : colors) {
            used.insert(color);
        }
        std::vector<std::vector<TTaskNode*>> roots(used.size());
        for (const auto& node : Graph_.Nodes()) {
            if (std::ranges::any_of(node->Outbound, [](const TTaskEdge* edge) {
                    return edge->Kind == ETaskEdgeKind::Rowset;
                })) {
                continue;
            }
            int color = 0;
            if (const auto it = colors.find(node.get()); it != colors.end()) {
                color = it->second;
            } else {
                const auto cut = std::ranges::find(Cuts_, node.get(), &TCut::Sink);
                ASSERT_NE(cut, Cuts_.end()) << node->ExecutionSlot;
                color = cut->SinkColor;
            }
            ASSERT_LT(static_cast<size_t>(color), roots.size());
            roots[color].push_back(node.get());
        }
        EXPECT_EQ(
            std::vector<std::vector<TTaskNode*>>(sealed.Graphs().begin(), sealed.Graphs().end()),
            roots);
    }

    TCut* FindCut(IConnection& connection) {
        for (auto& cut : Cuts_) {
            if (cut.Edge.Connection == &connection) {
                return &cut;
            }
        }
        return nullptr;
    }

    static NQdb::TRowSet EmptyBatch(int64_t rows) {
        return {.RowCount = rows, .RefCount = 1};
    }

    void BindEmptyBatchTransport(const TCut& cut) {
        struct TTransport {
            std::deque<int64_t> RowCounts;
            bool Finished = false;
        };
        auto transport = std::make_shared<TTransport>();
        auto* sink = dynamic_cast<TSinkTask*>(cut.Sink->Task);
        auto* source = dynamic_cast<TSourceTask*>(cut.Source->Task);
        ASSERT_NE(sink, nullptr);
        ASSERT_NE(source, nullptr);
        sink->BindCode(std::make_shared<TSinkCode>(
            [](void* state, const NQdb::TRowSet& rows) {
                EXPECT_EQ(rows.ColumnCount, 0);
                static_cast<TTransport*>(state)->RowCounts.push_back(rows.RowCount);
                return ETaskResult::OK;
            },
            [](void* state) {
                static_cast<TTransport*>(state)->Finished = true;
                return ETaskResult::OK;
            }), transport);
        source->BindCode(std::make_shared<TSourceCode>(
            [](void* state, NQdb::TRowSet& rows) {
                auto& transport = *static_cast<TTransport*>(state);
                if (!transport.RowCounts.empty()) {
                    rows = EmptyBatch(transport.RowCounts.front());
                    transport.RowCounts.pop_front();
                    return ETaskResult::OK;
                }
                return transport.Finished
                    ? ETaskResult::FINISHED
                    : ETaskResult::WAITING;
            }), transport);
    }

    TTaskGraph Graph_;
    std::vector<TCut> Cuts_;
};

TEST_F(TPartitionerTest, SingleNodeHasNoCuts) {
    auto& root = AddNode();
    std::string error;
    const auto sealed = Graph_.Seal(&error);
    ASSERT_TRUE(sealed) << error;

    for (int partitions : {1, 2, 4}) {
        SCOPED_TRACE(partitions);
        TPartitioner partitioner(sealed, partitions);
        partitioner.Do();
        ASSERT_NO_FATAL_FAILURE(CheckResult(partitioner, partitions));
        EXPECT_TRUE(partitioner.SplitEdges().empty());
        ASSERT_NO_FATAL_FAILURE(ApplyPlan(partitioner));
    }
}

TEST_F(TPartitionerTest, ChainKeepsOneColor) {
    auto& source = AddNode();
    auto& middle = AddNode();
    auto& root = AddNode();
    Connect(source, middle);
    Connect(middle, root);
    std::string error;
    const auto sealed = Graph_.Seal(&error);
    ASSERT_TRUE(sealed) << error;

    for (int partitions : {1, 2, 4}) {
        SCOPED_TRACE(partitions);
        TPartitioner partitioner(sealed, partitions);
        partitioner.Do();
        ASSERT_NO_FATAL_FAILURE(CheckResult(partitioner, partitions));
        EXPECT_EQ(partitioner.Colors().at(&source), partitioner.Colors().at(&root));
        EXPECT_EQ(partitioner.Colors().at(&middle), partitioner.Colors().at(&root));
        EXPECT_TRUE(partitioner.SplitEdges().empty());
        ASSERT_NO_FATAL_FAILURE(ApplyPlan(partitioner));
    }
}

class TPartitionerGatherTest
    : public TPartitionerTest
    , public testing::WithParamInterface<int>
{ };

// Apply cuts a graph once, so each partition count gets its own graph.
TEST_P(TPartitionerGatherTest, GatherBalancesItsProducers) {
    const int partitions = GetParam();
    constexpr size_t sourceCount = 7;
    std::array<TTaskNode*, sourceCount> sources;
    for (auto& source : sources) {
        source = &AddNode();
    }
    auto& root = AddNode();
    auto connection = std::make_unique<TGatherConnection>();
    connection->Resize(sourceCount, 1);
    auto& gather = Graph_.AddConnection(std::move(connection));
    for (size_t lane = 0; lane < sourceCount; ++lane) {
        AddEdge(*sources[lane], root, gather, lane, 0);
    }
    std::string error;
    const auto sealed = Graph_.Seal(&error);
    ASSERT_TRUE(sealed) << error;

    TPartitioner partitioner(sealed, partitions);
    partitioner.Do();
    ASSERT_NO_FATAL_FAILURE(CheckResult(partitioner, partitions));
    std::vector<size_t> counts(partitions);
    for (auto* source : sources) {
        ++counts[partitioner.Colors().at(source)];
    }
    const auto [minCount, maxCount] = std::minmax_element(counts.begin(), counts.end());
    EXPECT_LE(*maxCount - *minCount, 1u);
    const auto local = counts[partitioner.Colors().at(&root)];
    EXPECT_GT(local, 0u);
    EXPECT_EQ(partitioner.SplitEdges().size(), sourceCount - local);
    ASSERT_NO_FATAL_FAILURE(ApplyPlan(partitioner));
}

INSTANTIATE_TEST_SUITE_P(Partitions, TPartitionerGatherTest, testing::Values(1, 2, 3, 10));

TEST_F(TPartitionerTest, DiamondHasOnlyFinalColorCuts) {
    auto& source = AddNode();
    auto& left = AddNode();
    auto& right = AddNode();
    auto& root = AddNode();
    Connect(source, left);
    Connect(source, right);
    Connect(left, root);
    Connect(right, root);
    std::string error;
    const auto sealed = Graph_.Seal(&error);
    ASSERT_TRUE(sealed) << error;

    TPartitioner partitioner(sealed, 2);
    partitioner.Do();
    ASSERT_NO_FATAL_FAILURE(CheckResult(partitioner, 2));
    EXPECT_NE(partitioner.Colors().at(&left), partitioner.Colors().at(&right));
    EXPECT_EQ(partitioner.SplitEdges().size(), 2u);
    ASSERT_NO_FATAL_FAILURE(ApplyPlan(partitioner));
}

TEST_F(TPartitionerTest, ShortcutPreservesTopologicalLevels) {
    auto& source = AddNode();
    auto& middle = AddNode();
    auto& root = AddNode();
    Connect(source, root);
    Connect(source, middle);
    Connect(middle, root);
    std::string error;
    const auto sealed = Graph_.Seal(&error);
    ASSERT_TRUE(sealed) << error;

    TPartitioner partitioner(sealed, 2);
    partitioner.Do();
    ASSERT_NO_FATAL_FAILURE(CheckResult(partitioner, 2));
    EXPECT_EQ(partitioner.Colors().at(&source), partitioner.Colors().at(&root));
    EXPECT_EQ(partitioner.Colors().at(&middle), partitioner.Colors().at(&root));
    EXPECT_TRUE(partitioner.SplitEdges().empty());
    ASSERT_NO_FATAL_FAILURE(ApplyPlan(partitioner));
}

TEST_F(TPartitionerTest, ShuffleFindsCutsForEachLane) {
    std::array<TTaskNode*, 2> sources{&AddNode(), &AddNode()};
    std::array<TTaskNode*, 2> consumers{&AddNode(), &AddNode()};
    auto& root = AddNode();
    std::array<std::array<TTaskEdge*, 2>, 2> edges;
    for (size_t dst = 0; dst < consumers.size(); ++dst) {
        auto connection = std::make_unique<TGatherConnection>();
        connection->Resize(sources.size(), 1);
        auto& gather = Graph_.AddConnection(std::move(connection));
        for (size_t src = 0; src < sources.size(); ++src) {
            edges[src][dst] = &AddEdge(*sources[src], *consumers[dst], gather, src, 0);
        }
    }
    auto gatherConnection = std::make_unique<TGatherConnection>();
    gatherConnection->Resize(consumers.size(), 1);
    auto& gather = Graph_.AddConnection(std::move(gatherConnection));
    for (size_t lane = 0; lane < consumers.size(); ++lane) {
        AddEdge(*consumers[lane], root, gather, lane, 0);
    }
    std::string error;
    const auto sealed = Graph_.Seal(&error);
    ASSERT_TRUE(sealed) << error;

    TPartitioner partitioner(sealed, 2);
    partitioner.Do();
    ASSERT_NO_FATAL_FAILURE(CheckResult(partitioner, 2));
    EXPECT_NE(partitioner.Colors().at(sources[0]), partitioner.Colors().at(sources[1]));
    EXPECT_NE(partitioner.Colors().at(consumers[0]), partitioner.Colors().at(consumers[1]));
    EXPECT_EQ(partitioner.SplitEdges().size(), 3u);
    for (size_t lane = 0; lane < 2; ++lane) {
        EXPECT_EQ(
            partitioner.SplitEdges().contains(edges[lane][0])
                + partitioner.SplitEdges().contains(edges[lane][1]),
            1);
        EXPECT_EQ(
            partitioner.SplitEdges().contains(edges[0][lane])
                + partitioner.SplitEdges().contains(edges[1][lane]),
            1);
    }
    ASSERT_NO_FATAL_FAILURE(ApplyPlan(partitioner));
}

TEST_F(TPartitionerTest, SharedSourceWithTwoRootsColorsEveryNode) {
    auto& source = AddNode();
    auto& first = AddNode();
    auto& second = AddNode();
    Connect(source, first);
    Connect(source, second);
    std::string error;
    const auto sealed = Graph_.Seal(&error);
    ASSERT_TRUE(sealed) << error;
    ASSERT_EQ(sealed.Graphs().front().size(), 2u);

    TPartitioner partitioner(sealed, 2);
    partitioner.Do();
    ASSERT_NO_FATAL_FAILURE(CheckResult(partitioner, 2));
    ASSERT_NO_FATAL_FAILURE(ApplyPlan(partitioner));
}

TEST_F(TPartitionerTest, DisjointRootsAreAllColored) {
    auto& a = AddNode();
    auto& b = AddNode();
    auto& c = AddNode();
    auto& first = AddNode();
    auto& second = AddNode();
    Connect(a, first);
    Connect(b, first);
    Connect(c, second);
    std::string error;
    const auto sealed = Graph_.Seal(&error);
    ASSERT_TRUE(sealed) << error;

    TPartitioner partitioner(sealed, 2);
    partitioner.Do();
    ASSERT_NO_FATAL_FAILURE(CheckResult(partitioner, 2));
    // The components share no edge, so Apply sees no cut and keeps one fragment.
    EXPECT_TRUE(Graph_.Apply(partitioner.TakePlan()));
}

TEST_F(TPartitionerTest, RootBehindDependencyEdgeIsColored) {
    auto& source = AddNode();
    auto& root = AddNode();
    auto& gated = AddNode();
    Connect(source, root);
    Graph_.AddDependencyEdge(source, gated);
    std::string error;
    const auto sealed = Graph_.Seal(&error);
    ASSERT_TRUE(sealed) << error;

    TPartitioner partitioner(sealed, 2);
    partitioner.Do();
    ASSERT_NO_FATAL_FAILURE(CheckResult(partitioner, 2));
}

TEST_F(TPartitionerTest, RootBehindDependencyEdgeInOneColorApplies) {
    auto& source = AddNode();
    auto& root = AddNode();
    auto& gated = AddNode();
    Connect(source, root);
    Graph_.AddDependencyEdge(source, gated);
    std::string error;
    const auto sealed = Graph_.Seal(&error);
    ASSERT_TRUE(sealed) << error;

    TPartitioner partitioner(sealed, 1);
    partitioner.Do();
    ASSERT_NO_FATAL_FAILURE(CheckResult(partitioner, 1));
    ASSERT_NO_FATAL_FAILURE(ApplyPlan(partitioner));
}

TEST_F(TPartitionerTest, PlanFollowsNodeSlotOrder) {
    auto& root = AddNode();
    for (int i = 0; i < 8; ++i) {
        Connect(AddNode(), root);
    }
    std::string error;
    const auto sealed = Graph_.Seal(&error);
    ASSERT_TRUE(sealed) << error;

    TPartitioner partitioner(sealed, 4);
    partitioner.Do();
    const auto plan = partitioner.TakePlan();
    ASSERT_GT(plan.Splits.size(), 1u);
    for (size_t i = 1; i < plan.Splits.size(); ++i) {
        EXPECT_LT(
            plan.Splits[i - 1].Edge->Src->ExecutionSlot,
            plan.Splits[i].Edge->Src->ExecutionSlot);
    }
}

TEST_F(TPartitionerTest, RejectsNonPositivePartitions) {
    AddNode();
    std::string error;
    const auto sealed = Graph_.Seal(&error);
    ASSERT_TRUE(sealed) << error;
    EXPECT_THROW(TPartitioner(sealed, 0), std::invalid_argument);
    EXPECT_THROW(TPartitioner(sealed, -1), std::invalid_argument);
}

TEST_F(TPartitionerTest, IncludesCrossPartitionDependency) {
    auto& left = AddNode();
    auto& right = AddNode();
    auto& root = AddNode();
    Connect(left, root);
    Connect(right, root);
    auto& dependency = Graph_.AddDependencyEdge(left, right);
    std::string error;
    const auto sealed = Graph_.Seal(&error);
    ASSERT_TRUE(sealed) << error;

    TPartitioner partitioner(sealed, 2);
    partitioner.Do();
    ASSERT_NO_FATAL_FAILURE(CheckResult(partitioner, 2));
    EXPECT_NE(partitioner.Colors().at(&left), partitioner.Colors().at(&right));
    EXPECT_TRUE(partitioner.SplitEdges().contains(&dependency));
    EXPECT_EQ(partitioner.SplitEdges().size(), 2u);
    // The plan leaves dependency cuts out, and Apply rejects a gate across colors.
    try {
        Graph_.Apply(partitioner.TakePlan());
        FAIL() << "Apply accepted a dependency edge across colors";
    } catch (const std::logic_error& e) {
        EXPECT_NE(std::string(e.what()).find("dependency edge"), std::string::npos) << e.what();
        EXPECT_NE(std::string(e.what()).find("crosses colors"), std::string::npos) << e.what();
    }
}

TEST_F(TPartitionerTest, OneToOneSplitRebindsDestinationLane) {
    auto& left = AddNode();
    auto& right = AddNode();
    auto& root = AddNode();
    auto connection = std::make_unique<TOneToOneConnection>(3);
    connection->Resize(6, 6);
    connection->SetRowLayouts({{.Width = 8}});
    auto& original = Graph_.AddConnection(std::move(connection));
    AddEdge(left, root, original, 3, 3, 1);
    AddEdge(right, root, original, 5, 5, 2);
    std::string error;
    const auto sealed = Graph_.Seal(&error);
    ASSERT_TRUE(sealed) << error;

    TPartitioner partitioner(sealed, 2);
    partitioner.Do();
    ASSERT_NO_FATAL_FAILURE(CheckResult(partitioner, 2));
    ASSERT_NO_FATAL_FAILURE(ApplyPlan(partitioner));
    ASSERT_EQ(Cuts_.size(), 1u);
    auto& cut = Cuts_.front();
    EXPECT_EQ(cut.ReboundPort, EReboundPort::DestinationInput);
    EXPECT_EQ(cut.SinkEdge->Connection, &original);
    EXPECT_EQ(cut.SinkEdge->DstLane, cut.Edge.DstLane);
    EXPECT_EQ(cut.SourceEdge->Connection, cut.NewConnection);
    EXPECT_EQ(cut.SourceEdge->SrcLane, 0u);
    EXPECT_EQ(cut.SourceEdge->DstLane, 0u);
    EXPECT_EQ(cut.NewConnection->SrcCount(), 1u);
    EXPECT_EQ(cut.NewConnection->DstCount(), 1u);
    EXPECT_EQ(dynamic_cast<TOneToOneConnection&>(*cut.NewConnection).Capacity(), 3u);
    ASSERT_EQ(cut.NewConnection->RowLayouts().size(), 1u);
    EXPECT_EQ(cut.NewConnection->RowLayouts()[0].Width, 8);
    const auto& inputs = Task(root).Inputs;
    const auto input = std::ranges::find(inputs, cut.NewConnection, &TInputPort::Connection);
    ASSERT_NE(input, inputs.end());
    EXPECT_EQ(input->Lane, 0u);
    ASSERT_NO_FATAL_FAILURE(BindEmptyBatchTransport(cut));

    ASSERT_TRUE(original.Push(cut.Edge.SrcLane, EmptyBatch(42)));
    original.Finish(cut.Edge.SrcLane);
    EXPECT_EQ(cut.Sink->Task->Execute(), ETaskResult::OK);
    EXPECT_EQ(cut.Source->Task->Execute(), ETaskResult::OK);
    NQdb::TRowSet rows{};
    ASSERT_EQ(input->Fetch(rows), EFetchResult::OK);
    EXPECT_EQ(rows.RowCount, 42);
    NQdb::Release(&rows);
    EXPECT_EQ(cut.Source->Task->Execute(), ETaskResult::WAITING);
    EXPECT_EQ(input->Fetch(rows), EFetchResult::NO_DATA);
    EXPECT_EQ(cut.Sink->Task->Execute(), ETaskResult::FINISHED);
    EXPECT_EQ(cut.Source->Task->Execute(), ETaskResult::FINISHED);
    EXPECT_EQ(input->Fetch(rows), EFetchResult::FINISHED);
}

TEST_F(TPartitionerTest, GatherSplitRebindsSourceAndWaitsForRemoteEof) {
    auto& left = AddNode();
    auto& right = AddNode();
    auto& root = AddNode();
    auto connection = std::make_unique<TGatherConnection>(2);
    connection->Resize(2, 1);
    auto& original = Graph_.AddConnection(std::move(connection));
    AddEdge(left, root, original, 0, 0, 1);
    AddEdge(right, root, original, 1, 0, 1);
    std::string error;
    const auto sealed = Graph_.Seal(&error);
    ASSERT_TRUE(sealed) << error;

    TPartitioner partitioner(sealed, 2);
    partitioner.Do();
    ASSERT_NO_FATAL_FAILURE(CheckResult(partitioner, 2));
    ASSERT_NO_FATAL_FAILURE(ApplyPlan(partitioner));
    ASSERT_EQ(Cuts_.size(), 1u);
    auto& cut = Cuts_.front();
    EXPECT_EQ(cut.ReboundPort, EReboundPort::SourceOutput);
    EXPECT_EQ(cut.SinkEdge->Connection, cut.NewConnection);
    EXPECT_EQ(cut.SinkEdge->SrcLane, 0u);
    EXPECT_EQ(cut.SinkEdge->DstLane, 0u);
    EXPECT_EQ(cut.SourceEdge->Connection, &original);
    EXPECT_EQ(cut.SourceEdge->SrcLane, cut.Edge.SrcLane);
    EXPECT_EQ(cut.SourceEdge->DstLane, 0u);
    EXPECT_EQ(original.SrcCount(), 2u);
    EXPECT_EQ(dynamic_cast<TOneToOneConnection&>(*cut.NewConnection).Capacity(), 2u);
    const auto& output = Task(*cut.Edge.Src).Outputs.front();
    EXPECT_EQ(output.Connection, cut.NewConnection);
    EXPECT_EQ(output.Lane, 0u);
    ASSERT_NO_FATAL_FAILURE(BindEmptyBatchTransport(cut));

    const auto localLane = 1 - cut.Edge.SrcLane;
    ASSERT_TRUE(original.Push(localLane, EmptyBatch(11)));
    original.Finish(localLane);
    NQdb::TRowSet rows{};
    ASSERT_EQ(original.Fetch(0, rows), EFetchResult::OK);
    EXPECT_EQ(rows.RowCount, 11);
    NQdb::Release(&rows);
    EXPECT_EQ(original.Fetch(0, rows), EFetchResult::NO_DATA);

    ASSERT_TRUE(output.Push(EmptyBatch(22)));
    output.Finish();
    EXPECT_EQ(cut.Sink->Task->Execute(), ETaskResult::OK);
    EXPECT_EQ(cut.Source->Task->Execute(), ETaskResult::OK);
    ASSERT_EQ(original.Fetch(0, rows), EFetchResult::OK);
    EXPECT_EQ(rows.RowCount, 22);
    NQdb::Release(&rows);
    EXPECT_EQ(original.Fetch(0, rows), EFetchResult::NO_DATA);
    EXPECT_EQ(cut.Sink->Task->Execute(), ETaskResult::FINISHED);
    EXPECT_EQ(cut.Source->Task->Execute(), ETaskResult::FINISHED);
    EXPECT_EQ(original.Fetch(0, rows), EFetchResult::FINISHED);
}

TEST_F(TPartitionerTest, BroadcastSplitPreservesLocalAndRemoteCopies) {
    auto& producer = AddNode();
    auto& left = AddNode();
    auto& right = AddNode();
    auto& root = AddNode();
    auto connection = std::make_unique<TBroadcastConnection>(2);
    connection->Resize(1, 2);
    auto& original = Graph_.AddConnection(std::move(connection));
    AddEdge(producer, left, original, 0, 0, 1);
    AddEdge(producer, right, original, 0, 1, 1);
    Connect(left, root);
    Connect(right, root);
    std::string error;
    const auto sealed = Graph_.Seal(&error);
    ASSERT_TRUE(sealed) << error;

    TPartitioner partitioner(sealed, 2);
    partitioner.Do();
    ASSERT_NO_FATAL_FAILURE(CheckResult(partitioner, 2));
    ASSERT_NO_FATAL_FAILURE(ApplyPlan(partitioner));
    auto* cut = FindCut(original);
    ASSERT_NE(cut, nullptr);
    EXPECT_EQ(cut->ReboundPort, EReboundPort::DestinationInput);
    EXPECT_EQ(cut->SinkEdge->Connection, &original);
    EXPECT_EQ(cut->SinkEdge->SrcLane, 0u);
    EXPECT_EQ(cut->SinkEdge->DstLane, cut->Edge.DstLane);
    EXPECT_EQ(cut->SourceEdge->Connection, cut->NewConnection);
    EXPECT_EQ(original.DstCount(), 2u);
    const auto& input = Task(*cut->Edge.Dst).Inputs.front();
    EXPECT_EQ(input.Connection, cut->NewConnection);
    EXPECT_EQ(input.Lane, 0u);
    ASSERT_NO_FATAL_FAILURE(BindEmptyBatchTransport(*cut));

    ASSERT_TRUE(original.Push(0, EmptyBatch(7)));
    original.Finish(0);
    NQdb::TRowSet rows{};
    const auto localLane = 1 - cut->Edge.DstLane;
    ASSERT_EQ(original.Fetch(localLane, rows), EFetchResult::OK);
    EXPECT_EQ(rows.RowCount, 7);
    NQdb::Release(&rows);
    EXPECT_EQ(original.Fetch(localLane, rows), EFetchResult::FINISHED);
    EXPECT_EQ(cut->Sink->Task->Execute(), ETaskResult::OK);
    EXPECT_EQ(cut->Source->Task->Execute(), ETaskResult::OK);
    ASSERT_EQ(input.Fetch(rows), EFetchResult::OK);
    EXPECT_EQ(rows.RowCount, 7);
    NQdb::Release(&rows);
    EXPECT_EQ(cut->Sink->Task->Execute(), ETaskResult::FINISHED);
    EXPECT_EQ(cut->Source->Task->Execute(), ETaskResult::FINISHED);
    EXPECT_EQ(input.Fetch(rows), EFetchResult::FINISHED);
}

} // namespace
