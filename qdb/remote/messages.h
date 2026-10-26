#pragma once
#include <coroio/actors/actor.hpp>

namespace NQdb {
namespace NRemote {

struct TWakeUp {
    static constexpr NNet::NActors::TMessageId MessageId = 10;
};

struct TPing {
    static constexpr NNet::NActors::TMessageId MessageId = 100;
    uint64_t TotalSlots = 0;
    uint64_t UsedSlots = 0;
    uint64_t CpuTime = 0;
};

struct TSpawnComputeActor {
    static constexpr NNet::NActors::TMessageId MessageId = 100;
};

struct TSpawnComputeActorResponse {
    static constexpr NNet::NActors::TMessageId MessageId = 101;
    NNet::NActors::TActorId ActorId;
};

} // namespace NRemote
} // namespace NQdb
