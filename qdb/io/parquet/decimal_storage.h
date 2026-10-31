#pragma once

#include <arrow/api.h>

#include <memory>

namespace NQdb {

// The unscaled values of an Arrow Decimal32/64/128 array at the width of the qdb
// storage of its precision (DecimalStorageOf): a slice of the array's own values when
// the widths agree, else a sign-extended or truncated copy. Values under nulls are
// unspecified.
std::shared_ptr<arrow::Buffer> DecimalStorageValues(
    const arrow::Array& array,
    arrow::MemoryPool* memoryPool);

} // namespace NQdb
