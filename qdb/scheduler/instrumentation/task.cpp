#include <qdb/scheduler/instrumentation/task.h>

#include <qdb/exec/plan_builder.h>
#include <qdb/kernel/compiler.h>
#include <qdb/scheduler/plan_lowerer.h>

#include <algorithm>
#include <ctime>
#include <iomanip>
#include <map>
#include <ostream>
#include <stdexcept>
#include <tuple>

namespace NQdb {
namespace NScheduler {

TTaskInstrumentationSetup::TTaskInstrumentationSetup(
    TTaskNode& node,
    const std::unordered_map<const IConnection*, NQumir::NAst::TTypePtr>& types)
    : Node_(node)
    , Types_(types)
{}

void TTaskInstrumentationSetup::Input(TInputPort& port) {
    if (!port.Connection) {
        return;
    }
    auto& counter = Node_.Instrumentation->Inputs.emplace_back();
    counter.Kernel = Node_.Instrumentation->Kernel;
    counter.Widths = InstrumentationColumnWidths(Types_.at(port.Connection));
    port.Instrumentation = &counter;
}

void TTaskInstrumentationSetup::Output(TOutputPort& port) {
    if (port.Connection) {
        port.Instrumentation = &Output(*port.Connection);
    }
}

TRowSetInstrumentation& TTaskInstrumentationSetup::Output(IConnection& connection) {
    auto& output = Node_.Instrumentation->Output;
    output.Kernel = Node_.Instrumentation->Kernel;
    output.Widths = InstrumentationColumnWidths(Types_.at(&connection));
    return output;
}

TOperatorInstrumentation& TTaskInstrumentationSetup::Counters() {
    return *Node_.Instrumentation;
}

void EnableOperatorInstrumentation(TLoweredPlan& lowered) {
    auto plan = BuildExecPlan(lowered);
    if (!plan) {
        throw std::runtime_error("operator instrumentation: " + plan.error());
    }
    std::unordered_map<TExecStageId, NQumir::NAst::TTypePtr> stages;
    for (const auto& node : plan->Nodes) {
        // Synthetic plan nodes have no semantic stage. Their output schema
        // must not become the schema of every physical routing task.
        if (node.StageId != InvalidExecStageId) {
            stages.emplace(node.StageId, node.OutputType);
        }
    }
    std::unordered_map<const IConnection*, NQumir::NAst::TTypePtr> types;
    types.emplace(lowered.FinalGather, lowered.OutputType);
    // Semantic stages define schemas. Routing tasks preserve their input type;
    // propagate until every physical edge is typed, independent of node order.
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& node : lowered.Graph->Nodes()) {
            auto type = stages.contains(node->ExecStageId)
                ? stages.at(node->ExecStageId)
                : nullptr;
            if (!type) {
                for (const auto* edge : node->Inbound) {
                    if (edge->Kind == ETaskEdgeKind::Rowset && types.contains(edge->Connection)) {
                        type = types.at(edge->Connection);
                        break;
                    }
                }
            }
            if (type) {
                for (const auto* edge : node->Outbound) {
                    if (edge->Kind == ETaskEdgeKind::Rowset) {
                        changed |= types.emplace(edge->Connection, type).second;
                    }
                }
            }
        }
    }
    TKernelCompiler compiler({
        .Sink = &lowered.Kernels,
        .BindNow = false,
        .ExternalCatalog = lowered.ExternalCatalog,
    });
    auto kernel = compiler.CompileInstrumentation();
    for (const auto& node : lowered.Graph->Nodes()) {
        node->Instrumentation = std::make_unique<TOperatorInstrumentation>();
        auto& counters = *node->Instrumentation;
        counters.Kernel = kernel;
        counters.Inputs.reserve(node->Inbound.size() + 2);
        counters.MaterializesOutput = node->DebugKind == "aggregate" ||
            node->DebugKind == "join" || node->DebugKind == "join-probe" ||
            node->DebugKind == "cross-join" ||
            node->DebugKind == "sort" || node->DebugKind == "top-sort" ||
            node->DebugKind == "window" || node->DebugKind == "late-materialize" ||
            node->DebugKind == "hash-shuffle" || node->DebugKind == "cte-producer";
        TTaskInstrumentationSetup setup(*node, types);
        node->Task->ConfigureInstrumentation(setup);
    }
}

namespace {

uint64_t ThreadCpuNanoseconds() {
    timespec value{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) != 0) {
        throw std::runtime_error("thread CPU clock is unavailable");
    }
    return static_cast<uint64_t>(value.tv_sec) * 1'000'000'000 + value.tv_nsec;
}

struct TStageSnapshot {
    std::vector<uint64_t> TaskRows;
    uint64_t ActualRowsIn = 0;
    uint64_t RowsOut = 0;
    uint64_t BytesIn = 0;
    uint64_t BytesOut = 0;
    uint64_t Cpu = 0;
    uint64_t Groups = 0;
    uint64_t Capacity = 0;
    uint64_t Growths = 0;
    uint64_t Rehashes = 0;
    uint64_t HashMemory = 0;
    uint64_t RetainedInput = 0;
    uint64_t RetainedInputPeak = 0;
    uint64_t BuildMemory = 0;
    uint64_t Materialized = 0;
    uint64_t RowLayoutTasks = 0;
    bool HasHash = false;
    bool HasBuildMemory = false;
    bool Materializes = false;

