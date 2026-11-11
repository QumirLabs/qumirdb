#include <coroio/actors/actorsystem.hpp>
#include <expected>
#include <qdbd/actors.h>
#include <qdbd/messages.h>
#include <qdbd/partitioner/partitioner.h>
#include <qdbd/serialization.h>

#include <qdb/io/text/sink.h>
#include <qdb/kernel/image.h>
#include <qdb/scheduler/lowering/plan_lowerer.h>
#include <qdb/scheduler/spec/graph_spec.h>

#include <algorithm>
#include <chrono>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <unordered_map>

#include "query.h"
#include "collector.h"
#include "qdb/compiler/query.h"
#include "qumir/error.h"

namespace NQdb {
namespace NRemote {

using namespace NNet::NActors;

namespace {

// The external id of the sink that sends the query's rows to the query actor.
constexpr uint64_t ResultSinkId = 0;

using TClock = std::chrono::steady_clock;

double Seconds(TClock::time_point from)
{
    return std::chrono::duration<double>(TClock::now() - from).count();
}

uint64_t SelectedRows(const TRowSet& rows)
{
    if (!rows.Selection) {
        return static_cast<uint64_t>(rows.RowCount);
    }
    return static_cast<uint64_t>(std::count_if(rows.Selection, rows.Selection + rows.RowCount,
        [](uint8_t keep) { return keep != 0; }));
}

struct TQueryState {
    TActorId RespondTo;
    TActorId Collector;
    // Stopped when the query completes.
    std::vector<TActorId> ComputeActors;

    NCompiler::TQueryCompiler& Compiler;
    NScheduler::TSettings Settings;
    // Lowered on the request, split once the compute actors are known.
    std::optional<NScheduler::TLoweredPlan> Lowered;
    NScheduler::TSealedTaskGraph Sealed;
    // The sink keeps a reference to the schema, the schema a span of the columns.
    std::vector<TColumnSchema> Columns;
    TSchema Schema;
    std::ostringstream Out;
    std::unique_ptr<TCsvSink> Sink;
    TQueryTiming Timing;
    TClock::time_point ExecuteStart;
};

struct TBaseBehavior {
    TBaseBehavior(TQueryState* state, IBehaviorActor* parent, std::function<void(const std::string&)> logger)
        : State(state)
        , Parent(parent)
        , Logger(logger)
    { }

    void HandleUnknownMessage(TMessageId messageId, TBlob blob, TActorContext::TPtr ctx) {
        std::ostringstream str;
        str << "Query: unknown message " << messageId;
        Logger(str.str());
    }

    void Complete(const std::expected<std::string, NQumir::TError>& res, TActorContext::TPtr& ctx, bool unavailable = false,
        std::optional<TQueryTiming> timing = std::nullopt)
    {
        for (const auto actor : State->ComputeActors) {
            ctx->Send<TPoison>(actor, TPoison{});
        }
        State->ComputeActors.clear();
        ctx->Send<TExecuteQueryResponse>(State->RespondTo, TExecuteQueryResponse {
            .Response = res,
            .Unavailable = unavailable,
            .Timing = timing
        });
        ctx->Send<TPoison>(ctx->Self(), TPoison{});
    }

    void Fail(std::string message, TActorContext::TPtr& ctx) {
        Complete(std::unexpected(NQumir::TError(NQumir::TLocation{}, std::move(message))), ctx);
    }

    void Execute(TExecuteQuery& req, TActorContext::TPtr& ctx) {
        Logger("Query: compiling");
        const auto planStart = TClock::now();
        State->RespondTo = ctx->Sender();
        std::istringstream input(req.Query);
        auto parseResult = State->Compiler.Parse(input);
        if (!parseResult) {
            Complete(std::unexpected(parseResult.error()), ctx);
            return;
        }
        if (parseResult->Explain) {
            std::ostringstream explainOutput;
            for (auto& statement : parseResult.value().Statements) {
                auto prepareResult = State->Compiler.Prepare(statement, parseResult->Explain);
                if (!prepareResult) {
                    Complete(std::unexpected(prepareResult.error()), ctx);
                    return;
                }
                NCompiler::Explain(explainOutput, prepareResult->value().Plan, NCompiler::EExplainMode::Text);
            }
            Complete(explainOutput.str(), ctx);
            return;
        }

        std::optional<NCompiler::TPreparedQuery> query;
        for (auto& statement : parseResult->Statements) {
            auto prepared = State->Compiler.Prepare(statement);
            if (!prepared) {
                Complete(std::unexpected(prepared.error()), ctx);
                return;
            }
            if (!*prepared) {
                continue;
            }
            if (query) {
                Fail("distributed execution runs one SELECT statement", ctx);
                return;
            }
            query = std::move(**prepared);
        }
        if (!query) {
            Fail("no SELECT statement to run", ctx);
            return;
        }
        State->Timing.Planning = Seconds(planStart);
        try {
            const auto buildStart = TClock::now();
            Lower(*query);
            State->Timing.KernelBuild = Seconds(buildStart);
        } catch (const std::exception& e) {
            Fail(e.what(), ctx);
            return;
        }
        // One compute actor per live node: the scheduler caps the count at the node count.
        ctx->Send<TSpawnComputeActor>(
            WellKnownActorId(EWellKnownActors::Scheduler, ctx->Self().NodeId()),
            TSpawnComputeActor{.Count = std::numeric_limits<uint32_t>::max()});
    }

