#include <gtest/gtest.h>

#include <qdb/scheduler/connections/local.h>
#include <qdb/scheduler/impl/single_threaded_scheduler.h>
#include <qdb/scheduler/lowering/plan_lowerer.h>
#include <qdb/scheduler/spec/graph_spec.h>
#include <qdb/scheduler/tasks/runtime_adapter.h>

#include <memory>
#include <sstream>
#include <string>

using namespace NQdb;
using namespace NQdb::NScheduler;

namespace {

std::string PrintGraph(const TTaskGraph& graph)
{
    std::ostringstream out;
    graph.Print(out);
    return out.str();
}

TNodeSpec Node(std::string kind, std::string label, TNodeBody body)
{
    return TNodeSpec{
        .ExecStageId = 4,
        .TaskGroupId = 9,
        .DebugKind = std::move(kind),
        .DebugLabel = std::move(label),
        .Body = std::move(body),
    };
}

// source -> hash shuffle -> two forwards -> gather -> sink writer.
TGraphSpec FanOutSpec()
{
    TGraphSpec spec;
    spec.Connections = {
        {.Kind = EConnectionKind::HashShuffle, .Capacity = 2, .SrcCount = 1, .DstCount = 2,
            .DebugName = "shuffle", .RepartitionKeys = {"k"},
            .RowLayouts = {{.Width = 8}, {.Width = 1, .Bitmap = true}}},
        {.Kind = EConnectionKind::Gather, .Capacity = 3, .SrcCount = 2, .DstCount = 1,
            .DebugName = "gather"},
    };
    spec.Nodes = {
        Node("source", "in", TRemoteSourceNode{.Out = {0, 0}}),
        Node("forward", "left", TForwardNode{.In = {0, 0}, .Out = {1, 0}}),
        Node("forward", "right", TForwardNode{.In = {0, 1}, .Out = {1, 1}}),
        Node("sink", "out", TSinkWriterNode{.In = {1, 0}}),
    };
    spec.Edges = {
        {.Src = 0, .Dst = 1, .Connection = 0, .SrcLane = 0, .DstLane = 0},
        {.Src = 0, .Dst = 2, .Connection = 0, .SrcLane = 0, .DstLane = 1},
        {.Src = 1, .Dst = 3, .Connection = 1, .SrcLane = 0, .DstLane = 0},
        {.Src = 2, .Dst = 3, .Connection = 1, .SrcLane = 1, .DstLane = 0, .ExecInput = 1},
        {.Src = 0, .Dst = 3, .Kind = ETaskEdgeKind::Dependency},
    };
    spec.Externals = {{.Id = 5, .Node = 0}};
    return spec;
}

struct TCountingSink : ISink {
    void Write(const TRowSet& rowSet) override
    {
        Rows += rowSet.RowCount;
    }

    int64_t Rows = 0;
};

// The same graph as FanOutSpec, built with the scheduler API.
std::unique_ptr<TTaskGraph> BuildFanOutByHand(ISink& sink)
{
    auto graph = std::make_unique<TTaskGraph>();
    auto shuffle = std::make_unique<THashShuffleConnection>(2, TRepartitionSpec{.Keys = {"k"}});
    shuffle->Resize(1, 2);
    shuffle->SetDebugName("shuffle");
    shuffle->SetRowLayouts({TValueLayout{.Width = 8}, TValueLayout{.Width = 1, .Bitmap = true}});
    auto& shuffleRef = graph->AddConnection(std::move(shuffle));
    auto gather = std::make_unique<TGatherConnection>(3);
    gather->Resize(2, 1);
    gather->SetDebugName("gather");
    auto& gatherRef = graph->AddConnection(std::move(gather));

    auto forwardCode = std::make_shared<TUnaryCode>([](void*, TRowSet&) {});
    auto forward = [&](size_t lane) {
        return std::make_unique<TUnaryTask>(forwardCode, std::shared_ptr<void>{},
            TInputPort{.Connection = &shuffleRef, .Lane = lane},
            TOutputPort{.Connection = &gatherRef, .Lane = lane});
    };
    auto sinkCode = std::make_shared<TSinkCode>([](void* state, const TRowSet& rowSet) {
        static_cast<ISink*>(state)->Write(rowSet);
        return ETaskResult::OK;
    });
    auto& source = graph->AddOwnedNode(std::make_unique<TSourceTask>(
        nullptr, nullptr, TOutputPort{.Connection = &shuffleRef}));
    auto& left = graph->AddOwnedNode(forward(0));
    auto& right = graph->AddOwnedNode(forward(1));
    auto& out = graph->AddOwnedNode(std::make_unique<TSinkTask>(
        sinkCode, std::shared_ptr<void>(std::shared_ptr<void>{}, &sink),
        TInputPort{.Connection = &gatherRef}));
    for (auto [node, kind] : {std::pair{&source, "source"}, {&left, "forward"},
             {&right, "forward"}, {&out, "sink"}}) {
        node->DebugKind = kind;
    }
    graph->AddEdge(source, left, shuffleRef, 0, 0);
    graph->AddEdge(source, right, shuffleRef, 0, 1);
    graph->AddEdge(left, out, gatherRef, 0, 0);
    graph->AddEdge(right, out, gatherRef, 1, 0, 1);
    graph->AddDependencyEdge(source, out);
    graph->MarkExternal(source, 5);
    return graph;
}

} // namespace

