#include <qdb/exec/sort_key_ops.h>

#include <qdb/kernel/compiler.h>

#include <algorithm>

namespace NQdb {

bool RowSelected(const TRowSet& batch, int32_t row) {
    return !batch.Selection || batch.Selection[row] != 0;
}

size_t RadixWorkStride(const std::vector<TSortColumnRef>& keyColumns) {
    size_t slots = 1;
    for (const auto& keyColumn : keyColumns) {
        slots = std::max(slots, static_cast<size_t>(RowIdSortOf(keyColumn.Value).WorkSlots));
    }
    return slots;
}

} // namespace NQdb
