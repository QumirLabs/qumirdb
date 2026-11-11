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

// source -> broadcast -> two forwards -> gather -> sink writer.
TGraphSpec FanOutSpec()
{
    TGraphSpec spec;
    spec.Connections = {
        {.Kind = EConnectionKind::Broadcast, .Capacity = 2, .SrcCount = 1, .DstCount = 2,
            .DebugName = "broadcast", .RowLayouts = {{.Width = 8}, {.Width = 1, .Bitmap = true}}},
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
    auto broadcast = std::make_unique<TBroadcastConnection>(2);
    broadcast->Resize(1, 2);
    broadcast->SetDebugName("broadcast");
    broadcast->SetRowLayouts({TValueLayout{.Width = 8}, TValueLayout{.Width = 1, .Bitmap = true}});
    auto& broadcastRef = graph->AddConnection(std::move(broadcast));
    auto gather = std::make_unique<TGatherConnection>(3);
    gather->Resize(2, 1);
    gather->SetDebugName("gather");
    auto& gatherRef = graph->AddConnection(std::move(gather));

    auto forwardCode = std::make_shared<TUnaryCode>([](void*, TRowSet&) {});
    auto forward = [&](size_t lane) {
        return std::make_unique<TUnaryTask>(forwardCode, std::shared_ptr<void>{},
            TInputPort{.Connection = &broadcastRef, .Lane = lane},
            TOutputPort{.Connection = &gatherRef, .Lane = lane});
    };
    auto sinkCode = std::make_shared<TSinkCode>([](void* state, const TRowSet& rowSet) {
        static_cast<ISink*>(state)->Write(rowSet);
        return ETaskResult::OK;
    });
    auto& source = graph->AddOwnedNode(std::make_unique<TSourceTask>(
        nullptr, nullptr, TOutputPort{.Connection = &broadcastRef}));
    auto& left = graph->AddOwnedNode(forward(0));
    auto& right = graph->AddOwnedNode(forward(1));
    auto& out = graph->AddOwnedNode(std::make_unique<TSinkTask>(
        sinkCode, std::shared_ptr<void>(std::shared_ptr<void>{}, &sink),
        TInputPort{.Connection = &gatherRef}));
    for (auto [node, kind] : {std::pair{&source, "source"}, {&left, "forward"},
             {&right, "forward"}, {&out, "sink"}}) {
        node->DebugKind = kind;
    }
    graph->AddEdge(source, left, broadcastRef, 0, 0);
    graph->AddEdge(source, right, broadcastRef, 0, 1);
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
    EXPECT_EQ(decoded.Connections[0].Kind, EConnectionKind::Broadcast);
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
        .Out = {{1, 1}, {2, 1}},
        .Hash = TAggregateHashPlanData{.Kernel = 3, .KeyColumns = {0, 2}},
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
    ASSERT_EQ(shuffle.Out.size(), 2u);
    EXPECT_EQ(shuffle.Out[1].Connection, 2u);
    EXPECT_EQ(shuffle.Out[1].Lane, 1u);
    const auto& hash = std::get<TAggregateHashPlanData>(shuffle.Hash);
    EXPECT_EQ(hash.Kernel, 3u);
    EXPECT_EQ(hash.KeyColumns, (std::vector<int64_t>{0, 2}));
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

TEST(GraphSpec, PartialAndGroupingSetsAggregateRecordsRoundTrip)
{
    const TAggregateKernelMeta meta{
        .NumAggs = 1,
        .StateWords = 2,
        .OutputKeys = {{.IsNullable = true, .Size = 8, .Alignment = 8}},
        .OutputAggs = {{}},
        .Kernel = 5,
    };
    TGraphSpec spec;
    spec.Nodes.push_back(Node("aggregate", "aggregate partial", TPartialAggregateNode{
        .In = {0, 3},
        .Out = {1, 3},
        .Meta = meta,
        .Table = {.InitialCapacity = 256, .RowSwitchGroups = 11},
        .Policy = {.SampleRows = 1000, .MinReduction = 16, .MaxGroups = 4096},
        .PassThrough = TProjectPlanData{
            .Kernel = 6,
            .Columns = {{.Index = 0}, {.Computed = true, .Index = 0}},
            .ComputedWidths = {8},
            .ComputedIsString = {false},
            .ComputedIsNullable = {true},
        },
    }));
    spec.Nodes.push_back(Node("aggregate", "aggregate partial", TPartialAggregateNode{
        .In = {0, 1},
        .Out = {1, 1},
        .Meta = meta,
    }));
    spec.Nodes.push_back(Node("aggregate", "aggregate", TGroupingSetsAggregateNode{
        .In = {1, 0},
        .Out = {2, 0},
        .Meta = meta,
        .Table = {.Layout = EAggregateTableLayout::Row, .InitialGroups = 12},
        .Sets = {{0, 1}, {0}, {}},
        .NumKeys = 2,
    }));

    const auto bytes = SerializeGraphSpec(spec);
    const auto back = DeserializeGraphSpec(bytes);
    EXPECT_EQ(SerializeGraphSpec(back), bytes);
    for (size_t size = 0; size < bytes.size(); ++size) {
        EXPECT_THROW(DeserializeGraphSpec(std::string_view(bytes).substr(0, size)),
            std::runtime_error) << size;
    }

    ASSERT_EQ(back.Nodes.size(), 3u);
    const auto& partial = std::get<TPartialAggregateNode>(back.Nodes[0].Body);
    EXPECT_EQ(partial.In.Lane, 3u);
    EXPECT_EQ(partial.Out.Connection, 1u);
    EXPECT_EQ(partial.Meta.StateWords, 2u);
    ASSERT_EQ(partial.Meta.OutputKeys.size(), 1u);
    EXPECT_TRUE(partial.Meta.OutputKeys[0].IsNullable);
    EXPECT_EQ(partial.Meta.Kernel, 5u);
    EXPECT_EQ(partial.Table.InitialCapacity, 256);
    EXPECT_EQ(partial.Table.RowSwitchGroups, 11);
    EXPECT_EQ(partial.Policy.SampleRows, 1000u);
    EXPECT_EQ(partial.Policy.MinReduction, 16u);
    EXPECT_EQ(partial.Policy.MaxGroups, 4096);
    ASSERT_TRUE(partial.PassThrough);
    EXPECT_EQ(partial.PassThrough->Kernel, 6u);
    ASSERT_EQ(partial.PassThrough->Columns.size(), 2u);
    EXPECT_FALSE(partial.PassThrough->Columns[0].Computed);
    EXPECT_TRUE(partial.PassThrough->Columns[1].Computed);
    EXPECT_EQ(partial.PassThrough->ComputedWidths, (std::vector<size_t>{8}));
    EXPECT_EQ(partial.PassThrough->ComputedIsNullable, (std::vector<bool>{true}));

    EXPECT_FALSE(std::get<TPartialAggregateNode>(back.Nodes[1].Body).PassThrough);

    const auto& sets = std::get<TGroupingSetsAggregateNode>(back.Nodes[2].Body);
    EXPECT_EQ(sets.Out.Connection, 2u);
    EXPECT_EQ(sets.Meta.NumAggs, 1u);
    EXPECT_EQ(sets.Table.Layout, EAggregateTableLayout::Row);
    EXPECT_EQ(sets.Table.InitialGroups, 12);
    EXPECT_EQ(sets.Sets, (std::vector<std::vector<size_t>>{{0, 1}, {0}, {}}));
    EXPECT_EQ(sets.NumKeys, 2u);
}

TEST(GraphSpec, JoinAndJoinHashShuffleRecordsRoundTrip)
{
    using namespace NQumir::NAst;
    const TTypePtr inputType = std::make_shared<TStructType>(std::vector<std::pair<std::string, TTypePtr>>{
        {"k", std::make_shared<TIntegerType>(TIntegerType::I32)},
    });
    TGraphSpec spec;
    spec.Nodes.push_back(Node("hash-shuffle", "join-right-repartition", THashShuffleNode{
        .In = {0, 2},
        .Out = {{1, 2}},
        .Hash = TJoinHashPlanData{.Kernel = 5, .Entry = 1, .KeyColumns = {1, 0}},
        .InputType = inputType,
    }));
    spec.Nodes.push_back(Node("join", "join inner", THashJoinNode{
        .Left = {1, 0},
        .Right = {2, 0},
        .Out = {3, 1},
        .Kernels = {.LeftKeyColumns = {0, 2}, .RightKeyColumns = {1, 3}, .Kernel = 6},
        .JoinType = EJoinType::LeftAnti,
        .BuildSide = EJoinBuildSide::Right,
    }));
    spec.Nodes.push_back(Node("join", "join left", TRowJoinNode{
        .Left = {1, 1},
        .Right = {2, 1},
        .Out = {3, 0},
        .Kernels = {
            .RetainsBuildBatches = true,
            .EmitsByMark = true,
            .KeepsNullKeyRows = true,
            .BuildKeyColumns = {4},
            .ProbeKeyColumns = {5},
            .Kernel = 7,
            .ProbeMeasureKernel = 8,
            .ProbeMeasureWidths = {8, -1},
        },
        .BuildSide = EJoinBuildSide::Left,
        .ProbeHoldBytes = 1 << 24,
    }));
    spec.Nodes.push_back(Node("join", "cross-join full", TCrossJoinNode{
        .Left = {4, 2},
        .Right = {5, 2},
        .Out = {6, 2},
        .Kernel = 9,
        .JoinType = EJoinType::Full,
    }));

    const auto bytes = SerializeGraphSpec(spec);
    const auto back = DeserializeGraphSpec(bytes);
    EXPECT_EQ(SerializeGraphSpec(back), bytes);
    for (size_t size = 0; size < bytes.size(); ++size) {
        EXPECT_THROW(DeserializeGraphSpec(std::string_view(bytes).substr(0, size)),
            std::runtime_error) << size;
    }

    ASSERT_EQ(back.Nodes.size(), 4u);
    const auto& shuffle = std::get<THashShuffleNode>(back.Nodes[0].Body);
    const auto& hash = std::get<TJoinHashPlanData>(shuffle.Hash);
    EXPECT_EQ(hash.Kernel, 5u);
    EXPECT_EQ(hash.Entry, 1u);
    EXPECT_EQ(hash.KeyColumns, (std::vector<int64_t>{1, 0}));
    EXPECT_EQ(PrintTypeText(shuffle.InputType), PrintTypeText(inputType));

    const auto& hashJoin = std::get<THashJoinNode>(back.Nodes[1].Body);
    EXPECT_EQ(hashJoin.Left.Connection, 1u);
    EXPECT_EQ(hashJoin.Right.Connection, 2u);
    EXPECT_EQ(hashJoin.Out.Lane, 1u);
    EXPECT_EQ(hashJoin.Kernels.LeftKeyColumns, (std::vector<int64_t>{0, 2}));
    EXPECT_EQ(hashJoin.Kernels.RightKeyColumns, (std::vector<int64_t>{1, 3}));
    EXPECT_EQ(hashJoin.Kernels.Kernel, 6u);
    EXPECT_EQ(hashJoin.JoinType, EJoinType::LeftAnti);
    EXPECT_EQ(hashJoin.BuildSide, EJoinBuildSide::Right);

    const auto& rowJoin = std::get<TRowJoinNode>(back.Nodes[2].Body);
    EXPECT_EQ(rowJoin.Left.Lane, 1u);
    EXPECT_EQ(rowJoin.Right.Lane, 1u);
    EXPECT_EQ(rowJoin.Out.Connection, 3u);
    const auto& kernels = rowJoin.Kernels;
    EXPECT_TRUE(kernels.RetainsBuildBatches);
    EXPECT_TRUE(kernels.EmitsByMark);
    EXPECT_TRUE(kernels.KeepsNullKeyRows);
    EXPECT_EQ(kernels.BuildKeyColumns, (std::vector<int64_t>{4}));
    EXPECT_EQ(kernels.ProbeKeyColumns, (std::vector<int64_t>{5}));
    EXPECT_EQ(kernels.Kernel, 7u);
    EXPECT_EQ(kernels.ProbeMeasureKernel, 8u);
    EXPECT_EQ(kernels.ProbeMeasureWidths, (std::vector<int64_t>{8, -1}));
    EXPECT_EQ(rowJoin.BuildSide, EJoinBuildSide::Left);
    EXPECT_EQ(rowJoin.ProbeHoldBytes, 1u << 24);

    const auto& cross = std::get<TCrossJoinNode>(back.Nodes[3].Body);
    EXPECT_EQ(cross.Left.Connection, 4u);
    EXPECT_EQ(cross.Right.Connection, 5u);
    EXPECT_EQ(cross.Out.Connection, 6u);
    EXPECT_EQ(cross.Out.Lane, 2u);
    EXPECT_EQ(cross.Kernel, 9u);
    EXPECT_EQ(cross.JoinType, EJoinType::Full);
}

TEST(GraphSpec, LimitAndLateMaterializeRecordsRoundTrip)
{
    TGraphSpec spec;
    spec.Nodes.push_back(Node("limit", "limit", TLimitNode{
        .In = {0, 1},
        .Out = {1, 2},
        .Limit = 1'000'000'000'000,
        .Offset = 7,
    }));
    spec.Nodes.push_back(Node("late-lookup", "late-lookup", TLateLookupNode{
        .In = {1, 3},
        .Out = {2, 4},
        .Path = "/data/hits.parquet",
        .Columns = {"URL", "Title"},
        .LocatorColumn = 2,
    }));
    spec.Nodes.push_back(Node("late-materialize", "late-materialize", TLateMergeNode{
        .In = {{2, 0}, {2, 1}, {2, 2}},
        .Out = {3, 5},
    }));

    const auto bytes = SerializeGraphSpec(spec);
    const auto back = DeserializeGraphSpec(bytes);
    EXPECT_EQ(SerializeGraphSpec(back), bytes);
    for (size_t size = 0; size < bytes.size(); ++size) {
        EXPECT_THROW(DeserializeGraphSpec(std::string_view(bytes).substr(0, size)),
            std::runtime_error) << size;
    }

    ASSERT_EQ(back.Nodes.size(), 3u);
    const auto& limit = std::get<TLimitNode>(back.Nodes[0].Body);
    EXPECT_EQ(limit.In.Lane, 1u);
    EXPECT_EQ(limit.Out.Connection, 1u);
    EXPECT_EQ(limit.Out.Lane, 2u);
    EXPECT_EQ(limit.Limit, 1'000'000'000'000);
    EXPECT_EQ(limit.Offset, 7);

    const auto& lookup = std::get<TLateLookupNode>(back.Nodes[1].Body);
    EXPECT_EQ(lookup.In.Connection, 1u);
    EXPECT_EQ(lookup.In.Lane, 3u);
    EXPECT_EQ(lookup.Out.Lane, 4u);
    EXPECT_EQ(lookup.Path, "/data/hits.parquet");
    EXPECT_EQ(lookup.Columns, (std::vector<std::string>{"URL", "Title"}));
    EXPECT_EQ(lookup.LocatorColumn, 2);

    const auto& merge = std::get<TLateMergeNode>(back.Nodes[2].Body);
    ASSERT_EQ(merge.In.size(), 3u);
    EXPECT_EQ(merge.In[2].Connection, 2u);
    EXPECT_EQ(merge.In[2].Lane, 2u);
    EXPECT_EQ(merge.Out.Connection, 3u);
    EXPECT_EQ(merge.Out.Lane, 5u);
}

TEST(GraphSpec, CteRecordsAndTablesRoundTrip)
{
    TGraphSpec spec;
    spec.CteSpools = {{.Segments = 4}, {.Segments = 1}};
    spec.CteConsumerStates = {{.Spool = 1}, {.Spool = 0}, {.Spool = 0}};
    spec.Nodes.push_back(Node("cte-producer", "cte-producer 2", TCteProducerNode{
        .In = {0, 3},
        .Out = {1, 3},
        .Spool = 0,
        .Segment = 3,
    }));
    spec.Nodes.push_back(Node("cte-publish", "cte-publish 2", TCtePublishNode{
        .In = {1, 0},
        .Out = {2, 0},
        .Spool = 1,
    }));
    // Lane 2 of reference 1 over a four-segment spool.
    spec.Nodes.push_back(Node("cte-consumer", "cte-consumer 2", TCteConsumerNode{
        .In = {2, 6},
        .Out = {3, 2},
        .State = 2,
    }));

    const auto bytes = SerializeGraphSpec(spec);
    const auto back = DeserializeGraphSpec(bytes);
    EXPECT_EQ(SerializeGraphSpec(back), bytes);
    for (size_t size = 0; size < bytes.size(); ++size) {
        EXPECT_THROW(DeserializeGraphSpec(std::string_view(bytes).substr(0, size)),
            std::runtime_error) << size;
    }

    ASSERT_EQ(back.CteSpools.size(), 2u);
    EXPECT_EQ(back.CteSpools[0].Segments, 4u);
    EXPECT_EQ(back.CteSpools[1].Segments, 1u);
    ASSERT_EQ(back.CteConsumerStates.size(), 3u);
    EXPECT_EQ(back.CteConsumerStates[0].Spool, 1u);
    EXPECT_EQ(back.CteConsumerStates[1].Spool, 0u);
    EXPECT_EQ(back.CteConsumerStates[2].Spool, 0u);

    ASSERT_EQ(back.Nodes.size(), 3u);
    const auto& producer = std::get<TCteProducerNode>(back.Nodes[0].Body);
    EXPECT_EQ(producer.In.Lane, 3u);
    EXPECT_EQ(producer.Out.Connection, 1u);
    EXPECT_EQ(producer.Out.Lane, 3u);
    EXPECT_EQ(producer.Spool, 0u);
    EXPECT_EQ(producer.Segment, 3u);

    const auto& publish = std::get<TCtePublishNode>(back.Nodes[1].Body);
    EXPECT_EQ(publish.In.Connection, 1u);
    EXPECT_EQ(publish.Out.Connection, 2u);
    EXPECT_EQ(publish.Spool, 1u);

    const auto& consumer = std::get<TCteConsumerNode>(back.Nodes[2].Body);
    EXPECT_EQ(consumer.In.Connection, 2u);
    EXPECT_EQ(consumer.In.Lane, 6u);
    EXPECT_EQ(consumer.Out.Connection, 3u);
    EXPECT_EQ(consumer.Out.Lane, 2u);
    EXPECT_EQ(consumer.State, 2u);
}

TEST(GraphSpec, RuntimeFilterRecordsAndTableRoundTrip)
{
    using namespace NQumir::NAst;
    TGraphSpec spec;
    spec.RuntimeFilterKernel = 11;
    spec.RuntimeFilterBindings = {
        {.Id = 3, .ProducerCount = 4, .BloomBytes = 1u << 21},
        {.Id = 9, .ProducerCount = 1, .BloomBytes = 64},
    };
    spec.Nodes.push_back(Node("runtime-filter", "runtime-filter 9", TRuntimeFilterNode{
        .In = {0, 2},
        .Out = {1, 3},
        .Hash = {.Kernel = 5, .Entry = 1, .KeyColumns = {2, 0}},
        .Filter = 1,
        .AuxProducers = {7, 8, 9},
    }));
    spec.Nodes.push_back(Node("hash-shuffle", "join-right-repartition", THashShuffleNode{
        .In = {2, 1},
        .Out = {{3, 0}},
        .Hash = TJoinHashPlanData{.Kernel = 5, .Entry = 1, .KeyColumns = {0}},
        .InputType = std::make_shared<TStructType>(std::vector<std::pair<std::string, TTypePtr>>{
            {"k", std::make_shared<TIntegerType>(TIntegerType::I64)},
        }),
        .ProducedFilter = 0,
    }));

    const auto bytes = SerializeGraphSpec(spec);
    const auto back = DeserializeGraphSpec(bytes);
    EXPECT_EQ(SerializeGraphSpec(back), bytes);
    for (size_t size = 0; size < bytes.size(); ++size) {
        EXPECT_THROW(DeserializeGraphSpec(std::string_view(bytes).substr(0, size)),
            std::runtime_error) << size;
    }

    ASSERT_TRUE(back.RuntimeFilterKernel);
    EXPECT_EQ(*back.RuntimeFilterKernel, 11u);
    ASSERT_EQ(back.RuntimeFilterBindings.size(), 2u);
    EXPECT_EQ(back.RuntimeFilterBindings[0].Id, 3u);
    EXPECT_EQ(back.RuntimeFilterBindings[0].ProducerCount, 4u);
    EXPECT_EQ(back.RuntimeFilterBindings[0].BloomBytes, 1u << 21);
    EXPECT_EQ(back.RuntimeFilterBindings[1].Id, 9u);
    EXPECT_EQ(back.RuntimeFilterBindings[1].BloomBytes, 64u);

    ASSERT_EQ(back.Nodes.size(), 2u);
    const auto& filter = std::get<TRuntimeFilterNode>(back.Nodes[0].Body);
    EXPECT_EQ(filter.In.Lane, 2u);
    EXPECT_EQ(filter.Out.Connection, 1u);
    EXPECT_EQ(filter.Out.Lane, 3u);
    EXPECT_EQ(filter.Hash.Kernel, 5u);
    EXPECT_EQ(filter.Hash.Entry, 1u);
    EXPECT_EQ(filter.Hash.KeyColumns, (std::vector<int64_t>{2, 0}));
    EXPECT_EQ(filter.Filter, 1u);
    EXPECT_EQ(filter.AuxProducers, (std::vector<uint32_t>{7, 8, 9}));

    const auto& shuffle = std::get<THashShuffleNode>(back.Nodes[1].Body);
    ASSERT_TRUE(shuffle.ProducedFilter);
    EXPECT_EQ(*shuffle.ProducedFilter, 0u);
    EXPECT_FALSE(shuffle.UseInputHash);
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

    // A shuffle needs an output per partition, each inside the graph; kernel 0
    // has a slot, so the outputs are what throws.
    spec = FanOutSpec();
    spec.Nodes[1].Body = THashShuffleNode{.In = {0, 0}};
    const std::vector<std::shared_ptr<TKernelSlot>> slots{std::make_shared<TKernelSlot>()};
    EXPECT_THROW(MaterializeGraph(spec, &sink, slots), std::runtime_error);
    spec.Nodes[1].Body = THashShuffleNode{.In = {0, 0}, .Out = {{1, 0}, {9, 0}}};
    EXPECT_THROW(MaterializeGraph(spec, &sink, slots), std::runtime_error);

    // A join hash has entries 0 and 1 only.
    spec = FanOutSpec();
    spec.Nodes[1].Body = THashShuffleNode{.In = {0, 0}, .Out = {{1, 0}}, .Hash = TJoinHashPlanData{.Entry = 2}};
    EXPECT_THROW(MaterializeGraph(spec, &sink, slots), std::runtime_error);

    // CTE records index the spool and consumer state tables.
    spec = FanOutSpec();
    spec.Nodes[1].Body = TCtePublishNode{.In = {0, 0}, .Out = {1, 0}};
    EXPECT_THROW(MaterializeGraph(spec, &sink), std::runtime_error);
    spec.CteSpools = {{.Segments = 2}};
    EXPECT_NO_THROW(MaterializeGraph(spec, &sink));
    spec.Nodes[1].Body = TCteProducerNode{.In = {0, 0}, .Out = {1, 0}, .Segment = 2};
    EXPECT_THROW(MaterializeGraph(spec, &sink), std::runtime_error);
    spec.Nodes[1].Body = TCteConsumerNode{.In = {0, 0}, .Out = {1, 0}};
    EXPECT_THROW(MaterializeGraph(spec, &sink), std::runtime_error);
    spec.CteConsumerStates = {{.Spool = 0}};
    EXPECT_NO_THROW(MaterializeGraph(spec, &sink));
    spec.CteConsumerStates = {{.Spool = 1}};
    EXPECT_THROW(MaterializeGraph(spec, &sink), std::runtime_error);

    // A pass-through that computes a column needs its kernel.
    spec = FanOutSpec();
    spec.Nodes[1].Body = TPartialAggregateNode{
        .In = {0, 0}, .Out = {1, 0}, .PassThrough = TProjectPlanData{.ComputedWidths = {8}}};
    EXPECT_THROW(MaterializeGraph(spec, &sink, slots), std::runtime_error);

    // Runtime filters need the runtime-filter kernel; records index the filter
    // table and the nodes.
    spec = FanOutSpec();
    spec.RuntimeFilterBindings = {{.Id = 3, .ProducerCount = 1}};
    EXPECT_THROW(MaterializeGraph(spec, &sink, slots), std::runtime_error);
    spec.RuntimeFilterKernel = 0;
    EXPECT_NO_THROW(MaterializeGraph(spec, &sink, slots));
    spec.Nodes[1].Body = THashShuffleNode{.In = {0, 0}, .Out = {{1, 0}}, .ProducedFilter = 1};
    EXPECT_THROW(MaterializeGraph(spec, &sink, slots), std::runtime_error);
    spec.Nodes[1].Body = TRuntimeFilterNode{.In = {0, 0}, .Out = {1, 0}, .Filter = 1};
    EXPECT_THROW(MaterializeGraph(spec, &sink, slots), std::runtime_error);
    spec.Nodes[1].Body = TRuntimeFilterNode{.In = {0, 0}, .Out = {1, 0}, .AuxProducers = {4}};
    EXPECT_THROW(MaterializeGraph(spec, &sink, slots), std::runtime_error);
    spec.Nodes[1].Body = TRuntimeFilterNode{.In = {0, 0}, .Out = {1, 0}, .AuxProducers = {3}};
    EXPECT_NO_THROW(MaterializeGraph(spec, &sink, slots));
}

TEST(GraphSpec, RebindPortMovesTheMatchingPortOfItsKind)
{
    TNodeBody body = THashJoinNode{.Left = {0, 1}, .Right = {1, 0}, .Out = {2, 0}};
    EXPECT_FALSE(RebindPort(body, EPortKind::Output, {1, 0}, {5, 0}));
    EXPECT_TRUE(RebindPort(body, EPortKind::Input, {1, 0}, {5, 0}));
    EXPECT_EQ(std::get<THashJoinNode>(body).Right, (TPortSpec{5, 0}));
    EXPECT_EQ(std::get<THashJoinNode>(body).Left, (TPortSpec{0, 1}));

    body = THashShuffleNode{.In = {0, 0}, .Out = {{1, 0}, {1, 1}}};
    EXPECT_TRUE(RebindPort(body, EPortKind::Output, {1, 1}, {6, 0}));
    EXPECT_EQ(std::get<THashShuffleNode>(body).Out, (std::vector<TPortSpec>{{1, 0}, {6, 0}}));
}

// Chain a: remote source 0 -> forward 1 -> remote sink 2 over connections 0, 1;
// chain b: nodes 3..5 over connections 2, 3, with a dependency edge.
TEST(GraphSpec, SubgraphRenumbersItsNodesConnectionsAndExternals)
{
    TGraphSpec spec;
    spec.Connections = {{.DebugName = "a0"}, {.DebugName = "a1"}, {.DebugName = "b0"}, {.DebugName = "b1"}};
    for (uint32_t chain = 0; chain < 2; ++chain) {
        const uint32_t in = 2 * chain;
        spec.Nodes.push_back(Node("source", "", TRemoteSourceNode{.Out = {in, 0}}));
        spec.Nodes.push_back(Node("forward", "", TForwardNode{.In = {in, 0}, .Out = {in + 1, 0}}));
        spec.Nodes.push_back(Node("sink", "", TRemoteSinkNode{.In = {in + 1, 0}}));
        spec.Edges.push_back({.Src = 3 * chain, .Dst = 3 * chain + 1, .Connection = in});
        spec.Edges.push_back({.Src = 3 * chain + 1, .Dst = 3 * chain + 2, .Connection = in + 1});
    }
    spec.Edges.push_back({.Src = 3, .Dst = 5, .Kind = ETaskEdgeKind::Dependency});
    spec.Externals = {{.Id = 0, .Node = 2}, {.Id = 1, .Node = 3}, {.Id = 2, .Node = 0}, {.Id = 3, .Node = 5}};
    spec.CteSpools = {{.Segments = 1}};

    const std::vector<uint32_t> chainB{3, 4, 5};
    const auto part = SubgraphSpec(spec, chainB);
    ASSERT_EQ(part.Connections.size(), 2u);
    EXPECT_EQ(part.Connections[0].DebugName, "b0");
    EXPECT_EQ(part.Connections[1].DebugName, "b1");
    ASSERT_EQ(part.Nodes.size(), 3u);
    EXPECT_EQ(std::get<TForwardNode>(part.Nodes[1].Body).In, (TPortSpec{0, 0}));
    EXPECT_EQ(std::get<TForwardNode>(part.Nodes[1].Body).Out, (TPortSpec{1, 0}));
    ASSERT_EQ(part.Edges.size(), 3u);
    EXPECT_EQ(part.Edges[1].Connection, 1u);
    EXPECT_EQ(part.Edges[2].Src, 0u);
    EXPECT_EQ(part.Edges[2].Dst, 2u);
    ASSERT_EQ(part.Externals.size(), 2u);
    EXPECT_EQ(part.Externals[0].Id, 1u);
    EXPECT_EQ(part.Externals[0].Node, 0u);
    EXPECT_EQ(part.Externals[1].Id, 3u);
    EXPECT_EQ(part.Externals[1].Node, 2u);
    EXPECT_EQ(part.CteSpools.size(), 1u);
    std::string error;
    EXPECT_TRUE(MaterializeGraph(part)->Seal(&error)) << error;

    // Edges and CTE spools must not cross the subgraph's border.
    EXPECT_THROW(SubgraphSpec(spec, std::vector<uint32_t>{0, 1}), std::runtime_error);
    EXPECT_THROW(SubgraphSpec(spec, std::vector<uint32_t>{4, 3, 5}), std::runtime_error);
    spec.Nodes[1].Body = TCteProducerNode{.In = {0, 0}, .Out = {1, 0}};
    spec.Nodes[4].Body = TCtePublishNode{.In = {2, 0}, .Out = {3, 0}};
    EXPECT_THROW(SubgraphSpec(spec, chainB), std::runtime_error);
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