TEST(GraphSpec, SerializesAndDeserializes)
{
    const auto spec = FanOutSpec();
    const auto bytes = SerializeGraphSpec(spec);
    EXPECT_EQ(SerializeGraphSpec(DeserializeGraphSpec(bytes)), bytes);

    const auto decoded = DeserializeGraphSpec(bytes);
    ASSERT_EQ(decoded.Connections.size(), 2u);
    EXPECT_EQ(decoded.Connections[0].RepartitionKeys, std::vector<std::string>{"k"});
    EXPECT_TRUE(decoded.Connections[0].RowLayouts[1].Bitmap);
    EXPECT_EQ(decoded.Nodes[3].DebugLabel, "out");
    EXPECT_TRUE(std::holds_alternative<TSinkWriterNode>(decoded.Nodes[3].Body));
    EXPECT_EQ(decoded.Edges[4].Kind, ETaskEdgeKind::Dependency);
    EXPECT_FALSE(decoded.Edges[4].Connection.has_value());
    EXPECT_EQ(decoded.Externals[0].Id, 5u);
}

TEST(GraphSpec, RowLayoutWidthKeepsInt32)
{
    auto spec = FanOutSpec();
    spec.Connections[0].RowLayouts[0].Width = 0x7fffffff;
    spec.Connections[0].RowLayouts[1].Width = -5;
    const auto bytes = SerializeGraphSpec(spec);
    const auto back = DeserializeGraphSpec(bytes);
    EXPECT_EQ(back.Connections[0].RowLayouts[0].Width, 0x7fffffff);
    EXPECT_EQ(back.Connections[0].RowLayouts[1].Width, -5);
    EXPECT_EQ(SerializeGraphSpec(back), bytes);
    // Each layout is a 4-byte width plus a 1-byte bitmap flag.
    auto bare = spec;
    bare.Connections[0].RowLayouts.clear();
    EXPECT_EQ(bytes.size() - SerializeGraphSpec(bare).size(), 2 * (4 + 1));
}

TEST(GraphSpec, RejectsTruncatedInput)
{
    const auto bytes = SerializeGraphSpec(FanOutSpec());
    for (size_t size = 0; size < bytes.size(); ++size) {
        EXPECT_THROW(DeserializeGraphSpec(std::string_view(bytes).substr(0, size)),
            std::runtime_error) << size;
    }
    EXPECT_THROW(DeserializeGraphSpec(bytes + "x"), std::runtime_error);
}

TEST(GraphSpec, MaterializedGraphMatchesHandBuiltGraph)
{
    TCountingSink sink;
    auto graph = MaterializeGraph(DeserializeGraphSpec(SerializeGraphSpec(FanOutSpec())), &sink);
    auto byHand = BuildFanOutByHand(sink);
    ASSERT_TRUE(graph->Seal());
    ASSERT_TRUE(byHand->Seal());
    EXPECT_EQ(PrintGraph(*graph), PrintGraph(*byHand));
    EXPECT_EQ(graph->Nodes()[2]->ExecutionSlot, 2u);
    EXPECT_EQ(graph->Nodes()[1]->ExecStageId, 4u);
    EXPECT_EQ(graph->Nodes()[1]->TaskGroupId, 9u);
    EXPECT_EQ(graph->Nodes()[1]->DebugLabel, "left");
    EXPECT_EQ(graph->Connections()[0]->RowLayouts().size(), 2u);
}

TEST(GraphSpec, ExtractReturnsTheSameSpec)
{
    TCountingSink sink;
    const auto spec = FanOutSpec();
    auto graph = MaterializeGraph(spec, &sink);
    ASSERT_TRUE(graph->Seal());
    EXPECT_EQ(SerializeGraphSpec(ExtractGraphSpec(*graph, spec.Nodes)), SerializeGraphSpec(spec));
    EXPECT_THROW(ExtractGraphSpec(*graph, {}), std::runtime_error);
}

