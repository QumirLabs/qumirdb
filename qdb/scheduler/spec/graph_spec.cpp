#include <qdb/scheduler/spec/graph_spec.h>

#include <qdb/scheduler/connections/local.h>
#include <qdb/scheduler/tasks/runtime_adapter.h>
#include <qdb/utils/binary_io.h>

#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace NQdb {
namespace NScheduler {

namespace {

[[noreturn]] void Invalid(const std::string& what)
{
    throw std::runtime_error("invalid task graph spec: " + what);
}

std::unique_ptr<IConnection> MakeConnection(const TConnectionSpec& spec)
{
    std::unique_ptr<IConnection> connection;
    switch (spec.Kind) {
        case EConnectionKind::OneToOne:
            connection = std::make_unique<TOneToOneConnection>(spec.Capacity);
            break;
        case EConnectionKind::Gather:
            connection = std::make_unique<TGatherConnection>(spec.Capacity);
            break;
        case EConnectionKind::HashShuffle:
            connection = std::make_unique<THashShuffleConnection>(
                spec.Capacity, TRepartitionSpec{.Keys = spec.RepartitionKeys});
            break;
        case EConnectionKind::Broadcast:
            connection = std::make_unique<TBroadcastConnection>(spec.Capacity);
            break;
        default:
            Invalid("unknown connection kind");
    }
    connection->Resize(spec.SrcCount, spec.DstCount);
    connection->SetDebugName(spec.DebugName);
    std::vector<TValueLayout> layouts;
    layouts.reserve(spec.RowLayouts.size());
    for (const auto& layout : spec.RowLayouts) {
        layouts.push_back(TValueLayout{
            .Width = layout.Width, .Bitmap = layout.Bitmap});
    }
    connection->SetRowLayouts(std::move(layouts));
    return connection;
}

IConnection& ConnectionAt(
    const std::vector<IConnection*>& connections, uint32_t index)
{
    if (index >= connections.size()) {
        Invalid("connection index is out of range");
    }
    return *connections[index];
}

TInputPort InputPort(const std::vector<IConnection*>& connections, const TPortSpec& port)
{
    return TInputPort{.Connection = &ConnectionAt(connections, port.Connection), .Lane = port.Lane};
}

TOutputPort OutputPort(const std::vector<IConnection*>& connections, const TPortSpec& port)
{
    return TOutputPort{.Connection = &ConnectionAt(connections, port.Connection), .Lane = port.Lane};
}

std::unique_ptr<ITask> MakeTask(
    const TNodeBody& body, const std::vector<IConnection*>& connections, ISink* sink)
{
    return std::visit([&](const auto& node) -> std::unique_ptr<ITask> {
        using TNode = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<TNode, TForwardNode>) {
            auto code = std::make_shared<TUnaryCode>([](void*, TRowSet&) {});
            return std::make_unique<TUnaryTask>(
                std::move(code), std::shared_ptr<void>{},
                InputPort(connections, node.In), OutputPort(connections, node.Out));
        } else if constexpr (std::is_same_v<TNode, TRemoteSourceNode>) {
            return std::make_unique<TSourceTask>(
                nullptr, nullptr, OutputPort(connections, node.Out));
        } else if constexpr (std::is_same_v<TNode, TRemoteSinkNode>) {
            return std::make_unique<TSinkTask>(
                nullptr, nullptr, InputPort(connections, node.In));
        } else {
            static_assert(std::is_same_v<TNode, TSinkWriterNode>);
            if (!sink) {
                Invalid("a sink writer node needs a sink");
            }
            auto code = std::make_shared<TSinkCode>([](void* state, const TRowSet& rowSet) {
                static_cast<ISink*>(state)->Write(rowSet);
                return ETaskResult::OK;
            });
            // Aliasing shared_ptr: the task's state points at the caller's sink.
            std::shared_ptr<void> state(std::shared_ptr<void>{}, sink);
            return std::make_unique<TSinkTask>(
                std::move(code), std::move(state), InputPort(connections, node.In));
        }
    }, body);
}

TTaskNode& NodeAt(const std::vector<TTaskNode*>& nodes, uint32_t index)
{
    if (index >= nodes.size()) {
        Invalid("node index is out of range");
    }
    return *nodes[index];
}

TConnectionSpec ExtractConnection(const IConnection& connection)
{
    TConnectionSpec spec{
        .Kind = connection.Kind(),
        .SrcCount = static_cast<uint32_t>(connection.SrcCount()),
        .DstCount = static_cast<uint32_t>(connection.DstCount()),
        .DebugName = connection.DebugName(),
    };
    for (const auto& layout : connection.RowLayouts()) {
        spec.RowLayouts.push_back(TRowLayoutSpec{.Width = layout.Width, .Bitmap = layout.Bitmap});
    }
    switch (spec.Kind) {
        case EConnectionKind::OneToOne:
            spec.Capacity = static_cast<const TOneToOneConnection&>(connection).Capacity();
            break;
        case EConnectionKind::Gather:
            spec.Capacity = static_cast<const TGatherConnection&>(connection).Capacity();
            break;
        case EConnectionKind::HashShuffle: {
            const auto& shuffle = static_cast<const THashShuffleConnection&>(connection);
            spec.Capacity = shuffle.Capacity();
            spec.RepartitionKeys = shuffle.Repartition().Keys;
            break;
        }
        case EConnectionKind::Broadcast:
            spec.Capacity = static_cast<const TBroadcastConnection&>(connection).Capacity();
            break;
    }
    return spec;
}

} // namespace

