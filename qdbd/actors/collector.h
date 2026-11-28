#pragma once

#include <qdbd/time_source.h>

#include <coroio/actors/actor.hpp>

#include <chrono>
#include <vector>

namespace NQdb {
namespace NRemote {

// An answered collector waits this long for the missing replies and stops their actors. Links
// retry every second, so a minute covers a reconnect; a node silent longer is treated as lost.
inline constexpr std::chrono::seconds CollectorDrainTimeout{60};

NNet::NActors::IActor* CreateActorsCollector(
    NNet::NActors::TActorId parent,
    std::vector<NNet::NActors::TActorId> localSchedulers,
    uint32_t count,
    ITimeSource* clock = nullptr);

} // namespace NRemote
} // namespace NQdb
