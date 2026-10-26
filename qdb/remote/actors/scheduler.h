#pragma once

#include <coroio/actors/actor.hpp>

namespace NQdb {
namespace NRemote {

// Cluster "singleton", knows about all nodes and resources
NNet::NActors::IActor* CreateSchedulerActor(NNet::NActors::TNodeId leaderId);

} // NRemote
} // NQdb
