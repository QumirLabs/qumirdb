#include <algorithm>
#include <coroio/actors/actorsystem.hpp>
#include <qdbd/actors.h>
#include <qdbd/messages.h>
#include <qdbd/serialization.h>
#include <unordered_map>
#include <utility>
#include <chrono>
#include "scheduler.h"
#include "coroio/actors/actorid.hpp"

namespace NQdb {
namespace NRemote {

using namespace NNet::NActors;
using namespace std::chrono;

namespace {

struct TActorIdHash {
    static constexpr std::size_t operator()(const TActorId& actorId) {
        return std::hash<TNodeId>()(actorId.NodeId())
            ^ std::hash<TLocalActorId>()(actorId.ActorId())
            ^ std::hash<TCookie>()(actorId.Cookie())
            ;
    }
};

using TEvictionQueue = std::list<std::pair<uint64_t, TActorId>>;

struct TNodeStatus {
    uint64_t LastPingTime = 0;
    uint64_t TotalSlots = 0;
    uint64_t UsedSlots = 0;
    uint64_t CpuTime = 0;
    TEvictionQueue::iterator EvictionMark;
};

struct TSchedulerState {
    std::unordered_map<TActorId, TNodeStatus, TActorIdHash> Nodes;
    TEvictionQueue EvictionQueue;
    std::vector<TActorId> SchedulerQueue;
    size_t RoundRobinPosition = 0;

    void Ping(uint64_t currentTime, const TActorId& actorId, const TPing& ping) {
        auto& status = Nodes[actorId];
        bool newEntry = status.LastPingTime == 0;
        status.TotalSlots = ping.TotalSlots;
        status.UsedSlots = ping.UsedSlots;
        status.CpuTime = ping.CpuTime;
        status.LastPingTime = currentTime;
        if (newEntry) {
            SchedulerQueue.emplace_back(actorId);
        } else {
            EvictionQueue.erase(status.EvictionMark);
        }
        status.EvictionMark = EvictionQueue.insert(EvictionQueue.end(), {currentTime, actorId});
    }

    TActorId Schedule(uint64_t /*currentTime*/) {
        auto ret = SchedulerQueue[RoundRobinPosition];
        RoundRobinPosition = (RoundRobinPosition + 1) % SchedulerQueue.size();
        return ret;
    }

    void Evict(uint64_t currentTime, uint64_t timeout) {
        std::unordered_set<TActorId, TActorIdHash> erased;
        while (!EvictionQueue.empty() && currentTime < timeout + EvictionQueue.front().first) {
            const auto& [_, actorId] = EvictionQueue.front(); EvictionQueue.pop_front();
            erased.emplace(actorId);
            Nodes.erase(actorId);
        }
        if (!erased.empty()) {
            auto it = std::remove_if(SchedulerQueue.begin(), SchedulerQueue.end(), [&](const auto& x) {
                return erased.contains(x);
            });
            SchedulerQueue.resize(it - SchedulerQueue.begin());
            RoundRobinPosition = RoundRobinPosition % SchedulerQueue.size();
        }
    }
};

struct TActorsCollector : public IBehaviorActor {
    struct TDefaultBehavior : public TBehavior<TDefaultBehavior, TSpawnComputeActorResponse> {
        TDefaultBehavior(uint64_t waitResponses, TActorId sendResponseTo)
            : WaitResponses(waitResponses)
            , SendResponseTo(sendResponseTo)
        { }

        void Receive(TSpawnComputeActorResponse&& res, TBlob, TActorContext::TPtr ctx) {
            if (WaitResponses == 0) {
                // TODO: oops
                return;
            }

            ComputeActors.insert(ComputeActors.end(), res.ActorIds.begin(), res.ActorIds.end());
            if (--WaitResponses == 0) {
                ctx->Send<TSpawnComputeActorResponse>(SendResponseTo, TSpawnComputeActorResponse {
                    .ActorIds = std::move(ComputeActors)
                });
                ctx->Send<TPoison>(ctx->Self(), TPoison{});
            }
        }

