#pragma once

#include <atomic>
#include <memory>

#include <coroio/actors/actor.hpp>

namespace NQdb {
namespace NRemote {

struct TRuntimeResources {
    std::atomic<uint64_t> TotalSlots = 0;
    std::atomic<uint64_t> UsedSlots = 0;
    std::atomic<uint64_t> CpuTime = 0;
};

NNet::NActors::IActor* CreatePinger(
    std::shared_ptr<TRuntimeResources> resources,
    NNet::NActors::TActorId pingTo,
    uint64_t periodMs);

} // namespace NRemote
} // namespace NQdb
