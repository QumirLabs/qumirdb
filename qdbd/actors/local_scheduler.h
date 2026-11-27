#pragma once

#include <memory>

#include <qdbd/actors/runtime_resources.h>

#include <coroio/actors/actor.hpp>

namespace NQdb {
namespace NRemote {

// The node's compute actors all get `resources`, which the pinger also reports.
NNet::NActors::IActor* CreateLocalScheduler(NNet::NActors::TNodeId leader,
    uint64_t pingPeriodMs,
    std::shared_ptr<TRuntimeResources> resources);

} // NRemote
} // NQdb
