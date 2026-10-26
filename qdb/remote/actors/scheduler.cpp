#include <coroio/actors/actorsystem.hpp>

#include "scheduler.h"

namespace NQdb {
namespace NRemote {

using namespace NNet::NActors;

namespace {

struct TSchedulerActor : public IBehaviorActor {
    struct TDefaultBehavior : public TBehavior<TDefaultBehavior> {
        void HandleUnknownMessage(TMessageId messageId, TBlob blob, TActorContext::TPtr ctx) { }
    };

    TSchedulerActor()
    {
        Become(&Behavior);
    }

    TDefaultBehavior Behavior;
};

} // namespace

NNet::NActors::IActor* CreateSchedulerActor()
{
    return new TSchedulerActor();
}

} // namespace NRemote
} // namespace NQdb
