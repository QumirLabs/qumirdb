#include <coroio/actors/actorsystem.hpp>
#include <qdb/remote/messages.h>
#include <qdb/remote/serialization.h>

#include <qdb/io/text/sink.h>

#include <sstream>

#include "query.h"

namespace NQdb {
namespace NRemote {

using namespace NNet::NActors;

namespace {

struct TQueryState {
    TActorId RespondTo;
    TSchema Schema;
};

struct TBaseBehavior {
    TBaseBehavior(TQueryState* state, IBehaviorActor* parent)
        : State(state)
        , Parent(parent)
    { }

    void HandleUnknownMessage(TMessageId messageId, TBlob blob, TActorContext::TPtr ctx) { }

    void Compile(TExecuteQuery& req, TActorId clientId) {
        State->RespondTo = clientId;
        // TODO:
    }

    void StartGraph(const std::vector<TActorId>& actorIds, TActorContext::TPtr& ctx) {
        // TODO:
    }

    TQueryState* State;
    IBehaviorActor* Parent;
};

struct TQueryActor : public IBehaviorActor {
    struct TStartQuery : public TBaseBehavior, public TBehavior<TStartQuery, TExecuteQuery, TSpawnComputeActorResponse> {
        TStartQuery(TQueryState* state, IBehaviorActor* parent)
            : TBaseBehavior(state, parent)
        { }

        void Receive(TExecuteQuery&& req, TBlob, TActorContext::TPtr ctx) {
            Compile(req, ctx->Sender());
        }

        void Receive(TSpawnComputeActorResponse&& req, TBlob, TActorContext::TPtr ctx) {
            StartGraph(req.ActorIds, ctx);
            Parent->Become(&static_cast<TQueryActor*>(Parent)->MonitorQuery);
        }
    };

    struct TMonitorQuery : public TBaseBehavior, public TBehavior<TMonitorQuery, TDataChunk, TFinish> {
        TMonitorQuery(TQueryState* state, IBehaviorActor* parent)
            : TBaseBehavior(state, parent)
            , Sink(state->Schema, Out)
        { }

        void Receive(TDataChunk&& req, TBlob, TActorContext::TPtr ctx) {
            Sink.Write(req.RowSet);
        }

        void Receive(TFinish&& req, TBlob, TActorContext::TPtr ctx) {
            Sink.Flush();

            ctx->Send<TExecuteQueryResponse>(State->RespondTo, TExecuteQueryResponse {
                .Response = Out.str()
            });
            ctx->Send<TPoison>(ctx->Self(), TPoison{});
        }

        std::ostringstream Out;
        TConsoleSink Sink;
    };

    TQueryActor()
        : StartQuery(&State, this)
        , MonitorQuery(&State, this)
    {
        Become(&StartQuery);
    }

    TQueryState State;
    TStartQuery StartQuery;
    TMonitorQuery MonitorQuery;
};

} // namespace

NNet::NActors::IActor* CreateQueryActor()
{
    return new TQueryActor();
}

} // namespace NRemote
} // namespace NQdb
