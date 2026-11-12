#include <qdb/scheduler/spec/graph_spec.h>

#include <qdb/exec/filter_exec.h>
#include <qdb/exec/project_exec.h>
#include <qdb/exec/runtime_context.h>
#include <qdb/exec/sort_exec.h>
#include <qdb/exec/window_exec.h>
#include <qdb/kernel/compiler.h>
#include <qdb/scheduler/connections/local.h>
#include <qdb/scheduler/lowering/task_factory.h>
#include <qdb/scheduler/tasks/cte_spool.h>
#include <qdb/scheduler/tasks/runtime_adapter.h>
#include <qdb/utils/binary_io.h>

#include <limits>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace NQdb {
namespace NScheduler {

namespace {

[[noreturn]] void Invalid(const std::string& what)
{
    throw std::runtime_error("invalid task graph spec: " + what);
}

std::vector<TValueLayout> ValueLayouts(const std::vector<TRowLayoutSpec>& specs)
{
    std::vector<TValueLayout> layouts;
    layouts.reserve(specs.size());
    for (const auto& layout : specs) {
        layouts.push_back(TValueLayout{.Width = layout.Width, .Bitmap = layout.Bitmap});
    }
    return layouts;
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
        case EConnectionKind::Broadcast:
            connection = std::make_unique<TBroadcastConnection>(spec.Capacity);
            break;
        default:
            Invalid("unknown connection kind");
    }
    connection->Resize(spec.SrcCount, spec.DstCount);
    connection->SetDebugName(spec.DebugName);
    connection->SetRowLayouts(ValueLayouts(spec.RowLayouts));
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

const std::shared_ptr<TKernelSlot>& SlotAt(const TNodeTaskContext& context, uint32_t kernel)
{
    if (kernel >= context.Slots.size()) {
        Invalid("kernel index is out of range");
    }
    return context.Slots[kernel];
}

const std::shared_ptr<TCteSpool>& CteSpoolAt(const TNodeTaskContext& context, uint32_t spool)
{
    if (spool >= context.CteSpools.size()) {
        Invalid("cte spool index is out of range");
    }
    return context.CteSpools[spool];
}

std::unique_ptr<ITask> MakeProcessTask(
    TUnaryStreamProcess process, TInputPort input, TOutputPort output)
{
    auto code = std::make_shared<TUnaryCode>(
        [process = std::move(process)](void* state, TRowSet& rowSet) {
            process(rowSet, *static_cast<TUnaryStreamingKernelState*>(state));
        });
    return std::make_unique<TUnaryTask>(
        std::move(code), std::make_shared<TUnaryStreamingKernelState>(), input, output);
}

std::unique_ptr<ITask> MakeParquetScanTask(
    const TParquetScanNode& node, TOutputPort output, TNodeTaskContext& context)
{
    const auto& file = context.ParquetFiles.try_emplace(node.Path, node.Path).first->second;
    std::optional<std::unordered_set<std::string>> columns;
    if (node.Columns) {
        columns.emplace(node.Columns->begin(), node.Columns->end());
    }
    auto state = std::make_shared<NLowering::TSchedulerSourceState>();
    state->OwnedSource = file.MakeSource(node.RowGroups, std::move(columns));
    state->Source = state->OwnedSource.get();
    return std::make_unique<TSourceTask>(NLowering::MakeSourceCode(), std::move(state), output);
}

std::unique_ptr<ITask> MakeSortTask(
    const TSortNode& node, TInputPort input, TOutputPort output, const TNodeTaskContext& context)
{
    const auto& plan = node.Plan;
    TKernelCompiler::TTopSortDispatch topSort;
    TKernelCompiler::THeapTopKDispatch heapTopK;
    if (plan.Top) {
        topSort = BindTopSort(SlotAt(context, plan.Top->TopSortKernel));
        heapTopK = BindHeapTopK(SlotAt(context, plan.Top->HeapKernel));
    }
    auto radix = MakeSortRadixKernel(
        plan, BindSortRadixComposite(SlotAt(context, plan.RadixKernel)),
        std::move(topSort), std::move(heapTopK));
    std::shared_ptr<const TBlockingCode> code;
    std::shared_ptr<ITaskState> state;
    if (plan.Top) {
        code = NLowering::MakeDrainBlockingCode<NLowering::TTopSortBlockingState>();
        state = std::make_shared<NLowering::TTopSortBlockingState>(
            plan.OutputType, plan.Keys, plan.KeyColumns, std::move(radix), plan.Top->Limit);
    } else {
        code = NLowering::MakeDrainBlockingCode<NLowering::TSortBlockingState>();
        state = std::make_shared<NLowering::TSortBlockingState>(
            plan.OutputType, plan.Keys, plan.KeyColumns, std::move(radix));
    }
    return std::make_unique<TBlockingTask>(
        std::move(code), std::move(state), input, output);
}

TUnaryStreamProcess BindProjectProcess(const TProjectPlanData& plan, const TNodeTaskContext& context)
{
    TKernelCompiler::TProjectDispatch dispatch;
    if (!plan.Kernel && !plan.ComputedWidths.empty()) {
        Invalid("a project with computed columns needs a kernel");
    }
    if (plan.Kernel) {
        dispatch = BindProject(SlotAt(context, *plan.Kernel), context.Runtime);
        dispatch.Kernel = *plan.Kernel;
    }
    return MakeProjectProcess(plan, std::move(dispatch));
}

std::unique_ptr<ITask> MakeAggregateTask(
    const TAggregateNode& node, const std::vector<IConnection*>& connections, const TNodeTaskContext& context)
{
    auto state = std::make_shared<NLowering::TAggregateBlockingState>(
        BindAggregateKernels(SlotAt(context, node.Meta.Kernel), node.Meta), node.Table);
    return std::make_unique<TBlockingTask>(
        NLowering::MakeAggregateBlockingCode<NLowering::TAggregateBlockingState>(), std::move(state),
        InputPort(connections, node.In), OutputPort(connections, node.Out));
}

std::unique_ptr<ITask> MakePartialAggregateTask(
    const TPartialAggregateNode& node, const std::vector<IConnection*>& connections,
    const TNodeTaskContext& context)
{
    TUnaryStreamProcess passThrough;
    if (node.PassThrough) {
        passThrough = BindProjectProcess(*node.PassThrough, context);
    }
    auto state = std::make_shared<NLowering::TPartialAggregateBlockingState>(
        BindAggregateKernels(SlotAt(context, node.Meta.Kernel), node.Meta), node.Table, node.Policy,
        std::move(passThrough));
    return std::make_unique<TBlockingTask>(
        NLowering::MakePartialAggregateBlockingCode(), std::move(state),
        InputPort(connections, node.In), OutputPort(connections, node.Out));
}

std::unique_ptr<ITask> MakeGroupingSetsAggregateTask(
    const TGroupingSetsAggregateNode& node, const std::vector<IConnection*>& connections,
    const TNodeTaskContext& context)
{
    auto state = std::make_shared<NLowering::TGroupingSetsBlockingState>(
        BindAggregateKernels(SlotAt(context, node.Meta.Kernel), node.Meta), node.Table, node.Sets,
        node.NumKeys);
    return std::make_unique<TBlockingTask>(
        NLowering::MakeAggregateBlockingCode<NLowering::TGroupingSetsBlockingState>(), std::move(state),
        InputPort(connections, node.In), OutputPort(connections, node.Out));
}

THashShuffleCode::THash BindJoinHashPlan(const TJoinHashPlanData& hash, const TNodeTaskContext& context)
{
    if (hash.Entry > 1) {
        Invalid("a join hash entry is 0 (left) or 1 (right)");
    }
    return BindJoinHash(SlotAt(context, hash.Kernel), hash.Entry, hash.KeyColumns);
}

THashShuffleCode::THash BindShuffleHash(const THashShuffleNode::THash& hash, const TNodeTaskContext& context)
{
    if (const auto* join = std::get_if<TJoinHashPlanData>(&hash)) {
        return BindJoinHashPlan(*join, context);
    }
    const auto& aggregate = std::get<TAggregateHashPlanData>(hash);
    auto dispatch = BindAggregateHash(SlotAt(context, aggregate.Kernel), aggregate.KeyColumns);
    dispatch.Kernel = aggregate.Kernel;
    return dispatch;
}

const TRuntimeFilterBinding& RuntimeFilterAt(const TNodeTaskContext& context, uint32_t filter)
{
    if (filter >= context.RuntimeFilterBindings.size()) {
        Invalid("runtime filter index is out of range");
    }
    return context.RuntimeFilterBindings[filter];
}

// Build lanes deliver to the export of their filter where it has one.
std::shared_ptr<IRuntimeFilterProducer> RuntimeFilterProducerAt(
    const TNodeTaskContext& context, uint32_t filter)
{
    const auto& producer = RuntimeFilterAt(context, filter).Producer;
    if (filter < context.RuntimeFilterExports.size() && context.RuntimeFilterExports[filter]) {
        return context.RuntimeFilterExports[filter];
    }
    return producer;
}

// The exports of the filters that runtime filter sinks in `nodes` send, each
// counting the build lanes in `nodes` that deliver into it; one forwards to its
// filter too when `nodes` probe the filter.
void AddRuntimeFilterExports(const std::vector<TNodeSpec>& nodes, TNodeTaskContext& context)
{
    const auto filters = context.RuntimeFilterBindings.size();
    std::vector<size_t> probes(filters);
    std::vector<size_t> sinks(filters);
    std::vector<size_t> lanes(filters);
    auto count = [&](std::vector<size_t>& counts, uint32_t filter) {
        if (filter >= filters) {
            Invalid("runtime filter index is out of range");
        }
        ++counts[filter];
    };
    for (const auto& node : nodes) {
        if (const auto* probe = std::get_if<TRuntimeFilterNode>(&node.Body)) {
            count(probes, probe->Filter);
        } else if (const auto* sink = std::get_if<TRuntimeFilterSinkNode>(&node.Body)) {
            count(sinks, sink->Filter);
        } else if (const auto* lane = std::get_if<THashShuffleNode>(&node.Body); lane && lane->ProducedFilter) {
            count(lanes, *lane->ProducedFilter);
        }
    }
    context.RuntimeFilterExports.resize(filters);
    for (size_t i = 0; i < filters; ++i) {
        if (sinks[i]) {
            context.RuntimeFilterExports[i] = std::make_shared<TRuntimeFilterExport>(
                context.Runtime, context.RuntimeFilterBindings[i].Producer, probes[i] != 0, lanes[i]);
        }
    }
}

std::unique_ptr<ITask> MakeHashShuffleTask(
    const THashShuffleNode& node, const std::vector<IConnection*>& connections,
    const TNodeTaskContext& context)
{
    if (node.Out.empty()) {
        Invalid("a hash shuffle node needs an output per partition");
    }
    std::vector<TOutputPort> outputs;
    outputs.reserve(node.Out.size());
    for (const auto& port : node.Out) {
        outputs.push_back(OutputPort(connections, port));
    }
    auto code = std::make_shared<THashShuffleCode>(
        BindShuffleHash(node.Hash, context), node.InputType,
        node.TargetOutputBatchRows, node.MaxOutputBatchRows, node.TargetOutputBatchBytes);
    if (node.ProducedFilter) {
        code->ProducedFilter = RuntimeFilterProducerAt(context, *node.ProducedFilter);
    }
    code->UseInputHash = node.UseInputHash;
    return std::make_unique<THashShuffleTask>(
        std::move(code), InputPort(connections, node.In), std::move(outputs));
}

// Binary join code and its state over the node's ports.
template <class TState, class TNode, class... TArgs>
std::unique_ptr<ITask> MakeJoinTask(
    const TNode& node, const std::vector<IConnection*>& connections, TArgs&&... args)
{
    return std::make_unique<TBinaryBlockingTask>(
        NLowering::MakeBinaryJoinCode<TState>(),
        std::make_shared<TState>(std::forward<TArgs>(args)...),
        InputPort(connections, node.Left), InputPort(connections, node.Right),
        OutputPort(connections, node.Out));
}

TTaskNode& NodeAt(const std::vector<TTaskNode*>& nodes, uint32_t index)
{
    if (index >= nodes.size()) {
        Invalid("node index is out of range");
    }
    return *nodes[index];
}

// Calls input(port) on the In, Left and Right ports of a record and output(port)
// on its Out ports.
template <class TBody, class TInput, class TOutput>
void ForEachPort(TBody& body, TInput&& input, TOutput&& output)
{
    auto each = [](auto& ports, auto& fn) {
        if constexpr (std::is_same_v<std::remove_cvref_t<decltype(ports)>, TPortSpec>) {
            fn(ports);
        } else {
            for (auto& port : ports) {
                fn(port);
            }
        }
    };
    std::visit([&](auto& node) {
        if constexpr (requires { node.In; }) {
            each(node.In, input);
        }
        if constexpr (requires { node.Left; }) {
            each(node.Left, input);
        }
        if constexpr (requires { node.Right; }) {
            each(node.Right, input);
        }
        if constexpr (requires { node.Out; }) {
            each(node.Out, output);
        }
    }, body);
}

TConnectionSpec ExtractConnection(const IConnection& connection)
{
    if (!dynamic_cast<const TLocalConnection*>(&connection)) {
        throw std::runtime_error("ExtractGraphSpec supports local connections only");
    }
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
        case EConnectionKind::Broadcast:
            spec.Capacity = static_cast<const TBroadcastConnection&>(connection).Capacity();
            break;
    }
    return spec;
}

} // namespace

