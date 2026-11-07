#include "actor_task_node.h"

namespace NQdb {
namespace NScheduler {

TActorTaskNode::TActorTaskNode(NNet::INotifier& notifier)
    : Notifier(notifier)
{ }

ETaskResult TActorTaskNode::Execute() {
    Notifier.Notify();
    return ETaskResult::OK; // TODO
}

} // namespace NScheduler
} // namespace NQdb
