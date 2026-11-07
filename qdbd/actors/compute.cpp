#include <coroio/actors/actorsystem.hpp>
#include <qdbd/messages.h>

#include "compute.h"
#include "pinger.h"

namespace NQdb {
namespace NRemote {

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
    }

    void ReleaseSlot() {
        if (Resources) {
            Resources->UsedSlots.fetch_sub(1, std::memory_order_relaxed);
            Resources.reset();
        }
    }

    TDefaultBehavior Behavior;
    std::shared_ptr<TRuntimeResources> Resources;
};

} // namespace

NNet::NActors::IActor* CreateComputeActor(std::shared_ptr<TRuntimeResources> resources)
{
    return new TComputeActor(std::move(resources));
}

} // namespace NRemote
} // namespace NQdb
