#include <algorithm>
#include <iostream>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include <coroio/actors/actorsystem.hpp>
#include <qdbd/messages.h>
#include <qdbd/serialization.h>
#include <qdbd/scheduler/remote_task.h>
#include <qdb/scheduler/channel.h>
#include <qdb/scheduler/graph.h>
#include <qdb/scheduler/spec/graph_spec.h>

#include "compute.h"
#include "pinger.h"


namespace NQdb {
namespace NRemote {

using namespace NNet;
using namespace NNet::NActors;

namespace {

struct TComputeActor : public IBehaviorActor {
    struct TDefaultBehavior : public TBehavior<TDefaultBehavior, TTaskGraphMessage, TDataChunk, TDataConfirm, TFinish, TWakeUp> {
        TDefaultBehavior(TComputeActor* parent)
            : Parent(parent)
        { }

        void Receive(TTaskGraphMessage&& message, TBlob, TActorContext::TPtr ctx) {
            Parent->StartGraph(std::move(message), std::move(ctx));
        }

        void Receive(TDataChunk&& message, TBlob, TActorContext::TPtr ctx) {
            Parent->OnDataChunk(std::move(message), ctx);
        }

        void Receive(TDataConfirm&& message, TBlob, TActorContext::TPtr ctx) {
            Parent->OnDataConfirm(message, ctx);
        }

        void Receive(TFinish&& message, TBlob, TActorContext::TPtr ctx) {
            Parent->OnFinish(message, ctx);
        }

        void Receive(TWakeUp&&, TBlob, TActorContext::TPtr ctx) {
            Parent->SendStatus(ctx);
        }

        void HandleUnknownMessage(TMessageId messageId, TBlob blob, TActorContext::TPtr ctx)
        {
        }

        TComputeActor* Parent;
    };

    explicit TComputeActor(std::shared_ptr<TRuntimeResources> resources)
        : Behavior(this)
        , Resources(std::move(resources))
    {
        if (Resources) {
            SchedulerSettings = Resources->Scheduler;
            Resources->UsedSlots.fetch_add(1, std::memory_order_relaxed);
        }
        Become(&Behavior);
    }

    ~TComputeActor() override {
        ReleaseSlot();
    }

    void BeforeUnregister(TActorContext::TPtr ctx) override {
        if (StatusEvent) {
            ctx->Cancel(*StatusEvent);
            StatusEvent.reset();
        }
        ReleaseSlot();
    }

    void AfterRegister(TActorContext::TPtr ctx) override { }

    void ProcessNotifications(TActorContext::TPtr& ctx) {
        // TODO: need to implement it through a separate actor
        // e.g. ComputeActorNotifier -> consumer notification queue -> send to ComputeActor
        // need to do it if we move Actors to multithreaded model

        if (Failed) {
            return;
        }
        // An exception must not leave the coroutine: it would stop silently.
        try {
            auto& queue = ActorChannel->SinkQueue();
            NQdb::NScheduler::TRemoteSinkState* state = nullptr;
            while (queue.TryPop(state)) {
                // process
                state->SendQueue(ctx);
                SchedulerChannel->Notify(std::get<1>(SinkStates.at(state->Id)));
            }
            NQdb::NScheduler::TRemoteSourceState* source = nullptr;
            while (ActorChannel->SourceQueue().TryPop(source)) {
                source->SendConfirm(ctx);
            }
        } catch (const std::exception& e) {
            Fail(ctx, e.what());
        } catch (...) {
            Fail(ctx, "unknown error");
        }
    }

    // Stops moving data for the graph and tells the query actor.
    void Fail(TActorContext::TPtr& ctx, std::string message) {
        Failed = true;
        Scheduler->Cancel();
        SendError(ctx, "compute actor failed: " + std::move(message));
    }

    void SendError(TActorContext::TPtr& ctx, std::string message) {
        if (!QueryActorId) {
            std::cerr << "compute actor: " << message << "\n";
            return;
        }
        ctx->Send<TComputeError>(QueryActorId, TComputeError{.Message = std::move(message)});
    }

