#pragma once

#include <qdb/scheduler/graph.h>
#include <qdb/scheduler/settings/settings.h>

#include <string>

namespace NQdb {
namespace NScheduler {

class TSchedulerExecutor {
public:
    TSchedulerExecutor(
        TTaskNode& root,
        size_t taskCount,
        TSettings settings);

    bool Run(std::string* error = nullptr);
    TSchedulerRunStats Stats() const;

private:
    TTaskNode& Root_;
    size_t TaskCount_;
    TSettings Settings_;
    TSchedulerRunStats Stats_;
};

} // namespace NScheduler
} // namespace NQdb