    // The rows leave the graph through a remote sink to this actor.
    void Lower(const NCompiler::TPreparedQuery& query) {
        auto& lowered = State->Lowered.emplace(NScheduler::LowerPlanToGraph(
            query.Plan, State->Settings, nullptr, query.Annotation.ExternalCatalog));
        lowered.Graph->MarkExternal(NScheduler::AttachRemoteSink(lowered), ResultSinkId);
        std::string error;
        State->Sealed = lowered.Graph->Seal(&error);
        if (!State->Sealed) {
            throw std::runtime_error(error);
        }
        for (const auto& [name, type] : static_cast<const NQumir::NAst::TStructType&>(*lowered.OutputType).Fields) {
            State->Columns.push_back({.Name = name, .Type = type});
        }
        State->Schema = TSchema{State->Columns};
        State->Sink = std::make_unique<TCsvSink>(State->Schema, State->Out);
    }

    // Splits the graph into one fragment per compute actor (fewer when the
    // graph is small): fragment c runs on actors[c].
    std::vector<TTaskGraphMessage> SplitGraph(const std::vector<TActorId>& actors, TActorId self) {
        const auto buildStart = TClock::now();
        auto& lowered = *State->Lowered;
        NScheduler::TPartitioner partitioner(State->Sealed, static_cast<int>(actors.size()));
        partitioner.Do();
        const auto split = NScheduler::ApplySplit(lowered, partitioner.TakePlan());
        const auto fragments = NScheduler::ExtractFragmentSpecs(lowered);
        State->Timing.KernelBuild += Seconds(buildStart);
        const auto jitStart = TClock::now();
        const auto image = SerializeKernelImage(CompileKernelImage(lowered.Kernels));
        State->Timing.JitLLVM = Seconds(jitStart);
        std::vector<TTaskGraphMessage> messages(fragments.size());
        for (size_t i = 0; i < fragments.size(); ++i) {
            messages[i].Spec = NScheduler::SerializeGraphSpec(fragments[i]);
            messages[i].KernelImage = image;
            for (const auto& external : fragments[i].Externals) {
                if (external.Id == ResultSinkId) {
                    messages[i].Endpoints.push_back({
                        .Id = ResultSinkId, .Kind = ERemoteEndpointKind::Sink, .PeerActor = self});
                }
            }
        }
        std::unordered_map<const NScheduler::TTaskNode*, uint64_t> ids;
        for (const auto& external : lowered.Graph->Externals()) {
            ids.emplace(external.Node, external.Id);
        }
        // The colors are dense, so the fragment of color c is Graphs()[c].
        for (const auto& cut : split.Cuts()) {
            const auto sink = ids.at(cut.Sink);
            const auto source = ids.at(cut.Source);
            messages[cut.SinkColor].Endpoints.push_back({
                .Id = sink, .Kind = ERemoteEndpointKind::Sink,
                .PeerActor = actors[cut.SourceColor], .PeerTaskId = source});
            messages[cut.SourceColor].Endpoints.push_back({
                .Id = source, .Kind = ERemoteEndpointKind::Source,
                .PeerActor = actors[cut.SinkColor], .PeerTaskId = sink});
        }
        return messages;
    }

