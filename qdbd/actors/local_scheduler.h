#pragma once

#include <memory>

#include <coroio/actors/actor.hpp>

namespace NQdb {
namespace NRemote {

struct TRuntimeResources;

// The node's compute actors all get `resources`, which the pinger also reports.
NNet::NActors::IActor* CreateLocalScheduler(NNet::NActors::TNodeId leader,
    uint64_t pingPeriodMs,
    std::shared_ptr<TRuntimeResources> resources);

} // NRemote
} // NQdb
