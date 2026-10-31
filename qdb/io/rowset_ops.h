#pragma once

#include <qdb/io/io.h>

#include <cstdint>
#include <span>

namespace NQdb {

// Per-column layout of the value buffer. Bitmap marks a bit-packed bool column
// (Width must be 0); this is not NKernel::FixedValueWidth, which gives 1 for bool.
// Other fixed-width columns need Width in {1, 2, 4, 8, 16}. String columns are
// recognized by Offsets and their Width is ignored.
struct TValueLayout {
    int32_t Width = 0;
    bool Bitmap = false;
};

// Keeps the rows with Selection[i] != 0 and moves them down inside the same
// buffers: fixed-width values, bool and validity bitmaps (honoring their bit
// offsets), string offsets and characters, and Hash. Sets RowCount to the kept
// count, clears Selection and returns the count. Does nothing without Selection.
// Clearing only drops the pointer: producers keep the mask in Private (a vector)
// and free it from Destroy, never through Selection, so nothing leaks.
//
// layouts has one entry per column. Throws before changing anything if the
// count, a layout or a string OffsetWidth (4 or 8) is invalid.
//
// Precondition: the caller owns every column buffer (Data, Offsets, Mask, Hash),
// including the validity masks, which are written despite being const in TColumn.
// View row sets over another row set's buffers (broadcast and shuffle views)
// must not be compacted: gather them instead. RefCount != 1 is only a cheap
// guard and does not detect shared buffers.
int64_t CompactRowSet(TRowSet& rowSet, std::span<const TValueLayout> layouts);

} // namespace NQdb
