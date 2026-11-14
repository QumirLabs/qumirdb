#include <coroio/actors/actorsystem.hpp>
#include <memory>
#include <qdbd/actors.h>
#include <qdbd/messages.h>
#include <qdbd/serialization.h>

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
        TDefaultBehavior(
            TNodeId leader,
            uint64_t pingPeriodMs,
            const NScheduler::TSchedulerSettings& scheduler,
            const NScheduler::TTransportSettings& transport)
            : Leader(leader)
            , PingPeriodMs(pingPeriodMs)
            , Resources(std::make_shared<TRuntimeResources>())
        {
            Resources->Scheduler = scheduler;
            Resources->Transport = transport;
        }

        void Receive(TSpawnComputeActor&& request, TBlob, TActorContext::TPtr ctx)
        {
            TSpawnComputeActorResponse response;
            try {
                response.ActorIds.reserve(request.Count);
                for (uint32_t i = 0; i < request.Count; ++i) {
                    response.ActorIds.push_back(ctx->Register(
                        std::unique_ptr<IActor>(CreateComputeActor(Resources))));
                }
            } catch (const std::exception& error) {
                for (const auto actor : response.ActorIds) {
                    ctx->Send<TPoison>(actor, TPoison{});
                }
                response.ActorIds.clear();
                response.Error = "cannot create compute actors: " + std::string(error.what());
            }
            ctx->Send<TSpawnComputeActorResponse>(ctx->Sender(), std::move(response));
        }

        void HandleUnknownMessage(TMessageId messageId, TBlob blob, TActorContext::TPtr ctx)
        {
        }

        void StartPinger(TActorContext::TPtr& ctx) {
            auto pinger = CreatePinger(Resources, ctx->Self(), WellKnownActorId(EWellKnownActors::Scheduler, Leader), PingPeriodMs);
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

    TLocalSchedulerActor(
        TNodeId leader,
        uint64_t pingPeriodMs,
        const NScheduler::TSchedulerSettings& scheduler,
        const NScheduler::TTransportSettings& transport)
        : Behavior(leader, pingPeriodMs, scheduler, transport)
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

NNet::NActors::IActor* CreateLocalScheduler(
    TNodeId leader,
    uint64_t pingPeriodMs,
    const NScheduler::TSchedulerSettings& scheduler,
    const NScheduler::TTransportSettings& transport)
{
    return new TLocalSchedulerActor(leader, pingPeriodMs, scheduler, transport);
}

} // namespace NRemote
} // namespace NQdb
