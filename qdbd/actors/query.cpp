#include <coroio/actors/actorsystem.hpp>
#include <expected>
#include <qdbd/messages.h>
#include <qdbd/serialization.h>

#include <qdb/io/text/sink.h>

#include <sstream>

#include "query.h"
#include "qdb/compiler/query.h"
#include "qumir/error.h"

namespace NQdb {
namespace NRemote {

using namespace NNet::NActors;

namespace {

struct TQueryState {
    TActorId RespondTo;
    TSchema Schema;

    NCompiler::TQueryCompiler& Compiler;
};

struct TBaseBehavior {
    TBaseBehavior(TQueryState* state, IBehaviorActor* parent, std::function<void(const std::string&)> logger)
        : State(state)
        , Parent(parent)
        , Logger(logger)
    { }

    void HandleUnknownMessage(TMessageId messageId, TBlob blob, TActorContext::TPtr ctx) {
        std::ostringstream str; str << "Query: unknown message " <<  messageId;
        Logger(str.str());
    }

    void Complete(const std::expected<std::string, NQumir::TError>& res, TActorContext::TPtr& ctx) {
        ctx->Send<TExecuteQueryResponse>(State->RespondTo, TExecuteQueryResponse {
            .Response = res
        });
        ctx->Send<TPoison>(ctx->Self(), TPoison{});
    }

    void Execute(TExecuteQuery& req, TActorContext::TPtr& ctx) {
        Logger("Query: compiling");
        State->RespondTo = ctx->Sender();
        std::istringstream input(req.Query);
        auto parseResult = State->Compiler.Parse(input);
        if (!parseResult) {
            Complete(std::unexpected(parseResult.error()), ctx);
            return;
        }
        if (!parseResult->Explain) {
            Complete(std::unexpected(NQumir::TError(NQumir::TLocation{}, "Execution unsupported yet")), ctx);
            return;
        }

        std::ostringstream explainOutput;
        for (auto& statement : parseResult.value().Statements) {
            auto prepareResult = State->Compiler.Prepare(statement, true);
            if (!prepareResult) {
                Complete(std::unexpected(prepareResult.error()), ctx);
                return;
            }

            NCompiler::Explain(explainOutput, prepareResult->value().Plan, NCompiler::EExplainMode::Text);
        };

        Complete(explainOutput.str(), ctx);
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
            Execute(req, ctx);
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

    TQueryActor(NCompiler::TQueryCompiler& compiler, std::function<void(const std::string&)> logger)
        : State{.Compiler = compiler}
        , StartQuery(&State, this, logger)
        , MonitorQuery(&State, this, logger)
    {
        Become(&StartQuery);
    }

    TQueryState State;
    TStartQuery StartQuery;
    TMonitorQuery MonitorQuery;
};

} // namespace

NNet::NActors::IActor* CreateQueryActor(NCompiler::TQueryCompiler& compiler, std::function<void(const std::string&)> logger)
{
    return new TQueryActor(compiler, logger);
}

} // namespace NRemote
} // namespace NQdb