        void HandleUnknownMessage(TMessageId messageId, TBlob blob, TActorContext::TPtr ctx) { }

        uint64_t WaitResponses;
        TActorId SendResponseTo;

        std::vector<TActorId> ComputeActors;
    };

    TActorsCollector(uint64_t waitResponses, TActorId sendResponseTo)
        : Behavior(waitResponses, sendResponseTo)
    {
        Become(&Behavior);
    }

    TDefaultBehavior Behavior;
};

struct TSchedulerActor : public IBehaviorActor {
    struct TDefaultBehavior : public TBehavior<TDefaultBehavior, TPing, TWakeUp, TSpawnComputeActor> {
        TDefaultBehavior(ITimeSource* timeSource, TSchedulerState* state, TNodeId leaderId, std::function<void(const std::string&)> logger)
            : TimeSource(timeSource)
            , State(state)
            , LeaderId(leaderId)
            , Logger(logger)
        { }

        void SchedulePeriodicJob(TActorContext::TPtr& ctx) {
            ctx->Schedule<TWakeUp>(steady_clock::now() + milliseconds(PeriodicJobMs), ctx->Self(), ctx->Self());
        }

        void Receive(TWakeUp&&, TBlob, TActorContext::TPtr ctx) {
            auto currentTime = TimeSource->CurrentTime();
            State->Evict(currentTime, EvictionTimeout);
            SchedulePeriodicJob(ctx);
        }

        // Ping from LocalSchedulerActor
        void Receive(TPing&& ping, TBlob, TActorContext::TPtr ctx)
        {
            if (!MaybeForward(ping, ctx)) {
                auto currentTime = TimeSource->CurrentTime();
                auto sender = ctx->Sender();
                State->Ping(currentTime, sender, ping);
            }
        }

        void Receive(TSpawnComputeActor&& req, TBlob, TActorContext::TPtr ctx)
        {
            if (!MaybeForward(req, ctx)) {
                auto* collector = new TActorsCollector(req.Count, ctx->Sender());
                auto collectorId = ctx->Register(std::unique_ptr<IActor>(collector));
                auto currentTime = TimeSource->CurrentTime();
                for (size_t i = 0; i < req.Count; ++i) {
                    auto sendTo = State->Schedule(currentTime);
                    ctx->SendAs<TSpawnComputeActor>(collectorId, sendTo, TSpawnComputeActor {
                        .Count = 1
                    });
                }
            }
        }

        template<typename T>
        bool MaybeForward(T& msg, TActorContext::TPtr& ctx) {
            if (ctx->Self() != LeaderId) {
                if (!msg.Forwarded) {
                    msg.Forwarded = true;
                    ctx->Forward<T>(WellKnownActorId(EWellKnownActors::Scheduler, LeaderId), std::move(msg));
                }
                return true;
            }
            return false;
        }

        void HandleUnknownMessage(TMessageId messageId, TBlob blob, TActorContext::TPtr ctx) {
            std::ostringstream str; str << "Scheduler: unknown message " <<  messageId;
            Logger(str.str());
        }

        static constexpr uint64_t PeriodicJobMs = 1000;
        static constexpr uint64_t EvictionTimeout = 10000;

        ITimeSource* TimeSource;
        TSchedulerState* State;
        TNodeId LeaderId;
        std::function<void(const std::string&)> Logger;
    };

    TSchedulerActor(ITimeSource* timeSource, TNodeId leaderId, std::function<void(const std::string&)> logger)
        : Behavior(timeSource, &State, leaderId, logger)
    {
        Become(&Behavior);
    }

    void AfterRegister(TActorContext::TPtr ctx) override {
        Behavior.SchedulePeriodicJob(ctx);
    }

    TSchedulerState State;
    TDefaultBehavior Behavior;
};

} // namespace

NNet::NActors::IActor* CreateSchedulerActor(ITimeSource* timeSource, NNet::NActors::TNodeId leaderId, std::function<void(const std::string&)> logger)
{
    return new TSchedulerActor(timeSource, leaderId, logger);
}

} // namespace NRemote
} // namespace NQdb
