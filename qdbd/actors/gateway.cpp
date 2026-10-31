#include <coroio/actors/actorsystem.hpp>
#include <qdbd/messages.h>
#include <qdbd/serialization.h>
#include "gateway.h"
#include "query.h"


namespace NQdb {
namespace NRemote {

using namespace NNet::NActors;

namespace {

struct TGatewayActor : public IBehaviorActor {
    struct TDefaultBehavior : public TBehavior<TDefaultBehavior, TExecuteQuery> {
        void Receive(TExecuteQuery&& req, TBlob, TActorContext::TPtr ctx) {
            auto* queryActor = CreateQueryActor();
            auto queryActorId = ctx->Register(std::unique_ptr<IActor>(queryActor));
            ctx->Forward<TExecuteQuery>(queryActorId, std::move(req));
        }

        void HandleUnknownMessage(TMessageId messageId, TBlob blob, TActorContext::TPtr ctx) { }
    };

    TGatewayActor()
    {
        Become(&Behavior);
    }

    TDefaultBehavior Behavior;
};

} // namespace

NNet::NActors::IActor* CreateGatewayActor()
{
    return new TGatewayActor();
}

} // namespace NRemote
} // namespace NQdb