TEST(GraphSpec, LoweredPlanWithAllRecordsExtractsAndMaterializes)
{
    TCountingSink sink;
    const auto spec = FanOutSpec();
    TLoweredPlan lowered;
    lowered.Graph = MaterializeGraph(spec, &sink);
    ASSERT_TRUE(lowered.Graph->Seal());
    EXPECT_FALSE(ExtractLoweredGraphSpec(lowered));

    for (const auto& node : spec.Nodes) {
        lowered.NodeSpecs.push_back(node);
    }
    const auto extracted = ExtractLoweredGraphSpec(lowered);
    ASSERT_TRUE(extracted);
    auto again = MaterializeGraph(*extracted, &sink);
    ASSERT_TRUE(again->Seal());
    EXPECT_EQ(PrintGraph(*again), PrintGraph(*lowered.Graph));

    lowered.NodeSpecs[1].reset();
    std::vector<uint32_t> missing;
    EXPECT_FALSE(ExtractLoweredGraphSpec(lowered, &missing));
    EXPECT_EQ(missing, std::vector<uint32_t>{1});
}

struct TForeignConnection : IConnection {
    EConnectionKind Kind() const override
    {
        return EConnectionKind::OneToOne;
    }

    void Resize(size_t, size_t) override
    {}

    size_t SrcCount() const override
    {
        return 1;
    }

    size_t DstCount() const override
    {
        return 1;
    }

    bool CanPush(size_t) const override
    {
        return true;
    }

    bool Push(size_t, TRowSet&&) override
    {
        return true;
    }

    void Finish(size_t) override
    {}

    EFetchResult Fetch(size_t, TRowSet&) override
    {
        return EFetchResult::NO_DATA;
    }
};

TEST(GraphSpec, ExtractRejectsNonLocalConnectionAndMissingRowsetConnection)
{
    TCountingSink sink;
    const auto spec = FanOutSpec();

    auto foreign = MaterializeGraph(spec, &sink);
    foreign->AddConnection(std::make_unique<TForeignConnection>());
    EXPECT_THROW(ExtractGraphSpec(*foreign, spec.Nodes), std::runtime_error);

    auto bare = MaterializeGraph(spec, &sink);
    auto& src = *bare->Nodes()[0];
    auto& dst = *bare->Nodes()[3];
    bare->AddOwnedEdge(src, dst, nullptr);
    EXPECT_THROW(ExtractGraphSpec(*bare, spec.Nodes), std::runtime_error);
}

TEST(GraphSpec, RejectsInvalidSpecs)
{
    auto spec = FanOutSpec();
    EXPECT_THROW(MaterializeGraph(spec), std::runtime_error);

    spec.Edges[0].Dst = 17;
    TCountingSink sink;
    EXPECT_THROW(MaterializeGraph(spec, &sink), std::runtime_error);

    spec = FanOutSpec();
    spec.Edges[0].Connection.reset();
    EXPECT_THROW(MaterializeGraph(spec, &sink), std::runtime_error);

    spec = FanOutSpec();
    std::get<TForwardNode>(spec.Nodes[1].Body).In.Connection = 9;
    EXPECT_THROW(MaterializeGraph(spec, &sink), std::runtime_error);
}

TEST(GraphSpec, ForwardChainRunsRows)
{
    TGraphSpec spec;
    spec.Connections = {
        {.Kind = EConnectionKind::OneToOne, .Capacity = 2},
        {.Kind = EConnectionKind::OneToOne, .Capacity = 2},
    };
    spec.Nodes = {
        Node("source", "", TRemoteSourceNode{.Out = {0, 0}}),
        Node("forward", "", TForwardNode{.In = {0, 0}, .Out = {1, 0}}),
        Node("sink", "", TSinkWriterNode{.In = {1, 0}}),
    };
    spec.Edges = {
        {.Src = 0, .Dst = 1, .Connection = 0},
        {.Src = 1, .Dst = 2, .Connection = 1},
    };

    TCountingSink sink;
    auto graph = MaterializeGraph(DeserializeGraphSpec(SerializeGraphSpec(spec)), &sink);
    // The compute actor binds remote sources; the test plays that part.
    int produced = 0;
    static_cast<TSourceTask*>(graph->Nodes()[0]->Task)->BindCode(
        std::make_shared<TSourceCode>([&produced](void*, TRowSet& rowSet) {
            if (produced == 3) {
                return ETaskResult::FINISHED;
            }
            rowSet = TRowSet{.RowCount = 10 + produced++, .RefCount = 1};
            return ETaskResult::OK;
        }),
        nullptr);
    const auto sealed = graph->Seal();
    ASSERT_TRUE(sealed);

    TSingleThreadedScheduler scheduler(sealed);
    std::string error;
    ASSERT_TRUE(scheduler.Run(&error)) << error;
    EXPECT_EQ(sink.Rows, 10 + 11 + 12);
}

int main(int argc, char** argv)
{
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
