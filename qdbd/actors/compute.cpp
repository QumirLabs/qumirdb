#include <coroio/actors/actorsystem.hpp>
#include <qdbd/messages.h>
#include <qdbd/scheduler/actor_task_node.h>

#include "compute.h"
#include "pinger.h"

namespace NQdb {
namespace NRemote {

using namespace NNet;
using namespace NNet::NActors;

namespace {

struct TComputeActor : public IBehaviorActor {
    struct TDefaultBehavior : public TBehavior<TDefaultBehavior> {
        void HandleUnknownMessage(TMessageId messageId, TBlob blob, TActorContext::TPtr ctx)
        {
        }
    };

    explicit TComputeActor(std::shared_ptr<TRuntimeResources> resources)
        : Resources(std::move(resources))
    {
        if (Resources) {
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

    void AfterRegister(TActorContext::TPtr ctx) override {
        Channel = std::make_unique<NScheduler::TActorNotificationChannel>(ctx->CreateNotifier());

        NotificationTask = [&]() -> TVoidSuspendedTask {
            co_await Channel->Wait();
            ProcessNotifications();
        }();
    }

    void ProcessNotifications() {
        // TODO: need to implement it throght a separate actor
        // e.g. ComputeActorNotifier -> consumer notification queue -> send to ComputeActor
        // need to do it if we move Actors to multithreaded model

        auto& queue = Channel->Queue();
        NQdb::NScheduler::TActorTaskNode* task = nullptr;
        while (queue.TryPop(task)) {
            // process
            task->Mark();
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

    TDefaultBehavior Behavior;
    std::shared_ptr<TRuntimeResources> Resources;
    std::unique_ptr<NScheduler::TActorNotificationChannel> Channel;
    TVoidSuspendedTask NotificationTask;
};

} // namespace

NNet::NActors::IActor* CreateComputeActor(std::shared_ptr<TRuntimeResources> resources)
{
    return new TComputeActor(std::move(resources));
}

} // namespace NRemote
} // namespace NQdb
