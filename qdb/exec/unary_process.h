#pragma once

#include <qdb/io/io.h>
#include <qdb/kernel/execution_context.h>

#include <cstdint>
#include <functional>
#include <vector>

namespace NQdb {

// Per-batch scratch for a streaming (filter/project) kernel: a reusable
// selection vector the kernel narrows in place.
struct TUnaryStreamingKernelState {
    std::vector<uint8_t> Selection;
};

// A streaming transform applied to one batch in place (filter/project). The
// scheduler wraps this in a unary streaming task.
using TUnaryStreamProcess = std::function<void(
    TExecutionContext&, TRowSet&, TUnaryStreamingKernelState&)>;

} // namespace NQdb
