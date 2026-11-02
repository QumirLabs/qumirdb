#pragma once

#include <coroio/actors/actor.hpp>
#include <qdb/compiler/query.h>

namespace NQdb {
namespace NRemote {

// Client entry-point
NNet::NActors::IActor* CreateGatewayActor(NCompiler::TQueryCompiler& compiler, std::function<void(const std::string&)> logger);

} // NRemote
} // NQdb
