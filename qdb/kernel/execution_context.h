#pragma once

namespace NQdb {

// Mutable execution state of one worker. The worker owns it and passes it down
// to every kernel call; tasks never keep it, so a task may resume on another
// worker.
class TExecutionContext {
};

} // namespace NQdb
