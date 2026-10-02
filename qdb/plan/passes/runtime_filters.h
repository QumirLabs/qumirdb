#pragma once

#include <qdb/plan/ops/operator.h>

#include <cstdint>

namespace NQdb {

// Share nextId across plan roots to keep filter ids unique.
// `force` skips the selectivity test and emits wherever a build side can be
// named, leaving only the structural rules. It exists because the NDV in our
// Parquet stats saturates -- every inexact numeric column reports 5,000,000 --
// so the ratio decides nothing on real data; this measures the mechanism
// without it. Not a mode to ship on.
void AttachRuntimeFilters(
    const TOperatorPtr& root, uint32_t& nextId, bool force = false);

} // namespace NQdb
