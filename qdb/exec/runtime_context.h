#pragma once

#include <qdb/kernel/regex.h>

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace NQdb {

struct TKernelSlot;

// Query-lifetime runtime services shared by every physical stage. Keep the
// scalar kernel ABI in terms of direct handles; this object owns those handles
// and is the extension point for future query-scoped runtime state.
struct TRuntimeContext {
    NKernel::TRegexRegistry Regexes;
    // The query's runtime-filter kernel; set before any filter is created.
    std::shared_ptr<TKernelSlot> RuntimeFilterKernels;
    // Index of that kernel in the sink (0 without a sink).
    uint32_t RuntimeFilterKernel = 0;
};

// Rebuilds a context from data: the regex specs are registered in order, so
// every regex keeps its id, and the runtime-filter kernel is bound to the slot.
inline std::shared_ptr<TRuntimeContext> MakeRuntimeContext(
    const std::vector<NKernel::TRegexSpec>& regexSpecs,
    std::shared_ptr<TKernelSlot> runtimeFilterSlot)
{
    auto context = std::make_shared<TRuntimeContext>();
    for (const auto& spec : regexSpecs) {
        context->Regexes.Register(spec.Pattern, spec.Replacement);
    }
    context->RuntimeFilterKernels = std::move(runtimeFilterSlot);
    return context;
}

} // namespace NQdb
