#pragma once

#include <qdbd/time_source.h>

#include <coroio/actors/actor.hpp>

#include <vector>

namespace NQdb {
namespace NRemote {

NNet::NActors::IActor* CreateActorsCollector(
    NNet::NActors::TActorId parent,
    std::vector<NNet::NActors::TActorId> localSchedulers,
    uint32_t count,
    ITimeSource* clock = nullptr);

} // namespace NRemote
} // namespace NQdb
