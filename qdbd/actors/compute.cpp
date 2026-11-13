#include <algorithm>
#include <iostream>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <coroio/actors/actorsystem.hpp>
#include <qdbd/messages.h>
#include <qdbd/serialization.h>
#include <qdbd/scheduler/remote_task.h>
#include <qdb/kernel/image.h>
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
    struct TDefaultBehavior : public TBehavior<TDefaultBehavior, TTaskGraphMessage, TDataChunk, TDataConfirm, TFinish,
        TFilterPartial, TWakeUp>
    {
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

        void Receive(TFilterPartial&& message, TBlob, TActorContext::TPtr ctx) {
            Parent->OnFilterPartial(std::move(message), ctx);
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
            NQdb::NScheduler::TRuntimeFilterSinkState* filterSink = nullptr;
            while (ActorChannel->FilterSinkQueue().TryPop(filterSink)) {
                filterSink->SendPartial(ctx);
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
            const auto spec = NScheduler::DeserializeGraphSpec(message.Spec);
            const auto image = DeserializeKernelImage(message.KernelImage);
            std::vector<std::shared_ptr<TKernelSlot>> slots;
            for (size_t i = 0; i < image.Entrypoints.size(); ++i) {
                slots.push_back(std::make_shared<TKernelSlot>());
            }
            // An image without kernels has nothing to link.
            if (!slots.empty()) {
                LoadKernelImage(image, slots);
            }
            // No sink: a compute actor's results leave through its remote sinks.
            graph = NScheduler::MaterializeGraph(spec, nullptr, std::move(slots));
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
        auto count = [&](ERemoteEndpointKind kind) {
            return static_cast<size_t>(std::ranges::count(message.Endpoints, kind, &TRemoteEndpoint::Kind));
        };
        ActorChannel = std::make_unique<NScheduler::TActorNotificationChannel>(
            ctx->CreateNotifier(), count(ERemoteEndpointKind::Sink), count(ERemoteEndpointKind::Source),
            count(ERemoteEndpointKind::FilterSink));
        for (const auto& endpoint : message.Endpoints) {
            auto* node = Graph.ExternalNode(endpoint.Id);
            switch (endpoint.Kind) {
                case ERemoteEndpointKind::Source:
                    InitSource(endpoint, static_cast<NScheduler::TSourceTask&>(*node->Task), node);
                    break;
                case ERemoteEndpointKind::Sink:
                    InitSink(endpoint, static_cast<NScheduler::TSinkTask&>(*node->Task), node);
                    break;
                case ERemoteEndpointKind::FilterSource:
                    InitFilterSource(endpoint, static_cast<NScheduler::TRuntimeFilterSourceTask&>(*node->Task), node);
                    break;
                case ERemoteEndpointKind::FilterSink:
                    InitFilterSink(endpoint, static_cast<NScheduler::TRuntimeFilterSinkTask&>(*node->Task));
                    break;
            }
        }
        Scheduler = std::make_unique<NScheduler::TThreadedScheduler>(
            Graph, SchedulerSettings.WorkerCount, SchedulerSettings.ReadyQueueCapacity
        );
        SchedulerChannel = std::make_unique<NScheduler::TSchedulerChannel>(
            *Scheduler
        );
        // Run() blocks until every root of the graph finishes or the scheduler is cancelled.
        SchedulerThread = std::thread([this] {
            Scheduler->Run();
        });
        // Each source gets its chunks before its finish, as its sink sent them.
        for (auto& chunk : std::exchange(EarlyChunks, {})) {
            OnDataChunk(std::move(chunk), ctx);
        }
        for (const auto& finish : std::exchange(EarlyFinishes, {})) {
            OnFinish(finish, ctx);
        }
        for (auto& partial : std::exchange(EarlyFilterPartials, {})) {
            OnFilterPartial(std::move(partial), ctx);
        }
        ScheduleStatus(ctx);
        // The closure of a coroutine lambda dies after this statement: no captures.
        NotificationTask = [](TComputeActor* self, TActorContext::TPtr ctx) -> TVoidSuspendedTask {
            while (true) {
                co_await self->ActorChannel->Wait();
                self->ProcessNotifications(ctx);
            }
        }(this, std::move(ctx));
    }

    // A chunk that is not pushed releases its rows itself.
    void OnDataChunk(TDataChunk&& chunk, TActorContext::TPtr& ctx) {
        if (!QueryActorId) {
            EarlyChunks.push_back(std::move(chunk));
            return;
        }
        if (Failed) {
            return;
        }
        auto it = SourceStates.find(chunk.DstTaskId);
        if (it == SourceStates.end()) {
            SendError(ctx, "dropped chunk " + std::to_string(chunk.Seq) + " for unknown task "
                + std::to_string(chunk.DstTaskId));
            return;
        }
        auto& [state, node] = it->second;
        // A chunk the inbox refuses stays here and releases its rows.
        if (!state->Push(std::move(chunk))) {
            SendError(ctx, "dropped chunk " + std::to_string(chunk.Seq) + " for task "
                + std::to_string(chunk.DstTaskId) + ": out of order or beyond the window");
            return;
        }
        Scheduler->Schedule(node);
    }

    void OnFinish(const TFinish& finish, TActorContext::TPtr& ctx) {
        if (!QueryActorId) {
            EarlyFinishes.push_back(finish);
            return;
        }
        auto it = SourceStates.find(finish.DstTaskId);
        if (it == SourceStates.end()) {
            SendError(ctx, "dropped finish for unknown task " + std::to_string(finish.DstTaskId));
            return;
        }
        auto& [state, node] = it->second;
        state->Finish();
        Scheduler->Schedule(node);
    }

    void OnFilterPartial(TFilterPartial&& message, TActorContext::TPtr& ctx) {
        if (!QueryActorId) {
            EarlyFilterPartials.push_back(std::move(message));
            return;
        }
        if (Failed) {
            return;
        }
        auto it = FilterSourceStates.find(message.DstTaskId);
        if (it == FilterSourceStates.end()) {
            SendError(ctx, "dropped filter partial for unknown task " + std::to_string(message.DstTaskId));
            return;
        }
        auto& [state, node] = it->second;
        if (!state->Push(std::move(message.Partial))) {
            SendError(ctx, "dropped a second filter partial for task " + std::to_string(message.DstTaskId));
            return;
        }
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

    void InitFilterSource(
        const TRemoteEndpoint& endpoint,
        NScheduler::TRuntimeFilterSourceTask& sourceTask,
        NScheduler::TTaskNode* node)
    {
        auto [code, state] = NScheduler::MakeRuntimeFilterSource(endpoint.Id);
        sourceTask.BindCode(std::move(code), state);
        FilterSourceStates.emplace(endpoint.Id, std::tuple{std::move(state), node});
    }

    void InitFilterSink(const TRemoteEndpoint& endpoint, NScheduler::TRuntimeFilterSinkTask& sinkTask)
    {
        auto [code, state] = NScheduler::MakeRuntimeFilterSink(
            endpoint.Id, endpoint.PeerActor, endpoint.PeerTaskId, *ActorChannel);
        sinkTask.BindCode(std::move(code), std::move(state));
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
    std::unordered_map<uint64_t, std::tuple<std::shared_ptr<NScheduler::TRuntimeFilterSourceState>, NScheduler::TTaskNode*>>
        FilterSourceStates;
    std::thread SchedulerThread;
    TVoidSuspendedTask NotificationTask;
    TActorId QueryActorId;
    // A peer may send before this actor gets its graph, which sets QueryActorId;
    // its chunks, finishes and filter partials wait here until the sources are bound.
    std::vector<TDataChunk> EarlyChunks;
    std::vector<TFinish> EarlyFinishes;
    std::vector<TFilterPartial> EarlyFilterPartials;
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
