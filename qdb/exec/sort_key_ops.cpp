#include <qdb/exec/sort_key_ops.h>

#include <algorithm>

namespace NQdb {

bool RowSelected(const TRowSet& batch, int32_t row) {
    return !batch.Selection || batch.Selection[row] != 0;
}

size_t RadixWorkSlots(const TValueType& value) {
    if (value.Kind == EValueKind::String) {
        return 4;
    }
    if (value.Kind == EValueKind::Bool || value.Width == 16) {
        return 1;
    }
    return 3;
}

size_t RadixWorkStride(const std::vector<TSortColumnRef>& keyColumns) {
    size_t slots = 1;
    for (const auto& keyColumn : keyColumns) {
        slots = std::max(slots, RadixWorkSlots(keyColumn.Value));
    }
    return slots;
}

} // namespace NQdb
