#pragma once

#include <qdb/io/io.h>

#include <cstdint>
#include <span>

namespace NQdb {

// Bitmap: bit-packed bool column (Width must be 0). Otherwise Width is 1, 2, 4, 8
// or 16 (unlike NKernel::FixedValueWidth, bool is not 1). Strings are found by Offsets.
struct TValueLayout {
    int32_t Width = 0;
    bool Bitmap = false;
};

// Moves the rows with Selection[i] != 0 down in place (values, validity, strings,
// Hash), sets RowCount to the kept count and resets Selection, which is not owned.
// layouts has one entry per column; throws before any change if it is invalid.
// The caller must own all column buffers, including Mask (written despite const).
// Views over another row set's buffers must not be compacted. RefCount != 1 is
// only a cheap guard.
int64_t CompactRowSet(TRowSet& rowSet, std::span<const TValueLayout> layouts);

} // namespace NQdb
