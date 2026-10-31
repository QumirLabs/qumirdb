#pragma once

#include <gtest/gtest.h>

#include <qdb/exec/runtime_context.h>
#include <qdb/exec/runtime_filter.h>
#include <qdb/kernel/compiler.h>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace NQdb::NTest {

// Runtime filters work only through the runtime-filter kernel. Each test
// compiles its own, the way lowering does for a query. LLVM must be
// initialized first (TLLVMInitializer).
class TRuntimeFilterTest : public testing::Test {
protected:
    void SetUp() override {
        Context_ = std::make_shared<TRuntimeContext>();
        Context_->RuntimeFilterKernels = TKernelCompiler().CompileRuntimeFilter();
    }

    std::shared_ptr<TRuntimeContext> Context_;
};

// Probes the way a consumer task does: an unpublished or inactive filter
// passes every row. Returns one flag per hash.
inline std::vector<uint8_t> ProbeRuntimeFilter(
    IRuntimeFilterProbe& filter, std::span<const uint64_t> hashes)
{
    std::vector<uint8_t> output(hashes.size(), 0xff);
    auto state = filter.CreateProbeState();
    if (filter.BeginBatch(*state, hashes.size())) {
        TExecutionContext context;
        filter.MayContainBatch(context, hashes, output, *state);
    }
    return output;
}

inline bool RuntimeFilterMayContain(IRuntimeFilterProbe& filter, uint64_t hash) {
    return ProbeRuntimeFilter(filter, {&hash, 1}).front() != 0;
}

} // namespace NQdb::NTest
