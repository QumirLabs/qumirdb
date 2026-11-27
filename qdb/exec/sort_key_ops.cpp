#include <qdb/exec/sort_key_ops.h>

namespace NQdb {

bool RowSelected(const TRowSet& batch, int32_t row) {
    return !batch.Selection || batch.Selection[row] != 0;
}

size_t RadixWorkStride(const std::vector<TSortColumnRef>& keyColumns) {
    for (const auto& keyColumn : keyColumns) {
        if (keyColumn.Value.Kind == EValueKind::String || keyColumn.Value.Kind == EValueKind::Float) {
            return 4;
        }
    }
    return 1;
}

} // namespace NQdb
