#pragma once

#include <coroio/actors/actor.hpp>

namespace NQdb {
namespace NRemote {

// Client entry-point
NNet::NActors::IActor* CreateGatewayActor();

} // NRemote
} // NQdb
