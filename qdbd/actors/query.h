#pragma once

#include <coroio/actors/actor.hpp>
#include <qdb/compiler/query.h>
#include <qdb/scheduler/settings/settings.h>

namespace NQdb {
namespace NRemote {

// Query orchestrator: lowers a query with `settings`, splits it into one
// fragment per compute actor, starts them and answers with the rows as CSV.
NNet::NActors::IActor* CreateQueryActor(
    NCompiler::TQueryCompiler& compiler,
    NScheduler::TSettings settings,
    std::function<void(const std::string&)> logger);

} // NRemote
} // NQdb
