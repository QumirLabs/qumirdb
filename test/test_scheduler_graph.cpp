#include <gtest/gtest.h>

#include <qdb/scheduler/connection.h>
#include <qdb/scheduler/connections/local.h>
#include <qdb/scheduler/graph.h>
#include <qdb/scheduler/impl/single_threaded_scheduler.h>
#include <qdb/scheduler/impl/threaded_scheduler.h>
#include <qdb/scheduler/port.h>
#include <qdb/scheduler/tasks/runtime_adapter.h>

#include <algorithm>
#include <deque>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

using namespace NQdb::NScheduler;

namespace {

class TNoopTask : public ITask {
public:
    ETaskResult Execute() override {
        ++Calls_;
        return ETaskResult::FINISHED;
    }

    int Calls() const {
        return Calls_;
    }

private:
    int Calls_ = 0;
};

class TSourceTask : public ITask {
public:
    ETaskResult Execute() override {
        if (!Produced) {
            Produced = true;
            return ETaskResult::OK;
        }
        Finished = true;
        return ETaskResult::FINISHED;
    }

    bool Produced = false;
    bool Finished = false;
};

class TSinkTask : public ITask {
public:
    explicit TSinkTask(TSourceTask& source)
        : Source_(source)
    {}

    ETaskResult Execute() override {
        if (!Source_.Produced) {
            return ETaskResult::NEED_DATA;
        }
        if (!Consumed_) {
            Consumed_ = true;
            return ETaskResult::OK;
        }
        if (Source_.Finished) {
            Finished = true;
            return ETaskResult::FINISHED;
        }
        return ETaskResult::NEED_DATA;
    }

    bool Consumed() const {
        return Consumed_;
    }

    bool Finished = false;

private:
    TSourceTask& Source_;
    bool Consumed_ = false;
};

class TOrderedProbeTask : public ITask {
public:
    TTaskNode* Prerequisite = nullptr;
    bool RanAfterPrerequisite = false;

    ETaskResult Execute() override {
        if (Prerequisite->State.load() != ETaskState::Finished) {
            return ETaskResult::NEED_DATA;
        }
        RanAfterPrerequisite = true;
        return ETaskResult::FINISHED;
    }
};

class TWaitForBothTask : public ITask {
public:
    TTaskNode* Left = nullptr;
    TTaskNode* Right = nullptr;

