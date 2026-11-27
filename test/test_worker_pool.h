#pragma once

#include <qdb/scheduler/impl/worker_pool.h>

namespace NQdb {

// The worker pool of a unit test binary, one per process as in qdb and qdbd.
inline NScheduler::TWorkerPool& TestWorkerPool() {
    static NScheduler::TWorkerPool pool;
    return pool;
}

} // namespace NQdb
