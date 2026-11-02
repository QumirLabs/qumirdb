#pragma once

#include <coroio/actors/actor.hpp>
#include <qdb/compiler/query.h>

namespace NQdb {
namespace NRemote {

// Query orchestrator
NNet::NActors::IActor* CreateQueryActor(NCompiler::TQueryCompiler& compiler, std::function<void(const std::string&)> logger);

} // NRemote
} // NQdb
