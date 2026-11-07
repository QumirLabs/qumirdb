#pragma once

#include <atomic>
#include <memory>

#include <qdb/scheduler/settings/settings.h>

#include <coroio/actors/actor.hpp>

namespace NQdb {
namespace NRemote {

struct TRuntimeResources {
    std::atomic<uint64_t> TotalSlots = 0;
    std::atomic<uint64_t> UsedSlots = 0;
    std::atomic<uint64_t> CpuTime = 0;
    // How compute actors build their scheduler; filled once at node start.
    NScheduler::TSchedulerSettings Scheduler;
};

NNet::NActors::IActor* CreatePinger(
    std::shared_ptr<TRuntimeResources> resources,
    NNet::NActors::TActorId pingFrom,
    NNet::NActors::TActorId pingTo,
    uint64_t periodMs);

} // namespace NRemote
} // namespace NQdb
