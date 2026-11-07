#pragma once

#include <qdb/scheduler/graph.h>
#include <qdb/scheduler/settings/settings.h>

#include <string>

namespace NQdb {
namespace NScheduler {

class TSchedulerExecutor {
public:
    TSchedulerExecutor(
        TTaskGraph& graph,
        TSettings settings);

    bool Run(std::string* error = nullptr);
    TSchedulerRunStats Stats() const;

private:
    TTaskGraph& Graph_;
    TSettings Settings_;
    TSchedulerRunStats Stats_;
};

} // namespace NScheduler
} // namespace NQdb