    ETaskResult Execute() override {
        return Left->State.load() == ETaskState::Finished
                && Right->State.load() == ETaskState::Finished
            ? ETaskResult::FINISHED : ETaskResult::NEED_DATA;
    }
};

using TGraphs = std::vector<std::vector<TTaskNode*>>;

TGraphs Graphs(const TSealedTaskGraph& sealed) {
    return {sealed.Graphs().begin(), sealed.Graphs().end()};
}

void CheckControlDependency(bool threaded) {
    TTaskGraph graph;
    auto& build = graph.AddOwnedNode(std::make_unique<TNoopTask>());
    auto probeTask = std::make_unique<TOrderedProbeTask>();
    auto* probePtr = probeTask.get();
    auto& probe = graph.AddOwnedNode(std::move(probeTask));
    auto rootTask = std::make_unique<TWaitForBothTask>();
    auto& root = graph.AddOwnedNode(std::move(rootTask));
    probePtr->Prerequisite = &build;
    auto* rootPtr = static_cast<TWaitForBothTask*>(root.Task);
    rootPtr->Left = &build;
    rootPtr->Right = &probe;
    graph.AddOwnedEdge(build, root, std::make_unique<TOneToOneConnection>());
    graph.AddOwnedEdge(probe, root, std::make_unique<TOneToOneConnection>());
    graph.AddDependencyEdge(build, probe);
    const auto sealed = graph.Seal();
    ASSERT_TRUE(sealed);

    std::string error;
    if (threaded) {
        TThreadedScheduler scheduler(sealed, 2);
        ASSERT_TRUE(scheduler.Run(&error)) << error;
    } else {
        TSingleThreadedScheduler scheduler(sealed);
        ASSERT_TRUE(scheduler.Run(&error)) << error;
    }
    EXPECT_TRUE(probePtr->RanAfterPrerequisite);
    EXPECT_EQ(root.State.load(), ETaskState::Finished);
}

TEST(SchedulerGraph, BuildsAdjacencyAndValidates) {
    TTaskGraph graph;
    auto& source = graph.AddOwnedNode(std::make_unique<TNoopTask>());
    auto& sink = graph.AddOwnedNode(std::make_unique<TNoopTask>());
    graph.AddOwnedEdge(source, sink, std::make_unique<TOneToOneConnection>());
    std::string error;
    EXPECT_EQ(graph.Seal(&error).TaskCount(), 2u) << error;
    ASSERT_EQ(graph.Leaves().size(), 1u);
    EXPECT_EQ(graph.Leaves()[0], &source);
    EXPECT_EQ(graph.Graphs(), TGraphs{{&sink}});
    EXPECT_EQ(source.Outbound.size(), 1u);
    EXPECT_EQ(sink.Inbound.size(), 1u);
}

TEST(SchedulerGraph, ExposesConnectionsAndExternals) {
    TNoopTask task;
    TTaskGraph graph;
    auto& source = graph.AddNode(task);
    auto& sink = graph.AddNode(task);
    auto& edge = graph.AddOwnedEdge(source, sink, std::make_unique<TGatherConnection>(3));
    auto& extra = graph.AddConnection(std::make_unique<TBroadcastConnection>(5));
    graph.MarkExternal(sink, 9);
    graph.MarkExternal(source, 3);

    ASSERT_EQ(graph.Connections().size(), 2u);
    EXPECT_EQ(graph.Connections()[0].get(), edge.Connection);
    EXPECT_EQ(graph.Connections()[1].get(), &extra);
    EXPECT_EQ(static_cast<TGatherConnection&>(*edge.Connection).Capacity(), 3u);
    EXPECT_EQ(static_cast<TBroadcastConnection&>(extra).Capacity(), 5u);
    EXPECT_EQ(TOneToOneConnection(2).Capacity(), 2u);

    ASSERT_EQ(graph.Externals().size(), 2u);
    EXPECT_EQ(graph.Externals()[0].Id, 9u);
    EXPECT_EQ(graph.Externals()[0].Node, &sink);
    EXPECT_EQ(graph.Externals()[1].Id, 3u);
    EXPECT_EQ(graph.Externals()[1].Node, &source);
}

TEST(SchedulerGraph, RejectsEmptyGraph) {
    TTaskGraph graph;
    std::string error;
    EXPECT_FALSE(graph.Seal(&error));
    EXPECT_EQ(error, "task graph is empty");
}

TEST(SchedulerGraph, SealFreezesStructure) {
    TNoopTask task;
    TTaskGraph graph;
    auto& source = graph.AddNode(task);
    auto& sink = graph.AddNode(task);
    auto& connection = graph.AddConnection(std::make_unique<TOneToOneConnection>());
    graph.AddEdge(source, sink, connection);
    ASSERT_EQ(graph.Seal().TaskCount(), 2u);

    EXPECT_THROW(graph.AddNode(task), std::logic_error);
    EXPECT_THROW(graph.AddOwnedNode(std::make_unique<TNoopTask>()), std::logic_error);
    EXPECT_THROW(graph.AddConnection(std::make_unique<TOneToOneConnection>()), std::logic_error);
    EXPECT_THROW(graph.AddEdge(source, sink, connection), std::logic_error);
    EXPECT_THROW(graph.AddOwnedEdge(source, sink, {}), std::logic_error);
    EXPECT_THROW(graph.AddDependencyEdge(source, sink), std::logic_error);
    EXPECT_THROW(graph.MarkExternal(source, 1), std::logic_error);
    EXPECT_EQ(graph.Nodes().size(), 2u);
    EXPECT_EQ(graph.Edges().size(), 1u);
}

TEST(SchedulerGraph, ExternalNodeLookup) {
    TNoopTask task;
    TTaskGraph graph;
    auto& source = graph.AddNode(task);
    auto& sink = graph.AddNode(task);
    graph.AddOwnedEdge(source, sink, std::make_unique<TOneToOneConnection>());
    graph.MarkExternal(sink, 9);
    graph.MarkExternal(source, 3);
    std::string error;
    const auto sealed = graph.Seal(&error);
    ASSERT_TRUE(sealed) << error;

    EXPECT_EQ(sealed.ExternalNode(3), &source);
    EXPECT_EQ(sealed.ExternalNode(9), &sink);
    EXPECT_EQ(sealed.ExternalNode(4), nullptr);
}

TEST(SchedulerGraph, RejectsDuplicateExternalIds) {
    TNoopTask task;
    TTaskGraph graph;
    auto& source = graph.AddNode(task);
    auto& sink = graph.AddNode(task);
    graph.AddOwnedEdge(source, sink, std::make_unique<TOneToOneConnection>());
    graph.MarkExternal(source, 5);
    graph.MarkExternal(sink, 5);
    std::string error;
    EXPECT_FALSE(graph.Seal(&error));
    EXPECT_EQ(error, "task graph has duplicate external node id 5");
}

TEST(SchedulerGraph, MarkExternalRejectsForeignNode) {
    TNoopTask task;
    TTaskGraph graph;
    TTaskGraph other;
    graph.AddNode(task);
    auto& foreign = other.AddNode(task);
    EXPECT_THROW(graph.MarkExternal(foreign, 1), std::logic_error);
}

TEST(SchedulerGraph, RepeatedSealKeepsExternalNodes) {
    TNoopTask task;
    TTaskGraph graph;
    auto& source = graph.AddNode(task);
    auto& sink = graph.AddNode(task);
    graph.AddOwnedEdge(source, sink, std::make_unique<TOneToOneConnection>());
    graph.MarkExternal(sink, 9);
    graph.MarkExternal(source, 3);
    ASSERT_TRUE(graph.Seal());

    const auto sealed = graph.Seal();
    ASSERT_TRUE(sealed);
    EXPECT_EQ(sealed.ExternalNode(3), &source);
    EXPECT_EQ(sealed.ExternalNode(9), &sink);
}

TEST(SchedulerGraph, RepeatedSealPreservesTaskState) {
    TNoopTask task;
    TTaskGraph graph;
    auto& root = graph.AddNode(task);
    ASSERT_TRUE(graph.Seal());
    root.State.store(ETaskState::Finished);

    const auto sealed = graph.Seal();
    ASSERT_TRUE(sealed);
    EXPECT_EQ(Graphs(sealed), TGraphs{{&root}});
    EXPECT_EQ(sealed.TaskCount(), 1u);
    EXPECT_EQ(root.State.load(), ETaskState::Finished);
}

TEST(SchedulerGraph, FailedSealDoesNotFreezeStructure) {
    TNoopTask task;
    TTaskGraph graph;
    EXPECT_FALSE(graph.Seal());

    auto& source = graph.AddNode(task);
    auto& sink = graph.AddNode(task);
    graph.AddOwnedEdge(source, sink, std::make_unique<TOneToOneConnection>());
    const auto sealed = graph.Seal();
    ASSERT_TRUE(sealed);
    EXPECT_EQ(Graphs(sealed), TGraphs{{&sink}});
    EXPECT_EQ(sealed.TaskCount(), 2u);
}

TEST(SchedulerGraph, SealPutsEveryRootIntoOneGraph) {
    TTaskGraph graph;
    auto& firstSource = graph.AddOwnedNode(std::make_unique<TNoopTask>());
    auto& secondSource = graph.AddOwnedNode(std::make_unique<TNoopTask>());
    auto& secondSink = graph.AddOwnedNode(std::make_unique<TNoopTask>());
    auto& firstSink = graph.AddOwnedNode(std::make_unique<TNoopTask>());
    auto& lonely = graph.AddOwnedNode(std::make_unique<TNoopTask>());
    graph.AddOwnedEdge(firstSource, firstSink, std::make_unique<TOneToOneConnection>());
    graph.AddOwnedEdge(secondSource, secondSink, std::make_unique<TOneToOneConnection>());
    std::string error;
    const auto sealed = graph.Seal(&error);
    ASSERT_TRUE(sealed) << error;
    EXPECT_EQ(Graphs(sealed), (TGraphs{{&secondSink, &firstSink, &lonely}}));
    // A view of the graph's own lists.
    EXPECT_EQ(sealed.Graphs().data(), graph.Graphs().data());

    std::stringstream out;
    graph.Print(out);
    EXPECT_NE(out.str().find("graph g0 roots: n2 n3 n4\n"), std::string::npos);
}

TEST(SchedulerGraph, NodeWithOnlyADependencyOutputIsARoot) {
    TTaskGraph graph;
    auto& build = graph.AddOwnedNode(std::make_unique<TNoopTask>());
    auto& probe = graph.AddOwnedNode(std::make_unique<TNoopTask>());
    auto& sink = graph.AddOwnedNode(std::make_unique<TNoopTask>());
    graph.AddOwnedEdge(probe, sink, std::make_unique<TOneToOneConnection>());
    graph.AddDependencyEdge(build, probe);
    std::string error;
    const auto sealed = graph.Seal(&error);
    ASSERT_TRUE(sealed) << error;
    EXPECT_EQ(Graphs(sealed), (TGraphs{{&build, &sink}}));
}

// P feeds C1 and gates C2 through a dependency; Q only gates C2, so Q is a root.
TEST(SchedulerGraph, RetireAbandonsADependencyProducerWithItsLastConsumer) {
    TTaskGraph graph;
    auto& p = graph.AddOwnedNode(std::make_unique<TNoopTask>());
    auto& q = graph.AddOwnedNode(std::make_unique<TNoopTask>());
    auto& c1 = graph.AddOwnedNode(std::make_unique<TNoopTask>());
    auto& c2 = graph.AddOwnedNode(std::make_unique<TNoopTask>());
    auto& r1 = graph.AddOwnedNode(std::make_unique<TNoopTask>());
    auto& r2 = graph.AddOwnedNode(std::make_unique<TNoopTask>());
    graph.AddOwnedEdge(p, c1, std::make_unique<TOneToOneConnection>());
    graph.AddOwnedEdge(c1, r1, std::make_unique<TOneToOneConnection>());
    graph.AddOwnedEdge(c2, r2, std::make_unique<TOneToOneConnection>());
    graph.AddDependencyEdge(p, c2);
    graph.AddDependencyEdge(q, c2);
    std::string error;
    ASSERT_TRUE(graph.Seal(&error)) << error;

    Retire(r1);
    EXPECT_TRUE(c1.Retired.load());
    // C2 still waits for P's publication.
    EXPECT_FALSE(p.Retired.load());
    Retire(r2);
    EXPECT_TRUE(c2.Retired.load());
    EXPECT_TRUE(p.Retired.load());
    EXPECT_FALSE(q.Retired.load());
}

// P has two edges to C and one to D: it lives until both consumers retire.
TEST(SchedulerGraph, RetireCountsEveryEdgeToOneConsumer) {
    TTaskGraph graph;
    auto& p = graph.AddOwnedNode(std::make_unique<TNoopTask>());
    auto& c = graph.AddOwnedNode(std::make_unique<TNoopTask>());
    auto& d = graph.AddOwnedNode(std::make_unique<TNoopTask>());
    auto& r1 = graph.AddOwnedNode(std::make_unique<TNoopTask>());
    auto& r2 = graph.AddOwnedNode(std::make_unique<TNoopTask>());
    graph.AddOwnedEdge(p, c, std::make_unique<TOneToOneConnection>());
    graph.AddOwnedEdge(p, c, std::make_unique<TOneToOneConnection>());
    graph.AddOwnedEdge(p, d, std::make_unique<TOneToOneConnection>());
    graph.AddOwnedEdge(c, r1, std::make_unique<TOneToOneConnection>());
    graph.AddOwnedEdge(d, r2, std::make_unique<TOneToOneConnection>());
    std::string error;
    ASSERT_TRUE(graph.Seal(&error)) << error;
    EXPECT_EQ(p.LiveConsumers.load(), 3u);

    Retire(r1);
    EXPECT_TRUE(c.Retired.load());
    EXPECT_FALSE(p.Retired.load());
    EXPECT_EQ(p.LiveConsumers.load(), 1u);
    Retire(r2);
    EXPECT_TRUE(d.Retired.load());
    EXPECT_TRUE(p.Retired.load());
    // A node retired twice, e.g. one that finishes after it was abandoned, releases once.
    Retire(c);
    EXPECT_EQ(p.LiveConsumers.load(), 0u);
}

TEST(SchedulerGraph, RejectsCycleBesideAValidSubgraph) {
    TTaskGraph graph;
    auto& source = graph.AddOwnedNode(std::make_unique<TNoopTask>());
    auto& sink = graph.AddOwnedNode(std::make_unique<TNoopTask>());
    auto& left = graph.AddOwnedNode(std::make_unique<TNoopTask>());
    auto& right = graph.AddOwnedNode(std::make_unique<TNoopTask>());
    graph.AddOwnedEdge(source, sink, std::make_unique<TOneToOneConnection>());
    graph.AddOwnedEdge(left, right, std::make_unique<TOneToOneConnection>());
    graph.AddOwnedEdge(right, left, std::make_unique<TOneToOneConnection>());
    std::string error;
    EXPECT_FALSE(graph.Seal(&error));
    EXPECT_NE(error.find("scheduling cycle"), std::string::npos);
}

// Holds plain ports for Apply to rebind; finishes at once when run.
class TPortsTask : public ITask {
public:
    ETaskResult Execute() override {
        return ETaskResult::FINISHED;
    }

