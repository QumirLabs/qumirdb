#pragma once

#include <memory>

#include <qdb/io/parquet/table_cache.h>
#include <qdb/scheduler/settings/settings.h>

#include <coroio/actors/actor.hpp>

namespace NQdb {
namespace NRemote {

NNet::NActors::IActor* CreateLocalScheduler(NNet::NActors::TNodeId leader,
    uint64_t pingPeriodMs,
    const NScheduler::TSchedulerSettings& scheduler = {},
    const NScheduler::TTransportSettings& transport = {},
    // Compute actors open their scans' files through it.
    std::shared_ptr<TTableCache> tables = nullptr,
    // See TRuntimeResources::SerializeOnIoThreads.
    bool serializeOnIoThreads = false);

} // NRemote
} // NQdb
