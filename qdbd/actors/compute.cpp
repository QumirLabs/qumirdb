#include <iostream>
#include <memory>

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
    struct TDefaultBehavior : public TBehavior<TDefaultBehavior, TTaskGraphMessage, TDataChunk> {
        TDefaultBehavior(TComputeActor* parent)
            : Parent(parent)
        { }

        void Receive(TTaskGraphMessage&& message, TBlob, TActorContext::TPtr ctx) {
            Parent->StartGraph(std::move(message), std::move(ctx));
        }

        void Receive(TDataChunk&& message, TBlob, TActorContext::TPtr ctx) {
            Parent->OnDataChunk(std::move(message), ctx);
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

    void BeforeUnregister(TActorContext::TPtr) override {
        ReleaseSlot();
    }

    void AfterRegister(TActorContext::TPtr ctx) override { }

    void ProcessNotifications(TActorContext::TPtr& ctx) {
        // TODO: need to implement it throght a separate actor
        // e.g. ComputeActorNotifier -> consumer notification queue -> send to ComputeActor
        // need to do it if we move Actors to multithreaded model

        auto& queue = ActorChannel->Queue();
        NQdb::NScheduler::TRemoteSinkState* state = nullptr;
        while (queue.TryPop(state)) {
            // process
            state->SendQueue(ctx);
            SchedulerSchannel->Notify(Graph.ExternalNode(state->Id));
        }
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
        if (auto error = NScheduler::ValidateEndpoints(message.Graph, message.Endpoints); !error.empty()) {
            std::cerr << "compute actor: cannot start graph: " << error << "\n";
            return;
        }
        Graph = message.Graph;
        for (const auto& endpoint : message.Endpoints) {
            const bool isSource = endpoint.Kind == ERemoteEndpointKind::Source;
            const auto* node = Graph.ExternalNode(endpoint.Id);
            if (isSource) {
                const auto* source = dynamic_cast<const NScheduler::TSourceTask*>(node->Task);
                InitSource(source);
            } else {
                const auto* sink = dynamic_cast<const NScheduler::TSinkTask*>(node->Task);
                InitSink(sink);
            }
        }
        Scheduler = std::make_unique<NScheduler::TThreadedScheduler>(
            Graph, SchedulerSettings.WorkerCount, SchedulerSettings.ReadyQueueCapacity
        );
        SchedulerSchannel = std::make_unique<NScheduler::TSchedulerChannel>(
            *Scheduler
        );
        ActorChannel = std::make_unique<NScheduler::TActorNotificationChannel>(ctx->CreateNotifier());
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
            // TODO: protocol errors
            return;
        }
        auto& [state, node] = it->second;
        state.Push(std::move(chunk.RowSet));
        Scheduler->Schedule(node);
    }

    void InitSource(const NScheduler::TSourceTask* sourceTask) {
        // TODO: init state
    }

    void InitSink(const NScheduler::TSinkTask* sinkTask) {
        // TODO: init state
    }

    TDefaultBehavior Behavior;
    std::shared_ptr<TRuntimeResources> Resources;
    NScheduler::TSchedulerSettings SchedulerSettings;
    NScheduler::TSealedTaskGraph Graph;
    std::unique_ptr<NScheduler::TActorNotificationChannel> ActorChannel;
    std::unique_ptr<NScheduler::TSchedulerChannel> SchedulerSchannel;
    std::unique_ptr<NScheduler::TThreadedScheduler> Scheduler;
    std::unordered_map<uint64_t, std::tuple<NScheduler::TRemoteSourceState, NScheduler::TTaskNode*>> SourceStates;
    TVoidSuspendedTask NotificationTask;
};

} // namespace

NNet::NActors::IActor* CreateComputeActor(std::shared_ptr<TRuntimeResources> resources)
{
    return new TComputeActor(std::move(resources));
}

} // namespace NRemote
} // namespace NQdb
