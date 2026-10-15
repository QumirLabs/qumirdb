#pragma once

#include <qdb/exec/stage.h>
#include <qdb/exec/diagnostics.h>
#include <qdb/scheduler/connection.h>
#include <qdb/scheduler/state.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

namespace NQdb {
namespace NScheduler {

using TTaskGroupId = uint64_t;
inline constexpr TTaskGroupId InvalidTaskGroupId = 0;

struct TTaskEdge;
enum class ETaskEdgeKind { Rowset, Dependency };

struct TTaskNode {
    ITaskNode* Task = nullptr;
    std::unique_ptr<TOperatorDiagnostics> Diagnostics;
    TExecStageId ExecStageId = InvalidExecStageId;
    // Stable physical grouping assigned monotonically during one lowering.
    // ExecStageId remains the semantic identity; this ID is only for grouping
    // scheduler plumbing and physical lanes in diagnostics.
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

class TTaskGraph {
public:
    TTaskNode& AddNode(ITaskNode& task);
    TTaskNode& AddOwnedNode(std::unique_ptr<ITaskNode> task);
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

    void Build();
    bool Validate(std::string* error = nullptr) const;
    void SetConnectionStatsEnabled(bool enabled);
    void Print(std::ostream& out) const;
    void PrintConnectionStats(std::ostream& out) const;

    const std::vector<std::unique_ptr<TTaskNode>>& Nodes() const;
    const std::vector<std::unique_ptr<TTaskEdge>>& Edges() const;
    const std::vector<TTaskNode*>& Leaves() const;
    TTaskNode* Root() const;

private:
    // Task-local observers and ports remain valid through task destruction.
    std::vector<std::unique_ptr<TTaskNode>> Nodes_;
    std::vector<std::unique_ptr<IConnection>> OwnedConnections_;
    std::vector<std::unique_ptr<ITaskNode>> OwnedTasks_;
    std::vector<std::unique_ptr<TTaskEdge>> Edges_;
    std::vector<TTaskNode*> Leaves_;
    TTaskNode* Root_ = nullptr;
};

} // namespace NScheduler
} // namespace NQdb
