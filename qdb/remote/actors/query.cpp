#include <coroio/actors/actorsystem.hpp>
#include <qdb/remote/messages.h>

#include "query.h"

namespace NQdb {
namespace NRemote {

using namespace NNet::NActors;

namespace {

struct TQueryActor : public IBehaviorActor {
    struct TDefaultBehavior : public TBehavior<TDefaultBehavior> {
        void HandleUnknownMessage(TMessageId messageId, TBlob blob, TActorContext::TPtr ctx) { }
    };

    TQueryActor()
    {
        Become(&Behavior);
    }

    TDefaultBehavior Behavior;
};

} // namespace

NNet::NActors::IActor* CreateQueryActor()
{
    return new TQueryActor();
}

} // namespace NRemote
} // namespace NQdb
