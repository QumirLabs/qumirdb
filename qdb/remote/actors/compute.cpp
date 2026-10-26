#include <coroio/actors/actorsystem.hpp>
#include <qdb/remote/messages.h>

#include "compute.h"

namespace NQdb {
namespace NRemote {

using namespace NNet::NActors;

namespace {

struct TComputeActor : public IBehaviorActor {
    struct TDefaultBehavior : public TBehavior<TDefaultBehavior> {
        void HandleUnknownMessage(TMessageId messageId, TBlob blob, TActorContext::TPtr ctx) { }
    };

    TComputeActor()
    {
        Become(&Behavior);
    }

    TDefaultBehavior Behavior;
};

} // namespace

NNet::NActors::IActor* CreateComputeActor()
{
    return new TComputeActor();
}

} // namespace NRemote
} // namespace NQdb
