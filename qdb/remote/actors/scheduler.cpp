#include <coroio/actors/actorsystem.hpp>
#include <qdb/remote/messages.h>
#include "scheduler.h"

namespace NQdb {
namespace NRemote {

using namespace NNet::NActors;

namespace {

struct TSchedulerActor : public IBehaviorActor {
    struct TDefaultBehavior : public TBehavior<TDefaultBehavior, TPing> {
        TDefaultBehavior(TNodeId leaderId)
            : LeaderId(leaderId)
        { }

        void Receive(TPing&&, TBlob, TActorContext::TPtr ctx)
        {
        }

        void HandleUnknownMessage(TMessageId messageId, TBlob blob, TActorContext::TPtr ctx) { }

        TNodeId LeaderId;
    };

    TSchedulerActor(TNodeId leaderId)
        : Behavior(leaderId)
    {
        Become(&Behavior);
    }

    TDefaultBehavior Behavior;
};

} // namespace

NNet::NActors::IActor* CreateSchedulerActor(NNet::NActors::TNodeId leaderId)
{
    return new TSchedulerActor(leaderId);
}

} // namespace NRemote
} // namespace NQdb
