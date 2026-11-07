#include <coroio/actors/actorsystem.hpp>
#include <iostream>
#include <memory>
#include <qdbd/messages.h>
#include <qdbd/scheduler/remote_task.h>
#include <qdb/scheduler/channel.h>

#include "compute.h"
#include "pinger.h"

namespace NQdb {
namespace NRemote {

using namespace NNet;
using namespace NNet::NActors;

namespace {

struct TComputeActor : public IBehaviorActor {
    struct TDefaultBehavior : public TBehavior<TDefaultBehavior, TTaskGraphMessage> {
        TDefaultBehavior(TComputeActor* parent)
            : Parent(parent)
        { }

        void Receive(TTaskGraphMessage&& message, TBlob, TActorContext::TPtr ctx) {
            Parent->StartGraph(std::move(message), std::move(ctx));
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

    TDefaultBehavior Behavior;
    std::shared_ptr<TRuntimeResources> Resources;
    NScheduler::TSchedulerSettings SchedulerSettings;
    NScheduler::TSealedTaskGraph Graph;
    std::unique_ptr<NScheduler::TActorNotificationChannel> ActorChannel;
    std::unique_ptr<NScheduler::TSchedulerChannel> SchedulerSchannel;
    std::unique_ptr<NScheduler::TThreadedScheduler> Scheduler;
    TVoidSuspendedTask NotificationTask;
};

} // namespace

NNet::NActors::IActor* CreateComputeActor(std::shared_ptr<TRuntimeResources> resources)
{
    return new TComputeActor(std::move(resources));
}

} // namespace NRemote
} // namespace NQdb
