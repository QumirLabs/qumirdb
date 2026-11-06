#pragma once

#include <atomic>
#include <memory>

#include <coroio/actors/actor.hpp>

namespace NQdb {
namespace NRemote {

struct TRuntimeResources;

NNet::NActors::IActor* CreateComputeActor(std::shared_ptr<TRuntimeResources> resources = {});

} // namespace NRemote
} // namespace NQdb