std::unique_ptr<ITask> MakeNodeTask(
    const TNodeBody& body, const std::vector<IConnection*>& connections, TNodeTaskContext& context)
{
    return std::visit([&](const auto& node) -> std::unique_ptr<ITask> {
        using TNode = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<TNode, TParquetScanNode>) {
            return MakeParquetScanTask(node, OutputPort(connections, node.Out), context);
        } else if constexpr (std::is_same_v<TNode, TFilterNode>) {
            TKernelCompiler::TFilterDispatch dispatch{
                .Slot = SlotAt(context, node.Plan.Kernel),
                .Runtime = context.Runtime,
                .Kernel = node.Plan.Kernel,
            };
            return MakeProcessTask(
                MakeFilterProcess(node.Plan, std::move(dispatch)),
                InputPort(connections, node.In), OutputPort(connections, node.Out));
        } else if constexpr (std::is_same_v<TNode, TProjectNode>) {
            return MakeProcessTask(
                BindProjectProcess(node.Plan, context),
                InputPort(connections, node.In), OutputPort(connections, node.Out));
        } else if constexpr (std::is_same_v<TNode, TSortNode>) {
            return MakeSortTask(
                node, InputPort(connections, node.In), OutputPort(connections, node.Out), context);
        } else if constexpr (std::is_same_v<TNode, TSortMergeNode>) {
            std::vector<TInputPort> inputs;
            for (const auto& port : node.In) {
                inputs.push_back(InputPort(connections, port));
            }
            const auto& plan = node.Plan;
            return std::make_unique<TMergeTask>(
                NLowering::MakeMergeCode(),
                std::make_shared<NLowering::TMergeState>(
                    plan.OutputType, plan.Keys, plan.KeyColumns, inputs.size()),
                std::move(inputs), OutputPort(connections, node.Out));
        } else if constexpr (std::is_same_v<TNode, TWindowNode>) {
            const auto& plan = node.Plan;
            auto kernel = MakeWindowKernel(BindSortRadixComposite(SlotAt(context, plan.Kernel)));
            return std::make_unique<TBlockingTask>(
                NLowering::MakeDrainBlockingCode<NLowering::TWindowBlockingState>(),
                std::make_shared<NLowering::TWindowBlockingState>(
                    plan.OutputType, plan.Keys, plan.KeyColumns, std::move(kernel)),
                InputPort(connections, node.In), OutputPort(connections, node.Out));
        } else if constexpr (std::is_same_v<TNode, TAggregateNode>) {
            return MakeAggregateTask(node, connections, context);
        } else if constexpr (std::is_same_v<TNode, TPartialAggregateNode>) {
            return MakePartialAggregateTask(node, connections, context);
        } else if constexpr (std::is_same_v<TNode, TGroupingSetsAggregateNode>) {
            return MakeGroupingSetsAggregateTask(node, connections, context);
        } else if constexpr (std::is_same_v<TNode, THashShuffleNode>) {
            return MakeHashShuffleTask(node, connections, context);
        } else if constexpr (std::is_same_v<TNode, THashJoinNode>) {
            return MakeJoinTask<NLowering::TSchedulerInnerJoinState>(
                node, connections, BindJoinKernels(SlotAt(context, node.Kernels.Kernel), node.Kernels),
                node.JoinType, node.BuildSide);
        } else if constexpr (std::is_same_v<TNode, TRowJoinNode>) {
            const auto& kernels = node.Kernels;
            return MakeJoinTask<NLowering::TSchedulerRowJoinState>(
                node, connections,
                BindRowJoinKernels(
                    SlotAt(context, kernels.Kernel), SlotAt(context, kernels.ProbeMeasureKernel), kernels),
                node.BuildSide, node.ProbeHoldBytes);
        } else if constexpr (std::is_same_v<TNode, TCrossJoinNode>) {
            return MakeJoinTask<NLowering::TSchedulerCrossJoinState>(
                node, connections,
                BindCrossJoinKernels(SlotAt(context, node.Kernel), {.Kernel = node.Kernel}), node.JoinType);
        } else if constexpr (std::is_same_v<TNode, TLimitNode>) {
            return std::make_unique<TBlockingTask>(
                NLowering::MakeLimitBlockingCode(),
                std::make_shared<NLowering::TLimitBlockingState>(node.Limit, node.Offset),
                InputPort(connections, node.In), OutputPort(connections, node.Out));
        } else if constexpr (std::is_same_v<TNode, TLateLookupNode>) {
            const auto& file = context.ParquetFiles.try_emplace(node.Path, node.Path).first->second;
            auto reader = file.CompileReader(node.Columns);
            if (!reader) {
                Invalid("a late lookup could not compile its row reader");
            }
            return std::make_unique<TBlockingTask>(
                NLowering::MakeDrainBlockingCode<NLowering::TLateMaterializeBlockingState>(),
                std::make_shared<NLowering::TLateMaterializeBlockingState>(
                    std::move(reader), node.LocatorColumn),
                InputPort(connections, node.In), OutputPort(connections, node.Out));
        } else if constexpr (std::is_same_v<TNode, TLateMergeNode>) {
            std::vector<TInputPort> inputs;
            for (const auto& port : node.In) {
                inputs.push_back(InputPort(connections, port));
            }
            return std::make_unique<TMergeTask>(
                NLowering::MakeLateMergeCode(),
                std::make_shared<NLowering::TLateMaterializeMergeState>(inputs.size()),
                std::move(inputs), OutputPort(connections, node.Out));
        } else if constexpr (std::is_same_v<TNode, TCteProducerNode>) {
            const auto& spool = CteSpoolAt(context, node.Spool);
            if (node.Segment >= spool->SegmentCount()) {
                Invalid("cte spool segment is out of range");
            }
            return std::make_unique<TCteProducerTask>(
                spool, node.Segment, InputPort(connections, node.In), OutputPort(connections, node.Out));
        } else if constexpr (std::is_same_v<TNode, TCtePublishNode>) {
            return std::make_unique<TCtePublishTask>(
                CteSpoolAt(context, node.Spool), InputPort(connections, node.In),
                OutputPort(connections, node.Out));
        } else if constexpr (std::is_same_v<TNode, TCteConsumerNode>) {
            if (node.State >= context.CteConsumerStates.size()) {
                Invalid("cte consumer state index is out of range");
            }
            const auto& state = context.CteConsumerStates[node.State];
            if (node.Lane >= state->Spool()->SegmentCount()) {
                Invalid("cte consumer lane is out of range");
            }
            state->AddLane(node.Lane);
            return std::make_unique<TCteConsumerTask>(
                state, InputPort(connections, node.In), OutputPort(connections, node.Out));
        } else if constexpr (std::is_same_v<TNode, TCteSinkNode>) {
            const auto& spool = CteSpoolAt(context, node.Spool);
            for (const auto segment : node.Segments) {
                if (segment >= spool->SegmentCount()) {
                    Invalid("cte spool segment is out of range");
                }
            }
            std::vector<TInputPort> inputs;
            for (const auto& port : node.In) {
                inputs.push_back(InputPort(connections, port));
            }
            return std::make_unique<TCteSinkTask>(spool, node.Segments, std::move(inputs));
        } else if constexpr (std::is_same_v<TNode, TCteSourceNode>) {
            const auto& spool = CteSpoolAt(context, node.Spool);
            if (node.Segment >= spool->SegmentCount()) {
                Invalid("cte spool segment is out of range");
            }
            std::vector<TOutputPort> outputs;
            for (const auto& port : node.Out) {
                outputs.push_back(OutputPort(connections, port));
            }
            return std::make_unique<TCteSourceTask>(spool, node.Segment, node.Seal, std::move(outputs));
        } else if constexpr (std::is_same_v<TNode, TRuntimeFilterNode>) {
            return std::make_unique<TRuntimeFilterTask>(
                nullptr, InputPort(connections, node.In), OutputPort(connections, node.Out));
        } else if constexpr (std::is_same_v<TNode, TRuntimeFilterSinkNode>) {
            // AddRuntimeFilterExports made the export of every sink's filter.
            return std::make_unique<TRuntimeFilterSinkTask>(context.RuntimeFilterExports[node.Filter]);
        } else if constexpr (std::is_same_v<TNode, TRuntimeFilterSourceNode>) {
            return std::make_unique<TRuntimeFilterSourceTask>(RuntimeFilterAt(context, node.Filter).Producer);
        } else if constexpr (std::is_same_v<TNode, TForwardNode>) {
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
            auto* sink = context.Sink;
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

void AppendCteObjects(
    const std::vector<TCteSpoolSpec>& spools, const std::vector<TCteConsumerStateSpec>& states,
    TNodeTaskContext& context)
{
    for (size_t i = context.CteSpools.size(); i < spools.size(); ++i) {
        context.CteSpools.push_back(
            std::make_shared<TCteSpool>(spools[i].Segments, ValueLayouts(spools[i].RowLayouts)));
    }
    for (size_t i = context.CteConsumerStates.size(); i < states.size(); ++i) {
        context.CteConsumerStates.push_back(
            std::make_shared<TCteConsumerState>(CteSpoolAt(context, states[i].Spool)));
    }
}

void AppendRuntimeFilterBindings(
    const std::vector<TRuntimeFilterBindingSpec>& filters, IRuntimeFilterBindingFactory& factory,
    TNodeTaskContext& context)
{
    for (size_t i = context.RuntimeFilterBindings.size(); i < filters.size(); ++i) {
        const auto& filter = filters[i];
        auto binding = factory.Create(filter.Id, filter.ProducerCount, filter.BloomBytes, context.Runtime);
        if (!binding.Producer || !binding.Probe) {
            throw std::runtime_error("runtime filter binding factory returned an incomplete binding");
        }
        context.RuntimeFilterBindings.push_back(std::move(binding));
    }
}

void BindRuntimeFilterTask(
    const TRuntimeFilterNode& node, TTaskNode& task, const TNodeTaskContext& context)
{
    const auto& probe = RuntimeFilterAt(context, node.Filter).Probe;
    auto auxInput = std::make_shared<TAuxInput>();
    auxInput->AddFilter(probe);
    auxInput->SetConsumer(task);
    auto code = std::make_shared<TRuntimeFilterCode>(TRuntimeFilterCode{
        .Hash = BindJoinHashPlan(node.Hash, context),
        .Probe = probe,
        .AuxInput = std::move(auxInput),
    });
    static_cast<TRuntimeFilterTask*>(task.Task)->BindCode(std::move(code));
}

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

std::unique_ptr<TTaskGraph> MaterializeGraph(
    const TGraphSpec& spec, ISink* sink, std::vector<std::shared_ptr<TKernelSlot>> slots,
    std::shared_ptr<IRuntimeFilterBindingFactory> filterBindings)
{
    auto graph = std::make_unique<TTaskGraph>();
    TNodeTaskContext context{.Slots = std::move(slots), .Sink = sink};
    std::shared_ptr<TKernelSlot> filterKernel;
    if (spec.RuntimeFilterKernel) {
        filterKernel = SlotAt(context, *spec.RuntimeFilterKernel);
    } else if (!spec.RuntimeFilterBindings.empty()) {
        Invalid("runtime filters need the runtime-filter kernel");
    }
    context.Runtime = MakeRuntimeContext(spec.Regexes, std::move(filterKernel));
    AppendCteObjects(spec.CteSpools, spec.CteConsumerStates, context);
    if (!filterBindings) {
        filterBindings = std::make_shared<TLocalRuntimeFilterBindingFactory>();
    }
    AppendRuntimeFilterBindings(spec.RuntimeFilterBindings, *filterBindings, context);
    AddRuntimeFilterExports(spec.Nodes, context);

    std::vector<IConnection*> connections;
    connections.reserve(spec.Connections.size());
    for (const auto& connectionSpec : spec.Connections) {
        connections.push_back(&graph->AddConnection(MakeConnection(connectionSpec)));
    }

    std::vector<TTaskNode*> nodes;
    nodes.reserve(spec.Nodes.size());
    for (const auto& nodeSpec : spec.Nodes) {
        auto& node = graph->AddOwnedNode(MakeNodeTask(nodeSpec.Body, connections, context));
        node.ExecStageId = nodeSpec.ExecStageId;
        node.TaskGroupId = nodeSpec.TaskGroupId;
        node.DebugKind = nodeSpec.DebugKind;
        node.DebugLabel = nodeSpec.DebugLabel;
        nodes.push_back(&node);
    }
    for (size_t i = 0; i < spec.Nodes.size(); ++i) {
        if (const auto* filter = std::get_if<TRuntimeFilterNode>(&spec.Nodes[i].Body)) {
            BindRuntimeFilterTask(*filter, *nodes[i], context);
        } else if (std::holds_alternative<TRuntimeFilterSinkNode>(spec.Nodes[i].Body)) {
            static_cast<TRuntimeFilterSinkTask*>(nodes[i]->Task)->BindNode(*nodes[i]);
        }
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
        } else if (edge->Kind != ETaskEdgeKind::Dependency) {
            throw std::runtime_error("a rowset edge needs a connection");
        }
        spec.Edges.push_back(edgeSpec);
    }

    for (const auto& external : graph.Externals()) {
        spec.Externals.push_back(TExternalSpec{
            .Id = external.Id, .Node = nodeIds.at(external.Node)});
    }
    return spec;
}

bool RebindPort(TNodeBody& body, EPortKind kind, TPortSpec from, TPortSpec to)
{
    bool moved = false;
    auto rebind = [&](TPortSpec& port) {
        if (!moved && port == from) {
            port = to;
            moved = true;
        }
    };
    auto skip = [](TPortSpec&) {};
    if (kind == EPortKind::Input) {
        ForEachPort(body, rebind, skip);
    } else {
        ForEachPort(body, skip, rebind);
    }
    return moved;
}

TGraphSpec SubgraphSpec(const TGraphSpec& spec, std::span<const uint32_t> nodes)
{
    constexpr auto Absent = std::numeric_limits<uint32_t>::max();
    std::vector<uint32_t> nodeIds(spec.Nodes.size(), Absent);
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i] >= spec.Nodes.size() || (i > 0 && nodes[i] <= nodes[i - 1])) {
            Invalid("subgraph node indices must ascend within the spec");
        }
        nodeIds[nodes[i]] = static_cast<uint32_t>(i);
    }
    auto kept = [&](uint32_t node) {
        if (node >= nodeIds.size()) {
            Invalid("node index is out of range");
        }
        return nodeIds[node] != Absent;
    };

    // Connections keep their relative order.
    std::vector<uint32_t> connectionIds(spec.Connections.size(), Absent);
    auto use = [&](uint32_t connection) {
        if (connection >= connectionIds.size()) {
            Invalid("connection index is out of range");
        }
        connectionIds[connection] = 0;
    };
    for (const auto node : nodes) {
        ForEachPort(spec.Nodes[node].Body,
            [&](const TPortSpec& port) { use(port.Connection); },
            [&](const TPortSpec& port) { use(port.Connection); });
    }
    for (const auto& edge : spec.Edges) {
        if (kept(edge.Src) != kept(edge.Dst)) {
            Invalid("an edge leaves the subgraph");
        }
        if (kept(edge.Src) && edge.Connection) {
            use(*edge.Connection);
        }
    }

    TGraphSpec subgraph;
    for (uint32_t i = 0; i < spec.Connections.size(); ++i) {
        if (connectionIds[i] != Absent) {
            connectionIds[i] = static_cast<uint32_t>(subgraph.Connections.size());
            subgraph.Connections.push_back(spec.Connections[i]);
        }
    }
    auto renumber = [&](TPortSpec& port) {
        port.Connection = connectionIds[port.Connection];
    };
    for (const auto node : nodes) {
        auto& record = subgraph.Nodes.emplace_back(spec.Nodes[node]);
        ForEachPort(record.Body, renumber, renumber);
    }
    for (const auto& edge : spec.Edges) {
        if (!kept(edge.Src)) {
            continue;
        }
        auto& copy = subgraph.Edges.emplace_back(edge);
        copy.Src = nodeIds[edge.Src];
        copy.Dst = nodeIds[edge.Dst];
        if (copy.Connection) {
            copy.Connection = connectionIds[*copy.Connection];
        }
    }
    for (const auto& external : spec.Externals) {
        if (kept(external.Node)) {
            subgraph.Externals.push_back({.Id = external.Id, .Node = nodeIds[external.Node]});
        }
    }
    subgraph.Regexes = spec.Regexes;
    subgraph.CteSpools = spec.CteSpools;
    subgraph.CteConsumerStates = spec.CteConsumerStates;
    subgraph.RuntimeFilterKernel = spec.RuntimeFilterKernel;
    subgraph.RuntimeFilterBindings = spec.RuntimeFilterBindings;
    return subgraph;
}

} // namespace NScheduler
} // namespace NQdb
