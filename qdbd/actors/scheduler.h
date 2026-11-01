#pragma once

#include <coroio/actors/actor.hpp>
#include <qdbd/time_source.h>

namespace NQdb {
namespace NRemote {

// Cluster "singleton", knows about all nodes and resources
NNet::NActors::IActor* CreateSchedulerActor(ITimeSource* timeSource, NNet::NActors::TNodeId leaderId, std::function<void(const std::string&)> logger);

} // NRemote
} // NQdb