    void SendStatus(TActorContext::TPtr& ctx) {
        ctx->Send<TComputeStatus>(QueryActorId, TComputeStatus{});
        ScheduleStatus(ctx);
    }

    void ScheduleStatus(TActorContext::TPtr& ctx) {
        StatusEvent = ctx->Schedule<TWakeUp>(
            std::chrono::steady_clock::now() + std::chrono::milliseconds(SchedulerSettings.StatusPeriodMs),
            ctx->Self(), ctx->Self());
    }

    void StopScheduler() {
        if (SchedulerThread.joinable()) {
            Scheduler->Cancel();
            SchedulerThread.join();
        }
    }

    void ReleaseSlot() {
        StopScheduler();
        if (Resources) {
            Resources->UsedSlots.fetch_sub(1, std::memory_order_relaxed);
            Resources.reset();
        }

        if (NotificationTask) {
            NotificationTask.destroy();
            NotificationTask = {};
        }
    }

    void StartGraph(TTaskGraphMessage&& message, TActorContext::TPtr ctx) {
        // A compute actor runs one graph.
        if (Scheduler) {
            ctx->Send<TComputeError>(ctx->Sender(), TComputeError{.Message = "graph is already started"});
            return;
        }
        QueryActorId = ctx->Sender();
        std::unique_ptr<NScheduler::TTaskGraph> graph;
        NScheduler::TSealedTaskGraph sealed;
        std::string error;
        try {
            // No sink: a compute actor's results leave through its remote sinks.
            graph = NScheduler::MaterializeGraph(NScheduler::DeserializeGraphSpec(message.Spec));
            sealed = graph->Seal(&error);
        } catch (const std::exception& e) {
            error = e.what();
        }
        if (error.empty()) {
            error = NScheduler::ValidateEndpoints(sealed, message.Endpoints);
        }
        if (!error.empty()) {
            SendError(ctx, "cannot start graph: " + error);
            return;
        }
        OwnedGraph = std::move(graph);
        Graph = sealed;
        const auto sources = static_cast<size_t>(std::ranges::count_if(message.Endpoints, [](const auto& endpoint) {
            return endpoint.Kind == ERemoteEndpointKind::Source;
        }));
        ActorChannel = std::make_unique<NScheduler::TActorNotificationChannel>(
            ctx->CreateNotifier(), message.Endpoints.size() - sources, sources);
        for (const auto& endpoint : message.Endpoints) {
            const bool isSource = endpoint.Kind == ERemoteEndpointKind::Source;
            auto* node = Graph.ExternalNode(endpoint.Id);
            if (isSource) {
                auto* source = dynamic_cast<NScheduler::TSourceTask*>(node->Task);
                InitSource(endpoint, *source, node);
            } else {
                auto* sink = dynamic_cast<NScheduler::TSinkTask*>(node->Task);
                InitSink(endpoint, *sink, node);
            }
        }
        Scheduler = std::make_unique<NScheduler::TThreadedScheduler>(
            Graph, SchedulerSettings.WorkerCount, SchedulerSettings.ReadyQueueCapacity
        );
        SchedulerChannel = std::make_unique<NScheduler::TSchedulerChannel>(
            *Scheduler
        );
        // Run() blocks until the graph's root finishes or the scheduler is cancelled.
        SchedulerThread = std::thread([this] {
            Scheduler->Run();
        });
        ScheduleStatus(ctx);
        // The closure of a coroutine lambda dies after this statement: no captures.
        NotificationTask = [](TComputeActor* self, TActorContext::TPtr ctx) -> TVoidSuspendedTask {
            while (true) {
                co_await self->ActorChannel->Wait();
                self->ProcessNotifications(ctx);
            }
        }(this, std::move(ctx));
    }

