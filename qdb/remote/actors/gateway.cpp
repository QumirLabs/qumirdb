#include <coroio/actors/actorsystem.hpp>
#include <qdb/remote/messages.h>
#include "gateway.h"


namespace NNet::NActors {

using namespace NQdb::NRemote;

template<>
void SerializeToStream<TExecuteQuery>(const TExecuteQuery& obj, std::ostringstream& oss) {
    oss << obj.Query;
}

template<>
void DeserializeFromStream<TExecuteQuery>(TExecuteQuery& obj, std::istringstream& iss) {
    obj.Query = iss.str();
}

template<>
void SerializeToStream<TExecuteQueryResponse>(const TExecuteQueryResponse& obj, std::ostringstream& oss) {
    oss << obj.Response;
}

template<>
void DeserializeFromStream<TExecuteQueryResponse>(TExecuteQueryResponse& obj, std::istringstream& iss) {
    obj.Response = iss.str();
}

} // namespace NNet::NActors


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
