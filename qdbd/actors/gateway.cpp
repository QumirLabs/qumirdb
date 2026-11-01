#include <coroio/actors/actorsystem.hpp>
#include <qdbd/messages.h>
#include <qdbd/serialization.h>
#include <sstream>
#include "gateway.h"
#include "query.h"


namespace NQdb {
namespace NRemote {

using namespace NNet::NActors;

namespace {

struct TGatewayActor : public IBehaviorActor {
    struct TDefaultBehavior : public TBehavior<TDefaultBehavior, TExecuteQuery> {
        TDefaultBehavior(std::function<void(const std::string&)> logger)
            : Logger(logger)
        { }

        void Receive(TExecuteQuery&& req, TBlob, TActorContext::TPtr ctx) {
            auto* queryActor = CreateQueryActor();
            auto queryActorId = ctx->Register(std::unique_ptr<IActor>(queryActor));
            Logger("Gateway: got request");
            ctx->Forward<TExecuteQuery>(queryActorId, std::move(req));
        }

        void HandleUnknownMessage(TMessageId messageId, TBlob blob, TActorContext::TPtr ctx) {
            std::ostringstream str; str << "Gateway: unknown message " <<  messageId;
            Logger(str.str());
        }

        std::function<void(const std::string&)> Logger;
    };

    TGatewayActor(std::function<void(const std::string&)> logger)
        : Behavior(logger)
    {
        Become(&Behavior);
    }

    TDefaultBehavior Behavior;
};

} // namespace

NNet::NActors::IActor* CreateGatewayActor(std::function<void(const std::string&)> logger)
{
    return new TGatewayActor(logger);
}

} // namespace NRemote
} // namespace NQdb
