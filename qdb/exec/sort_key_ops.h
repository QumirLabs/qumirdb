#pragma once

#include <qdb/exec/sort_exec.h>
#include <qdb/io/io.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace NQdb {

// Small helpers shared by the sort, merge and window executors.

// A row is selected unless the batch carries a selection vector that zeroes it.
bool RowSelected(const TRowSet& batch, int32_t row);

// i64 work slots per row for the radix cascade: string keys sort {prefix, rowId}
// pairs (4 slots), everything else needs one.
size_t RadixWorkStride(const std::vector<TSortColumnRef>& keyColumns);

} // namespace NQdb
