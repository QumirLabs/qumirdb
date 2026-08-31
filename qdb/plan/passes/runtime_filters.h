#pragma once

#include <qdb/plan/ops/operator.h>

#include <cstdint>

namespace NQdb {

// Share nextId across plan roots to keep filter ids unique.
void AttachRuntimeFilters(const TOperatorPtr& root, uint32_t& nextId);

} // namespace NQdb
