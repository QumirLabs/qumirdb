#pragma once

#include <coroio/actors/actor.hpp>

namespace NQdb {
namespace NRemote {

// Query orchestrator
NNet::NActors::IActor* CreateQueryActor();

} // NRemote
} // NQdb
