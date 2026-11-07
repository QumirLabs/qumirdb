#pragma once

#include <qdb/scheduler/task.h>

#include <coroio/notifier.hpp>

namespace NQdb {
namespace NScheduler {

class TActorTaskNode : public ITaskNode {
public:
    TActorTaskNode(NNet::INotifier& notifier);

    ETaskResult Execute() override;

private:
    NNet::INotifier& Notifier;
};

} // namespace NScheduler
} // namespace NQdb