    void OnDataChunk(TDataChunk&& chunk, TActorContext::TPtr& ctx) {
        if (Failed) {
            Release(&chunk.RowSet);
            return;
        }
        auto it = SourceStates.find(chunk.DstTaskId);
        if (it == SourceStates.end()) {
            SendError(ctx, "dropped chunk " + std::to_string(chunk.Seq) + " for unknown task "
                + std::to_string(chunk.DstTaskId));
            Release(&chunk.RowSet);
            return;
        }
        auto& [state, node] = it->second;
        if (!state->Push(chunk.Seq, std::move(chunk.RowSet))) {
            SendError(ctx, "dropped chunk " + std::to_string(chunk.Seq) + " for task "
                + std::to_string(chunk.DstTaskId) + ": out of order or beyond the window");
            Release(&chunk.RowSet);
            return;
        }
        Scheduler->Schedule(node);
    }

    void OnFinish(const TFinish& finish, TActorContext::TPtr& ctx) {
        auto it = SourceStates.find(finish.DstTaskId);
        if (it == SourceStates.end()) {
            SendError(ctx, "dropped finish for unknown task " + std::to_string(finish.DstTaskId));
            return;
        }
        auto& [state, node] = it->second;
        state->Finish();
        Scheduler->Schedule(node);
    }

    void OnDataConfirm(const TDataConfirm& confirm, TActorContext::TPtr& ctx) {
        auto it = SinkStates.find(confirm.SinkId);
        if (it == SinkStates.end() || !std::get<0>(it->second)->OnConfirm(confirm.LastApplied, ctx)) {
            SendError(ctx, "dropped confirm " + std::to_string(confirm.LastApplied)
                + " for task " + std::to_string(confirm.SinkId));
            return;
        }
        // The drain may have freed outbox space for a waiting sink task.
        SchedulerChannel->Notify(std::get<1>(it->second));
    }

    void InitSource(
        const TRemoteEndpoint& endpoint,
        NScheduler::TSourceTask& sourceTask,
        NScheduler::TTaskNode* node)
    {
        auto [code, state] = NScheduler::MakeRemoteSource(
            endpoint.Id, endpoint.PeerActor, endpoint.PeerTaskId, *ActorChannel);
        sourceTask.BindCode(std::move(code), state);
        SourceStates.emplace(endpoint.Id, std::tuple{std::move(state), node});
    }

    void InitSink(
        const TRemoteEndpoint& endpoint,
        NScheduler::TSinkTask& sinkTask,
        NScheduler::TTaskNode* node)
    {
        auto [code, state] = NScheduler::MakeRemoteSink(
            endpoint.Id, endpoint.PeerActor, endpoint.PeerTaskId, sinkTask.InputConnection()->RowLayouts(), *ActorChannel);
        sinkTask.BindCode(std::move(code), state);
        SinkStates.emplace(endpoint.Id, std::tuple{std::move(state), node});
    }

    TDefaultBehavior Behavior;
    std::shared_ptr<TRuntimeResources> Resources;
    NScheduler::TSchedulerSettings SchedulerSettings;
    // Declared before Graph and Scheduler: they point into it and die first.
    std::unique_ptr<NScheduler::TTaskGraph> OwnedGraph;
    NScheduler::TSealedTaskGraph Graph;
    std::unique_ptr<NScheduler::TActorNotificationChannel> ActorChannel;
    std::unique_ptr<NScheduler::TSchedulerChannel> SchedulerChannel;
    std::unique_ptr<NScheduler::TThreadedScheduler> Scheduler;
    std::unordered_map<uint64_t, std::tuple<std::shared_ptr<NScheduler::TRemoteSourceState>, NScheduler::TTaskNode*>> SourceStates;
    std::unordered_map<uint64_t, std::tuple<std::shared_ptr<NScheduler::TRemoteSinkState>, NScheduler::TTaskNode*>> SinkStates;
    std::thread SchedulerThread;
    TVoidSuspendedTask NotificationTask;
    TActorId QueryActorId;
    bool Failed = false;
    std::optional<NNet::NActors::TEvent> StatusEvent;
};

} // namespace

NNet::NActors::IActor* CreateComputeActor(std::shared_ptr<TRuntimeResources> resources)
{
    return new TComputeActor(std::move(resources));
}

} // namespace NRemote
} // namespace NQdb
