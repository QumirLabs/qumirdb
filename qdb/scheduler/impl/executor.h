#pragma once

#include <qdb/scheduler/graph.h>
#include <qdb/scheduler/settings/settings.h>

#include <string>

namespace NQdb {
namespace NScheduler {

// Runs the serializable part (sealed graph, kernels) with a dynamic part, the
// scheduler and its task heaps, that lives only inside Run(). Task and connection
// destructors run after it, so they must not free task-heap blocks.
class TSchedulerExecutor {
public:
    // Gets only the static graph part, the sealed root. The other static part, the
    // kernels, is TLoweredPlan::Kernels: FinalizeKernels compiles it into the kernel
    // slots the tasks share before Run().
    TSchedulerExecutor(TSealedTaskGraph graph, TSettings settings);

    bool Run(std::string* error = nullptr);
    TSchedulerRunStats Stats() const;

private:
    TSealedTaskGraph Graph_;
    TSettings Settings_;
    TSchedulerRunStats Stats_;
};

} // namespace NScheduler
} // namespace NQdb
