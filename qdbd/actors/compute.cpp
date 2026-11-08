#include <iostream>
#include <chrono>
#include <memory>
#include <optional>

#include <coroio/actors/actorsystem.hpp>
#include <qdbd/messages.h>
#include <qdbd/serialization.h>
#include <qdbd/scheduler/remote_task.h>
#include <qdb/scheduler/channel.h>
#include <qdb/scheduler/graph.h>

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
        // TODO: need to implement it throght a separate actor
        // e.g. ComputeActorNotifier -> consumer notification queue -> send to ComputeActor
        // need to do it if we move Actors to multithreaded model

        auto& queue = ActorChannel->SinkQueue();
        NQdb::NScheduler::TRemoteSinkState* state = nullptr;
        while (queue.TryPop(state)) {
            // process
            state->SendQueue(ctx);
            SchedulerSchannel->Notify(std::get<1>(SinkStates.at(state->Id)));
        }
        NQdb::NScheduler::TRemoteSourceState* source = nullptr;
        while (ActorChannel->SourceQueue().TryPop(source)) {
            source->SendConfirm(ctx);
        }
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

    void ReleaseSlot() {
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
        QueryActorId = ctx->Sender();
        if (auto error = NScheduler::ValidateEndpoints(message.Graph, message.Endpoints); !error.empty()) {
            SendError(ctx, "cannot start graph: " + error);
            return;
        }
        Graph = message.Graph;
        Endpoints = std::move(message.Endpoints);
        ActorChannel = std::make_unique<NScheduler::TActorNotificationChannel>(ctx->CreateNotifier());
        for (const auto& endpoint : Endpoints) {
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
        SchedulerSchannel = std::make_unique<NScheduler::TSchedulerChannel>(
            *Scheduler
        );
        ScheduleStatus(ctx);
        NotificationTask = [&](TActorContext::TPtr ctx) -> TVoidSuspendedTask {
            while (true) {
                co_await ActorChannel->Wait();
                ProcessNotifications(ctx);
            }
        }(std::move(ctx));
    }

    void OnDataChunk(TDataChunk&& chunk, TActorContext::TPtr& ctx) {
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
        SchedulerSchannel->Notify(std::get<1>(it->second));
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
            endpoint.Id, endpoint.PeerActor, endpoint.PeerTaskId, endpoint.Layouts.data(), *ActorChannel);
        sinkTask.BindCode(std::move(code), state);
        SinkStates.emplace(endpoint.Id, std::tuple{std::move(state), node});
    }

    TDefaultBehavior Behavior;
    std::shared_ptr<TRuntimeResources> Resources;
    NScheduler::TSchedulerSettings SchedulerSettings;
    NScheduler::TSealedTaskGraph Graph;
    // Owns the sink Layouts that the sink states use as descriptors.
    std::vector<TRemoteEndpoint> Endpoints;
    std::unique_ptr<NScheduler::TActorNotificationChannel> ActorChannel;
    std::unique_ptr<NScheduler::TSchedulerChannel> SchedulerSchannel;
    std::unique_ptr<NScheduler::TThreadedScheduler> Scheduler;
    std::unordered_map<uint64_t, std::tuple<std::shared_ptr<NScheduler::TRemoteSourceState>, NScheduler::TTaskNode*>> SourceStates;
    std::unordered_map<uint64_t, std::tuple<std::shared_ptr<NScheduler::TRemoteSinkState>, NScheduler::TTaskNode*>> SinkStates;
    TVoidSuspendedTask NotificationTask;
    TActorId QueryActorId;
    std::optional<NNet::NActors::TEvent> StatusEvent;
};

} // namespace

NNet::NActors::IActor* CreateComputeActor(std::shared_ptr<TRuntimeResources> resources)
{
    return new TComputeActor(std::move(resources));
}

} // namespace NRemote
} // namespace NQdb
