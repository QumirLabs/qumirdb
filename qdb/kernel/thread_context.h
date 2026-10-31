#pragma once

namespace NQdb {

// Mutable kernel execution state of one thread. A scheduler worker owns one
// and passes it down to every kernel call; tasks never keep it, so a task may
// resume on another worker.
class TThreadExecutionContext {
};

} // namespace NQdb
