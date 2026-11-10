#include <qdb/scheduler/connections/local.h>
#include <qdb/scheduler/graph.h>
#include <qdb/scheduler/tasks/runtime_adapter.h>
#include <qdbd/partitioner/partitioner.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace NQdb::NScheduler;

class TNoopTask final : public ITask {
public:
    ETaskResult Execute() override {
        ADD_FAILURE() << "partitioning must not execute tasks";
        return ETaskResult::FINISHED;
    }
};

class TPartitionerTest : public testing::Test {
protected:
    TTaskNode& AddNode() {
        return Graph_.AddOwnedNode(std::make_unique<TNoopTask>());
    }

    TTaskEdge& Connect(TTaskNode& src, TTaskNode& dst) {
        return Graph_.AddOwnedEdge(src, dst, std::make_unique<TOneToOneConnection>());
    }

    void CheckResult(const TPartitioner& partitioner, int partitions) {
        const auto& colors = partitioner.Colors();
        const auto& splits = partitioner.SplitEdges();
        ASSERT_EQ(colors.size(), Graph_.Nodes().size());
        for (const auto& node : Graph_.Nodes()) {
            const auto it = colors.find(node.get());
            ASSERT_NE(it, colors.end()) << node->ExecutionSlot;
            ASSERT_GE(it->second, 0);
            ASSERT_LT(it->second, partitions);
        }
        size_t expectedSplits = 0;
        size_t rowsetSplits = 0;
        for (const auto& edge : Graph_.Edges()) {
            const bool crosses = colors.at(edge->Src) != colors.at(edge->Dst);
            EXPECT_EQ(splits.contains(edge.get()), crosses)
                << edge->Src->ExecutionSlot << " -> " << edge->Dst->ExecutionSlot;
            expectedSplits += crosses;
            rowsetSplits += crosses && edge->Kind == ETaskEdgeKind::Rowset;
        }
        EXPECT_EQ(splits.size(), expectedSplits);
        EXPECT_EQ(partitioner.ConnectionSplits().size(), rowsetSplits);
        for (const auto& split : partitioner.ConnectionSplits()) {
            ASSERT_TRUE(splits.contains(split->OriginalEdge));
            EXPECT_EQ(split->SinkEdge.Src, split->OriginalEdge->Src);
            EXPECT_EQ(split->SinkEdge.Dst, &split->SinkNode);
            EXPECT_EQ(split->SourceEdge.Src, &split->SourceNode);
            EXPECT_EQ(split->SourceEdge.Dst, split->OriginalEdge->Dst);
            EXPECT_EQ(split->SinkNode.Task, split->SinkTask.get());
            EXPECT_EQ(split->SourceNode.Task, split->SourceTask.get());
            EXPECT_EQ(split->SinkPartition, colors.at(split->OriginalEdge->Src));
            EXPECT_EQ(split->SourcePartition, colors.at(split->OriginalEdge->Dst));
            EXPECT_EQ(split->SinkEdge.ExecInput, 0u);
            EXPECT_EQ(split->SourceEdge.ExecInput, split->OriginalEdge->ExecInput);
        }
    }

    TConnectionSplitPlan* FindSplit(const TPartitioner& partitioner, IConnection& connection) {
        for (const auto& split : partitioner.ConnectionSplits()) {
            if (split->OriginalEdge->Connection == &connection) {
                return split.get();
            }
        }
        return nullptr;
    }

    static NQdb::TRowSet EmptyBatch(int64_t rows) {
        return {.RowCount = rows, .RefCount = 1};
    }

