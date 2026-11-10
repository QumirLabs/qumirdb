#pragma once

#include <qdb/exec/stage.h>
#include <qdb/exec/instrumentation/operator.h>
#include <qdb/scheduler/connection.h>
#include <qdb/scheduler/task.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace NQdb {
namespace NScheduler {

using TTaskGroupId = uint64_t;
inline constexpr TTaskGroupId InvalidTaskGroupId = 0;

struct TTaskEdge;
enum class ETaskEdgeKind { Rowset, Dependency };

struct TTaskNode {
    size_t ExecutionSlot = 0;
    ITask* Task = nullptr;
    std::unique_ptr<TOperatorInstrumentation> Instrumentation;
    TExecStageId ExecStageId = InvalidExecStageId;
    // Stable physical grouping assigned monotonically during one lowering.
    // ExecStageId remains the semantic identity; this ID is only for grouping
    // scheduler plumbing and physical lanes in instrumentation.
    TTaskGroupId TaskGroupId = InvalidTaskGroupId;
    std::string DebugKind;
    std::string DebugLabel;
    std::vector<TTaskEdge*> Inbound;
    std::vector<TTaskEdge*> Outbound;
    std::atomic<ETaskState> State = ETaskState::Idle;
};

struct TTaskEdge {
    TTaskNode* Src = nullptr;
    TTaskNode* Dst = nullptr;
    IConnection* Connection = nullptr;
    ETaskEdgeKind Kind = ETaskEdgeKind::Rowset;
    size_t SrcLane = 0;
    size_t DstLane = 0;
    // Stable semantic-stage input. This is distinct from DstLane: all left
    // join partitions target input 0 and all right partitions target input 1.
    size_t ExecInput = 0;
};

// A node woken from outside the graph (e.g. by a remote channel) under `Id`.
struct TExternalNode {
    uint64_t Id = 0;
    TTaskNode* Node = nullptr;
};

// The side of a cut edge whose port moves to the new 1->1 connection: Dst's
// input for a one-to-one or broadcast cut, Src's output for a gather cut, so
// the gather keeps all its producer lanes.
enum class EReboundPort { SourceOutput, DestinationInput };

// Cuts one rowset edge into Src -> SinkTask and SourceTask -> Dst, the remote
// sink in Src's color and the remote source in Dst's color. The placeholder
// tasks come with unbound ports; Apply binds them to the new edges.
struct TEdgeSplit {
    TTaskEdge* Edge = nullptr;
    EReboundPort ReboundPort = EReboundPort::DestinationInput;
    int SinkColor = 0;
    int SourceColor = 0;
    std::unique_ptr<IConnection> NewConnection;
    std::unique_ptr<ITask> SinkTask;
    std::unique_ptr<ITask> SourceTask;
};

struct TSplitPlan {
    std::vector<TEdgeSplit> Splits;
};

// A validated, frozen graph; only TTaskGraph::Seal() and Apply() make a non-empty one.
class TSealedTaskGraph {
public:
    TSealedTaskGraph() = default;

    explicit operator bool() const {
        return !Graphs_.empty();
    }

    // See TTaskGraph::Graphs().
    std::span<const std::vector<TTaskNode*>> Graphs() const {
        return Graphs_;
    }

    size_t TaskCount() const {
        return TaskCount_;
    }

    // The node marked external under `id`, or nullptr.
    TTaskNode* ExternalNode(uint64_t id) const;

private:
    friend class TTaskGraph;

    TSealedTaskGraph(
        std::span<const std::vector<TTaskNode*>> graphs,
        size_t taskCount,
        std::span<const TExternalNode> externals)
        : Graphs_(graphs)
        , TaskCount_(taskCount)
        , Externals_(externals)
    {}

    std::span<const std::vector<TTaskNode*>> Graphs_;
    size_t TaskCount_ = 0;
    // Sorted by Id.
    std::span<const TExternalNode> Externals_;
};

class TTaskGraph {
public:
    TTaskNode& AddNode(ITask& task);
    TTaskNode& AddOwnedNode(std::unique_ptr<ITask> task);
    IConnection& AddConnection(std::unique_ptr<IConnection> connection);

    TTaskEdge& AddEdge(
        TTaskNode& src,
        TTaskNode& dst,
        IConnection& connection,
        size_t srcLane = 0,
        size_t dstLane = 0,
        size_t execInput = 0);

    TTaskEdge& AddOwnedEdge(
        TTaskNode& src,
        TTaskNode& dst,
        std::unique_ptr<IConnection> connection,
        size_t srcLane = 0,
        size_t dstLane = 0,
        size_t execInput = 0);

    // Carries wakeups and demand, but no rowsets or exec-stage input.
    TTaskEdge& AddDependencyEdge(TTaskNode& producer, TTaskNode& consumer);

    // Names a node that is woken from outside the graph; ids must be unique.
    void MarkExternal(TTaskNode& node, uint64_t id);

    // Builds adjacency, validates and freezes the graph; empty on error.
    TSealedTaskGraph Seal(std::string* error = nullptr);
    // Cuts the plan's edges of a sealed graph and regroups it into one
    // subgraph per color; earlier sealed views become invalid. Colors must be
    // 0..K-1; a graph is split once. Throws std::logic_error on a plan it
    // cannot apply; after that the graph may only be destroyed.
    TSealedTaskGraph Apply(TSplitPlan&& plan);
    void SetConnectionStatsEnabled(bool enabled);
    void Print(std::ostream& out) const;
    void PrintConnectionStats(std::ostream& out) const;

    const std::vector<std::unique_ptr<TTaskNode>>& Nodes() const;
    const std::vector<std::unique_ptr<TTaskEdge>>& Edges() const;
    const std::vector<TTaskNode*>& Leaves() const;
    // The subgraphs, each given by its roots (nodes without a rowset output) in
    // ExecutionSlot order. Seal() makes one of all roots; Apply() makes one per
    // color, so Graphs()[c] is the fragment of color c.
    const std::vector<std::vector<TTaskNode*>>& Graphs() const;
    // Owned connections in creation order.
    std::span<const std::unique_ptr<IConnection>> Connections() const;
    // Marked nodes; sorted by id once the graph is sealed.
    std::span<const TExternalNode> Externals() const;

private:
    void CheckMutable() const;
    void Build();
    bool Validate(std::string* error) const;

    // Task-local observers and ports remain valid through task destruction.
    std::vector<std::unique_ptr<TTaskNode>> Nodes_;
    std::vector<std::unique_ptr<IConnection>> OwnedConnections_;
    std::vector<std::unique_ptr<ITask>> OwnedTasks_;
    std::vector<std::unique_ptr<TTaskEdge>> Edges_;
    std::vector<TTaskNode*> Leaves_;
    std::vector<TExternalNode> Externals_;
    std::vector<std::vector<TTaskNode*>> Graphs_;
    bool Sealed_ = false;
    bool Split_ = false;
};

} // namespace NScheduler
} // namespace NQdb
