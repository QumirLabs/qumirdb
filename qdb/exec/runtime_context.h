#pragma once

#include <qdb/kernel/regex.h>

#include <memory>

namespace NQdb {

struct TKernelSlot;

// Query-lifetime runtime services shared by every physical stage. Keep the
// scalar kernel ABI in terms of direct handles; this object owns those handles
// and is the extension point for future query-scoped runtime state.
struct TRuntimeContext {
    NKernel::TRegexRegistry Regexes;
    // Set before any runtime filter is created (see TRuntimeFilterKernels).
    std::shared_ptr<TKernelSlot> RuntimeFilterKernels;
};

} // namespace NQdb