    void BindEmptyBatchTransport(TConnectionSplitPlan& split) {
        struct TTransport {
            std::deque<int64_t> RowCounts;
            bool Finished = false;
        };
        auto transport = std::make_shared<TTransport>();
        auto* sink = dynamic_cast<TSinkTask*>(split.SinkNode.Task);
        auto* source = dynamic_cast<TSourceTask*>(split.SourceNode.Task);
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
};

TEST_F(TPartitionerTest, SingleNodeHasNoCuts) {
    auto& root = AddNode();
    std::string error;
    ASSERT_TRUE(Graph_.Seal(&error)) << error;

    for (int partitions : {1, 2, 4}) {
        SCOPED_TRACE(partitions);
        TPartitioner partitioner(&root, partitions);
        partitioner.Do();
        ASSERT_NO_FATAL_FAILURE(CheckResult(partitioner, partitions));
        EXPECT_TRUE(partitioner.SplitEdges().empty());
    }
}

TEST_F(TPartitionerTest, ChainKeepsOneColor) {
    auto& source = AddNode();
    auto& middle = AddNode();
    auto& root = AddNode();
    Connect(source, middle);
    Connect(middle, root);
    std::string error;
    ASSERT_TRUE(Graph_.Seal(&error)) << error;

    for (int partitions : {1, 2, 4}) {
        SCOPED_TRACE(partitions);
        TPartitioner partitioner(&root, partitions);
        partitioner.Do();
        ASSERT_NO_FATAL_FAILURE(CheckResult(partitioner, partitions));
        EXPECT_EQ(partitioner.Colors().at(&source), partitioner.Colors().at(&root));
        EXPECT_EQ(partitioner.Colors().at(&middle), partitioner.Colors().at(&root));
        EXPECT_TRUE(partitioner.SplitEdges().empty());
    }
}

TEST_F(TPartitionerTest, GatherBalancesItsProducers) {
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
        Graph_.AddEdge(*sources[lane], root, gather, lane, 0);
    }
    std::string error;
    ASSERT_TRUE(Graph_.Seal(&error)) << error;

    for (int partitions : {1, 2, 3, 10}) {
        SCOPED_TRACE(partitions);
        TPartitioner partitioner(&root, partitions);
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
    }
}

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
    ASSERT_TRUE(Graph_.Seal(&error)) << error;

    TPartitioner partitioner(&root, 2);
    partitioner.Do();
    ASSERT_NO_FATAL_FAILURE(CheckResult(partitioner, 2));
    EXPECT_NE(partitioner.Colors().at(&left), partitioner.Colors().at(&right));
    EXPECT_EQ(partitioner.SplitEdges().size(), 2u);
}

TEST_F(TPartitionerTest, ShortcutPreservesTopologicalLevels) {
    auto& source = AddNode();
    auto& middle = AddNode();
    auto& root = AddNode();
    Connect(source, root);
    Connect(source, middle);
    Connect(middle, root);
    std::string error;
    ASSERT_TRUE(Graph_.Seal(&error)) << error;

    TPartitioner partitioner(&root, 2);
    partitioner.Do();
    ASSERT_NO_FATAL_FAILURE(CheckResult(partitioner, 2));
    EXPECT_EQ(partitioner.Colors().at(&source), partitioner.Colors().at(&root));
    EXPECT_EQ(partitioner.Colors().at(&middle), partitioner.Colors().at(&root));
    EXPECT_TRUE(partitioner.SplitEdges().empty());
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
            edges[src][dst] = &Graph_.AddEdge(
                *sources[src], *consumers[dst], gather, src, 0);
        }
    }
    auto gatherConnection = std::make_unique<TGatherConnection>();
    gatherConnection->Resize(consumers.size(), 1);
    auto& gather = Graph_.AddConnection(std::move(gatherConnection));
    for (size_t lane = 0; lane < consumers.size(); ++lane) {
        Graph_.AddEdge(*consumers[lane], root, gather, lane, 0);
    }
    std::string error;
    ASSERT_TRUE(Graph_.Seal(&error)) << error;

    TPartitioner partitioner(&root, 2);
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
}

TEST_F(TPartitionerTest, IncludesCrossPartitionDependency) {
    auto& left = AddNode();
    auto& right = AddNode();
    auto& root = AddNode();
    Connect(left, root);
    Connect(right, root);
    auto& dependency = Graph_.AddDependencyEdge(left, right);
    std::string error;
    ASSERT_TRUE(Graph_.Seal(&error)) << error;

    TPartitioner partitioner(&root, 2);
    partitioner.Do();
    ASSERT_NO_FATAL_FAILURE(CheckResult(partitioner, 2));
    EXPECT_NE(partitioner.Colors().at(&left), partitioner.Colors().at(&right));
    EXPECT_TRUE(partitioner.SplitEdges().contains(&dependency));
    EXPECT_EQ(partitioner.SplitEdges().size(), 2u);
}

