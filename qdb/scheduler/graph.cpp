#include <qdb/scheduler/graph.h>

#include <algorithm>
#include <deque>
#include <format>
#include <ostream>
#include <ranges>
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

const char* ConnectionKindName(EConnectionKind kind) {
    switch (kind) {
        case EConnectionKind::OneToOne:
            return "one-to-one";
        case EConnectionKind::Gather:
            return "gather";
        case EConnectionKind::HashShuffle:
            return "hash-shuffle";
        case EConnectionKind::Broadcast:
            return "broadcast";
    }
    return "unknown";
}

} // namespace

TTaskNode& TTaskGraph::AddNode(ITaskNode& task) {
    auto node = std::make_unique<TTaskNode>();
    node->Task = &task;
    auto* ptr = node.get();
    Nodes_.push_back(std::move(node));
    return *ptr;
}

TTaskNode& TTaskGraph::AddOwnedNode(std::unique_ptr<ITaskNode> task) {
    auto* ptr = task.get();
    OwnedTasks_.push_back(std::move(task));
    return AddNode(*ptr);
}

IConnection& TTaskGraph::AddConnection(std::unique_ptr<IConnection> connection) {
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
    Root_ = nullptr;

    for (auto& node : Nodes_) {
        node->Inbound.clear();
        node->Outbound.clear();
        node->State.store(ETaskState::Idle, std::memory_order_release);
    }

    for (auto& edge : Edges_) {
        edge->Src->Outbound.push_back(edge.get());
        edge->Dst->Inbound.push_back(edge.get());
    }
    for (auto& node : Nodes_) {
        if (std::ranges::none_of(node->Inbound, [](const auto* edge) {
                return edge->Kind == ETaskEdgeKind::Rowset;
            })) {
            Leaves_.push_back(node.get());
        }
        if (std::ranges::none_of(node->Outbound, [](const auto* edge) {
                return edge->Kind == ETaskEdgeKind::Rowset;
            })) {
            Root_ = node.get();
        }
    }
}

bool TTaskGraph::Validate(std::string* error) const {
    if (Nodes_.empty()) {
        SetError(error, "task graph is empty");
        return false;
    }

    std::unordered_set<const TTaskNode*> nodes;
    nodes.reserve(Nodes_.size());
    size_t rootCount = 0;
    size_t leafCount = 0;
    for (auto& node : Nodes_) {
        if (!node->Task) {
            SetError(error, "task graph contains a node without a task");
            return false;
        }
        nodes.insert(node.get());
        if (std::ranges::none_of(node->Inbound, [](const auto* edge) {
                return edge->Kind == ETaskEdgeKind::Rowset;
            })) {
            ++leafCount;
        }
        if (std::ranges::none_of(node->Outbound, [](const auto* edge) {
                return edge->Kind == ETaskEdgeKind::Rowset;
            })) {
            ++rootCount;
        }
    }

    if (leafCount == 0) {
        SetError(error, "task graph has no source leaf");
        return false;
    }
    if (rootCount != 1) {
        SetError(error, std::format(
            "task graph must have exactly one root, got {}", rootCount));
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

    // A dependency edge must not turn a data-flow DAG into a scheduling
    // cycle. In particular, a probe cannot gate its own build-side producer.
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

    // The threaded scheduler stops once the single root (sink) is Finished, so
    // every node must be an ancestor of the root; an orphan branch would be
    // silently abandoned. (Guaranteed for an acyclic single-root graph, but a
    // cycle disconnected from the root would slip past the root-count check.)
    std::unordered_set<const TTaskNode*> reaches;
    std::vector<const TTaskNode*> stack{Root_};
    reaches.insert(Root_);
    while (!stack.empty()) {
        const auto* node = stack.back();
        stack.pop_back();
        for (const auto* edge : node->Inbound) {
            if (reaches.insert(edge->Src).second) {
                stack.push_back(edge->Src);
            }
        }
    }
    if (reaches.size() != Nodes_.size()) {
        SetError(error, "task graph has nodes that cannot reach the root");
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
    if (Root_) {
        out << "root: n" << nodeIds[Root_] << "\n";
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

const std::vector<std::unique_ptr<TTaskNode>>& TTaskGraph::Nodes() const {
    return Nodes_;
}

const std::vector<std::unique_ptr<TTaskEdge>>& TTaskGraph::Edges() const {
    return Edges_;
}

const std::vector<TTaskNode*>& TTaskGraph::Leaves() const {
    return Leaves_;
}

TTaskNode* TTaskGraph::Root() const {
    return Root_;
}

} // namespace NScheduler
} // namespace NQdb
