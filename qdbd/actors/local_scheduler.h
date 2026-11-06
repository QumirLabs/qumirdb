#pragma once

#include <coroio/actors/actor.hpp>

namespace NQdb {
namespace NRemote {

NNet::NActors::IActor* CreateLocalScheduler(NNet::NActors::TNodeId leader, uint64_t pingPeriodMs);

} // NRemote
} // NQdb
