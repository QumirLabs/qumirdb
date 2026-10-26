#include <coroio/actors/actorsystem.hpp>
#include <memory>
#include <qdb/remote/actors.h>
#include <qdb/remote/messages.h>

#include "pinger.h"
#include "compute.h"
#include "local_scheduler.h"

namespace NQdb {
namespace NRemote {

using namespace NNet::NActors;
using namespace std::chrono;

namespace {

struct TLocalSchedulerActor : public IBehaviorActor {
    struct TDefaultBehavior : public TBehavior<TDefaultBehavior, TSpawnComputeActor> {
        TDefaultBehavior(TNodeId leader, uint64_t pingPeriodMs)
            : Leader(leader)
            , PingPeriodMs(pingPeriodMs)
            , Resources(std::make_shared<TRuntimeResources>())
        { }

        void Receive(TSpawnComputeActor&&, TBlob, TActorContext::TPtr ctx)
        {
            auto actor = CreateComputeActor();
            auto actorId = ctx->Register(std::unique_ptr<IActor>(actor));
            ctx->Send<TSpawnComputeActorResponse>(ctx->Sender(), TSpawnComputeActorResponse {
                .ActorId = actorId
            });
        }

        void HandleUnknownMessage(TMessageId messageId, TBlob blob, TActorContext::TPtr ctx) { }

        void StartPinger(TActorContext::TPtr& ctx) {
            auto pinger = CreatePinger(Resources, WellKnownActorId(EWellKnownActors::Scheduler, Leader), PingPeriodMs);
            PingerActorId = ctx->Register(std::unique_ptr<IActor>(pinger));
        }

        void StopPinger(TActorContext::TPtr& ctx) {
            ctx->Send<TPoison>(PingerActorId, TPoison{});
        }

        TNodeId Leader;
        uint64_t PingPeriodMs;
        std::shared_ptr<TRuntimeResources> Resources;

        TActorId PingerActorId;
    };

    TLocalSchedulerActor(TNodeId leader, uint64_t pingPeriodMs)
        : Behavior(leader, pingPeriodMs)
    {
        Become(&Behavior);
    }

    void AfterRegister(TActorContext::TPtr ctx) override {
        Behavior.StartPinger(ctx);
    }

    void BeforeUnregister(TActorContext::TPtr ctx) override {
        Behavior.StopPinger(ctx);
    }

    TDefaultBehavior Behavior;
};

} // namespace

NNet::NActors::IActor* CreateLocalScheduler(TNodeId leader, uint64_t pingPeriodMs)
{
    return new TLocalSchedulerActor(leader, pingPeriodMs);
}

} // namespace NRemote
} // namespace NQdb
