#pragma once

#include <coroio/actors/actor.hpp>
#include <qdbd/time_source.h>

namespace NQdb {
namespace NRemote {

NNet::NActors::IActor* CreateLocalScheduler(ITimeSource* timeSource, NNet::NActors::TNodeId leader, uint64_t pingPeriodMs);

} // NRemote
} // NQdb
