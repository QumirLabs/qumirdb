#pragma once

#include <qdb/plan/ops/operator.h>

#include <cstdint>

namespace NQdb {

// Share nextId across plan roots to keep filter ids unique.
// `force` bypasses cost and size estimates for measurements while retaining
// join semantic rules. Normal planning uses the estimated benefit and the
// filtered-build fallback.
void AttachRuntimeFilters(
    const TOperatorPtr& root, uint32_t& nextId, bool force = false);

} // namespace NQdb
