#pragma once

#include <qdb/scheduler/settings/settings.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace NQdb {
namespace NScheduler {

struct TScanRowGroup {
    size_t RowGroup = 0;
    int64_t RowCount = 0;
    // Uncompressed bytes of the columns read by this scan, when known.
    int64_t ByteSize = 0;
};

// Rows [FirstRow, FirstRow + RowCount) of one row group.
struct TScanRowRange {
    int32_t RowGroup = 0;
    int64_t FirstRow = 0;
    int64_t RowCount = 0;

    bool operator==(const TScanRowRange&) const = default;

    void Fields(this auto& self, auto& v)
    {
        v(self.RowGroup);
        v(self.FirstRow);
        v(self.RowCount);
    }
};

struct TScanSplit {
    // Predicate pruning can leave a non-contiguous set of row groups, and a
    // row group is cut between splits when there are fewer groups than tasks.
    std::vector<TScanRowRange> Ranges;
    int64_t RowCount = 0;
    int64_t ByteSize = 0;
    bool SerialRead = false;
};

// Resolve an explicit cap or choose one for a source after pruning. Join inputs
// stay within the worker count; other large scans may use two lanes per worker.
// Row groups do not cap the count: BuildScanSplits cuts them.
size_t ChooseScanTaskCount(
    const std::vector<TScanRowGroup>& rowGroups,
    const TSettings& settings,
    bool feedsJoin);

std::vector<TScanSplit> BuildScanSplits(
    const std::vector<TScanRowGroup>& rowGroups,
    const TScanSplitSettings& settings);

} // namespace NScheduler
} // namespace NQdb
