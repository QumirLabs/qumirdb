#pragma once

#include <qdb/exec/runtime_context.h>
#include <qdb/exec/runtime_filter.h>
#include <qdb/kernel/compiler.h>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace NQdb::NTest {

// Runtime filters work only through the runtime-filter kernel. It is compiled
// once per test process; LLVM must be initialized first (TLLVMInitializer).
inline std::shared_ptr<TRuntimeContext> RuntimeFilterContext() {
    static const std::shared_ptr<TRuntimeContext> context = [] {
        auto context = std::make_shared<TRuntimeContext>();
        context->RuntimeFilterKernels = TKernelCompiler().CompileRuntimeFilter();
        return context;
    }();
    return context;
}

// Probes the way a consumer task does: an unpublished or inactive filter
// passes every row. Returns one flag per hash.
inline std::vector<uint8_t> ProbeRuntimeFilter(
    IRuntimeFilterProbe& filter, std::span<const uint64_t> hashes)
{
    std::vector<uint8_t> output(hashes.size(), 0xff);
    auto state = filter.CreateProbeState();
    if (filter.BeginBatch(*state, hashes.size())) {
        filter.MayContainBatch(hashes, output, *state);
    }
    return output;
}

inline bool RuntimeFilterMayContain(IRuntimeFilterProbe& filter, uint64_t hash) {
    return ProbeRuntimeFilter(filter, {&hash, 1}).front() != 0;
}

} // namespace NQdb::NTest
