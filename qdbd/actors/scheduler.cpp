#include "scheduler.h"

#include <coroio/actors/actorsystem.hpp>
#include <qdbd/actors.h>
#include <qdbd/messages.h>
#include <qdbd/serialization.h>

#include <algorithm>
#include <chrono>
#include <list>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace NQdb {
namespace NRemote {

using namespace NNet::NActors;
using namespace std::chrono;

namespace {

using TEvictionQueue = std::list<std::pair<NNet::TTime, TActorId>>;

struct TNodeStatus {
    uint64_t TotalSlots = 0;
    uint64_t UsedSlots = 0;
    uint64_t CpuTime = 0;
    TEvictionQueue::iterator EvictionMark;
};

struct TSchedulerState {
    std::unordered_map<TActorId, TNodeStatus> Nodes;
    TEvictionQueue EvictionQueue;
    std::vector<TActorId> SchedulerQueue;
    size_t RoundRobinPosition = 0;

    void Ping(NNet::TTime currentTime, const TActorId& actorId, const TPing& ping) {
        auto [it, newEntry] = Nodes.try_emplace(actorId);
        auto& status = it->second;
        status.TotalSlots = ping.TotalSlots;
        status.UsedSlots = ping.UsedSlots;
        status.CpuTime = ping.CpuTime;
        if (newEntry) {
            SchedulerQueue.emplace_back(actorId);
        } else {
            EvictionQueue.erase(status.EvictionMark);
        }
        status.EvictionMark = EvictionQueue.insert(EvictionQueue.end(), {currentTime, actorId});
    }

    TActorId Schedule() {
        if (SchedulerQueue.empty()) {
            return {};
        }
        auto ret = SchedulerQueue[RoundRobinPosition];
        RoundRobinPosition = (RoundRobinPosition + 1) % SchedulerQueue.size();
        return ret;
    }

    void Evict(NNet::TTime currentTime, NNet::TClock::duration timeout) {
        std::unordered_set<TActorId> erased;
        while (!EvictionQueue.empty() && currentTime - EvictionQueue.front().first >= timeout) {
            const auto actorId = EvictionQueue.front().second;
            EvictionQueue.pop_front();
            erased.emplace(actorId);
            Nodes.erase(actorId);
        }
        if (!erased.empty()) {
            auto it = std::remove_if(SchedulerQueue.begin(), SchedulerQueue.end(), [&](const auto& x) {
                return erased.contains(x);
            });
            SchedulerQueue.resize(it - SchedulerQueue.begin());
            RoundRobinPosition = SchedulerQueue.empty()
                ? 0
                : RoundRobinPosition % SchedulerQueue.size();
        }
    }
};

struct TSchedulerActor : public IBehaviorActor,
    public TBehavior<TSchedulerActor, TPing, TWakeUp, TSpawnComputeActor>
{
    TSchedulerActor(ITimeSource* clock, TNodeId leaderId, std::function<void(const std::string&)> logger)
        : Clock(clock)
        , LeaderId(leaderId)
        , Logger(std::move(logger))
    {
        Become(this);
    }

    template<class T>
    bool MaybeForward(T& message, TActorContext::TPtr& ctx) {
        if (ctx->Self().NodeId() == LeaderId) {
            return false;
        }
        if (!message.Forwarded) {
            message.Forwarded = true;
            ctx->Forward<T>(WellKnownActorId(EWellKnownActors::Scheduler, LeaderId), std::move(message));
        } else {
            Logger("Scheduler: message reached a non-leader twice");
            if constexpr (std::is_same_v<T, TSpawnComputeActor>) {
                TSpawnComputeActorResponse response;
                response.Error = "scheduler leader mismatch: request reached a non-leader node twice";
                ctx->Send<TSpawnComputeActorResponse>(ctx->Sender(), std::move(response));
            }
        }
        return true;
    }

    void Receive(TPing&& ping, TBlob, TActorContext::TPtr ctx) {
        if (!MaybeForward(ping, ctx)) {
            State.Ping(Clock->CurrentTime(), ctx->Sender(), ping);
        }
    }

    void Receive(TSpawnComputeActor&& request, TBlob, TActorContext::TPtr ctx) {
        if (MaybeForward(request, ctx)) {
            return;
        }
        State.Evict(Clock->CurrentTime(), seconds(10));
        TSpawnComputeActorResponse response;
        if (request.Count != 0 && State.SchedulerQueue.empty()) {
            response.Error = "no live compute nodes";
        }
        const auto count = std::min<size_t>(request.Count, State.SchedulerQueue.size());
        response.ActorIds.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            response.ActorIds.push_back(State.Schedule());
        }
        ctx->Send<TSpawnComputeActorResponse>(ctx->Sender(), std::move(response));
    }

    void ScheduleCheck(TActorContext::TPtr& ctx) {
        ctx->Schedule<TWakeUp>(NNet::TClock::now() + seconds(1), ctx->Self(), ctx->Self());
    }

    void Receive(TWakeUp&&, TBlob, TActorContext::TPtr ctx) {
        State.Evict(Clock->CurrentTime(), seconds(10));
        ScheduleCheck(ctx);
    }

    void AfterRegister(TActorContext::TPtr ctx) override {
        ScheduleCheck(ctx);
    }

    void HandleUnknownMessage(TMessageId messageId, TBlob, TActorContext::TPtr) {
        Logger("Scheduler: unknown message " + std::to_string(messageId));
    }

    ITimeSource* Clock;
    TNodeId LeaderId;
    std::function<void(const std::string&)> Logger;
    TSchedulerState State;
};

} // namespace

IActor* CreateSchedulerActor(ITimeSource* timeSource, TNodeId leaderId, std::function<void(const std::string&)> logger) {
    return new TSchedulerActor(timeSource, leaderId, std::move(logger));
}

} // namespace NRemote
} // namespace NQdb
