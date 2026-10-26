#include <coroio/actors/actorsystem.hpp>
#include <memory>
#include <qdb/remote/actors.h>
#include <qdb/remote/messages.h>

#include "pinger.h"

namespace NQdb {
namespace NRemote {

using namespace NNet::NActors;
using namespace std::chrono;

struct TLocalSchedulerActor : public IBehaviorActor {
    struct TDefaultBehavior : public TBehavior<TDefaultBehavior, TSpawnComputeActor> {
        TDefaultBehavior(TNodeId leader, uint64_t pingPeriodMs)
            : Leader(leader)
            , PingPeriodMs(pingPeriodMs)
            , Resources(std::make_shared<TRuntimeResources>())
        { }

        void Receive(TSpawnComputeActor&&, TBlob, TActorContext::TPtr ctx)
        {
            // TODO: implement
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
    { }

    void AfterRegister(TActorContext::TPtr ctx) override {
        Behavior.StartPinger(ctx);
    }

    void BeforeUnregister(TActorContext::TPtr ctx) override {
        Behavior.StopPinger(ctx);
    }

    TDefaultBehavior Behavior;
};

} // namespace NRemote
} // namespace NQdb
