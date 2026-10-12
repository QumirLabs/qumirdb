#pragma once

#include <qdb/plan/ops/operator.h>

#include <cstdint>

namespace NQdb {

// Share nextId across plan roots to keep filter ids unique.
// Select each join's filter and consumer placement together. `force` bypasses
// cost and size estimates for measurements while retaining join semantic rules
// and placing the consumer at the deepest safe boundary.
void AttachRuntimeFilters(
    const TOperatorPtr& root, uint32_t& nextId, bool force = false);

// Place consumers for hand-written or parsed emit-filter plans that do not
// already carry a use-filter annotation. Idempotent during lowering.
void PlaceRuntimeFilterConsumers(const TOperatorPtr& root);

} // namespace NQdb
