#pragma once

#include <qdb/plan/ops/operator.h>
#include <qdb/plan/passes/const_fold.h>

#include <cstdint>

namespace NQdb {

// Rewrites `filter(window(x), rank <= K && rest)`, where the window computes
// only ranking functions and K is a constant, to
// `filter(window(filter(window-partial(x), local <= K)), rank <= K && rest)`.
// The partial window ranks each lane's rows on their own, before the exchange
// into the window. A row's local rank is at most its global rank, so every row
// the original filter keeps survives the partial one and keeps its rank.
// `nextId` numbers the local rank columns across the query.
void ApplyWindowTopK(
    NQumir::NAst::TTypeFactory& types,
    const TOperatorPtr& root,
    TConstFolder& folder,
    uint32_t& nextId);

} // namespace NQdb
