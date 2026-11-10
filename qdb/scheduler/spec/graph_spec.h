#pragma once

#include <qdb/exec/plan_data.h>
#include <qdb/exec/runtime_filter.h>
#include <qdb/io/io.h>
#include <qdb/io/parquet/source.h>
#include <qdb/kernel/regex.h>
#include <qdb/plan/ops/join_type.h>
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

class TCteConsumerState;
class TCteSpool;

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
    std::vector<TRowLayoutSpec> RowLayouts;

    void Fields(this auto& self, auto& v)
    {
        v(self.Kind);
        v(self.Capacity);
        v(self.SrcCount);
        v(self.DstCount);
        v(self.DebugName);
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

// A sort, or a top-sort when the plan has Top.
struct TSortNode {
    TPortSpec In;
    TPortSpec Out;
    TSortPlanData Plan;

    void Fields(this auto& self, auto& v)
    {
        v(self.In);
        v(self.Out);
        v(self.Plan);
    }
};

// Merges the sorted runs of its inputs, one run per input.
struct TSortMergeNode {
    std::vector<TPortSpec> In;
    TPortSpec Out;
    TSortMergePlanData Plan;

    void Fields(this auto& self, auto& v)
    {
        v(self.In);
        v(self.Out);
        v(self.Plan);
    }
};

struct TWindowNode {
    TPortSpec In;
    TPortSpec Out;
    TWindowPlanData Plan;

    void Fields(this auto& self, auto& v)
    {
        v(self.In);
        v(self.Out);
        v(self.Plan);
    }
};

// A blocking aggregate over all its input rows.
struct TAggregateNode {
    TPortSpec In;
    TPortSpec Out;
    TAggregateKernelMeta Meta;
    TAggregateTablePlan Table;

    void Fields(this auto& self, auto& v)
    {
        v(self.In);
        v(self.Out);
        v(self.Meta);
        v(self.Table);
    }
};

// The partial phase of a grouped cascade: a bounded aggregate that hands its
// rows through PassThrough once hashing stops paying off.
struct TPartialAggregateNode {
    TPortSpec In;
    TPortSpec Out;
    TAggregateKernelMeta Meta;
    TAggregateTablePlan Table;
    TPartialAggregatePolicy Policy;
    // Turns input rows into single-row partial states; without it the table
    // is only bounded.
    std::optional<TProjectPlanData> PassThrough;

    void Fields(this auto& self, auto& v)
    {
        v(self.In);
        v(self.Out);
        v(self.Meta);
        v(self.Table);
        v(self.Policy);
        v(self.PassThrough);
    }
};

// One aggregate table over all grouping sets: each batch is replayed once per
// set, with the keys outside the set masked NULL.
struct TGroupingSetsAggregateNode {
    TPortSpec In;
    TPortSpec Out;
    TAggregateKernelMeta Meta;
    TAggregateTablePlan Table;
    // The key indexes of each set.
    std::vector<std::vector<size_t>> Sets;
    // The leading input columns that are group keys.
    size_t NumKeys = 0;

    void Fields(this auto& self, auto& v)
    {
        v(self.In);
        v(self.Out);
        v(self.Meta);
        v(self.Table);
        v(self.Sets);
        v(self.NumKeys);
    }
};

// Partitions its input by the hash of the keys: an aggregate hash or one side
// of a join hash.
struct THashShuffleNode {
    using THash = std::variant<TAggregateHashPlanData, TJoinHashPlanData>;

    TPortSpec In;
    // Out[d] receives the rows whose hash picks partition d.
    std::vector<TPortSpec> Out;
    THash Hash;
    NQumir::NAst::TTypePtr InputType;
    uint64_t TargetOutputBatchRows = 0;
    uint64_t MaxOutputBatchRows = 0;
    uint64_t TargetOutputBatchBytes = 0;
    // Reuses the hashes a runtime filter task left on the input for this key.
    bool UseInputHash = false;
    // The runtime filter this build lane publishes a partial into.
    std::optional<uint32_t> ProducedFilter;

    void Fields(this auto& self, auto& v)
    {
        v(self.In);
        v(self.Out);
        v(self.Hash);
        TypeField(v, self.InputType);
        v(self.TargetOutputBatchRows);
        v(self.MaxOutputBatchRows);
        v(self.TargetOutputBatchBytes);
        v(self.UseInputHash);
        v(self.ProducedFilter);
    }
};

// A join over the symmetric Robin Hood join table.
struct THashJoinNode {
    TPortSpec Left;
    TPortSpec Right;
    TPortSpec Out;
    TJoinKernelMeta Kernels;
    EJoinType JoinType = EJoinType::Inner;
    EJoinBuildSide BuildSide = EJoinBuildSide::Auto;

    void Fields(this auto& self, auto& v)
    {
        v(self.Left);
        v(self.Right);
        v(self.Out);
        v(self.Kernels);
        v(self.JoinType);
        v(self.BuildSide);
    }
};

// An equi-join over the row join table: builds BuildSide, probes the other.
struct TRowJoinNode {
    TPortSpec Left;
    TPortSpec Right;
    TPortSpec Out;
    TRowJoinKernelMeta Kernels;
    EJoinBuildSide BuildSide = EJoinBuildSide::Auto;
    // Probe bytes the task may hold while its table is being built.
    uint64_t ProbeHoldBytes = 0;

    void Fields(this auto& self, auto& v)
    {
        v(self.Left);
        v(self.Right);
        v(self.Out);
        v(self.Kernels);
        v(self.BuildSide);
        v(self.ProbeHoldBytes);
    }
};

// A keyless join of each Left batch against the buffered Right input.
struct TCrossJoinNode {
    TPortSpec Left;
    TPortSpec Right;
    TPortSpec Out;
    // Index of the cross join kernel in the sink (0 without a sink).
    uint32_t Kernel = 0;
    EJoinType JoinType = EJoinType::Inner;

    void Fields(this auto& self, auto& v)
    {
        v(self.Left);
        v(self.Right);
        v(self.Out);
        v(self.Kernel);
        v(self.JoinType);
    }
};

// Skips Offset rows, then passes Limit rows.
struct TLimitNode {
    TPortSpec In;
    TPortSpec Out;
    int64_t Limit = 0;
    int64_t Offset = 0;

    void Fields(this auto& self, auto& v)
    {
        v(self.In);
        v(self.Out);
        v(self.Limit);
        v(self.Offset);
    }
};

// Reads Columns of a parquet file at the row locators of its input.
struct TLateLookupNode {
    TPortSpec In;
    TPortSpec Out;
    std::string Path;
    // File column names the lookup fetches.
    std::vector<std::string> Columns;
    // Index of the locator column in the input.
    int32_t LocatorColumn = 0;

    void Fields(this auto& self, auto& v)
    {
        v(self.In);
        v(self.Out);
        v(self.Path);
        v(self.Columns);
        v(self.LocatorColumn);
    }
};

// Joins the column slices of the lookups, one input per lookup, in order.
struct TLateMergeNode {
    std::vector<TPortSpec> In;
    TPortSpec Out;

    void Fields(this auto& self, auto& v)
    {
        v(self.In);
        v(self.Out);
    }
};

// The spool of a materialized CTE, one segment per producer lane.
struct TCteSpoolSpec {
    uint32_t Segments = 0;

    void Fields(this auto& self, auto& v)
    {
        v(self.Segments);
    }
};

// The read progress of one CTE reference, shared by its lanes.
struct TCteConsumerStateSpec {
    uint32_t Spool = 0;

    void Fields(this auto& self, auto& v)
    {
        v(self.Spool);
    }
};

// One lane of a CTE producer: appends In to its segment of the spool and then
// finishes its lane of Out, the publish task's input.
struct TCteProducerNode {
    TPortSpec In;
    TPortSpec Out;
    uint32_t Spool = 0;
    uint32_t Segment = 0;

    void Fields(this auto& self, auto& v)
    {
        v(self.In);
        v(self.Out);
        v(self.Spool);
        v(self.Segment);
    }
};

// Seals the spool once every producer lane of In has finished, then finishes
// every consumer lane of Out.
struct TCtePublishNode {
    TPortSpec In;
    TPortSpec Out;
    uint32_t Spool = 0;

    void Fields(this auto& self, auto& v)
    {
        v(self.In);
        v(self.Out);
        v(self.Spool);
    }
};

// One lane of a CTE reference. In is its lane of the publish task's output:
// reference * lanes + lane.
struct TCteConsumerNode {
    TPortSpec In;
    TPortSpec Out;
    uint32_t State = 0;

    void Fields(this auto& self, auto& v)
    {
        v(self.In);
        v(self.Out);
        v(self.State);
    }
};

// A runtime filter of a join: every build lane publishes one partial into it,
// the filter tasks probe it. Created through the binding factory.
struct TRuntimeFilterBindingSpec {
    // Query-scoped; a factory may bind it to a remote filter.
    uint32_t Id = 0;
    uint32_t ProducerCount = 0;
    uint64_t BloomBytes = 0;

    void Fields(this auto& self, auto& v)
    {
        v(self.Id);
        v(self.ProducerCount);
        v(self.BloomBytes);
    }
};

// Passes the rows of In whose key hash Filter may contain, once the filter is
// published. AuxProducers are the build lanes it waits for; they may follow it
// in the graph, so BindRuntimeFilterTask binds the task after every node exists.
struct TRuntimeFilterNode {
    TPortSpec In;
    TPortSpec Out;
    // The probe side of a join hash over In's key columns.
    TJoinHashPlanData Hash;
    uint32_t Filter = 0;
    // Node indices.
    std::vector<uint32_t> AuxProducers;

    void Fields(this auto& self, auto& v)
    {
        v(self.In);
        v(self.Out);
        v(self.Hash);
        v(self.Filter);
        v(self.AuxProducers);
    }
};

using TNodeBody = std::variant<
    TForwardNode, TRemoteSourceNode, TRemoteSinkNode, TSinkWriterNode,
    TParquetScanNode, TFilterNode, TProjectNode, TSortNode, TSortMergeNode,
    TWindowNode, TAggregateNode, THashShuffleNode, THashJoinNode, TRowJoinNode,
    TCrossJoinNode, TLimitNode, TLateLookupNode, TLateMergeNode,
    TCteProducerNode, TCtePublishNode, TCteConsumerNode, TPartialAggregateNode,
    TGroupingSetsAggregateNode, TRuntimeFilterNode>;

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
    // Shared by the CTE records, which index them.
    std::vector<TCteSpoolSpec> CteSpools;
    std::vector<TCteConsumerStateSpec> CteConsumerStates;
    // The runtime-filter kernel; required when there are runtime filters.
    std::optional<uint32_t> RuntimeFilterKernel;
    // Shared by the runtime filter and build shuffle records, which index them.
    std::vector<TRuntimeFilterBindingSpec> RuntimeFilterBindings;

    void Fields(this auto& self, auto& v)
    {
        v(self.Connections);
        v(self.Nodes);
        v(self.Edges);
        v(self.Externals);
        v(self.Regexes);
        v(self.CteSpools);
        v(self.CteConsumerStates);
        v(self.RuntimeFilterKernel);
        v(self.RuntimeFilterBindings);
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
    // Built from the CTE tables, by index. They keep the spooled rows alive, so
    // a context lives only while its graph is built.
    std::vector<std::shared_ptr<TCteSpool>> CteSpools;
    std::vector<std::shared_ptr<TCteConsumerState>> CteConsumerStates;
    // Created from the runtime filter table, by index.
    std::vector<TRuntimeFilterBinding> RuntimeFilterBindings;
};

// Builds the CTE spools and consumer states of the tables past those the
// context already holds, so indices match the tables. Throws
// std::runtime_error on a consumer state of an unknown spool.
void AppendCteObjects(
    const std::vector<TCteSpoolSpec>& spools, const std::vector<TCteConsumerStateSpec>& states,
    TNodeTaskContext& context);

// Creates the runtime filters of the table past those the context already
// holds through `factory`, over the context's runtime. Throws
// std::runtime_error on an incomplete binding.
void AppendRuntimeFilterBindings(
    const std::vector<TRuntimeFilterBindingSpec>& filters, IRuntimeFilterBindingFactory& factory,
    TNodeTaskContext& context);

// Builds the task of one node; port connections index `connections`. The
// lowerer builds the tasks of these kinds through this too. A runtime filter
// task stays unbound until BindRuntimeFilterTask. Throws std::runtime_error on
// an invalid body.
std::unique_ptr<ITask> MakeNodeTask(
    const TNodeBody& body, const std::vector<IConnection*>& connections, TNodeTaskContext& context);

// Binds the task MakeNodeTask built for `node` to its filter, its hash and its
// producers, which index the nodes of `graph`. Throws std::runtime_error on an
// invalid record.
void BindRuntimeFilterTask(
    const TRuntimeFilterNode& node, TTaskNode& task, const TTaskGraph& graph,
    const TNodeTaskContext& context);

// Builds an unsealed graph: node i has ExecutionSlot i. Remote source and sink
// tasks have empty code, which the compute actor binds. `sink` is required for
// SinkWriter nodes; `slots` are the query's kernel slots by kernel index. The
// runtime context is rebuilt from the spec's regexes and runtime-filter kernel,
// the CTE objects from its CTE tables, and the runtime filters through
// `filterBindings` (local filters by default). Throws std::runtime_error on an
// invalid spec.
std::unique_ptr<TTaskGraph> MaterializeGraph(
    const TGraphSpec& spec, ISink* sink = nullptr,
    std::vector<std::shared_ptr<TKernelSlot>> slots = {},
    std::shared_ptr<IRuntimeFilterBindingFactory> filterBindings = nullptr);

// Reads connections, edges and externals from a graph built by MaterializeGraph
// or by hand. The caller supplies the node records, one per graph node in
// order. Externals keep the graph's current order (sorted by id after Seal);
// regexes, CTE and runtime filter tables are left to the caller.
// Throws std::runtime_error for a connection that is not a local one and for a
// rowset edge without a connection.
TGraphSpec ExtractGraphSpec(const TTaskGraph& graph, std::vector<TNodeSpec> nodes);

} // namespace NScheduler
} // namespace NQdb
