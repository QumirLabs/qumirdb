#pragma once

#include <qdb/plan/ops/operator.h>
#include <qdb/plan/passes/const_fold.h>

#include <cstdint>

namespace NQdb {

// Adds a partial top-K below a ranking window that a filter bounds by a constant
// rank, so each lane drops the rows that cannot pass before the exchange into the
// window. `nextId` numbers the local rank columns across the query.
void ApplyWindowTopK(
    NQumir::NAst::TTypeFactory& types,
    const TOperatorPtr& root,
    TConstFolder& folder,
    uint32_t& nextId);

} // namespace NQdb
