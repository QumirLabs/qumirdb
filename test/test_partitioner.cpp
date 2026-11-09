#include <qdb/scheduler/connections/local.h>
#include <qdb/scheduler/graph.h>
#include <qdbd/partitioner/partitioner.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
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
        for (const auto& edge : Graph_.Edges()) {
            const bool crosses = colors.at(edge->Src) != colors.at(edge->Dst);
            EXPECT_EQ(splits.contains(edge.get()), crosses)
                << edge->Src->ExecutionSlot << " -> " << edge->Dst->ExecutionSlot;
            expectedSplits += crosses;
        }
        EXPECT_EQ(splits.size(), expectedSplits);
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

} // namespace