    // Returns whether the fragments are sent; otherwise the query is completed.
    bool StartGraph(const std::vector<TActorId>& actorIds, TActorContext::TPtr& ctx) {
        State->ComputeActors = actorIds;
        std::vector<TTaskGraphMessage> messages;
        try {
            messages = SplitGraph(actorIds, ctx->Self());
        } catch (const std::exception& e) {
            Fail(std::string("cannot start the query: ") + e.what(), ctx);
            return false;
        }
        // The actors past the fragments have nothing to run.
        for (size_t i = messages.size(); i < actorIds.size(); ++i) {
            ctx->Send<TPoison>(actorIds[i], TPoison{});
        }
        State->ComputeActors.resize(messages.size());
        for (size_t i = 0; i < messages.size(); ++i) {
            ctx->Send<TTaskGraphMessage>(actorIds[i], std::move(messages[i]));
        }
        State->ExecuteStart = TClock::now();
        Logger("Query: started " + std::to_string(messages.size()) + " fragments");
        return true;
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

        // The scheduler's live nodes: one compute actor on each.
        void Receive(TSpawnComputeActorResponse&& req, TBlob, TActorContext::TPtr ctx) {
            if (!req.Error.empty()) {
                Complete(std::unexpected(NQumir::TError(req.Error)), ctx, true);
                return;
            }
            const auto count = static_cast<uint32_t>(req.ActorIds.size());
            State->Collector = ctx->Register(std::unique_ptr<IActor>(CreateActorsCollector(
                ctx->Self(), std::move(req.ActorIds), count)));
            Parent->Become(&static_cast<TQueryActor*>(Parent)->WaitComputeActors);
        }
    };

    struct TWaitComputeActors : public TBaseBehavior,
        public TBehavior<TWaitComputeActors, TSpawnComputeActorResponse>
    {
        TWaitComputeActors(TQueryState* state, IBehaviorActor* parent, std::function<void(const std::string&)> logger)
            : TBaseBehavior(state, parent, logger)
        { }

        void Receive(TSpawnComputeActorResponse&& response, TBlob, TActorContext::TPtr ctx) {
            if (ctx->Sender() != State->Collector) {
                return;
            }
            if (!response.Error.empty()) {
                Complete(std::unexpected(NQumir::TError(response.Error)), ctx, true);
                return;
            }
            if (StartGraph(response.ActorIds, ctx)) {
                Parent->Become(&static_cast<TQueryActor*>(Parent)->MonitorQuery);
            }
        }
    };

    // Takes the rows of the result sink and confirms them, as a remote source would.
    struct TMonitorQuery : public TBaseBehavior,
        public TBehavior<TMonitorQuery, TDataChunk, TFinish, TComputeStatus, TComputeError>
    {
        TMonitorQuery(TQueryState* state, IBehaviorActor* parent, std::function<void(const std::string&)> logger)
            : TBaseBehavior(state, parent, logger)
        { }

        // The chunk releases its rows when the handler returns.
        void Receive(TDataChunk&& chunk, TBlob, TActorContext::TPtr ctx) {
            try {
                State->Sink->Write(chunk.RowSet);
                State->Timing.Rows += SelectedRows(chunk.RowSet);
            } catch (const std::exception& e) {
                Fail(e.what(), ctx);
                return;
            }
            ctx->Send<TDataConfirm>(ctx->Sender(), TDataConfirm{.SinkId = ResultSinkId, .LastApplied = chunk.Seq});
        }

        void Receive(TFinish&&, TBlob, TActorContext::TPtr ctx) {
            State->Sink->Flush();
            State->Timing.Execute = Seconds(State->ExecuteStart);
            Complete(State->Out.str(), ctx, false, State->Timing);
        }

        void Receive(TComputeStatus&&, TBlob, TActorContext::TPtr) {
        }

        void Receive(TComputeError&& error, TBlob, TActorContext::TPtr ctx) {
            Fail(std::move(error.Message), ctx);
        }
    };

    TQueryActor(
        NCompiler::TQueryCompiler& compiler,
        NScheduler::TSettings settings,
        std::function<void(const std::string&)> logger)
        : State{.Compiler = compiler, .Settings = std::move(settings)}
        , StartQuery(&State, this, logger)
        , WaitComputeActors(&State, this, logger)
        , MonitorQuery(&State, this, logger)
    {
        Become(&StartQuery);
    }

    TQueryState State;
    TStartQuery StartQuery;
    TWaitComputeActors WaitComputeActors;
    TMonitorQuery MonitorQuery;
};

} // namespace

NNet::NActors::IActor* CreateQueryActor(
    NCompiler::TQueryCompiler& compiler,
    NScheduler::TSettings settings,
    std::function<void(const std::string&)> logger)
{
    return new TQueryActor(compiler, std::move(settings), logger);
}

} // namespace NRemote
} // namespace NQdb
