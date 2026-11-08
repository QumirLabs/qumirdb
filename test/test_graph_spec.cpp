#include <gtest/gtest.h>

#include <qdb/plan/types/decimal.h>
#include <qdb/plan/types/nullable.h>
#include <qdb/plan/types/type_text.h>
#include <qdb/scheduler/connections/local.h>
#include <qdb/scheduler/impl/single_threaded_scheduler.h>
#include <qdb/scheduler/lowering/plan_lowerer.h>
#include <qdb/scheduler/spec/graph_spec.h>
#include <qdb/scheduler/tasks/runtime_adapter.h>

#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

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

TEST(GraphSpec, ScanFilterProjectRecordsRoundTrip)
{
    using TRowGroups = std::optional<std::vector<int32_t>>;
    TGraphSpec spec;
    for (const auto& rowGroups : {TRowGroups{}, TRowGroups{std::vector<int32_t>{}}, TRowGroups{{1, 3}}}) {
        spec.Nodes.push_back(Node("source", "t", TParquetScanNode{
            .Out = {0, 2}, .Path = "/data/t.parquet",
            .Columns = std::vector<std::string>{"a", "b"}, .RowGroups = rowGroups}));
    }
    spec.Nodes.push_back(Node("source", "u", TParquetScanNode{.Path = "/data/u.parquet"}));
    spec.Nodes.push_back(Node("filter", "", TFilterNode{.In = {0, 2}, .Out = {1, 2}, .Plan = {.Kernel = 2}}));
    spec.Nodes.push_back(Node("filter", "", TFilterNode{
        .Plan = {.Kernel = 3, .KeptInputColumns = std::vector<int32_t>{2, 0}}}));
    spec.Nodes.push_back(Node("project", "", TProjectNode{.In = {1, 2}, .Out = {2, 2}, .Plan = {
        .Kernel = 4,
        .Columns = {{.Index = 1}, {.Computed = true, .Index = 0}, {.Computed = true, .Index = 1}},
        .ComputedWidths = {16, 8},
        .ComputedIsString = {true, false},
        .ComputedIsNullable = {false, true},
    }}));
    spec.Nodes.push_back(Node("project", "", TProjectNode{.Plan = {.Columns = {{.Index = 0}}}}));
    spec.Regexes = {{.Pattern = "a+", .Replacement = "b"}, {.Pattern = "^x", .Replacement = ""}};

    const auto bytes = SerializeGraphSpec(spec);
    const auto back = DeserializeGraphSpec(bytes);
    EXPECT_EQ(SerializeGraphSpec(back), bytes);
    for (size_t size = 0; size < bytes.size(); ++size) {
        EXPECT_THROW(DeserializeGraphSpec(std::string_view(bytes).substr(0, size)),
            std::runtime_error) << size;
    }

    ASSERT_EQ(back.Nodes.size(), 8u);
    EXPECT_FALSE(std::get<TParquetScanNode>(back.Nodes[0].Body).RowGroups);
    EXPECT_EQ(std::get<TParquetScanNode>(back.Nodes[1].Body).RowGroups, TRowGroups{std::vector<int32_t>{}});
    const auto& scan = std::get<TParquetScanNode>(back.Nodes[2].Body);
    EXPECT_EQ(scan.RowGroups, (TRowGroups{{1, 3}}));
    EXPECT_EQ(scan.Path, "/data/t.parquet");
    EXPECT_EQ(scan.Columns, (std::vector<std::string>{"a", "b"}));
    EXPECT_EQ(scan.Out.Lane, 2u);
    EXPECT_FALSE(std::get<TParquetScanNode>(back.Nodes[3].Body).Columns);

    EXPECT_EQ(std::get<TFilterNode>(back.Nodes[4].Body).Plan.Kernel, 2u);
    EXPECT_FALSE(std::get<TFilterNode>(back.Nodes[4].Body).Plan.KeptInputColumns);
    EXPECT_EQ(std::get<TFilterNode>(back.Nodes[5].Body).Plan.KeptInputColumns,
        (std::vector<int32_t>{2, 0}));

    const auto& project = std::get<TProjectNode>(back.Nodes[6].Body).Plan;
    EXPECT_EQ(project.Kernel, 4u);
    ASSERT_EQ(project.Columns.size(), 3u);
    EXPECT_FALSE(project.Columns[0].Computed);
    EXPECT_EQ(project.Columns[0].Index, 1);
    EXPECT_TRUE(project.Columns[2].Computed);
    EXPECT_EQ(project.Columns[2].Index, 1);
    EXPECT_EQ(project.ComputedWidths, (std::vector<size_t>{16, 8}));
    EXPECT_EQ(project.ComputedIsString, (std::vector<bool>{true, false}));
    EXPECT_EQ(project.ComputedIsNullable, (std::vector<bool>{false, true}));
    EXPECT_FALSE(std::get<TProjectNode>(back.Nodes[7].Body).Plan.Kernel);

    ASSERT_EQ(back.Regexes.size(), 2u);
    EXPECT_EQ(back.Regexes[1].Pattern, "^x");
    EXPECT_EQ(back.Regexes[0].Replacement, "b");
}

