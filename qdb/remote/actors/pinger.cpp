#include <coroio/actors/actorsystem.hpp>
#include <qdb/remote/messages.h>

#include "pinger.h"

namespace NQdb {
namespace NRemote {

using namespace NNet::NActors;
using namespace std::chrono;

namespace {

struct TPingerActor : public IBehaviorActor {
    struct TDefaultBehavior : public TBehavior<TDefaultBehavior, TWakeUp> {
        TDefaultBehavior(std::shared_ptr<TRuntimeResources> resources, NNet::NActors::TActorId pingTo, uint64_t periodMs)
            : Resources(std::move(resources))
            , PingTo(pingTo)
            , PeriodMs(periodMs)
        { }

        void Receive(TWakeUp&&, TBlob, TActorContext::TPtr ctx) {
            ctx->Send<TPing>(PingTo, TPing {
                .TotalSlots = Resources->TotalSlots,
                .UsedSlots = Resources->UsedSlots,
                .CpuTime = Resources->CpuTime
            });
            Schedule(ctx);
        }

        void HandleUnknownMessage(TMessageId messageId, TBlob blob, TActorContext::TPtr ctx) { }

        void Schedule(TActorContext::TPtr& ctx) {
            ctx->Schedule<TWakeUp>(steady_clock::now() + milliseconds(PeriodMs), ctx->Self(), ctx->Self());
        }

        std::shared_ptr<TRuntimeResources> Resources;
        TActorId PingTo;
        uint64_t PeriodMs;
    };

    TPingerActor(std::shared_ptr<TRuntimeResources> resources, NNet::NActors::TActorId pingTo, uint64_t periodMs)
        : Behavior(std::move(resources), pingTo, periodMs)
    {
        Become(&Behavior);
    }

    void AfterRegister(TActorContext::TPtr ctx) override {
        Behavior.Schedule(ctx);
    }

    NNet::NActors::TActorId PingTo;
    TDefaultBehavior Behavior;
};

} // namespace

NNet::NActors::IActor* CreatePinger(
    std::shared_ptr<TRuntimeResources> resources,
    NNet::NActors::TActorId pingTo,
    uint64_t periodMs)
{
    return new TPingerActor(std::move(resources), pingTo, periodMs);
}

} // namespace NRemote
} // namespace NQdb
