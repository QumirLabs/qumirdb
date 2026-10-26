#pragma once

#include <coroio/actors/actor.hpp>

namespace NQdb {
namespace NRemote {

enum class EWellKnownActors : uint32_t {
    LocalScheduler = 1,
    Scheduler = 2,
};


inline NNet::NActors::TActorId WellKnownActorId(EWellKnownActors actorId, NNet::NActors::TNodeId nodeId) {
    return NNet::NActors::TActorId(nodeId, (NNet::NActors::TLocalActorId) actorId, 0);
}

} // namespace NRemote
} // namespace NQdb
