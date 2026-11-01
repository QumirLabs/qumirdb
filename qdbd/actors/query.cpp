#include <coroio/actors/actorsystem.hpp>
#include <qdbd/messages.h>
#include <qdbd/serialization.h>

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
    TBaseBehavior(TQueryState* state, IBehaviorActor* parent, std::function<void(const std::string&)> logger)
        : State(state)
        , Parent(parent)
        , Logger(logger)
    { }

    void HandleUnknownMessage(TMessageId messageId, TBlob blob, TActorContext::TPtr ctx) { }

    void Compile(TExecuteQuery& req, TActorId clientId) {
        Logger("Query: compiling");
        State->RespondTo = clientId;
        // TODO:
    }

    void StartGraph(const std::vector<TActorId>& actorIds, TActorContext::TPtr& ctx) {
        // TODO:
    }

    TQueryState* State;
    IBehaviorActor* Parent;
    std::function<void(const std::string&)> Logger;
};

struct TQueryActor : public IBehaviorActor {
    struct TStartQuery : public TBaseBehavior, public TBehavior<TStartQuery, TExecuteQuery, TSpawnComputeActorResponse> {
        TStartQuery(TQueryState* state, IBehaviorActor* parent, std::function<void(const std::string&)> logger)
            : TBaseBehavior(state, parent, logger)
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
        TMonitorQuery(TQueryState* state, IBehaviorActor* parent, std::function<void(const std::string&)> logger)
            : TBaseBehavior(state, parent, logger)
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

    TQueryActor(std::function<void(const std::string&)> logger)
        : StartQuery(&State, this, logger)
        , MonitorQuery(&State, this, logger)
    {
        Become(&StartQuery);
    }

    TQueryState State;
    TStartQuery StartQuery;
    TMonitorQuery MonitorQuery;
};

} // namespace

NNet::NActors::IActor* CreateQueryActor(std::function<void(const std::string&)> logger)
{
    return new TQueryActor(logger);
}

} // namespace NRemote
} // namespace NQdb