    bool RebindPort(const TPortRebind& rebind) override {
        return std::ranges::any_of(Inputs, [&](auto& port) { return rebind.Apply(port); })
            || std::ranges::any_of(Outputs, [&](auto& port) { return rebind.Apply(port); });
    }

    std::vector<TInputPort> Inputs;
    std::vector<TOutputPort> Outputs;
};

struct TPortsNode {
    TTaskNode& Node;
    TPortsTask& Task;
};

TPortsNode AddPortsNode(
    TTaskGraph& graph,
    std::vector<TInputPort> inputs,
    std::vector<TOutputPort> outputs)
{
    auto task = std::make_unique<TPortsTask>();
    auto& ref = *task;
    ref.Inputs = std::move(inputs);
    ref.Outputs = std::move(outputs);
    return {graph.AddOwnedNode(std::move(task)), ref};
}

using TRemoteSinkTask = NQdb::NScheduler::TSinkTask;
using TRemoteSourceTask = NQdb::NScheduler::TSourceTask;

TEdgeSplit Split(TTaskEdge& edge, EReboundPort port, int sinkColor, int sourceColor) {
    return {
        .Edge = &edge,
        .ReboundPort = port,
        .SinkColor = sinkColor,
        .SourceColor = sourceColor,
        .NewConnection = std::make_unique<TOneToOneConnection>(),
        .SinkTask = std::make_unique<TRemoteSinkTask>(nullptr, nullptr, TInputPort{}),
        .SourceTask = std::make_unique<TRemoteSourceTask>(nullptr, nullptr, TOutputPort{}),
    };
}

template <class... TSplits>
TSplitPlan Plan(TSplits... splits) {
    TSplitPlan plan;
    (plan.Splits.push_back(std::move(splits)), ...);
    return plan;
}

std::string ApplyError(TTaskGraph& graph, TSplitPlan plan) {
    try {
        graph.Apply(std::move(plan));
    } catch (const std::logic_error& e) {
        return e.what();
    }
    return {};
}

using TEdgeShape = std::tuple<const TTaskNode*, const TTaskNode*, const IConnection*, size_t, size_t, size_t>;

// {Src, Dst, Connection, SrcLane, DstLane, ExecInput}
TEdgeShape Shape(const TTaskEdge& edge) {
    return {edge.Src, edge.Dst, edge.Connection, edge.SrcLane, edge.DstLane, edge.ExecInput};
}

NQdb::TRowSet EmptyBatch(int64_t rows) {
    return {.RowCount = rows, .RefCount = 1};
}

// Carries the row counts of empty batches from a remote sink to its remote source.
void BindTransport(TTaskNode& sink, TTaskNode& source) {
    struct TTransport {
        std::deque<int64_t> RowCounts;
        bool Finished = false;
    };
    auto transport = std::make_shared<TTransport>();
    auto* sinkTask = dynamic_cast<TRemoteSinkTask*>(sink.Task);
    auto* sourceTask = dynamic_cast<TRemoteSourceTask*>(source.Task);
    ASSERT_NE(sinkTask, nullptr);
    ASSERT_NE(sourceTask, nullptr);
    sinkTask->BindCode(std::make_shared<TSinkCode>(
        [](void* state, const NQdb::TRowSet& rows) {
            static_cast<TTransport*>(state)->RowCounts.push_back(rows.RowCount);
            return ETaskResult::OK;
        },
        [](void* state) {
            static_cast<TTransport*>(state)->Finished = true;
            return ETaskResult::OK;
        }), transport);
    sourceTask->BindCode(std::make_shared<TSourceCode>(
        [](void* state, NQdb::TRowSet& rows) {
            auto& transport = *static_cast<TTransport*>(state);
            if (transport.RowCounts.empty()) {
                return transport.Finished
                    ? ETaskResult::FINISHED
                    : ETaskResult::WAITING;
            }
            rows = EmptyBatch(transport.RowCounts.front());
            transport.RowCounts.pop_front();
            return ETaskResult::OK;
        }), transport);
}

// The row count of the next batch on `port`; fails unless one is there.
int64_t FetchRows(const TInputPort& port) {
    NQdb::TRowSet rows{};
    EXPECT_EQ(port.Fetch(rows), EFetchResult::OK);
    const auto count = rows.RowCount;
    if (rows.RefCount) {
        NQdb::Release(&rows);
    }
    return count;
}

EFetchResult FetchState(const TInputPort& port) {
    NQdb::TRowSet rows{};
    const auto result = port.Fetch(rows);
    EXPECT_NE(result, EFetchResult::OK);
    return result;
}

TEST(SchedulerGraph, ApplyMovesTheDestinationOfAOneToOneCut) {
    TTaskGraph graph;
    auto connection = std::make_unique<TOneToOneConnection>();
    connection->Resize(6, 6);
    auto& original = graph.AddConnection(std::move(connection));
    auto left = AddPortsNode(graph, {}, {{.Connection = &original, .Lane = 3}});
    auto right = AddPortsNode(graph, {}, {{.Connection = &original, .Lane = 5}});
    auto root = AddPortsNode(
        graph,
        {{.Connection = &original, .Lane = 3}, {.Connection = &original, .Lane = 5}},
        {});
    graph.AddEdge(left.Node, root.Node, original, 3, 3, 1);
    auto& cut = graph.AddEdge(right.Node, root.Node, original, 5, 5, 2);
    std::string error;
    ASSERT_TRUE(graph.Seal(&error)) << error;

    const auto sealed = graph.Apply(Plan(Split(cut, EReboundPort::DestinationInput, 1, 0)));
    ASSERT_EQ(graph.Nodes().size(), 5u);
    auto& sink = *graph.Nodes()[3];
    auto& source = *graph.Nodes()[4];
    EXPECT_EQ(sink.DebugKind, "remote-sink");
    EXPECT_EQ(source.DebugKind, "remote-source");
    EXPECT_EQ(Graphs(sealed), (TGraphs{{&root.Node}, {&sink}}));
    const auto* added = graph.Connections().back().get();
    ASSERT_EQ(graph.Edges().size(), 3u);
    EXPECT_EQ(Shape(*graph.Edges()[0]), (TEdgeShape{&left.Node, &root.Node, &original, 3, 3, 1}));
    EXPECT_EQ(Shape(*graph.Edges()[1]), (TEdgeShape{&right.Node, &sink, &original, 5, 5, 0}));
    EXPECT_EQ(Shape(*graph.Edges()[2]), (TEdgeShape{&source, &root.Node, added, 0, 0, 2}));
    EXPECT_EQ(root.Task.Inputs[0].Connection, &original);
    EXPECT_EQ(root.Task.Inputs[1].Connection, added);
    EXPECT_EQ(root.Task.Inputs[1].Lane, 0u);

    // Rows the right side writes to lane 5 reach the root through the new connection.
    ASSERT_NO_FATAL_FAILURE(BindTransport(sink, source));
    ASSERT_TRUE(right.Task.Outputs[0].Push(EmptyBatch(42)));
    right.Task.Outputs[0].Finish();
    EXPECT_EQ(sink.Task->Execute(), ETaskResult::OK);
    EXPECT_EQ(source.Task->Execute(), ETaskResult::OK);
    EXPECT_EQ(FetchRows(root.Task.Inputs[1]), 42);
    EXPECT_EQ(sink.Task->Execute(), ETaskResult::FINISHED);
    EXPECT_EQ(source.Task->Execute(), ETaskResult::FINISHED);
    EXPECT_EQ(FetchState(root.Task.Inputs[1]), EFetchResult::FINISHED);
}

TEST(SchedulerGraph, ApplyMovesTheSourceOfAGatherCut) {
    TTaskGraph graph;
    auto connection = std::make_unique<TGatherConnection>();
    connection->Resize(2, 1);
    auto& original = graph.AddConnection(std::move(connection));
    auto left = AddPortsNode(graph, {}, {{.Connection = &original, .Lane = 0}});
    auto right = AddPortsNode(graph, {}, {{.Connection = &original, .Lane = 1}});
    auto root = AddPortsNode(graph, {{.Connection = &original}}, {});
    graph.AddEdge(left.Node, root.Node, original, 0, 0, 1);
    auto& cut = graph.AddEdge(right.Node, root.Node, original, 1, 0, 1);
    std::string error;
    ASSERT_TRUE(graph.Seal(&error)) << error;

    const auto sealed = graph.Apply(Plan(Split(cut, EReboundPort::SourceOutput, 1, 0)));
    ASSERT_EQ(graph.Nodes().size(), 5u);
    auto& sink = *graph.Nodes()[3];
    auto& source = *graph.Nodes()[4];
    EXPECT_EQ(Graphs(sealed), (TGraphs{{&root.Node}, {&sink}}));
    const auto* added = graph.Connections().back().get();
    ASSERT_EQ(graph.Edges().size(), 3u);
    EXPECT_EQ(Shape(*graph.Edges()[1]), (TEdgeShape{&right.Node, &sink, added, 0, 0, 0}));
    EXPECT_EQ(Shape(*graph.Edges()[2]), (TEdgeShape{&source, &root.Node, &original, 1, 0, 1}));
    EXPECT_EQ(right.Task.Outputs[0].Connection, added);
    EXPECT_EQ(right.Task.Outputs[0].Lane, 0u);
    EXPECT_EQ(root.Task.Inputs[0].Connection, &original);

    // The gather still waits for its remote lane 1 after the local lane ends.
    ASSERT_NO_FATAL_FAILURE(BindTransport(sink, source));
    ASSERT_TRUE(left.Task.Outputs[0].Push(EmptyBatch(11)));
    left.Task.Outputs[0].Finish();
    EXPECT_EQ(FetchRows(root.Task.Inputs[0]), 11);
    EXPECT_EQ(FetchState(root.Task.Inputs[0]), EFetchResult::NO_DATA);
    ASSERT_TRUE(right.Task.Outputs[0].Push(EmptyBatch(22)));
    right.Task.Outputs[0].Finish();
    EXPECT_EQ(sink.Task->Execute(), ETaskResult::OK);
    EXPECT_EQ(source.Task->Execute(), ETaskResult::OK);
    EXPECT_EQ(FetchRows(root.Task.Inputs[0]), 22);
    EXPECT_EQ(FetchState(root.Task.Inputs[0]), EFetchResult::NO_DATA);
    EXPECT_EQ(sink.Task->Execute(), ETaskResult::FINISHED);
    EXPECT_EQ(source.Task->Execute(), ETaskResult::FINISHED);
    EXPECT_EQ(FetchState(root.Task.Inputs[0]), EFetchResult::FINISHED);
}

TEST(SchedulerGraph, ApplyMovesTheDestinationOfABroadcastCut) {
    TTaskGraph graph;
    auto connection = std::make_unique<TBroadcastConnection>();
    connection->Resize(1, 2);
    auto& original = graph.AddConnection(std::move(connection));
    auto producer = AddPortsNode(graph, {}, {{.Connection = &original}});
    auto left = AddPortsNode(graph, {{.Connection = &original, .Lane = 0}}, {});
    auto right = AddPortsNode(graph, {{.Connection = &original, .Lane = 1}}, {});
    graph.AddEdge(producer.Node, left.Node, original, 0, 0, 1);
    auto& cut = graph.AddEdge(producer.Node, right.Node, original, 0, 1, 1);
    std::string error;
    ASSERT_TRUE(graph.Seal(&error)) << error;

    const auto sealed = graph.Apply(Plan(Split(cut, EReboundPort::DestinationInput, 0, 1)));
    ASSERT_EQ(graph.Nodes().size(), 5u);
    auto& sink = *graph.Nodes()[3];
    auto& source = *graph.Nodes()[4];
    EXPECT_EQ(Graphs(sealed), (TGraphs{{&left.Node, &sink}, {&right.Node}}));
    const auto* added = graph.Connections().back().get();
    ASSERT_EQ(graph.Edges().size(), 3u);
    EXPECT_EQ(Shape(*graph.Edges()[1]), (TEdgeShape{&producer.Node, &sink, &original, 0, 1, 0}));
    EXPECT_EQ(Shape(*graph.Edges()[2]), (TEdgeShape{&source, &right.Node, added, 0, 0, 1}));
    EXPECT_EQ(right.Task.Inputs[0].Connection, added);
    EXPECT_EQ(left.Task.Inputs[0].Connection, &original);

    // Both the local lane 0 and the remote lane 1 get the copy.
    ASSERT_NO_FATAL_FAILURE(BindTransport(sink, source));
    ASSERT_TRUE(producer.Task.Outputs[0].Push(EmptyBatch(7)));
    producer.Task.Outputs[0].Finish();
    EXPECT_EQ(FetchRows(left.Task.Inputs[0]), 7);
    EXPECT_EQ(FetchState(left.Task.Inputs[0]), EFetchResult::FINISHED);
    EXPECT_EQ(sink.Task->Execute(), ETaskResult::OK);
    EXPECT_EQ(source.Task->Execute(), ETaskResult::OK);
    EXPECT_EQ(FetchRows(right.Task.Inputs[0]), 7);
    EXPECT_EQ(sink.Task->Execute(), ETaskResult::FINISHED);
    EXPECT_EQ(source.Task->Execute(), ETaskResult::FINISHED);
    EXPECT_EQ(FetchState(right.Task.Inputs[0]), EFetchResult::FINISHED);
}

TEST(SchedulerGraph, ApplyGroupsPiecesOfOneColorIntoOneGraph) {
    TTaskGraph graph;
    auto& first = graph.AddConnection(std::make_unique<TOneToOneConnection>());
    auto& second = graph.AddConnection(std::make_unique<TOneToOneConnection>());
    auto source = AddPortsNode(graph, {}, {{.Connection = &first}});
    auto middle = AddPortsNode(graph, {{.Connection = &first}}, {{.Connection = &second}});
    auto root = AddPortsNode(graph, {{.Connection = &second}}, {});
    auto& in = graph.AddEdge(source.Node, middle.Node, first);
    auto& out = graph.AddEdge(middle.Node, root.Node, second);
    auto& gate = graph.AddDependencyEdge(source.Node, root.Node);
    std::string error;
    ASSERT_TRUE(graph.Seal(&error)) << error;
    EXPECT_EQ(Graphs(graph.Apply({})), (TGraphs{{&root.Node}}));

    // Only the middle moves to color 1; color 0 keeps two pieces joined by the gate.
    const auto sealed = graph.Apply(Plan(
        Split(in, EReboundPort::DestinationInput, 0, 1),
        Split(out, EReboundPort::DestinationInput, 1, 0)));
    ASSERT_EQ(graph.Nodes().size(), 7u);
    EXPECT_EQ(Graphs(sealed), (TGraphs{{&root.Node, graph.Nodes()[3].get()}, {graph.Nodes()[5].get()}}));
    ASSERT_EQ(graph.Edges().size(), 5u);
    EXPECT_EQ(graph.Edges().back().get(), &gate);
    EXPECT_EQ(middle.Task.Inputs[0].Connection, graph.Connections()[2].get());
    EXPECT_EQ(root.Task.Inputs[0].Connection, graph.Connections()[3].get());
}

// Nodes reachable upstream from the roots of one fragment.
std::set<const TTaskNode*> Fragment(const std::vector<TTaskNode*>& roots) {
    std::set<const TTaskNode*> seen;
    std::vector<const TTaskNode*> stack(roots.begin(), roots.end());
    while (!stack.empty()) {
        const auto* node = stack.back();
        stack.pop_back();
        if (seen.insert(node).second) {
            for (const auto* edge : node->Inbound) {
                stack.push_back(edge->Src);
            }
        }
    }
    return seen;
}

// source -> middle -> root with both edges cut; the plan lists the later edge first.
struct TTwoCuts {
    TTaskGraph Graph;
    TTaskNode* Source = nullptr;
    TTaskNode* Root = nullptr;
    TTaskEdge* In = nullptr;
    TTaskEdge* Out = nullptr;
};

void BuildTwoCuts(
    TTwoCuts& g,
    std::optional<uint64_t> rootId,
    std::optional<uint64_t> sourceId = std::nullopt)
{
    auto& first = g.Graph.AddConnection(std::make_unique<TOneToOneConnection>());
    auto& second = g.Graph.AddConnection(std::make_unique<TOneToOneConnection>());
    auto source = AddPortsNode(g.Graph, {}, {{.Connection = &first}});
    auto middle = AddPortsNode(g.Graph, {{.Connection = &first}}, {{.Connection = &second}});
    auto root = AddPortsNode(g.Graph, {{.Connection = &second}}, {});
    g.Source = &source.Node;
    g.Root = &root.Node;
    g.In = &g.Graph.AddEdge(source.Node, middle.Node, first);
    g.Out = &g.Graph.AddEdge(middle.Node, root.Node, second);
    if (rootId) {
        g.Graph.MarkExternal(root.Node, *rootId);
    }
    if (sourceId) {
        g.Graph.MarkExternal(source.Node, *sourceId);
    }
    std::string error;
    ASSERT_TRUE(g.Graph.Seal(&error)) << error;
}

TEST(SchedulerGraph, ApplyRecordsCutsInPlanOrderWithExternalIds) {
    TTwoCuts g;
    ASSERT_NO_FATAL_FAILURE(BuildTwoCuts(g, std::nullopt));
    EXPECT_TRUE(g.Graph.Cuts().empty());
    EXPECT_TRUE(g.Graph.Apply({}).Cuts().empty());

    const auto sealed = g.Graph.Apply(Plan(
        Split(*g.Out, EReboundPort::DestinationInput, 1, 0),
        Split(*g.In, EReboundPort::DestinationInput, 0, 1)));
    ASSERT_EQ(sealed.Cuts().size(), 2u);
    ASSERT_EQ(g.Graph.Cuts().size(), 2u);
    // Nodes 3..6 are created in edge order: In's pair first, then Out's.
    const auto& nodes = g.Graph.Nodes();
    const auto& out = sealed.Cuts()[0];
    const auto& in = sealed.Cuts()[1];
    EXPECT_EQ(out.Sink, nodes[5].get());
    EXPECT_EQ(out.Source, nodes[6].get());
    EXPECT_EQ(out.SinkColor, 1);
    EXPECT_EQ(out.SourceColor, 0);
    EXPECT_EQ(in.Sink, nodes[3].get());
    EXPECT_EQ(in.Source, nodes[4].get());
    EXPECT_EQ(in.SinkColor, 0);
    EXPECT_EQ(in.SourceColor, 1);

    // No external node existed: base 0, pairs by plan index.
    EXPECT_EQ(sealed.ExternalNode(0), out.Sink);
    EXPECT_EQ(sealed.ExternalNode(1), out.Source);
    EXPECT_EQ(sealed.ExternalNode(2), in.Sink);
    EXPECT_EQ(sealed.ExternalNode(3), in.Source);
    EXPECT_EQ(sealed.ExternalNode(4), nullptr);
    EXPECT_EQ(g.Graph.Externals().size(), 4u);

    // Each placeholder lives in the fragment of its own color.
    const auto graphs = sealed.Graphs();
    ASSERT_EQ(graphs.size(), 2u);
    for (const auto& cut : sealed.Cuts()) {
        EXPECT_TRUE(Fragment(graphs[cut.SinkColor]).contains(cut.Sink));
        EXPECT_TRUE(Fragment(graphs[cut.SourceColor]).contains(cut.Source));
    }
}

TEST(SchedulerGraph, ApplyNumbersCutExternalsPastExistingOnes) {
    TTwoCuts g;
    // Out of order on purpose: the maximum (10) is not the last one marked.
    ASSERT_NO_FATAL_FAILURE(BuildTwoCuts(g, 10, 3));
    const auto sealed = g.Graph.Apply(Plan(Split(*g.In, EReboundPort::DestinationInput, 0, 1)));
    ASSERT_EQ(sealed.Cuts().size(), 1u);
    EXPECT_EQ(sealed.ExternalNode(10), g.Root);
    EXPECT_EQ(sealed.ExternalNode(3), g.Source);
    EXPECT_EQ(sealed.ExternalNode(11), sealed.Cuts()[0].Sink);
    EXPECT_EQ(sealed.ExternalNode(12), sealed.Cuts()[0].Source);
    EXPECT_EQ(sealed.ExternalNode(7), nullptr);
    EXPECT_EQ(sealed.ExternalNode(13), nullptr);
    EXPECT_EQ(g.Graph.Externals().size(), 4u);
}

TEST(SchedulerGraph, ApplyRejectsADependencyAcrossColors) {
    TTaskGraph graph;
    auto& first = graph.AddConnection(std::make_unique<TOneToOneConnection>());
    auto& second = graph.AddConnection(std::make_unique<TOneToOneConnection>());
    auto build = AddPortsNode(graph, {}, {{.Connection = &first}});
    auto probe = AddPortsNode(graph, {}, {{.Connection = &second}});
    auto root = AddPortsNode(graph, {{.Connection = &first}, {.Connection = &second}}, {});
    auto& cut = graph.AddEdge(build.Node, root.Node, first);
    graph.AddEdge(probe.Node, root.Node, second);
    graph.AddDependencyEdge(build.Node, probe.Node);
    std::string error;
    ASSERT_TRUE(graph.Seal(&error)) << error;
    EXPECT_EQ(
        ApplyError(graph, Plan(Split(cut, EReboundPort::DestinationInput, 1, 0))),
        "split plan: dependency edge n0 -> n1 crosses colors 1 and 0");
}

TDependencySplit DependencySplit(std::vector<TTaskEdge*> edges, int sinkColor, int sourceColor) {
    return {
        .Edges = std::move(edges),
        .SinkColor = sinkColor,
        .SourceColor = sourceColor,
        .SinkTask = std::make_unique<TRuntimeFilterSinkTask>(),
        .SourceTask = std::make_unique<TRuntimeFilterSourceTask>(),
    };
}

// Two build lanes in color 0 gate a probe in color 0 and two in color 1, whose
// root sends to color 0.
struct TFilterGraph {
    TTaskGraph Graph;
    std::vector<TTaskNode*> Builds;
    std::vector<TTaskNode*> Probes;
    TTaskNode* Root = nullptr;
    TTaskNode* RemoteRoot = nullptr;
    TTaskEdge* Out = nullptr;
    std::vector<TTaskEdge*> Remote;
};

void BuildFilterGraph(TFilterGraph& g) {
    auto connection = [&] {
        return &g.Graph.AddConnection(std::make_unique<TOneToOneConnection>());
    };
    std::vector<IConnection*> local{connection(), connection(), connection()};
    std::vector<IConnection*> remote{connection(), connection()};
    auto* out = connection();
    for (int i = 0; i < 2; ++i) {
        g.Builds.push_back(&AddPortsNode(g.Graph, {}, {{.Connection = local[i]}}).Node);
    }
    g.Probes.push_back(&AddPortsNode(g.Graph, {}, {{.Connection = local[2]}}).Node);
    for (auto* connection : remote) {
        g.Probes.push_back(&AddPortsNode(g.Graph, {}, {{.Connection = connection}}).Node);
    }
    g.RemoteRoot = &AddPortsNode(g.Graph,
        {{.Connection = remote[0]}, {.Connection = remote[1]}}, {{.Connection = out}}).Node;
    g.Root = &AddPortsNode(g.Graph,
        {{.Connection = local[0]}, {.Connection = local[1]}, {.Connection = local[2]}, {.Connection = out}}, {}).Node;
    for (int i = 0; i < 3; ++i) {
        g.Graph.AddEdge(i < 2 ? *g.Builds[i] : *g.Probes[0], *g.Root, *local[i]);
    }
    for (int i = 0; i < 2; ++i) {
        g.Graph.AddEdge(*g.Probes[i + 1], *g.RemoteRoot, *remote[i]);
    }
    g.Out = &g.Graph.AddEdge(*g.RemoteRoot, *g.Root, *out);
    for (auto* build : g.Builds) {
        for (auto* probe : g.Probes) {
            auto& edge = g.Graph.AddDependencyEdge(*build, *probe);
            if (probe != g.Probes[0]) {
                g.Remote.push_back(&edge);
            }
        }
    }
    std::string error;
    ASSERT_TRUE(g.Graph.Seal(&error)) << error;
}

// The sources, or else the destinations, of the dependency edges among `edges`.
std::set<const TTaskNode*> Gates(const std::vector<TTaskEdge*>& edges, bool sources) {
    std::set<const TTaskNode*> nodes;
    for (const auto* edge : edges) {
        if (edge->Kind == ETaskEdgeKind::Dependency) {
            nodes.insert(sources ? edge->Src : edge->Dst);
        }
    }
    return nodes;
}

TEST(SchedulerGraph, ApplyCutsTheDependencyEdgesOfOneFilter) {
    TFilterGraph g;
    ASSERT_NO_FATAL_FAILURE(BuildFilterGraph(g));
    TSplitPlan plan = Plan(Split(*g.Out, EReboundPort::DestinationInput, 1, 0));
    plan.DependencySplits.push_back(DependencySplit(g.Remote, 0, 1));
    const auto sealed = g.Graph.Apply(std::move(plan));

    // The rowset cut comes first, though its nodes are created last.
    ASSERT_EQ(sealed.Cuts().size(), 2u);
    const auto& rows = sealed.Cuts()[0];
    const auto& filter = sealed.Cuts()[1];
    EXPECT_EQ(rows.Kind, ETaskEdgeKind::Rowset);
    EXPECT_EQ(filter.Kind, ETaskEdgeKind::Dependency);
    EXPECT_EQ(filter.SinkColor, 0);
    EXPECT_EQ(filter.SourceColor, 1);
    EXPECT_EQ(filter.Sink->DebugKind, "runtime-filter-sink");
    EXPECT_EQ(filter.Source->DebugKind, "runtime-filter-source");
    EXPECT_TRUE(dynamic_cast<TRuntimeFilterSinkTask*>(filter.Sink->Task));
    EXPECT_TRUE(dynamic_cast<TRuntimeFilterSourceTask*>(filter.Source->Task));
    EXPECT_EQ(sealed.ExternalNode(2), filter.Sink);
    EXPECT_EQ(sealed.ExternalNode(3), filter.Source);

    // Each build lane gates the sink once, the source gates each remote probe once.
    const std::set<const TTaskNode*> builds(g.Builds.begin(), g.Builds.end());
    EXPECT_EQ(filter.Sink->Inbound.size(), 2u);
    EXPECT_EQ(Gates(filter.Sink->Inbound, true), builds);
    EXPECT_TRUE(filter.Sink->Outbound.empty());
    EXPECT_TRUE(filter.Source->Inbound.empty());
    EXPECT_EQ(filter.Source->Outbound.size(), 2u);
    EXPECT_EQ(Gates(filter.Source->Outbound, false), (std::set<const TTaskNode*>{g.Probes[1], g.Probes[2]}));
    EXPECT_EQ(Gates(g.Probes[0]->Inbound, true), builds);
    for (auto* probe : {g.Probes[1], g.Probes[2]}) {
        EXPECT_EQ(Gates(probe->Inbound, true), (std::set<const TTaskNode*>{filter.Source}));
    }

    // Both placeholders are roots of their own colors.
    EXPECT_EQ(Graphs(sealed), (TGraphs{{g.Root, filter.Sink}, {rows.Sink, filter.Source}}));
}

TEST(SchedulerGraph, ApplyRejectsInvalidDependencySplits) {
    TFilterGraph g;
    ASSERT_NO_FATAL_FAILURE(BuildFilterGraph(g));
    auto plan = [&](TDependencySplit split) {
        TSplitPlan plan = Plan(Split(*g.Out, EReboundPort::DestinationInput, 1, 0));
        plan.DependencySplits.push_back(std::move(split));
        return plan;
    };
    auto incomplete = DependencySplit(g.Remote, 0, 1);
    incomplete.SourceTask.reset();
    EXPECT_EQ(ApplyError(g.Graph, plan(std::move(incomplete))), "split plan has an incomplete dependency split");
    EXPECT_EQ(ApplyError(g.Graph, plan(DependencySplit({}, 0, 1))), "split plan has an incomplete dependency split");
    EXPECT_EQ(ApplyError(g.Graph, plan(DependencySplit({g.Out}, 1, 0))),
        "split plan cuts a rowset edge as a dependency edge");
    auto twice = plan(DependencySplit(g.Remote, 0, 1));
    twice.DependencySplits.push_back(DependencySplit({g.Remote[0]}, 0, 1));
    EXPECT_EQ(ApplyError(g.Graph, std::move(twice)), "split plan cuts one edge twice");
    TTaskEdge foreign = *g.Remote[0];
    EXPECT_EQ(ApplyError(g.Graph, plan(DependencySplit({&foreign}, 0, 1))),
        "split plan cuts an edge of another task graph");
}

// A dependency split that names the wrong colors leaves a gate between colors.
TEST(SchedulerGraph, ApplyRejectsADependencySplitOfTheWrongColors) {
    TFilterGraph g;
    ASSERT_NO_FATAL_FAILURE(BuildFilterGraph(g));
    TSplitPlan plan = Plan(Split(*g.Out, EReboundPort::DestinationInput, 1, 0));
    plan.DependencySplits.push_back(DependencySplit(g.Remote, 1, 0));
    const auto error = ApplyError(g.Graph, std::move(plan));
    EXPECT_NE(error.find("crosses colors"), std::string::npos) << error;
}

TEST(SchedulerGraph, ApplyRejectsATaskWithoutTheCutPort) {
    TTaskGraph graph;
    auto& connection = graph.AddConnection(std::make_unique<TOneToOneConnection>());
    auto source = AddPortsNode(graph, {}, {{.Connection = &connection}});
    auto& sink = graph.AddOwnedNode(std::make_unique<TNoopTask>());
    auto& cut = graph.AddEdge(source.Node, sink, connection);
    std::string error;
    ASSERT_TRUE(graph.Seal(&error)) << error;
    EXPECT_EQ(
        ApplyError(graph, Plan(Split(cut, EReboundPort::DestinationInput, 0, 1))),
        "split plan: task n1 has no input port to rebind");
}

TEST(SchedulerGraph, ApplyRejectsInvalidPlans) {
    TTaskGraph graph;
    auto& connection = graph.AddConnection(std::make_unique<TOneToOneConnection>());
    auto source = AddPortsNode(graph, {}, {{.Connection = &connection}});
    auto sink = AddPortsNode(graph, {{.Connection = &connection}}, {});
    auto& edge = graph.AddEdge(source.Node, sink.Node, connection);
    auto& gate = graph.AddDependencyEdge(source.Node, sink.Node);
    EXPECT_EQ(
        ApplyError(graph, Plan(Split(edge, EReboundPort::DestinationInput, 0, 1))),
        "split plan applies only to a sealed task graph");
    std::string error;
    ASSERT_TRUE(graph.Seal(&error)) << error;
    EXPECT_EQ(
        ApplyError(graph, Plan(Split(gate, EReboundPort::DestinationInput, 0, 1))),
        "split plan cuts a dependency edge as a rowset edge");
    EXPECT_EQ(
        ApplyError(graph, Plan(
            Split(edge, EReboundPort::DestinationInput, 0, 1),
            Split(edge, EReboundPort::DestinationInput, 0, 1))),
        "split plan cuts one edge twice");
    TTaskEdge foreign = edge;
    EXPECT_EQ(
        ApplyError(graph, Plan(Split(foreign, EReboundPort::DestinationInput, 0, 1))),
        "split plan cuts an edge of another task graph");
    EXPECT_EQ(sink.Task.Inputs[0].Connection, &connection);
}

TEST(SchedulerGraph, ApplyRejectsTwoColorsInOnePiece) {
    TTaskGraph graph;
    auto& first = graph.AddConnection(std::make_unique<TOneToOneConnection>());
    auto& second = graph.AddConnection(std::make_unique<TOneToOneConnection>());
    auto source = AddPortsNode(graph, {}, {{.Connection = &first}, {.Connection = &second}});
    auto root = AddPortsNode(graph, {{.Connection = &first}, {.Connection = &second}}, {});
    auto& cut = graph.AddEdge(source.Node, root.Node, first);
    graph.AddEdge(source.Node, root.Node, second);
    std::string error;
    ASSERT_TRUE(graph.Seal(&error)) << error;
    // The uncut second path keeps the sink and the source in one piece.
    EXPECT_EQ(
        ApplyError(graph, Plan(Split(cut, EReboundPort::DestinationInput, 0, 1))),
        "split plan gives one subgraph colors 0 and 1");
}

TEST(SchedulerGraph, ApplyRejectsAPieceWithoutColor) {
    TTaskGraph graph;
    auto& first = graph.AddConnection(std::make_unique<TOneToOneConnection>());
    auto& second = graph.AddConnection(std::make_unique<TOneToOneConnection>());
    auto source = AddPortsNode(graph, {}, {{.Connection = &first}});
    auto root = AddPortsNode(graph, {{.Connection = &first}}, {});
    auto other = AddPortsNode(graph, {}, {{.Connection = &second}});
    auto otherRoot = AddPortsNode(graph, {{.Connection = &second}}, {});
    auto& cut = graph.AddEdge(source.Node, root.Node, first);
    graph.AddEdge(other.Node, otherRoot.Node, second);
    std::string error;
    ASSERT_TRUE(graph.Seal(&error)) << error;
    EXPECT_EQ(
        ApplyError(graph, Plan(Split(cut, EReboundPort::DestinationInput, 0, 1))),
        "split plan leaves task n2 without a color");
}

TEST(SchedulerGraph, ApplyRejectsAPlanThatBreaksTheGraph) {
    TTaskGraph graph;
    auto& connection = graph.AddConnection(std::make_unique<TOneToOneConnection>());
    auto source = AddPortsNode(graph, {}, {{.Connection = &connection}});
    auto root = AddPortsNode(graph, {{.Connection = &connection}}, {});
    auto& cut = graph.AddEdge(source.Node, root.Node, connection);
    std::string error;
    ASSERT_TRUE(graph.Seal(&error)) << error;
    auto split = Split(cut, EReboundPort::DestinationInput, 0, 1);
    split.NewConnection->Resize(0, 0);
    EXPECT_EQ(
        ApplyError(graph, Plan(std::move(split))),
        "split plan breaks the task graph: task graph contains an invalid source lane id");
}

TEST(SchedulerGraph, ApplyRejectsAnIncompleteSplit) {
    TTaskGraph graph;
    auto& connection = graph.AddConnection(std::make_unique<TOneToOneConnection>());
    auto source = AddPortsNode(graph, {}, {{.Connection = &connection}});
    auto root = AddPortsNode(graph, {{.Connection = &connection}}, {});
    auto& cut = graph.AddEdge(source.Node, root.Node, connection);
    std::string error;
    ASSERT_TRUE(graph.Seal(&error)) << error;
    auto split = Split(cut, EReboundPort::DestinationInput, 0, 1);
    split.SourceTask.reset();
    EXPECT_EQ(ApplyError(graph, Plan(std::move(split))), "split plan has an incomplete split");
}

TEST(SchedulerGraph, ApplyRejectsColorsWithGaps) {
    TTaskGraph graph;
    auto& connection = graph.AddConnection(std::make_unique<TOneToOneConnection>());
    auto source = AddPortsNode(graph, {}, {{.Connection = &connection}});
    auto root = AddPortsNode(graph, {{.Connection = &connection}}, {});
    auto& cut = graph.AddEdge(source.Node, root.Node, connection);
    std::string error;
    ASSERT_TRUE(graph.Seal(&error)) << error;
    const auto gaps = "split plan colors must be 0..K-1 without gaps";
    EXPECT_EQ(ApplyError(graph, Plan(Split(cut, EReboundPort::DestinationInput, 0, 2))), gaps);
    EXPECT_EQ(ApplyError(graph, Plan(Split(cut, EReboundPort::DestinationInput, 1, 2))), gaps);
    EXPECT_EQ(ApplyError(graph, Plan(Split(cut, EReboundPort::DestinationInput, -1, 0))), gaps);
}

TEST(SchedulerGraph, ApplyRejectsASecondSplit) {
    TTaskGraph graph;
    auto& connection = graph.AddConnection(std::make_unique<TOneToOneConnection>());
    auto source = AddPortsNode(graph, {}, {{.Connection = &connection}});
    auto root = AddPortsNode(graph, {{.Connection = &connection}}, {});
    auto& cut = graph.AddEdge(source.Node, root.Node, connection);
    std::string error;
    ASSERT_TRUE(graph.Seal(&error)) << error;
    graph.Apply(Plan(Split(cut, EReboundPort::DestinationInput, 0, 1)));
    auto& again = *graph.Edges().front();
    EXPECT_EQ(
        ApplyError(graph, Plan(Split(again, EReboundPort::DestinationInput, 0, 1))),
        "task graph is already split");
    EXPECT_TRUE(graph.Apply({}));
}

TEST(SchedulerGraph, FailedApplyLeavesTheGraphDestroyable) {
    TTaskGraph graph;
    auto& first = graph.AddConnection(std::make_unique<TOneToOneConnection>());
    auto& second = graph.AddConnection(std::make_unique<TOneToOneConnection>());
    auto source = AddPortsNode(graph, {}, {{.Connection = &first}});
    auto middle = AddPortsNode(graph, {{.Connection = &first}}, {{.Connection = &second}});
    auto& sink = graph.AddOwnedNode(std::make_unique<TNoopTask>());
    graph.AddEdge(source.Node, middle.Node, first);
    auto& cut = graph.AddEdge(middle.Node, sink, second);
    std::string error;
    ASSERT_TRUE(graph.Seal(&error)) << error;
    // The sink has no input port, so Apply throws after it has moved the kept edge.
    EXPECT_EQ(
        ApplyError(graph, Plan(Split(cut, EReboundPort::DestinationInput, 0, 1))),
        "split plan: task n2 has no input port to rebind");
}

// Apply makes one subgraph per color; a run starts from the roots of all of them.
TEST(SchedulerGraph, SchedulersRunTheRootsOfEverySubgraph) {
    for (bool threaded : {false, true}) {
        SCOPED_TRACE(threaded);
        TTaskGraph graph;
        auto& connection = graph.AddConnection(std::make_unique<TOneToOneConnection>());
        auto source = AddPortsNode(graph, {}, {{.Connection = &connection}});
        auto sink = AddPortsNode(graph, {{.Connection = &connection}}, {});
        auto& cut = graph.AddEdge(source.Node, sink.Node, connection);
        ASSERT_TRUE(graph.Seal());
        auto split = Split(cut, EReboundPort::DestinationInput, 0, 1);
        auto remoteSink = std::make_unique<TPortsTask>();
        remoteSink->Inputs.emplace_back();
        auto remoteSource = std::make_unique<TPortsTask>();
        remoteSource->Outputs.emplace_back();
        split.SinkTask = std::move(remoteSink);
        split.SourceTask = std::move(remoteSource);
        const auto sealed = graph.Apply(Plan(std::move(split)));
        auto& remoteSinkNode = *graph.Nodes()[2];
        ASSERT_EQ(Graphs(sealed), (TGraphs{{&remoteSinkNode}, {&sink.Node}}));

        std::string error;
        if (threaded) {
            ASSERT_TRUE(TThreadedScheduler(sealed, 2).Run(&error)) << error;
        } else {
            ASSERT_TRUE(TSingleThreadedScheduler(sealed).Run(&error)) << error;
        }
        EXPECT_EQ(remoteSinkNode.State.load(), ETaskState::Finished);
        EXPECT_EQ(sink.Node.State.load(), ETaskState::Finished);
    }
}

TEST(SchedulerGraph, SchedulersRejectFailedSeal) {
    TTaskGraph graph;
    const auto sealed = graph.Seal();
    ASSERT_FALSE(sealed);
    std::string error;
    EXPECT_FALSE(TSingleThreadedScheduler(sealed).Run(&error));
    EXPECT_EQ(error, "task graph is not sealed");
    error.clear();
    EXPECT_FALSE(TThreadedScheduler(sealed, 2).Run(&error));
    EXPECT_EQ(error, "task graph is not sealed");
}

TEST(SchedulerGraph, RejectsMissingConnection) {
    TTaskGraph graph;
    auto& source = graph.AddOwnedNode(std::make_unique<TNoopTask>());
    auto& sink = graph.AddOwnedNode(std::make_unique<TNoopTask>());
    graph.AddOwnedEdge(source, sink, {});
    std::string error;
    EXPECT_FALSE(graph.Seal(&error));
    EXPECT_NE(error.find("without a connection"), std::string::npos);
}

TEST(SchedulerGraph, RejectsInvalidLaneIds) {
    TTaskGraph graph;
    auto& source = graph.AddOwnedNode(std::make_unique<TNoopTask>());
    auto& sink = graph.AddOwnedNode(std::make_unique<TNoopTask>());
    graph.AddOwnedEdge(source, sink, std::make_unique<TOneToOneConnection>(), 1, 0);
    std::string error;
    EXPECT_FALSE(graph.Seal(&error));
    EXPECT_NE(error.find("source lane"), std::string::npos);
}

TEST(SchedulerGraph, PrintsConnectionsAndLaneMapping) {
    auto connection = std::make_unique<TGatherConnection>();
    connection->Resize(2, 1);

    TTaskGraph graph;
    auto& left = graph.AddOwnedNode(std::make_unique<TNoopTask>());
    auto& right = graph.AddOwnedNode(std::make_unique<TNoopTask>());
    auto& sink = graph.AddOwnedNode(std::make_unique<TNoopTask>());
    auto& conn = graph.AddConnection(std::move(connection));
    graph.AddEdge(left, sink, conn, 0, 0);
    graph.AddEdge(right, sink, conn, 1, 0);
    ASSERT_TRUE(graph.Seal());

    std::stringstream out;
    graph.Print(out);
    const auto text = out.str();
    EXPECT_NE(text.find("conn c0 - gather src=2 dst=1"), std::string::npos);
    EXPECT_NE(text.find("edge e0 n0[0] -> n2[0] via c0"), std::string::npos);
    EXPECT_NE(text.find("edge e1 n1[1] -> n2[0] via c0"), std::string::npos);
}

TEST(SchedulerGraph, RejectsDependencyCycle) {
    TTaskGraph graph;
    auto& source = graph.AddOwnedNode(std::make_unique<TNoopTask>());
    auto& sink = graph.AddOwnedNode(std::make_unique<TNoopTask>());
    graph.AddOwnedEdge(source, sink, std::make_unique<TOneToOneConnection>());
    graph.AddDependencyEdge(sink, source);
    std::string error;
    EXPECT_FALSE(graph.Seal(&error));
    EXPECT_NE(error.find("scheduling cycle"), std::string::npos);
}

TEST(SchedulerGraph, DependencyEdgesDoNotCarryRowsets) {
    TTaskGraph graph;
    auto& build = graph.AddOwnedNode(std::make_unique<TNoopTask>());
    auto& probe = graph.AddOwnedNode(std::make_unique<TNoopTask>());
    auto& sink = graph.AddOwnedNode(std::make_unique<TNoopTask>());
    graph.AddOwnedEdge(build, sink, std::make_unique<TOneToOneConnection>());
    graph.AddOwnedEdge(probe, sink, std::make_unique<TOneToOneConnection>());
    graph.AddDependencyEdge(build, probe);
    ASSERT_TRUE(graph.Seal());

    ASSERT_EQ(probe.Inbound.size(), 1u);
    EXPECT_EQ(probe.Inbound.front()->Kind, ETaskEdgeKind::Dependency);
    EXPECT_EQ(probe.Inbound.front()->Src, &build);
    EXPECT_EQ(probe.Inbound.front()->Connection, nullptr);
    std::stringstream out;
    graph.Print(out);
    EXPECT_NE(out.str().find("dependency n0 -> n1"), std::string::npos);
}

TEST(SchedulerGraph, SingleThreadedDependencyInput) {
    CheckControlDependency(false);
}

TEST(SchedulerGraph, ThreadedDependencyInput) {
    CheckControlDependency(true);
}

TEST(SchedulerGraph, SingleThreadedSchedulerRunsSmallGraph) {
    auto sourceTask = std::make_unique<TSourceTask>();
    auto* sourcePtr = sourceTask.get();
    auto sinkTask = std::make_unique<TSinkTask>(*sourcePtr);
    auto* sinkPtr = sinkTask.get();

    TTaskGraph graph;
    auto& source = graph.AddOwnedNode(std::move(sourceTask));
    auto& sink = graph.AddOwnedNode(std::move(sinkTask));
    graph.AddOwnedEdge(source, sink, std::make_unique<TOneToOneConnection>());
    const auto sealed = graph.Seal();
    ASSERT_TRUE(sealed);

    TSingleThreadedScheduler scheduler(sealed);
    std::string error;
    ASSERT_TRUE(scheduler.Run(&error)) << error;
    EXPECT_TRUE(sourcePtr->Finished);
    EXPECT_TRUE(sinkPtr->Consumed());
    EXPECT_TRUE(sinkPtr->Finished);
}

TEST(SchedulerGraph, SingleThreadedSchedulerCannotRunTwice) {
    TTaskGraph graph;
    graph.AddOwnedNode(std::make_unique<TNoopTask>());
    const auto sealed = graph.Seal();
    ASSERT_TRUE(sealed);

    TSingleThreadedScheduler scheduler(sealed);
    std::string error;
    ASSERT_TRUE(scheduler.Run(&error)) << error;
    EXPECT_FALSE(scheduler.Run(&error));
    EXPECT_NE(error.find("cannot run twice"), std::string::npos);
}

} // namespace

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
