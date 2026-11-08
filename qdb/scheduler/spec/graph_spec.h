#pragma once

#include <qdb/exec/plan_data.h>
#include <qdb/io/io.h>
#include <qdb/io/parquet/source.h>
#include <qdb/kernel/regex.h>
#include <qdb/scheduler/connection.h>
#include <qdb/scheduler/graph.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace NQdb {

struct TKernelSlot;
struct TRuntimeContext;

namespace NScheduler {

// Plain-data description of a task graph. Connections, nodes and edges are
// referenced by their position, which is also the order they are built in.
// TODO: no versioning or compatibility checks yet.

struct TRowLayoutSpec {
    int32_t Width = 0;
    bool Bitmap = false;

    void Fields(this auto& self, auto& v)
    {
        v(self.Width);
        v(self.Bitmap);
    }
};

struct TConnectionSpec {
    EConnectionKind Kind = EConnectionKind::OneToOne;
    uint64_t Capacity = 1;
    uint32_t SrcCount = 1;
    uint32_t DstCount = 1;
    std::string DebugName;
    // Hash shuffle only.
    std::vector<std::string> RepartitionKeys;
    std::vector<TRowLayoutSpec> RowLayouts;

    void Fields(this auto& self, auto& v)
    {
        v(self.Kind);
        v(self.Capacity);
        v(self.SrcCount);
        v(self.DstCount);
        v(self.DebugName);
        v(self.RepartitionKeys);
        v(self.RowLayouts);
    }
};

struct TPortSpec {
    uint32_t Connection = 0;
    uint32_t Lane = 0;

    void Fields(this auto& self, auto& v)
    {
        v(self.Connection);
        v(self.Lane);
    }
};

struct TForwardNode {
    TPortSpec In;
    TPortSpec Out;

    void Fields(this auto& self, auto& v)
    {
        v(self.In);
        v(self.Out);
    }
};

// A source or sink whose code the compute actor binds.
struct TRemoteSourceNode {
    TPortSpec Out;

    void Fields(this auto& self, auto& v)
    {
        v(self.Out);
    }
};

struct TRemoteSinkNode {
    TPortSpec In;

    void Fields(this auto& self, auto& v)
    {
        v(self.In);
    }
};

// Writes into the caller's ISink; query side only.
struct TSinkWriterNode {
    TPortSpec In;

    void Fields(this auto& self, auto& v)
    {
        v(self.In);
    }
};

// Path names the same file wherever the graph runs.
struct TParquetScanNode {
    TPortSpec Out;
    std::string Path;
    // File columns to read, sorted; nullopt reads all of them.
    std::optional<std::vector<std::string>> Columns;
    // nullopt reads the whole file; an empty list is a proven-empty scan.
    std::optional<std::vector<int32_t>> RowGroups;

    void Fields(this auto& self, auto& v)
    {
        v(self.Out);
        v(self.Path);
        v(self.Columns);
        v(self.RowGroups);
    }
};

struct TFilterNode {
    TPortSpec In;
    TPortSpec Out;
    TFilterPlanData Plan;

    void Fields(this auto& self, auto& v)
    {
        v(self.In);
        v(self.Out);
        v(self.Plan);
    }
};

struct TProjectNode {
    TPortSpec In;
    TPortSpec Out;
    TProjectPlanData Plan;

    void Fields(this auto& self, auto& v)
    {
        v(self.In);
        v(self.Out);
        v(self.Plan);
    }
};

using TNodeBody = std::variant<
    TForwardNode, TRemoteSourceNode, TRemoteSinkNode, TSinkWriterNode,
    TParquetScanNode, TFilterNode, TProjectNode>;

struct TNodeSpec {
    uint64_t ExecStageId = InvalidExecStageId;
    TTaskGroupId TaskGroupId = InvalidTaskGroupId;
    std::string DebugKind;
    std::string DebugLabel;
    TNodeBody Body;

    void Fields(this auto& self, auto& v)
    {
        v(self.ExecStageId);
        v(self.TaskGroupId);
        v(self.DebugKind);
        v(self.DebugLabel);
        v(self.Body);
    }
};

struct TEdgeSpec {
    uint32_t Src = 0;
    uint32_t Dst = 0;
    ETaskEdgeKind Kind = ETaskEdgeKind::Rowset;
    // Set for rowset edges only.
    std::optional<uint32_t> Connection;
    uint32_t SrcLane = 0;
    uint32_t DstLane = 0;
    uint32_t ExecInput = 0;

    void Fields(this auto& self, auto& v)
    {
        v(self.Src);
        v(self.Dst);
        v(self.Kind);
        v(self.Connection);
        v(self.SrcLane);
        v(self.DstLane);
        v(self.ExecInput);
    }
};

struct TExternalSpec {
    uint64_t Id = 0;
    uint32_t Node = 0;

    void Fields(this auto& self, auto& v)
    {
        v(self.Id);
        v(self.Node);
    }
};

struct TGraphSpec {
    std::vector<TConnectionSpec> Connections;
    std::vector<TNodeSpec> Nodes;
    // In the order the edges were added.
    std::vector<TEdgeSpec> Edges;
    std::vector<TExternalSpec> Externals;
    // The query's regexes in registration order, so each keeps its id.
    std::vector<NKernel::TRegexSpec> Regexes;

    void Fields(this auto& self, auto& v)
    {
        v(self.Connections);
        v(self.Nodes);
        v(self.Edges);
        v(self.Externals);
        v(self.Regexes);
    }
};

std::string SerializeGraphSpec(const TGraphSpec& spec);

// Throws std::runtime_error on truncated or malformed input.
TGraphSpec DeserializeGraphSpec(std::string_view bytes);

// Query objects the node tasks of one graph bind.
struct TNodeTaskContext {
    // Kernel slots by kernel index.
    std::vector<std::shared_ptr<TKernelSlot>> Slots;
    std::shared_ptr<TRuntimeContext> Runtime;
    // Opened on first use; the scans of one path share the file.
    std::unordered_map<std::string, TParquetFile> ParquetFiles;
    // Required for SinkWriter nodes.
    ISink* Sink = nullptr;
};

// Builds the task of one node; port connections index `connections`. The
// lowerer builds the tasks of these kinds through this too. Throws
// std::runtime_error on an invalid body.
std::unique_ptr<ITask> MakeNodeTask(
    const TNodeBody& body, const std::vector<IConnection*>& connections, TNodeTaskContext& context);

// Builds an unsealed graph: node i has ExecutionSlot i. Remote source and sink
// tasks have empty code, which the compute actor binds. `sink` is required for
// SinkWriter nodes; `slots` are the query's kernel slots by kernel index. The
// runtime context is rebuilt from the spec's regexes. Throws
// std::runtime_error on an invalid spec.
std::unique_ptr<TTaskGraph> MaterializeGraph(
    const TGraphSpec& spec, ISink* sink = nullptr,
    std::vector<std::shared_ptr<TKernelSlot>> slots = {});

// Reads connections, edges and externals from a graph built by MaterializeGraph
// or by hand. The caller supplies the node records, one per graph node in
// order. Externals keep the graph's current order (sorted by id after Seal);
// regexes are left to the caller.
// Throws std::runtime_error for a connection that is not a local one and for a
// rowset edge without a connection.
TGraphSpec ExtractGraphSpec(const TTaskGraph& graph, std::vector<TNodeSpec> nodes);

} // namespace NScheduler
} // namespace NQdb
