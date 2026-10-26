#pragma once

#include <qdb/kernel/annotate_type.h>
#include <qdb/plan/build/binder.h>
#include <qdb/plan/sql_lowering.h>

#include <qumir/error.h>
#include <qumir/parser/ast.h>

#include <expected>
#include <string>
#include <unordered_map>
#include <vector>

namespace NQdb {
namespace NBuild {

// The GROUP BY of one SELECT: keys over the FROM columns, computed ones
// materialized below the aggregate under generated names.
struct TGrouping {
    bool Present = false;
    std::vector<TGroupKey> Keys;
    // Printed computed key -> its column.
    std::unordered_map<std::string, std::string> Computed;
};

// Turns the aggregate calls of the bound `clauses` (SELECT, HAVING, ORDER BY)
// into aggregate columns and, when the block aggregates, tops its tree with the
// aggregate.
std::expected<void, NQumir::TError> Aggregate(
    TBlock& block,
    const TGrouping& grouping,
    const std::vector<NQumir::NAst::TExprPtr*>& clauses,
    TFreshNames& names,
    const NKernel::TAnnotationContext& annotation);

} // namespace NBuild
} // namespace NQdb