TEST_F(TPartitionerTest, OneToOneSplitRebindsDestinationLane) {
    auto& left = AddNode();
    auto& right = AddNode();
    auto& root = AddNode();
    auto connection = std::make_unique<TOneToOneConnection>(3);
    connection->Resize(6, 6);
    connection->SetRowLayouts({{.Width = 8}});
    auto& original = Graph_.AddConnection(std::move(connection));
    Graph_.AddEdge(left, root, original, 3, 3, 1);
    Graph_.AddEdge(right, root, original, 5, 5, 2);
    std::string error;
    ASSERT_TRUE(Graph_.Seal(&error)) << error;

    TPartitioner partitioner(&root, 2);
    partitioner.Do();
    ASSERT_NO_FATAL_FAILURE(CheckResult(partitioner, 2));
    ASSERT_EQ(partitioner.ConnectionSplits().size(), 1u);
    auto& split = *partitioner.ConnectionSplits().front();
    EXPECT_EQ(split.ReboundPort, EReboundPort::DestinationInput);
    EXPECT_EQ(split.SinkEdge.Connection, &original);
    EXPECT_EQ(split.SinkEdge.DstLane, split.OriginalEdge->DstLane);
    EXPECT_EQ(split.SourceEdge.Connection, split.NewConnection.get());
    EXPECT_EQ(split.SourceEdge.SrcLane, 0u);
    EXPECT_EQ(split.SourceEdge.DstLane, 0u);
    EXPECT_EQ(split.NewConnection->SrcCount(), 1u);
    EXPECT_EQ(split.NewConnection->DstCount(), 1u);
    EXPECT_EQ(dynamic_cast<TOneToOneConnection&>(*split.NewConnection).Capacity(), 3u);
    ASSERT_EQ(split.NewConnection->RowLayouts().size(), 1u);
    EXPECT_EQ(split.NewConnection->RowLayouts()[0].Width, 8);
    ASSERT_NO_FATAL_FAILURE(BindEmptyBatchTransport(split));

    ASSERT_TRUE(original.Push(split.OriginalEdge->SrcLane, EmptyBatch(42)));
    original.Finish(split.OriginalEdge->SrcLane);
    EXPECT_EQ(split.SinkTask->Execute(), ETaskResult::OK);
    EXPECT_EQ(split.SourceTask->Execute(), ETaskResult::OK);
    NQdb::TRowSet rows{};
    ASSERT_EQ(split.NewConnection->Fetch(0, rows), EFetchResult::OK);
    EXPECT_EQ(rows.RowCount, 42);
    NQdb::Release(&rows);
    EXPECT_EQ(split.SourceTask->Execute(), ETaskResult::WAITING);
    EXPECT_EQ(split.NewConnection->Fetch(0, rows), EFetchResult::NO_DATA);
    EXPECT_EQ(split.SinkTask->Execute(), ETaskResult::FINISHED);
    EXPECT_EQ(split.SourceTask->Execute(), ETaskResult::FINISHED);
    EXPECT_EQ(split.NewConnection->Fetch(0, rows), EFetchResult::FINISHED);
}

