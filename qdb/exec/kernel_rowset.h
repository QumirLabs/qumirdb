#pragma once

#include <qdb/io/io.h>
#include <qdb/kernel/execution_context.h>
#include <qdb/modules/qumirdb_runtime.h>

#include <cstdint>

namespace NQdb {

// Destroy callback for rowsets materialized inside a generated kernel
// (agg_finish_rowset, jt_materialize): every buffer was qdb_alloc'd and
// recorded in the owners list the kernel left in Private (owners[0] = count,
// then the pointers). Kernels set Destroy to null; the host assigns this.
inline void DestroyKernelOwnedRowSet(TRowSet* rowSet) {
    auto* owners = static_cast<int64_t*>(rowSet->Private);
    if (!owners) {
        return;
    }
    const int64_t count = owners[0];
    for (int64_t i = 0; i < count; ++i) {
        qdb_free(reinterpret_cast<void*>(owners[i + 1]));
    }
    qdb_free(owners);
}

// Output may outlive its producer and the query. Its owners list becomes the
// sole owner, so releasing it on another worker never accesses the task heap.
inline void AdoptKernelOwnedRowSet(TRowSet& rowSet) {
    auto* owners = static_cast<int64_t*>(rowSet.Private);
    if (owners) {
        for (int64_t i = 0; i < owners[0]; ++i) {
            TKernelHeap::Detach(reinterpret_cast<void*>(owners[i + 1]));
        }
        TKernelHeap::Detach(owners);
    }
    rowSet.Destroy = DestroyKernelOwnedRowSet;
}

} // namespace NQdb
