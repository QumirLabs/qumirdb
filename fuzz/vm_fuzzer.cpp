#include "pipeline.h"

#include <qdb/io/io.h>
#include <qdb/io/schema.h>
#include <qdb/io/text/sink.h>
#include <qdb/kernel/finalize.h>
#include <qdb/scheduler/lowering/plan_lowerer.h>

#include <qumir/parser/type.h>

#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace NQdb;

// A cross product of more scans takes seconds even over five-row tables and
// would read as a hang.
constexpr size_t MaxScans = 6;

// Lowering, VM kernel compilation and a single-threaded run into a CSV sink, as
// `qdb --sql --kernel:backend=vm --scheduler:mode=single` runs the query.
void Execute(const NCompiler::TPreparedQuery& prepared, bool explain, size_t scans) {
    if (explain) {
        std::ostringstream output;
        NCompiler::Explain(output, prepared.Plan, NCompiler::EExplainMode::Text);
        return;
    }
    if (scans > MaxScans) {
        return;
    }
    NScheduler::TSettings settings;
    settings.Scheduler.Mode = NScheduler::EExecutionMode::SingleThreadedScheduler;
    settings.Scheduler.WorkerCount = 1;
    settings.ScanSplit.MaxScanTasks = 1;
    settings.Kernel.Backend = EKernelBackend::Vm;
    auto lowered = NScheduler::LowerPlanToGraph(
        prepared.Plan, settings, nullptr, prepared.Annotation.ExternalCatalog);
    FinalizeKernels(lowered.Kernels, settings.Kernel.Backend, nullptr, lowered.ExternalCatalog);
    const auto outputType = lowered.OutputType;
    auto output = NQumir::NAst::TMaybeType<NQumir::NAst::TStructType>(outputType);
    if (!output) {
        return;
    }
    std::vector<TColumnSchema> columns;
    for (const auto& [name, type] : output.Cast()->Fields) {
        columns.push_back({.Name = name, .Type = type});
    }
    const TSchema schema{columns};
    std::ostringstream text;
    TCsvSink sink(schema, text);
    std::string error;
    NScheduler::RunPlanIntoSink(std::move(lowered), sink, settings, nullptr, &error);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    NFuzz::RunStatements(std::string_view(reinterpret_cast<const char*>(data), size), Execute);
    return 0;
}