TEST_F(TPartitionerTest, GatherSplitRebindsSourceAndWaitsForRemoteEof) {
    auto& left = AddNode();
    auto& right = AddNode();
    auto& root = AddNode();
    auto connection = std::make_unique<TGatherConnection>(2);
    connection->Resize(2, 1);
    auto& original = Graph_.AddConnection(std::move(connection));
    Graph_.AddEdge(left, root, original, 0, 0, 1);
    Graph_.AddEdge(right, root, original, 1, 0, 1);
    std::string error;
    ASSERT_TRUE(Graph_.Seal(&error)) << error;

    TPartitioner partitioner(&root, 2);
    partitioner.Do();
    ASSERT_NO_FATAL_FAILURE(CheckResult(partitioner, 2));
    ASSERT_EQ(partitioner.ConnectionSplits().size(), 1u);
    auto& split = *partitioner.ConnectionSplits().front();
    EXPECT_EQ(split.ReboundPort, EReboundPort::SourceOutput);
    EXPECT_EQ(split.SinkEdge.Connection, split.NewConnection.get());
    EXPECT_EQ(split.SinkEdge.SrcLane, 0u);
    EXPECT_EQ(split.SinkEdge.DstLane, 0u);
    EXPECT_EQ(split.SourceEdge.Connection, &original);
    EXPECT_EQ(split.SourceEdge.SrcLane, split.OriginalEdge->SrcLane);
    EXPECT_EQ(split.SourceEdge.DstLane, 0u);
    EXPECT_EQ(original.SrcCount(), 2u);
    EXPECT_EQ(dynamic_cast<TOneToOneConnection&>(*split.NewConnection).Capacity(), 2u);
    ASSERT_NO_FATAL_FAILURE(BindEmptyBatchTransport(split));

    const auto localLane = 1 - split.OriginalEdge->SrcLane;
    ASSERT_TRUE(original.Push(localLane, EmptyBatch(11)));
    original.Finish(localLane);
    NQdb::TRowSet rows{};
    ASSERT_EQ(original.Fetch(0, rows), EFetchResult::OK);
    EXPECT_EQ(rows.RowCount, 11);
    NQdb::Release(&rows);
    EXPECT_EQ(original.Fetch(0, rows), EFetchResult::NO_DATA);

    ASSERT_TRUE(split.NewConnection->Push(0, EmptyBatch(22)));
    split.NewConnection->Finish(0);
    EXPECT_EQ(split.SinkTask->Execute(), ETaskResult::OK);
    EXPECT_EQ(split.SourceTask->Execute(), ETaskResult::OK);
    ASSERT_EQ(original.Fetch(0, rows), EFetchResult::OK);
    EXPECT_EQ(rows.RowCount, 22);
    NQdb::Release(&rows);
    EXPECT_EQ(original.Fetch(0, rows), EFetchResult::NO_DATA);
    EXPECT_EQ(split.SinkTask->Execute(), ETaskResult::FINISHED);
    EXPECT_EQ(split.SourceTask->Execute(), ETaskResult::FINISHED);
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
    Graph_.AddEdge(producer, left, original, 0, 0, 1);
    Graph_.AddEdge(producer, right, original, 0, 1, 1);
    Connect(left, root);
    Connect(right, root);
    std::string error;
    ASSERT_TRUE(Graph_.Seal(&error)) << error;

    TPartitioner partitioner(&root, 2);
    partitioner.Do();
    ASSERT_NO_FATAL_FAILURE(CheckResult(partitioner, 2));
    auto* split = FindSplit(partitioner, original);
    ASSERT_NE(split, nullptr);
    EXPECT_EQ(split->ReboundPort, EReboundPort::DestinationInput);
    EXPECT_EQ(split->SinkEdge.Connection, &original);
    EXPECT_EQ(split->SinkEdge.SrcLane, 0u);
    EXPECT_EQ(split->SinkEdge.DstLane, split->OriginalEdge->DstLane);
    EXPECT_EQ(split->SourceEdge.Connection, split->NewConnection.get());
    EXPECT_EQ(original.DstCount(), 2u);
    ASSERT_NO_FATAL_FAILURE(BindEmptyBatchTransport(*split));

    ASSERT_TRUE(original.Push(0, EmptyBatch(7)));
    original.Finish(0);
    NQdb::TRowSet rows{};
    const auto localLane = 1 - split->OriginalEdge->DstLane;
    ASSERT_EQ(original.Fetch(localLane, rows), EFetchResult::OK);
    EXPECT_EQ(rows.RowCount, 7);
    NQdb::Release(&rows);
    EXPECT_EQ(original.Fetch(localLane, rows), EFetchResult::FINISHED);
    EXPECT_EQ(split->SinkTask->Execute(), ETaskResult::OK);
    EXPECT_EQ(split->SourceTask->Execute(), ETaskResult::OK);
    ASSERT_EQ(split->NewConnection->Fetch(0, rows), EFetchResult::OK);
    EXPECT_EQ(rows.RowCount, 7);
    NQdb::Release(&rows);
    EXPECT_EQ(split->SinkTask->Execute(), ETaskResult::FINISHED);
    EXPECT_EQ(split->SourceTask->Execute(), ETaskResult::FINISHED);
    EXPECT_EQ(split->NewConnection->Fetch(0, rows), EFetchResult::FINISHED);
}

} // namespace
