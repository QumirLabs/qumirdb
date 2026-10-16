#pragma once

namespace NQdb {

struct TOperatorInstrumentation;

namespace NScheduler {

// Mutable operator state owns its observer. Prepared code can be shared by
// tasks and queries without capturing their counters.
class ITaskState {
public:
    virtual ~ITaskState() = default;
    virtual void ConfigureInstrumentation(TOperatorInstrumentation*) {}
};

} // namespace NScheduler
} // namespace NQdb
