#include "rowset_ops.h"

#include <qumir/error.h>

#include <cstring>
#include <string>

namespace {

// Calls move(first, last, kept) for every maximal run of selected rows, where
// `kept` is the number of rows kept before the run. Returns the total kept.
template <class F>
int64_t ForEachRun(const uint8_t* sel, int64_t rows, F&& move) {
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
    return kept;
}

template <class TOffset>
int64_t CompactStrings(TOffset* offsets, char* chars, const uint8_t* sel, int64_t rows) {
    return ForEachRun(sel, rows, [&](int64_t first, int64_t last, int64_t kept) {
        if (kept == first) {
            return;
        }
        // Writes land at indices <= the ones still to be read, so one pass is safe.
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

int64_t CompactFixed(void* data, int64_t width, const uint8_t* sel, int64_t rows) {
    auto* bytes = static_cast<char*>(data);
    return ForEachRun(sel, rows, [&](int64_t first, int64_t last, int64_t kept) {
        if (kept != first) {
            std::memmove(bytes + kept * width, bytes + first * width, static_cast<size_t>((last - first) * width));
        }
    });
}

int64_t CompactBits(uint8_t* bits, int64_t bitOffset, const uint8_t* sel, int64_t rows) {
    return ForEachRun(sel, rows, [&](int64_t first, int64_t last, int64_t kept) {
        if (kept == first) {
            return;
        }
        for (int64_t t = 0; t < last - first; ++t) {
            SetBit(bits, bitOffset + kept + t, GetBit(bits, bitOffset + first + t));
        }
    });
}

int64_t CompactStringColumn(const NQdb::TColumn& col, const uint8_t* sel, int64_t rows) {
    switch (col.OffsetWidth) {
    case 4:
        return CompactStrings(static_cast<int32_t*>(col.Offsets), col.Data, sel, rows);
    case 8:
        return CompactStrings(static_cast<int64_t*>(col.Offsets), col.Data, sel, rows);
    default:
        throw NQumir::TError("unsupported string offset width " + std::to_string(col.OffsetWidth));
    }
}

} // namespace

namespace NQdb {

int64_t CompactRowSet(TRowSet& rowSet, std::span<const int32_t> valueWidths) {
    if (!rowSet.Selection) {
        return rowSet.RowCount;
    }
    if (rowSet.RefCount != 1) {
        throw NQumir::TError("cannot compact a shared row set in place");
    }
    if (static_cast<int64_t>(valueWidths.size()) != rowSet.ColumnCount) {
        throw NQumir::TError("compaction needs one value width per column");
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
        } else if (valueWidths[c] == 0) {
            CompactBits(reinterpret_cast<uint8_t*>(col.Data), col.DataBitOffset, sel, rows);
        } else {
            CompactFixed(col.Data, valueWidths[c], sel, rows);
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