std::string SerializeGraphSpec(const TGraphSpec& spec)
{
    TBinaryWriter writer;
    writer(spec);
    return writer.Take();
}

TGraphSpec DeserializeGraphSpec(std::string_view bytes)
{
    TBinaryReader reader(bytes);
    TGraphSpec spec;
    reader(spec);
    if (!reader.AtEnd()) {
        throw std::runtime_error("task graph spec has trailing bytes");
    }
    return spec;
}

std::unique_ptr<TTaskGraph> MaterializeGraph(const TGraphSpec& spec, ISink* sink)
{
    auto graph = std::make_unique<TTaskGraph>();

    std::vector<IConnection*> connections;
    connections.reserve(spec.Connections.size());
    for (const auto& connectionSpec : spec.Connections) {
        connections.push_back(&graph->AddConnection(MakeConnection(connectionSpec)));
    }

    std::vector<TTaskNode*> nodes;
    nodes.reserve(spec.Nodes.size());
    for (const auto& nodeSpec : spec.Nodes) {
        auto& node = graph->AddOwnedNode(MakeTask(nodeSpec.Body, connections, sink));
        node.ExecStageId = nodeSpec.ExecStageId;
        node.TaskGroupId = nodeSpec.TaskGroupId;
        node.DebugKind = nodeSpec.DebugKind;
        node.DebugLabel = nodeSpec.DebugLabel;
        nodes.push_back(&node);
    }

    for (const auto& edge : spec.Edges) {
        auto& src = NodeAt(nodes, edge.Src);
        auto& dst = NodeAt(nodes, edge.Dst);
        if (edge.Kind == ETaskEdgeKind::Dependency) {
            graph->AddDependencyEdge(src, dst);
            continue;
        }
        if (!edge.Connection) {
            Invalid("a rowset edge needs a connection");
        }
        graph->AddEdge(
            src, dst, ConnectionAt(connections, *edge.Connection),
            edge.SrcLane, edge.DstLane, edge.ExecInput);
    }

    for (const auto& external : spec.Externals) {
        graph->MarkExternal(NodeAt(nodes, external.Node), external.Id);
    }
    return graph;
}

TGraphSpec ExtractGraphSpec(const TTaskGraph& graph, std::vector<TNodeSpec> nodes)
{
    if (nodes.size() != graph.Nodes().size()) {
        throw std::runtime_error("ExtractGraphSpec needs one node record per graph node");
    }
    TGraphSpec spec;
    spec.Nodes = std::move(nodes);

    std::unordered_map<const IConnection*, uint32_t> connectionIds;
    for (const auto& connection : graph.Connections()) {
        connectionIds.emplace(connection.get(), static_cast<uint32_t>(spec.Connections.size()));
        spec.Connections.push_back(ExtractConnection(*connection));
    }

    std::unordered_map<const TTaskNode*, uint32_t> nodeIds;
    for (const auto& node : graph.Nodes()) {
        nodeIds.emplace(node.get(), static_cast<uint32_t>(nodeIds.size()));
    }

    for (const auto& edge : graph.Edges()) {
        TEdgeSpec edgeSpec{
            .Src = nodeIds.at(edge->Src),
            .Dst = nodeIds.at(edge->Dst),
            .Kind = edge->Kind,
            .SrcLane = static_cast<uint32_t>(edge->SrcLane),
            .DstLane = static_cast<uint32_t>(edge->DstLane),
            .ExecInput = static_cast<uint32_t>(edge->ExecInput),
        };
        if (edge->Connection) {
            edgeSpec.Connection = connectionIds.at(edge->Connection);
        }
        spec.Edges.push_back(edgeSpec);
    }

    for (const auto& external : graph.Externals()) {
        spec.Externals.push_back(TExternalSpec{
            .Id = external.Id, .Node = nodeIds.at(external.Node)});
    }
    return spec;
}

} // namespace NScheduler
} // namespace NQdb
