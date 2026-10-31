#include "rowset_ops.h"

#include <qumir/error.h>

#include <cstring>
#include <string>

namespace {

// Calls move(first, last, kept) per run of selected rows; kept = rows kept before it.
template <class F>
void ForEachRun(const uint8_t* sel, int64_t rows, F&& move) {
    int64_t kept = 0;
    int64_t i = 0;
    while (i < rows) {
        if (!sel[i]) {
            ++i;
            continue;
        }
        int64_t j = i + 1;
        while (j < rows && sel[j]) {
            ++j;
        }
        move(i, j, kept);
        kept += j - i;
        i = j;
    }
}

template <class TOffset>
void CompactStrings(TOffset* offsets, char* chars, const uint8_t* sel, int64_t rows) {
    ForEachRun(sel, rows, [&](int64_t first, int64_t last, int64_t kept) {
        if (kept == first) {
            return;
        }
        // Writes never pass the unread data, so one pass is safe.
        const TOffset srcBegin = offsets[first];
        const TOffset dstBegin = offsets[kept];
        std::memmove(chars + dstBegin, chars + srcBegin, static_cast<size_t>(offsets[last] - srcBegin));
        for (int64_t t = 1; t <= last - first; ++t) {
            offsets[kept + t] = static_cast<TOffset>(dstBegin + (offsets[first + t] - srcBegin));
        }
    });
}

bool GetBit(const uint8_t* bits, int64_t i) {
    return (bits[i / 8] >> (i % 8)) & 1;
}

void SetBit(uint8_t* bits, int64_t i, bool value) {
    const uint8_t m = static_cast<uint8_t>(1u << (i % 8));
    bits[i / 8] = value ? (bits[i / 8] | m) : (bits[i / 8] & ~m);
}

void CompactFixed(void* data, int64_t width, const uint8_t* sel, int64_t rows) {
    auto* bytes = static_cast<char*>(data);
    ForEachRun(sel, rows, [&](int64_t first, int64_t last, int64_t kept) {
        if (kept != first) {
            std::memmove(bytes + kept * width, bytes + first * width, static_cast<size_t>((last - first) * width));
        }
    });
}

void CompactBits(uint8_t* bits, int64_t bitOffset, const uint8_t* sel, int64_t rows) {
    ForEachRun(sel, rows, [&](int64_t first, int64_t last, int64_t kept) {
        if (kept == first) {
            return;
        }
        for (int64_t t = 0; t < last - first; ++t) {
            SetBit(bits, bitOffset + kept + t, GetBit(bits, bitOffset + first + t));
        }
    });
}

void CompactStringColumn(const NQdb::TColumn& col, const uint8_t* sel, int64_t rows) {
    if (col.OffsetWidth == 4) {
        CompactStrings(static_cast<int32_t*>(col.Offsets), col.Data, sel, rows);
    } else {
        CompactStrings(static_cast<int64_t*>(col.Offsets), col.Data, sel, rows);
    }
}

bool IsFixedWidth(int32_t w) {
    return w == 1 || w == 2 || w == 4 || w == 8 || w == 16;
}

} // namespace

namespace NQdb {

int64_t CompactRowSet(TRowSet& rowSet, std::span<const TValueLayout> layouts) {
    if (!rowSet.Selection) {
        return rowSet.RowCount;
    }
    if (rowSet.RefCount != 1) {
        throw NQumir::TError("cannot compact a shared row set in place");
    }
    if (static_cast<int64_t>(layouts.size()) != rowSet.ColumnCount) {
        throw NQumir::TError("compaction needs one value layout per column");
    }
    // Validate first so a failure leaves the row set untouched.
    for (int64_t c = 0; c < rowSet.ColumnCount; ++c) {
        const TColumn& col = rowSet.Columns[c];
        const TValueLayout& layout = layouts[c];
        if (col.Offsets) {
            if (col.OffsetWidth != 4 && col.OffsetWidth != 8) {
                throw NQumir::TError("unsupported string offset width " + std::to_string(col.OffsetWidth));
            }
        } else if (layout.Bitmap) {
            if (layout.Width != 0) {
                throw NQumir::TError("bitmap column must have width 0");
            }
        } else if (!IsFixedWidth(layout.Width)) {
            throw NQumir::TError("unsupported value width " + std::to_string(layout.Width));
        }
    }
    const uint8_t* sel = rowSet.Selection;
    const int64_t rows = rowSet.RowCount;
    int64_t kept = 0;
    for (const uint8_t s : std::span(sel, rows)) {
        kept += s != 0;
    }
    for (int64_t c = 0; c < rowSet.ColumnCount; ++c) {
        TColumn& col = rowSet.Columns[c];
        if (col.Offsets) {
            CompactStringColumn(col, sel, rows);
        } else if (layouts[c].Bitmap) {
            CompactBits(reinterpret_cast<uint8_t*>(col.Data), col.DataBitOffset, sel, rows);
        } else {
            CompactFixed(col.Data, layouts[c].Width, sel, rows);
        }
        if (col.Mask) {
            CompactBits(const_cast<uint8_t*>(col.Mask), col.MaskBitOffset, sel, rows);
        }
    }
    if (rowSet.Hash) {
        CompactFixed(rowSet.Hash, sizeof(uint64_t), sel, rows);
    }
    rowSet.RowCount = kept;
    rowSet.Selection = nullptr;
    return kept;
}

} // namespace NQdb