TEST(GraphSpec, SortMergeWindowRecordsRoundTrip)
{
    using namespace NQumir::NAst;
    const auto i64 = std::make_shared<TIntegerType>(TIntegerType::I64);
    const auto nullableI64 = std::make_shared<TNullable>(i64);
    const auto type = std::make_shared<TStructType>(TStructType({{"k", nullableI64}, {"v", i64}}));
    const std::vector<TSortKey> keys = {
        {.Column = "k", .Direction = ESortDirection::Desc, .Nulls = ESortNulls::Last},
        {.Column = "v"},
    };
    const std::vector<TSortColumnRef> keyColumns = {{.Index = 0, .Type = nullableI64}, {.Index = 1, .Type = i64}};

    TGraphSpec spec;
    spec.Nodes.push_back(Node("sort", "sort", TSortNode{.In = {0, 1}, .Out = {2, 3}, .Plan = {
        .OutputType = type, .Keys = keys, .KeyColumns = keyColumns, .RadixKernel = 4, .RadixNullable = true}}));
    spec.Nodes.push_back(Node("top-sort", "top-sort", TSortNode{.Plan = {
        .OutputType = type, .Keys = keys, .KeyColumns = keyColumns, .RadixKernel = 5, .RadixNullable = true,
        .Top = TSortPlanData::TTop{.TopSortKernel = 6, .HeapKernel = 7, .Limit = 100}}}));
    spec.Nodes.push_back(Node("merge", "merge", TSortMergeNode{
        .In = {{0, 0}, {1, 0}, {2, 0}}, .Out = {3, 1},
        .Plan = {.OutputType = type, .Keys = keys, .KeyColumns = keyColumns}}));
    spec.Nodes.push_back(Node("window", "window", TWindowNode{.In = {4, 0}, .Out = {5, 0}, .Plan = {
        .OutputType = type, .Keys = keys, .KeyColumns = keyColumns, .Kernel = 9}}));

    const auto bytes = SerializeGraphSpec(spec);
    const auto back = DeserializeGraphSpec(bytes);
    EXPECT_EQ(SerializeGraphSpec(back), bytes);
    for (size_t size = 0; size < bytes.size(); ++size) {
        EXPECT_THROW(DeserializeGraphSpec(std::string_view(bytes).substr(0, size)),
            std::runtime_error) << size;
    }

    ASSERT_EQ(back.Nodes.size(), 4u);
    const auto& sort = std::get<TSortNode>(back.Nodes[0].Body);
    EXPECT_EQ(sort.In.Lane, 1u);
    EXPECT_EQ(sort.Out.Connection, 2u);
    EXPECT_EQ(sort.Plan.RadixKernel, 4u);
    EXPECT_TRUE(sort.Plan.RadixNullable);
    EXPECT_FALSE(sort.Plan.Top);
    EXPECT_EQ(sort.Plan.OutputType->ToString(), type->ToString());
    ASSERT_EQ(sort.Plan.Keys.size(), 2u);
    EXPECT_EQ(sort.Plan.Keys[0].Column, "k");
    EXPECT_EQ(sort.Plan.Keys[0].Direction, ESortDirection::Desc);
    EXPECT_EQ(sort.Plan.Keys[0].Nulls, ESortNulls::Last);
    ASSERT_EQ(sort.Plan.KeyColumns.size(), 2u);
    EXPECT_TRUE(TMaybeType<TNullable>(sort.Plan.KeyColumns[0].Type));
    EXPECT_FALSE(TMaybeType<TNullable>(sort.Plan.KeyColumns[1].Type));
    EXPECT_EQ(sort.Plan.KeyColumns[0].Type->ToString(), nullableI64->ToString());

    const auto& top = std::get<TSortNode>(back.Nodes[1].Body).Plan.Top;
    ASSERT_TRUE(top);
    EXPECT_EQ(top->TopSortKernel, 6u);
    EXPECT_EQ(top->HeapKernel, 7u);
    EXPECT_EQ(top->Limit, 100);

    const auto& merge = std::get<TSortMergeNode>(back.Nodes[2].Body);
    ASSERT_EQ(merge.In.size(), 3u);
    EXPECT_EQ(merge.In[2].Connection, 2u);
    EXPECT_EQ(merge.Out.Lane, 1u);
    EXPECT_EQ(merge.Plan.KeyColumns[0].Index, 0);
    EXPECT_TRUE(TMaybeType<TNullable>(merge.Plan.KeyColumns[0].Type));

    const auto& window = std::get<TWindowNode>(back.Nodes[3].Body);
    EXPECT_EQ(window.Plan.Kernel, 9u);
    EXPECT_EQ(window.Plan.OutputType->ToString(), type->ToString());
    EXPECT_EQ(window.Plan.Keys[1].Column, "v");
}

