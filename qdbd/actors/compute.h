#pragma once

#include <atomic>
#include <memory>

#include <coroio/actors/actor.hpp>

namespace NQdb {
namespace NRemote {

NNet::NActors::IActor* CreateComputeActor();

} // namespace NRemote
} // namespace NQdb
