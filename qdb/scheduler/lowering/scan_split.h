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

struct TScanSplit {
    // Predicate pruning can leave a non-contiguous set of row groups.
    std::vector<size_t> RowGroups;
    int64_t RowCount = 0;
    int64_t ByteSize = 0;
    bool SerialRead = false;
    // Morsel i is RowGroups[MorselEnds[i - 1], MorselEnds[i]); empty when the
    // split's own lane reads all of it.
    std::vector<size_t> MorselEnds;
};

// Resolve an explicit cap or choose one for a source after pruning. Join inputs
// stay within the worker count; other large scans may use two lanes per worker.
size_t ChooseScanTaskCount(
    const std::vector<TScanRowGroup>& rowGroups,
    const TSettings& settings,
    bool feedsJoin);

// Several splits get morsels when settings.MorselRows is positive.
std::vector<TScanSplit> BuildScanSplits(
    const std::vector<TScanRowGroup>& rowGroups,
    const TScanSplitSettings& settings);

} // namespace NScheduler
} // namespace NQdb
