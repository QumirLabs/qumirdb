#include <coroio/actors/actorsystem.hpp>
#include <memory>
#include <qdb/remote/actors.h>
#include <qdb/remote/messages.h>
#include <qdb/remote/serialization.h>
#include <qdb/remote/time_source.h>

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
        TDefaultBehavior(ITimeSource* timeSource, TNodeId leader, uint64_t pingPeriodMs)
            : TimeSource(timeSource)
            , Leader(leader)
            , PingPeriodMs(pingPeriodMs)
            , Resources(std::make_shared<TRuntimeResources>())
        { }

        void Receive(TSpawnComputeActor&& request, TBlob, TActorContext::TPtr ctx)
        {
            auto actor = CreateComputeActor();
            std::vector<NNet::NActors::TActorId> actorIds;
            actorIds.reserve(request.Count);
            for (size_t i = 0; i < request.Count; ++i) {
                auto actorId = ctx->Register(std::unique_ptr<IActor>(actor));
                actorIds.emplace_back(actorId);
            }
            ctx->Send<TSpawnComputeActorResponse>(ctx->Sender(), TSpawnComputeActorResponse {
                .Count = request.Count,
                .ActorIds = std::move(actorIds)
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

        ITimeSource* TimeSource;
        TNodeId Leader;
        uint64_t PingPeriodMs;
        std::shared_ptr<TRuntimeResources> Resources;

        TActorId PingerActorId;
    };

    TLocalSchedulerActor(ITimeSource* timeSource, TNodeId leader, uint64_t pingPeriodMs)
        : Behavior(timeSource, leader, pingPeriodMs)
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

NNet::NActors::IActor* CreateLocalScheduler(ITimeSource* timeSource, TNodeId leader, uint64_t pingPeriodMs)
{
    return new TLocalSchedulerActor(timeSource, leader, pingPeriodMs);
}

} // namespace NRemote
} // namespace NQdb
