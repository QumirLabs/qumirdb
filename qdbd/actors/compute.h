#pragma once

#include <atomic>
#include <memory>

#include <qdbd/actors/runtime_resources.h>

#include <coroio/actors/actor.hpp>

namespace NQdb {
namespace NRemote {

// The actor runs its graph on the threads of `resources`.
NNet::NActors::IActor* CreateComputeActor(std::shared_ptr<TRuntimeResources> resources);

} // namespace NRemote
} // namespace NQdb
