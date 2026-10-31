#pragma once

#include <qdb/io/io.h>
#include <qdb/kernel/execution_context.h>

#include <cstdint>
#include <vector>

namespace NQdb {

class TLateMaterializeProcessor {
public:
    TLateMaterializeProcessor(
        std::shared_ptr<const IPhysicalRowReader> reader,
        int32_t locatorColumn);

    // The context is unused: lookups run no kernels.
    void Add(TExecutionContext& context, TRowSet& rowSet);
    bool Next(TExecutionContext& context, TRowSet& output);

private:
    std::shared_ptr<const IPhysicalRowReader> Reader_;
    int32_t LocatorColumn_ = 0;
    std::vector<TPhysicalRowId> RowIds_;
    bool Finished_ = false;
};

// Join adjacent column ranges produced by parallel lookup tasks without
// copying their buffers.
void MergeLateMaterializedColumns(
    std::vector<TRowSet>& partitions,
    TRowSet& output);

} // namespace NQdb
