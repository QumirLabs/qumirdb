#pragma once

#include <atomic>
#include <memory>

#include <qdb/io/parquet/table_cache.h>
#include <qdb/scheduler/impl/worker_pool.h>
#include <qdb/scheduler/settings/settings.h>
#include <qdbd/scheduler/graph_cleaner.h>

namespace NQdb {
namespace NRemote {

struct TRuntimeResources {
    std::atomic<uint64_t> TotalSlots = 0;
    std::atomic<uint64_t> UsedSlots = 0;
    std::atomic<uint64_t> CpuTime = 0;
    // How compute actors build their scheduler; filled once at node start.
    NScheduler::TSchedulerSettings Scheduler;
    NScheduler::TTransportSettings Transport;
    // Tables opened by the node's earlier queries; null opens every file anew.
    std::shared_ptr<TTableCache> Tables;
    // net:io_threads > 0: the I/O threads serialize the chunks the compute actors send.
    bool SerializeOnIoThreads = false;
    // jit:perf: the kernel image links write perf jitdump records.
    bool PerfJit = false;
    // The threads of the compute actors' schedulers, kept between queries. Declared
    // before GraphCleaner, so it is joined after the cleaner's thread (see bin/daemon.cpp).
    NScheduler::TWorkerPool Workers;
    // Destroys the graphs of stopped compute actors; null destroys them on the actor thread.
    std::shared_ptr<NScheduler::TGraphCleaner> GraphCleaner;
};

} // namespace NRemote
} // namespace NQdb
