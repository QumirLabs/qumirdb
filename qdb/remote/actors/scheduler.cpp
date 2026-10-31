#include <coroio/actors/actorsystem.hpp>
#include <qdb/remote/messages.h>
#include <unordered_map>
#include "scheduler.h"

namespace NQdb {
namespace NRemote {

using namespace NNet::NActors;

namespace {

struct TActorIdHash {
    static constexpr std::size_t operator()(const TActorId& actorId) {
        return std::hash<TNodeId>()(actorId.NodeId())
            ^ std::hash<TLocalActorId>()(actorId.ActorId())
            ^ std::hash<TCookie>()(actorId.Cookie())
            ;
    }
};

struct TNodeStatus {
    uint64_t LastPingTime = 0;
    uint64_t TotalSlots = 0;
    uint64_t UsedSlots = 0;
    uint64_t CpuTime = 0;
    std::list<uint64_t>::iterator EvictionMark;
};

struct TSchedulerState {
    std::unordered_map<TActorId, TNodeStatus, TActorIdHash> Nodes;
    std::list<uint64_t> EvictionQueue;

    void Ping(uint64_t currentTime, const TActorId& actorId, const TPing& ping) {
        auto& status = Nodes[actorId];
        bool newEntry = status.LastPingTime == 0;
        status.TotalSlots = ping.TotalSlots;
        status.UsedSlots = ping.UsedSlots;
        status.CpuTime = ping.CpuTime;
        status.LastPingTime = currentTime;
        if (!newEntry) {
            EvictionQueue.erase(status.EvictionMark);
        }
        status.EvictionMark = EvictionQueue.insert(EvictionQueue.end(), currentTime);
    }
};

struct TSchedulerActor : public IBehaviorActor {
    struct TDefaultBehavior : public TBehavior<TDefaultBehavior, TPing> {
        TDefaultBehavior(ITimeSource* timeSource, TSchedulerState* state, TNodeId leaderId)
            : TimeSource(timeSource)
            , State(state)
            , LeaderId(leaderId)
        { }

        void Receive(TPing&& ping, TBlob, TActorContext::TPtr ctx)
        {
            auto currentTime = TimeSource->CurrentTime();
            auto sender = ctx->Sender();
            State->Ping(currentTime, sender, ping);
        }

        void HandleUnknownMessage(TMessageId messageId, TBlob blob, TActorContext::TPtr ctx) { }

        ITimeSource* TimeSource;
        TSchedulerState* State;
        TNodeId LeaderId;
    };

    TSchedulerActor(ITimeSource* timeSource, TNodeId leaderId)
        : Behavior(timeSource, &State, leaderId)
    {
        Become(&Behavior);
    }

    TSchedulerState State;
    TDefaultBehavior Behavior;
};

} // namespace

NNet::NActors::IActor* CreateSchedulerActor(ITimeSource* timeSource, NNet::NActors::TNodeId leaderId)
{
    return new TSchedulerActor(timeSource, leaderId);
}

} // namespace NRemote
} // namespace NQdb
