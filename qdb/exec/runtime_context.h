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
    // The query's runtime-filter kernel; set before any filter is created.
    std::shared_ptr<TKernelSlot> RuntimeFilterKernels;
    // The query's parquet decoder kernel, shared by its parquet sources.
    std::shared_ptr<TKernelSlot> ParquetDecoder;
};

} // namespace NQdb
