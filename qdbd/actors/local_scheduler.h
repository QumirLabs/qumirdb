#pragma once

#include <qdb/scheduler/settings/settings.h>

#include <coroio/actors/actor.hpp>

namespace NQdb {
namespace NRemote {

NNet::NActors::IActor* CreateLocalScheduler(NNet::NActors::TNodeId leader,
    uint64_t pingPeriodMs,
    const NScheduler::TSchedulerSettings& scheduler = {});

} // NRemote
} // NQdb
