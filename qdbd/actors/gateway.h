#pragma once

#include <coroio/actors/actor.hpp>
#include <qdb/compiler/query.h>
#include <qdb/scheduler/settings/settings.h>
#include "query.h"

namespace NQdb {
namespace NRemote {

// Client entry-point
// `settings` lower the queries.
NNet::NActors::IActor* CreateGatewayActor(
    TCompilerFactory compilerFactory,
    NScheduler::TSettings settings,
    std::function<void(const std::string&)> logger);

} // NRemote
} // NQdb
