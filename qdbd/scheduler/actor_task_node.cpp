#include "actor_task_node.h"

namespace NQdb {
namespace NScheduler {

TActorTaskNode::TActorTaskNode(TActorNotificationChannel& actorChannel)
    : ActorChannel(actorChannel)
{ }

ETaskResult TActorTaskNode::Execute() {
    if (Queued.exchange(true) == false) {
        if (ActorChannel.Notify(this)) {
            return ETaskResult::WAITING;
        } else {
            // full queue, retry on next scheduler iteration
            return ETaskResult::OK;
        }
    }
    return ETaskResult::WAITING;
}

} // namespace NScheduler
} // namespace NQdb