    void Add(const TOperatorInstrumentation& counters) {
        uint64_t inputRows = 0;
        for (const auto& input : counters.Inputs) {
            inputRows += input.Rows;
            BytesIn += input.Bytes;
        }
        TaskRows.push_back(counters.Inputs.empty() ? counters.Output.Rows : inputRows);
        ActualRowsIn += inputRows;
        RowsOut += counters.Output.Rows;
        BytesOut += counters.Output.Bytes;
        Cpu += counters.ExecutionCpuNanoseconds;
        Groups += counters.Groups;
        Capacity += counters.HashCapacity;
        Growths += counters.HashGrowthEvents;
        Rehashes += counters.Rehashes;
        HashMemory += counters.HashMemoryPeakBytes;
        RetainedInput += counters.RetainedInputBytes;
        RetainedInputPeak += counters.RetainedInputPeakBytes;
        BuildMemory += counters.BuildMemoryPeakBytes;
        Materialized += counters.MaterializesOutput ? counters.Output.Bytes : 0;
        RowLayoutTasks += counters.RowLayout;
        HasHash |= counters.HasHash;
        HasBuildMemory |= counters.HasBuildMemory;
        Materializes |= counters.MaterializesOutput;
    }
};

void WriteSnapshot(std::ostream& out, const TStageSnapshot& value) {
    uint64_t rowsIn = 0;
    for (const auto rows : value.TaskRows) {
        rowsIn += rows;
    }
    out << " rows_in=" << value.ActualRowsIn << " rows_out=" << value.RowsOut
        << " bytes_in=" << value.BytesIn << " bytes_out=" << value.BytesOut
        << " execution_cpu_seconds=" << std::setprecision(9)
        << static_cast<double>(value.Cpu) / 1e9;
    if (value.HasHash) {
        out << " groups=" << value.Groups << " hash_capacity=" << value.Capacity
            << " hash_growth_events=" << value.Growths << " rehashes=" << value.Rehashes
            << " hash_memory_peak_bytes=" << value.HashMemory
            << " hash_row_layout_tasks=" << value.RowLayoutTasks;
    } else {
        out << " groups=null hash_capacity=null hash_growth_events=null rehashes=null"
            << " hash_memory_peak_bytes=null hash_row_layout_tasks=null";
    }
    if (value.HasBuildMemory) {
        out << " retained_input_bytes=" << value.RetainedInput
            << " retained_input_peak_bytes=" << value.RetainedInputPeak
            << " build_memory_peak_bytes=" << value.BuildMemory;
    } else {
        out << " retained_input_bytes=null retained_input_peak_bytes=null build_memory_peak_bytes=null";
    }
    out << " materialized_bytes=";
    if (value.Materializes) {
        out << value.Materialized;
    } else {
        out << "null";
    }
    auto bounds = std::minmax_element(value.TaskRows.begin(), value.TaskRows.end());
    out << " task_rows_min=" << *bounds.first << " task_rows_max=" << *bounds.second
        << " partition_skew=";
    if (rowsIn) {
        out << static_cast<double>(*bounds.second) * value.TaskRows.size() / rowsIn;
    } else {
        out << "null";
    }
    out << '\n';
}

} // namespace

ETaskResult ExecuteTask(TTaskNode& node) {
    if (!node.Instrumentation) {
        return node.Task->Execute();
    }
    const auto start = ThreadCpuNanoseconds();
    const auto result = node.Task->Execute();
    node.Instrumentation->ExecutionCpuNanoseconds += ThreadCpuNanoseconds() - start;
    return result;
}

void PrintOperatorInstrumentation(std::ostream& out, const TTaskGraph& graph) {
    out << "\noperator-instrumentation schema_version=1 bytes=logical_nonnull_payload"
        << " memory=batch_boundary_hash_plus_retained_payload stage_memory=sum_task_peaks cpu=thread_execute\n";
    using TKey = std::tuple<TExecStageId, TTaskGroupId, std::string>;
    std::map<TKey, TStageSnapshot> stages;
    size_t task = 0;
    for (const auto& node : graph.Nodes()) {
        if (!node->Instrumentation) {
            continue;
        }
        const auto& counters = *node->Instrumentation;
        TStageSnapshot value;
        value.Add(counters);
        auto label = node->DebugLabel;
        std::replace(label.begin(), label.end(), '\n', ' ');
        std::replace(label.begin(), label.end(), '\r', ' ');
        out << "operator-task task=" << task++ << " stage=" << node->ExecStageId
            << " group=" << node->TaskGroupId << " kind=" << node->DebugKind
            << " label=" << std::quoted(label)
            << " finished=" << (node->State.load(std::memory_order_relaxed) == ETaskState::Finished);
        WriteSnapshot(out, value);
        for (size_t input = 0; input < counters.Inputs.size(); ++input) {
            out << "operator-input task=" << task - 1 << " input=" << input
                << " rows=" << counters.Inputs[input].Rows
                << " bytes=" << counters.Inputs[input].Bytes << '\n';
        }
        stages[{node->ExecStageId, node->TaskGroupId, node->DebugKind}].Add(counters);
    }
    for (const auto& [key, value] : stages) {
        const auto& [stage, group, kind] = key;
        out << "operator-stage stage=" << stage << " group=" << group << " kind=" << kind
            << " tasks=" << value.TaskRows.size();
        WriteSnapshot(out, value);
    }
}

} // namespace NScheduler
} // namespace NQdb
