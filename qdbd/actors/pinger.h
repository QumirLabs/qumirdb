#pragma once

#include <memory>

#include <qdbd/actors/runtime_resources.h>

#include <coroio/actors/actor.hpp>

namespace NQdb {
namespace NRemote {

NNet::NActors::IActor* CreatePinger(
    std::shared_ptr<TRuntimeResources> resources,
    NNet::NActors::TActorId pingFrom,
    NNet::NActors::TActorId pingTo,
    uint64_t periodMs);

} // namespace NRemote
} // namespace NQdb
