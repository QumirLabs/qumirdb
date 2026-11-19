#pragma once

#include <vector>
#include <unordered_set>
#include <optional>
#include <string>

#include <qumir/parser/ast.h>
#include <qdb/scheduler/lowering/scan_split.h>
#include <qdb/plan/ops/stats.h>

#include "schema.h"
#include "io.h"

namespace NQdb {

struct ITable {
    virtual ~ITable() = default;

    // nullopt ranges read the whole table and an empty list nothing; nullopt
    // columns read every column.
    virtual std::unique_ptr<ISource> MakeSource(
        std::optional<std::vector<NScheduler::TScanRowRange>> ranges,
        std::optional<std::unordered_set<std::string>> restrictedColumns) const = 0;

    virtual const TStatsPtr Stats() const = 0;
    virtual const TSchema& Schema() const = 0;
    // Names the table wherever the graph runs; scan specs refer to it.
    virtual const std::string& Path() const = 0;
    // Random access by physical row locator; null if the table has none.
    virtual std::unique_ptr<IRowLookupSource> MakeLookupSource() const = 0;

    virtual std::vector<NScheduler::TScanRowGroup> PruneRowGroups(
        const NQumir::NAst::TExprPtr& predicate,
        std::string_view sourceAlias,
        std::ostream* diagnostics = nullptr,
        const std::unordered_set<std::string>* columnNames = nullptr) const = 0;
};

} // namespace NQdb
