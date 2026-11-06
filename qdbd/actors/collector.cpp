#include "collector.h"

#include <qdbd/messages.h>
#include <qdbd/serialization.h>

#include <coroio/actors/actorsystem.hpp>

#include <algorithm>
#include <chrono>
#include <unordered_set>

namespace NQdb {
namespace NRemote {

using namespace NNet::NActors;
using namespace std::chrono;

namespace {

struct TActorIdHash {
    size_t operator()(const TActorId& actor) const {
        return std::hash<TNodeId>()(actor.NodeId())
            ^ std::hash<TLocalActorId>()(actor.ActorId())
            ^ std::hash<TCookie>()(actor.Cookie());
    }
};

struct TPlacement {
    TActorId Scheduler;
    uint32_t Count = 0;
    bool Replied = false;
};

struct TActorsCollector : public IBehaviorActor,
    public TBehavior<TActorsCollector, TSpawnComputeActorResponse, TWakeUp>
{
    TActorsCollector(TActorId parent, std::vector<TActorId> schedulers, uint32_t count, ITimeSource* clock)
        : Clock(clock ? clock : &RealClock)
        , Parent(parent)
        , Deadline(Clock->CurrentTime() + seconds(5))
    {
        const auto size = std::min<size_t>(count, schedulers.size());
        for (size_t i = 0; i < size; ++i) {
            Placements.push_back({
                .Scheduler = schedulers[i],
                .Count = static_cast<uint32_t>(count / size + (i < count % size)),
            });
        }
        MissingNodes = count != 0 && size == 0;
        Become(this);
    }

    bool AllReplied() const {
        return std::all_of(Placements.begin(), Placements.end(), [](const auto& placement) {
            return placement.Replied;
        });
    }

    void StopActors(TActorContext::TPtr& ctx) {
        for (const auto actor : Actors) {
            ctx->Send<TPoison>(actor, TPoison{});
        }
        Actors.clear();
    }

    void Complete(std::string error, TActorContext::TPtr& ctx) {
        if (Completed) {
            return;
        }
        Completed = true;
        if (!error.empty()) {
            StopActors(ctx);
        }
        ctx->Send<TSpawnComputeActorResponse>(Parent, TSpawnComputeActorResponse{
            .ActorIds = std::move(Actors),
            .Error = std::move(error),
        });
        Actors.clear();
        ctx->Cancel(CheckEvent);
        ctx->Send<TPoison>(ctx->Self(), TPoison{});
    }

    void Receive(TSpawnComputeActorResponse&& response, TBlob, TActorContext::TPtr ctx) {
        if (Completed) {
            return;
        }
        auto found = std::find_if(Placements.begin(), Placements.end(), [&](const auto& placement) {
            return placement.Scheduler == ctx->Sender();
        });
        if (found == Placements.end() || found->Replied) {
            return;
        }
        found->Replied = true;
        bool valid = response.ActorIds.size() == found->Count;
        for (const auto actor : response.ActorIds) {
            if (actor.ActorId() == 0 || actor.NodeId() != ctx->Sender().NodeId()
                || !UniqueActors.insert(actor).second)
            {
                valid = false;
            } else {
                Actors.push_back(actor);
            }
        }
        if (Clock->CurrentTime() >= Deadline) {
            Complete("compute actor allocation timed out", ctx);
        } else if (!response.Error.empty()) {
            Complete(std::move(response.Error), ctx);
        } else if (!valid) {
            Complete("invalid compute actor allocation response", ctx);
        } else if (AllReplied()) {
            Complete({}, ctx);
        }
    }

    void ScheduleCheck(TActorContext::TPtr& ctx) {
        CheckEvent = ctx->Schedule<TWakeUp>(NNet::TClock::now() + seconds(1), ctx->Self(), ctx->Self());
    }

    void Receive(TWakeUp&&, TBlob, TActorContext::TPtr ctx) {
        if (Completed) {
            return;
        }
        if (Clock->CurrentTime() >= Deadline) {
            Complete("compute actor allocation timed out", ctx);
        } else {
            ScheduleCheck(ctx);
        }
    }

    void AfterRegister(TActorContext::TPtr ctx) override {
        if (MissingNodes || Placements.empty()) {
            Complete(MissingNodes ? "no live compute nodes" : "", ctx);
            return;
        }
        std::unordered_set<TActorId, TActorIdHash> schedulers;
        for (const auto& placement : Placements) {
            if (!placement.Scheduler || !schedulers.insert(placement.Scheduler).second) {
                Placements.clear();
                Complete("invalid local scheduler placement", ctx);
                return;
            }
        }
        for (const auto& placement : Placements) {
            ctx->Send<TSpawnComputeActor>(placement.Scheduler, TSpawnComputeActor{.Count = placement.Count});
        }
        ScheduleCheck(ctx);
    }

    void BeforeUnregister(TActorContext::TPtr ctx) override {
        ctx->Cancel(CheckEvent);
        StopActors(ctx);
    }

    void HandleUnknownMessage(TMessageId, TBlob, TActorContext::TPtr) {}

    TTimeSource RealClock;
    ITimeSource* Clock;
    TActorId Parent;
    std::vector<TPlacement> Placements;
    NNet::TTime Deadline;
    TEvent CheckEvent;
    bool MissingNodes = false;
    bool Completed = false;
    std::vector<TActorId> Actors;
    std::unordered_set<TActorId, TActorIdHash> UniqueActors;
};

} // namespace

IActor* CreateActorsCollector(TActorId parent, std::vector<TActorId> localSchedulers,
    uint32_t count, ITimeSource* clock)
{
    return new TActorsCollector(parent, std::move(localSchedulers), count, clock);
}

} // namespace NRemote
} // namespace NQdb