TEST(GraphSpec, AggregateAndHashShuffleRecordsRoundTrip)
{
    using namespace NQumir::NAst;
    const TTypePtr inputType = std::make_shared<TStructType>(std::vector<std::pair<std::string, TTypePtr>>{
        {"k", std::make_shared<TIntegerType>(TIntegerType::I64)},
        {"d", std::make_shared<TNullable>(std::make_shared<TDecimal>(18, 2))},
        {"s", std::make_shared<TStringType>()},
    });
    TGraphSpec spec;
    spec.Nodes.push_back(Node("hash-shuffle", "aggregate-repartition", THashShuffleNode{
        .In = {0, 1},
        .Out = 1,
        .SourceLane = 1,
        .Hash = {.Kernel = 3, .KeyColumns = {0, 2}},
        .InputType = inputType,
        .TargetOutputBatchRows = 4096,
        .MaxOutputBatchRows = 65536,
        .TargetOutputBatchBytes = 1 << 20,
        .UseInputHash = true,
    }));
    spec.Nodes.push_back(Node("aggregate", "aggregate", TAggregateNode{
        .In = {1, 1},
        .Out = {2, 1},
        .Meta = {
            .NumAggs = 2,
            .StateWords = 3,
            .RowBytes = 40,
            .StringMetadataBuffers = {2},
            .OutputKeys = {
                {.Kind = EAggregateOutputKeyKind::String, .IsNullable = true, .Size = 16, .Alignment = 8},
                {.Size = 4, .Alignment = 4},
            },
            .OutputAggs = {{.IsNullable = true}, {}},
            .Kernel = 4,
        },
        .Table = {
            .Layout = EAggregateTableLayout::Row,
            .InitialCapacity = 64,
            .InitialGroups = 40,
            .RowSwitchGroups = 7,
            .RowExpectedGroups = 9,
        },
    }));

    const auto bytes = SerializeGraphSpec(spec);
    const auto back = DeserializeGraphSpec(bytes);
    EXPECT_EQ(SerializeGraphSpec(back), bytes);
    for (size_t size = 0; size < bytes.size(); ++size) {
        EXPECT_THROW(DeserializeGraphSpec(std::string_view(bytes).substr(0, size)),
            std::runtime_error) << size;
    }

    ASSERT_EQ(back.Nodes.size(), 2u);
    const auto& shuffle = std::get<THashShuffleNode>(back.Nodes[0].Body);
    EXPECT_EQ(shuffle.In.Lane, 1u);
    EXPECT_EQ(shuffle.Out, 1u);
    EXPECT_EQ(shuffle.SourceLane, 1u);
    EXPECT_EQ(shuffle.Hash.Kernel, 3u);
    EXPECT_EQ(shuffle.Hash.KeyColumns, (std::vector<int64_t>{0, 2}));
    ASSERT_TRUE(shuffle.InputType);
    EXPECT_NE(shuffle.InputType, inputType);
    EXPECT_EQ(PrintTypeText(shuffle.InputType), PrintTypeText(inputType));
    EXPECT_EQ(shuffle.TargetOutputBatchRows, 4096u);
    EXPECT_EQ(shuffle.MaxOutputBatchRows, 65536u);
    EXPECT_EQ(shuffle.TargetOutputBatchBytes, 1u << 20);
    EXPECT_TRUE(shuffle.UseInputHash);

    const auto& aggregate = std::get<TAggregateNode>(back.Nodes[1].Body);
    EXPECT_EQ(aggregate.Out.Connection, 2u);
    const auto& meta = aggregate.Meta;
    EXPECT_EQ(meta.NumAggs, 2u);
    EXPECT_EQ(meta.StateWords, 3u);
    EXPECT_EQ(meta.RowBytes, 40);
    EXPECT_EQ(meta.StringMetadataBuffers, (std::vector<int64_t>{2}));
    ASSERT_EQ(meta.OutputKeys.size(), 2u);
    EXPECT_EQ(meta.OutputKeys[0].Kind, EAggregateOutputKeyKind::String);
    EXPECT_TRUE(meta.OutputKeys[0].IsNullable);
    EXPECT_EQ(meta.OutputKeys[0].Size, 16u);
    EXPECT_EQ(meta.OutputKeys[1].Kind, EAggregateOutputKeyKind::Fixed);
    EXPECT_EQ(meta.OutputKeys[1].Alignment, 4u);
    ASSERT_EQ(meta.OutputAggs.size(), 2u);
    EXPECT_TRUE(meta.OutputAggs[0].IsNullable);
    EXPECT_FALSE(meta.OutputAggs[1].IsNullable);
    EXPECT_EQ(meta.Kernel, 4u);
    EXPECT_EQ(aggregate.Table.Layout, EAggregateTableLayout::Row);
    EXPECT_EQ(aggregate.Table.InitialCapacity, 64);
    EXPECT_EQ(aggregate.Table.InitialGroups, 40);
    EXPECT_EQ(aggregate.Table.RowSwitchGroups, 7);
    EXPECT_EQ(aggregate.Table.RowExpectedGroups, 9);
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

    // Kernel 0 has no slot.
    spec = FanOutSpec();
    spec.Nodes[1].Body = TFilterNode{.In = {0, 0}, .Out = {1, 0}};
    EXPECT_THROW(MaterializeGraph(spec, &sink), std::runtime_error);

    // Connection 1 is a gather, not a hash shuffle.
    spec = FanOutSpec();
    spec.Nodes[1].Body = THashShuffleNode{.In = {0, 0}, .Out = 1};
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
