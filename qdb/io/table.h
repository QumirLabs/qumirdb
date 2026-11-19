#pragma once

#include <memory>
#include <vector>
#include <unordered_set>
#include <optional>
#include <string>

#include <qdb/scheduler/lowering/scan_split.h>
#include <qdb/plan/ops/stats.h>

#include "schema.h"
#include "io.h"

namespace NQdb {

class TRowGroupPredicate;

struct ITable {
    virtual ~ITable() = default;

    // The source may skip rows whose column ranges rule out the block
    // predicate, compiled over Schema(); a filter above still checks the rest.
    virtual std::unique_ptr<ISource> MakeSource(
        std::optional<std::vector<int>> rowGroups,
        std::optional<std::unordered_set<std::string>> restrictedColumns,
        std::shared_ptr<const TRowGroupPredicate> blockPredicate) const = 0;

    virtual const TStatsPtr Stats() const = 0;
    virtual const TSchema& Schema() const = 0;
    // Names the table wherever the graph runs; scan specs refer to it.
    virtual const std::string& Path() const = 0;
    // Random access by physical row locator; null if the table has none.
    virtual std::unique_ptr<IRowLookupSource> MakeLookupSource() const = 0;

    // The row groups `predicate`, compiled over Schema(), may keep; all of
    // them without one.
    virtual std::vector<NScheduler::TScanRowGroup> PruneRowGroups(
        const TRowGroupPredicate* predicate,
        std::ostream* diagnostics = nullptr,
        const std::unordered_set<std::string>* columnNames = nullptr) const = 0;
};

} // namespace NQdb
