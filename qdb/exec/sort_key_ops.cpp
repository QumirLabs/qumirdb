#include <qdb/exec/sort_key_ops.h>

#include <qdb/plan/types/nullable.h>

#include <qumir/parser/type.h>

namespace NQdb {

using namespace NQumir::NAst;

bool RowSelected(const TRowSet& batch, int32_t row) {
    return !batch.Selection || batch.Selection[row] != 0;
}

namespace {

bool IsStringKeyType(const TTypePtr& type) {
    return static_cast<bool>(TMaybeType<TStringType>(
        UnwrapNamedType(UnwrapNullableType(type))));
}

} // namespace

size_t RadixWorkStride(const std::vector<TSortColumnRef>& keyColumns) {
    for (const auto& keyColumn : keyColumns) {
        if (IsStringKeyType(keyColumn.Type)) {
            return 4;
        }
    }
    return 1;
}

} // namespace NQdb
