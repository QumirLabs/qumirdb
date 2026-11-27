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

// i64 work slots per row that a sort by one key of this type needs: a string
// sorts {prefix, rowId} pairs and their copy (4), a bool or 16-byte key its
// row ids (1), any other key its keys, their copy and the row ids' copy (3).
size_t RadixWorkSlots(const TValueType& value);

// The work slots per row of a sort by all keyColumns.
size_t RadixWorkStride(const std::vector<TSortColumnRef>& keyColumns);

} // namespace NQdb
