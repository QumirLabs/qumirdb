#include <coroio/actors/actorsystem.hpp>
#include <qdb/remote/messages.h>
#include <qdb/remote/serialization.h>
#include "gateway.h"


namespace NQdb {
namespace NRemote {

using namespace NNet::NActors;

namespace {

struct TGatewayActor : public IBehaviorActor {
    struct TDefaultBehavior : public TBehavior<TDefaultBehavior, TExecuteQuery> {
        void Receive(TExecuteQuery&&, TBlob, TActorContext::TPtr ctx) {
            // TODO: implement
            ctx->Send<TExecuteQueryResponse>(ctx->Sender(), TExecuteQueryResponse{});
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
