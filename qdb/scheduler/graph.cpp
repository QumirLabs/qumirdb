#include <qdb/scheduler/graph.h>

#include <qdb/scheduler/port.h>

#include <algorithm>
#include <cassert>
#include <deque>
#include <format>
#include <map>
#include <numeric>
#include <optional>
#include <ostream>
#include <ranges>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace NQdb {
namespace NScheduler {
namespace {

void SetError(std::string* error, std::string message) {
    if (error) {
        *error = std::move(message);
    }
}

bool IsRowsetEdge(const TTaskEdge* edge) {
    return edge->Kind == ETaskEdgeKind::Rowset;
}

const char* ConnectionKindName(EConnectionKind kind) {
    switch (kind) {
        case EConnectionKind::OneToOne:
            return "one-to-one";
        case EConnectionKind::Gather:
            return "gather";
        case EConnectionKind::Broadcast:
            return "broadcast";
    }
    return "unknown";
}

void Rebind(TTaskNode& node, const TPortRebind& rebind) {
    if (!node.Task->RebindPort(rebind)) {
        throw std::logic_error(std::format(
            "split plan: task n{} has no {} port to rebind",
            node.ExecutionSlot,
            rebind.Kind == EPortKind::Input ? "input" : "output"));
    }
}

// Replaces split.Edge by Src -> remote sink and remote source -> Dst. Both
// start as copies of it; the rebound side then uses lane 0 of the new connection.
void Cut(
    TTaskGraph& graph,
    TEdgeSplit& split,
    std::vector<std::unique_ptr<TTaskEdge>>& edges,
    std::vector<std::optional<int>>& colors)
{
    const auto& edge = *split.Edge;
    auto& connection = graph.AddConnection(std::move(split.NewConnection));
    auto& sink = graph.AddOwnedNode(std::move(split.SinkTask));
    sink.DebugKind = "remote-sink";
    auto& source = graph.AddOwnedNode(std::move(split.SourceTask));
    source.DebugKind = "remote-source";
    colors.resize(graph.Nodes().size());
    colors[sink.ExecutionSlot] = split.SinkColor;
    colors[source.ExecutionSlot] = split.SourceColor;

    auto sinkEdge = std::make_unique<TTaskEdge>(edge);
    sinkEdge->Dst = &sink;
    sinkEdge->ExecInput = 0;
    auto sourceEdge = std::make_unique<TTaskEdge>(edge);
    sourceEdge->Src = &source;
    if (split.ReboundPort == EReboundPort::SourceOutput) {
        Rebind(*edge.Src, {
            .Kind = EPortKind::Output,
            .From = edge.Connection,
            .FromLane = edge.SrcLane,
            .To = &connection,
        });
        sinkEdge->Connection = &connection;
        sinkEdge->SrcLane = 0;
        sinkEdge->DstLane = 0;
    } else {
        Rebind(*edge.Dst, {
            .Kind = EPortKind::Input,
            .From = edge.Connection,
            .FromLane = edge.DstLane,
            .To = &connection,
        });
        sourceEdge->Connection = &connection;
        sourceEdge->SrcLane = 0;
        sourceEdge->DstLane = 0;
    }
    Rebind(sink, {.Kind = EPortKind::Input, .To = sinkEdge->Connection, .ToLane = sinkEdge->DstLane});
    Rebind(source, {.Kind = EPortKind::Output, .To = sourceEdge->Connection, .ToLane = sourceEdge->SrcLane});
    edges.push_back(std::move(sinkEdge));
    edges.push_back(std::move(sourceEdge));
}

// Union-find over rowset edges: each piece takes the color of its remote
// sinks and sources. A dependency edge is a scheduling gate with no transport
// between fragments yet, so it must stay within one color.
std::vector<std::vector<TTaskNode*>> GroupByColor(
    const std::vector<std::unique_ptr<TTaskNode>>& nodes,
    const std::vector<std::unique_ptr<TTaskEdge>>& edges,
    const std::vector<std::optional<int>>& colors)
{
    std::vector<size_t> parent(nodes.size());
    std::iota(parent.begin(), parent.end(), size_t{0});
    auto find = [&](size_t slot) {
        while (parent[slot] != slot) {
            parent[slot] = parent[parent[slot]];
            slot = parent[slot];
        }
        return slot;
    };
    for (const auto& edge : edges) {
        if (IsRowsetEdge(edge.get())) {
            parent[find(edge->Src->ExecutionSlot)] = find(edge->Dst->ExecutionSlot);
        }
    }

    std::vector<std::optional<int>> pieceColors(nodes.size());
    for (size_t slot = 0; slot < colors.size(); ++slot) {
        if (!colors[slot]) {
            continue;
        }
        auto& pieceColor = pieceColors[find(slot)];
        if (pieceColor && *pieceColor != *colors[slot]) {
            throw std::logic_error(std::format(
                "split plan gives one subgraph colors {} and {}", *pieceColor, *colors[slot]));
        }
        pieceColor = colors[slot];
    }
    auto colorOf = [&](const TTaskNode& node) {
        const auto& color = pieceColors[find(node.ExecutionSlot)];
        if (!color) {
            throw std::logic_error(std::format(
                "split plan leaves task n{} without a color", node.ExecutionSlot));
        }
        return *color;
    };

    for (const auto& edge : edges) {
        if (!IsRowsetEdge(edge.get()) && colorOf(*edge->Src) != colorOf(*edge->Dst)) {
            throw std::logic_error(std::format(
                "split plan: dependency edge n{} -> n{} crosses colors {} and {}",
                edge->Src->ExecutionSlot,
                edge->Dst->ExecutionSlot,
                colorOf(*edge->Src),
                colorOf(*edge->Dst)));
        }
    }

    std::map<int, std::vector<TTaskNode*>> roots;
    for (const auto& node : nodes) {
        const int color = colorOf(*node);
        if (IsRoot(*node)) {
            roots[color].push_back(node.get());
        }
    }
    std::vector<std::vector<TTaskNode*>> graphs;
    for (auto& [color, colorRoots] : roots) {
        graphs.push_back(std::move(colorRoots));
    }
    return graphs;
}

// `node` reads no more input: abandons each producer it was the last live consumer of.
void ReleaseProducers(const TTaskNode& node) {
    for (auto* edge : node.Inbound) {
        auto& producer = *edge->Src;
        if (producer.LiveConsumers.fetch_sub(1, std::memory_order_acq_rel) == 1
            && !IsRoot(producer)
            && !producer.Retired.exchange(true, std::memory_order_acq_rel))
        {
            ReleaseProducers(producer);
        }
    }
}

} // namespace

bool IsRoot(const TTaskNode& node) {
    return std::ranges::none_of(node.Outbound, IsRowsetEdge);
}

void Retire(TTaskNode& node) {
    if (!node.Retired.exchange(true, std::memory_order_acq_rel)) {
        ReleaseProducers(node);
    }
}

void CloseInputs(const TTaskNode& node) {
    for (const auto* edge : node.Inbound) {
        if (IsRowsetEdge(edge)) {
            edge->Connection->Close(edge->DstLane);
        }
    }
}

TTaskNode& TTaskGraph::AddNode(ITask& task) {
    CheckMutable();
    auto node = std::make_unique<TTaskNode>();
    node->Task = &task;
    node->ExecutionSlot = Nodes_.size();
    auto* ptr = node.get();
    Nodes_.push_back(std::move(node));
    return *ptr;
}

TTaskNode& TTaskGraph::AddOwnedNode(std::unique_ptr<ITask> task) {
    CheckMutable();
    auto* ptr = task.get();
    OwnedTasks_.push_back(std::move(task));
    return AddNode(*ptr);
}

void TTaskGraph::MarkExternal(TTaskNode& node, uint64_t id) {
    CheckMutable();
    if (std::ranges::none_of(Nodes_, [&](const auto& owned) { return owned.get() == &node; })) {
        throw std::logic_error("cannot mark a node from another task graph as external");
    }
    Externals_.push_back({id, &node});
}

TTaskNode* TSealedTaskGraph::ExternalNode(uint64_t id) const {
    auto it = std::ranges::lower_bound(Externals_, id, {}, &TExternalNode::Id);
    return it != Externals_.end() && it->Id == id ? it->Node : nullptr;
}

IConnection& TTaskGraph::AddConnection(std::unique_ptr<IConnection> connection) {
    CheckMutable();
    auto* ptr = connection.get();
    OwnedConnections_.push_back(std::move(connection));
    return *ptr;
}

TTaskEdge& TTaskGraph::AddEdge(
    TTaskNode& src,
    TTaskNode& dst,
    IConnection& connection,
    size_t srcLane,
    size_t dstLane,
    size_t execInput)
{
    CheckMutable();
    auto edge = std::make_unique<TTaskEdge>();
    edge->Src = &src;
    edge->Dst = &dst;
    edge->Connection = &connection;
    edge->SrcLane = srcLane;
    edge->DstLane = dstLane;
    edge->ExecInput = execInput;
    auto* ptr = edge.get();
    Edges_.push_back(std::move(edge));
    return *ptr;
}

TTaskEdge& TTaskGraph::AddOwnedEdge(
    TTaskNode& src,
    TTaskNode& dst,
    std::unique_ptr<IConnection> connection,
    size_t srcLane,
    size_t dstLane,
    size_t execInput)
{
    CheckMutable();
    if (!connection) {
        auto edge = std::make_unique<TTaskEdge>();
        edge->Src = &src;
        edge->Dst = &dst;
        edge->SrcLane = srcLane;
        edge->DstLane = dstLane;
        edge->ExecInput = execInput;
        auto* ptr = edge.get();
        Edges_.push_back(std::move(edge));
        return *ptr;
    }

    auto& stored = AddConnection(std::move(connection));
    return AddEdge(src, dst, stored, srcLane, dstLane, execInput);
}

TTaskEdge& TTaskGraph::AddDependencyEdge(
    TTaskNode& producer, TTaskNode& consumer)
{
    CheckMutable();
    auto edge = std::make_unique<TTaskEdge>();
    edge->Src = &producer;
    edge->Dst = &consumer;
    edge->Kind = ETaskEdgeKind::Dependency;
    auto* ptr = edge.get();
    Edges_.push_back(std::move(edge));
    return *ptr;
}

void TTaskGraph::Build() {
    Leaves_.clear();

    for (auto& node : Nodes_) {
        node->Inbound.clear();
        node->Outbound.clear();
        node->State.store(ETaskState::Idle, std::memory_order_release);
        node->Retired.store(false, std::memory_order_release);
    }

    // Validate() and ExternalNode() rely on Externals_ being sorted by id.
    std::ranges::stable_sort(Externals_, {}, &TExternalNode::Id);

    for (auto& edge : Edges_) {
        edge->Src->Outbound.push_back(edge.get());
        edge->Dst->Inbound.push_back(edge.get());
    }

    for (auto& node : Nodes_) {
        node->LiveConsumers.store(node->Outbound.size(), std::memory_order_release);
        if (std::ranges::none_of(node->Inbound, IsRowsetEdge)) {
            Leaves_.push_back(node.get());
        }
    }
}

TSealedTaskGraph TTaskGraph::Seal(std::string* error) {
    if (!Sealed_) {
        Build();
        if (!Validate(error)) {
            return {};
        }
        // One subgraph of all roots; only Apply() splits it.
        Graphs_.assign(1, {});
        for (const auto& node : Nodes_) {
            if (IsRoot(*node)) {
                Graphs_.front().push_back(node.get());
            }
        }
        Sealed_ = true;
    }
    return TSealedTaskGraph(Graphs_, Nodes_.size(), Externals_);
}

TSealedTaskGraph TTaskGraph::Apply(TSplitPlan&& plan) {
    if (!Sealed_) {
        throw std::logic_error("split plan applies only to a sealed task graph");
    }
    if (plan.Splits.empty()) {
        return TSealedTaskGraph(Graphs_, Nodes_.size(), Externals_);
    }
    if (Split_) {
        throw std::logic_error("task graph is already split");
    }
    std::unordered_map<const TTaskEdge*, TEdgeSplit*> splits;
    for (auto& split : plan.Splits) {
        if (!split.Edge || !split.NewConnection || !split.SinkTask || !split.SourceTask) {
            throw std::logic_error("split plan has an incomplete split");
        }
        if (!IsRowsetEdge(split.Edge)) {
            throw std::logic_error("split plan cuts a dependency edge");
        }
        if (!splits.emplace(split.Edge, &split).second) {
            throw std::logic_error("split plan cuts one edge twice");
        }
    }
    std::set<int> planColors;
    for (const auto& split : plan.Splits) {
        planColors.insert(split.SinkColor);
        planColors.insert(split.SourceColor);
    }
    if (*planColors.begin() != 0 || *planColors.rbegin() != static_cast<int>(planColors.size()) - 1) {
        throw std::logic_error("split plan colors must be 0..K-1 without gaps");
    }
    const auto known = std::ranges::count_if(Edges_, [&](const auto& edge) {
        return splits.contains(edge.get());
    });
    if (static_cast<size_t>(known) != splits.size()) {
        throw std::logic_error("split plan cuts an edge of another task graph");
    }

    Sealed_ = false;
    std::vector<std::optional<int>> colors(Nodes_.size());
    std::vector<std::unique_ptr<TTaskEdge>> edges;
    edges.reserve(Edges_.size() + splits.size());
    for (auto& edge : Edges_) {
        if (auto it = splits.find(edge.get()); it != splits.end()) {
            Cut(*this, *it->second, edges, colors);
        } else {
            edges.push_back(std::move(edge));
        }
    }
    Edges_ = std::move(edges);
    Build();
    std::string error;
    if (!Validate(&error)) {
        throw std::logic_error("split plan breaks the task graph: " + error);
    }
    Graphs_ = GroupByColor(Nodes_, Edges_, colors);
    Split_ = true;
    Sealed_ = true;
    return TSealedTaskGraph(Graphs_, Nodes_.size(), Externals_);
}

void TTaskGraph::CheckMutable() const {
    if (Sealed_) {
        throw std::logic_error("cannot modify a sealed task graph");
    }
}

bool TTaskGraph::Validate(std::string* error) const {
    if (Nodes_.empty()) {
        SetError(error, "task graph is empty");
        return false;
    }

    std::unordered_set<const TTaskNode*> nodes;
    nodes.reserve(Nodes_.size());
    size_t leafCount = 0;
    for (auto& node : Nodes_) {
        if (!node->Task) {
            SetError(error, "task graph contains a node without a task");
            return false;
        }
        nodes.insert(node.get());
        if (std::ranges::none_of(node->Inbound, IsRowsetEdge)) {
            ++leafCount;
        }
    }

    assert(std::ranges::is_sorted(Externals_, {}, &TExternalNode::Id));
    for (size_t i = 1; i < Externals_.size(); ++i) {
        if (Externals_[i].Id == Externals_[i - 1].Id) {
            SetError(error, std::format(
                "task graph has duplicate external node id {}", Externals_[i].Id));
            return false;
        }
    }

    if (leafCount == 0) {
        SetError(error, "task graph has no source leaf");
        return false;
    }

    for (auto& edge : Edges_) {
        if (!edge->Src || !edge->Dst) {
            SetError(error, "task graph contains an edge without endpoints");
            return false;
        }
        if (!nodes.contains(edge->Src) || !nodes.contains(edge->Dst)) {
            SetError(error, "task graph contains an edge with unknown endpoints");
            return false;
        }
        if (edge->Kind == ETaskEdgeKind::Dependency) {
            if (edge->Connection) {
                SetError(error, "dependency edge must not carry a rowset connection");
                return false;
            }
            continue;
        }
        if (!edge->Connection) {
            SetError(error, "task graph contains an edge without a connection");
            return false;
        }
        if (edge->SrcLane >= edge->Connection->SrcCount()) {
            SetError(error, "task graph contains an invalid source lane id");
            return false;
        }
        if (edge->DstLane >= edge->Connection->DstCount()) {
            SetError(error, "task graph contains an invalid destination lane id");
            return false;
        }
    }

    // A dependency edge must not turn a data-flow DAG into a scheduling cycle
    // (a probe cannot gate its own build-side producer). An acyclic graph has
    // a root downstream of every node, so a run from the roots pulls every task.
    std::unordered_map<const TTaskNode*, size_t> pending;
    for (const auto& node : Nodes_) {
        pending.emplace(node.get(), node->Inbound.size());
    }
    std::deque<const TTaskNode*> ready;
    for (const auto& [node, count] : pending) {
        if (count == 0) {
            ready.push_back(node);
        }
    }
    size_t visited = 0;
    while (!ready.empty()) {
        const auto* node = ready.front();
        ready.pop_front();
        ++visited;
        for (const auto* edge : node->Outbound) {
            if (--pending.at(edge->Dst) == 0) {
                ready.push_back(edge->Dst);
            }
        }
    }
    if (visited != Nodes_.size()) {
        SetError(error, "task graph contains a scheduling cycle");
        return false;
    }

    return true;
}

void TTaskGraph::SetConnectionStatsEnabled(bool enabled) {
    for (auto& connection : OwnedConnections_) {
        connection->SetStatsEnabled(enabled);
    }
}

void TTaskGraph::Print(std::ostream& out) const {
    std::unordered_map<const TTaskNode*, size_t> nodeIds;
    nodeIds.reserve(Nodes_.size());
    for (size_t i = 0; i < Nodes_.size(); ++i) {
        nodeIds.emplace(Nodes_[i].get(), i);
    }

    std::unordered_map<const IConnection*, size_t> connectionIds;
    connectionIds.reserve(OwnedConnections_.size());
    for (size_t i = 0; i < OwnedConnections_.size(); ++i) {
        connectionIds.emplace(OwnedConnections_[i].get(), i);
    }

    out << "nodes: " << Nodes_.size()
        << ", edges: " << Edges_.size()
        << ", connections: " << OwnedConnections_.size() << "\n";
    out << "leaves:";
    for (auto* leaf : Leaves_) {
        out << " n" << nodeIds[leaf];
    }
    out << "\n";
    for (size_t i = 0; i < Graphs_.size(); ++i) {
        out << "graph g" << i << " roots:";
        for (auto* root : Graphs_[i]) {
            out << " n" << nodeIds[root];
        }
        out << "\n";
    }

    for (size_t i = 0; i < OwnedConnections_.size(); ++i) {
        const auto& conn = OwnedConnections_[i];
        out << "conn c" << i
            << " " << (conn->DebugName().empty() ? "-" : conn->DebugName())
            << " " << ConnectionKindName(conn->Kind())
            << " src=" << conn->SrcCount()
            << " dst=" << conn->DstCount() << "\n";
    }

    for (size_t i = 0; i < Nodes_.size(); ++i) {
        const auto& node = Nodes_[i];
        out << "node n" << i
            << " in=" << node->Inbound.size()
            << " out=" << node->Outbound.size();
        if (!node->DebugKind.empty()) {
            out << " kind=" << node->DebugKind;
        }
        out << "\n";
    }

    for (size_t i = 0; i < Edges_.size(); ++i) {
        const auto& edge = Edges_[i];
        if (edge->Kind == ETaskEdgeKind::Dependency) {
            out << "dependency n" << nodeIds[edge->Src]
                << " -> n" << nodeIds[edge->Dst] << "\n";
            continue;
        }
        out << "edge e" << i
            << " n" << nodeIds[edge->Src]
            << "[" << edge->SrcLane << "] -> n"
            << nodeIds[edge->Dst]
            << "[" << edge->DstLane << "] via c"
            << connectionIds[edge->Connection] << "\n";
    }
}

void TTaskGraph::PrintConnectionStats(std::ostream& out) const {
    for (size_t i = 0; i < OwnedConnections_.size(); ++i) {
        const auto& conn = OwnedConnections_[i];
        auto stats = conn->Stats();
        out << "conn c" << i
            << " " << (conn->DebugName().empty() ? "-" : conn->DebugName())
            << " " << ConnectionKindName(conn->Kind())
            << " pushed=" << stats.Pushed
            << " popped=" << stats.Popped
            << " finished=" << stats.Finished
            << " blocked_push=" << stats.BlockedPush
            << " empty_fetch=" << stats.EmptyFetch
            << " finished_fetch=" << stats.FinishedFetch
            << "\n";
    }
}

std::span<const std::unique_ptr<IConnection>> TTaskGraph::Connections() const {
    return OwnedConnections_;
}

std::span<const TExternalNode> TTaskGraph::Externals() const {
    return Externals_;
}

const std::vector<std::unique_ptr<TTaskNode>>& TTaskGraph::Nodes() const {
    return Nodes_;
}

const std::vector<std::unique_ptr<TTaskEdge>>& TTaskGraph::Edges() const {
    return Edges_;
}

const std::vector<TTaskNode*>& TTaskGraph::Leaves() const {
    return Leaves_;
}

const std::vector<std::vector<TTaskNode*>>& TTaskGraph::Graphs() const {
    return Graphs_;
}

} // namespace NScheduler
} // namespace NQdb
