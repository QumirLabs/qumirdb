#pragma once

#include <qdb/io/io.h>

#include <cstdint>
#include <span>

namespace NQdb {

// Keeps the rows with Selection[i] != 0 and moves them down inside the same
// buffers: fixed-width values, bool and validity bitmaps (honoring their bit
// offsets), string offsets and characters, and Hash. Sets RowCount to the kept
// count, clears Selection and returns the count. Does nothing without Selection.
//
// valueWidths has one entry per column: the byte width of a value, 0 for a bool
// bitmap. String columns are recognized by Offsets.
//
// The row set must own all its buffers (including the validity masks, which are
// written despite being const in TColumn). Throws if it is shared (RefCount != 1);
// views such as filter or shuffle outputs must be gathered instead.
int64_t CompactRowSet(TRowSet& rowSet, std::span<const int32_t> valueWidths);

} // namespace NQdb
