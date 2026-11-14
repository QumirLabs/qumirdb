#pragma once

#include <qdb/compiler/query.h>

#include <cstddef>
#include <functional>
#include <string_view>

namespace NQdb {
namespace NFuzz {

// Called with the optimized plan of each statement. `scans` counts the table
// scans the statement reads.
using TPlanHandler = std::function<void(
    const NCompiler::TPreparedQuery& prepared,
    bool explain,
    size_t scans)>;

// Parses, binds and optimizes every statement of `sql` over tiny Parquet copies
// of the regression fixture tables, the way `qdb --sql` does. An input that the
// engine rejects with an error ends the run. Findings are crashes, sanitizer
// reports, hangs and std::out_of_range, which escapes.
void RunStatements(std::string_view sql, const TPlanHandler& handler);

} // namespace NFuzz
} // namespace NQdb
