#pragma once

#include "test_types.h"

#include <qdb/io/io.h>
#include <qdb/io/schema.h>
#include <qdb/io/table.h>
#include <qdb/plan/types/type_text.h>

#include <qumir/parser/type.h>

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace NQdb {

// A plan operator over a test-owned table that outlives the plan.
inline std::shared_ptr<ITable> Borrow(ITable& table) {
    return std::shared_ptr<ITable>(std::shared_ptr<ITable>{}, &table);
}

// Scan specs find a table by path, so every test table needs its own.
inline std::string NextMockTablePath() {
    static std::atomic<uint64_t> next = 0;
    return "mock:" + std::to_string(next++);
}

// Reads the table's own cursor, like the lanes that used to share one source:
// lanes split its batches and a drained table stays drained.
struct TMockReader final : ISource {
    explicit TMockReader(ISource& table)
        : Table(table)
    {}

    TSchema Schema(NQumir::NAst::TTypeFactory& types) const override { return Table.Schema(types); }
    const TStatsPtr Stats() const override { return Table.Stats(); }
    bool Next(TRowSet& rowSet) override { return Table.Next(rowSet); }

    ISource& Table;
};

// Disambiguates the column-schema constructor from the plain name list one.
struct TMockColumns {};

// Its column types are kept in TestTypes() and rebuilt in the factory a schema is
// asked for, as a table builds them in the factory of the query.
struct TMockSource : ISource, ITable {
    std::vector<std::string> Names; // owns backing strings (TColumnSchema::Name is a view)
    std::vector<TColumnSchema> Cols;
    std::vector<TRowSet> Batches;
    size_t Index = 0;
    std::string Path_ = NextMockTablePath();

    TMockSource() = default;

    // Column names (+ optional per-column types, default i64) and optional batches.
    explicit TMockSource(
        std::vector<std::string> names,
        std::vector<TRowSet> batches = {},
        std::vector<NQumir::NAst::TTypePtr> types = {})
        : Names(std::move(names))
        , Batches(std::move(batches))
    {
        BuildFromNames(types);
    }

    // Same, with the (types, batches) argument order some tests use.
    TMockSource(
        std::vector<std::string> names,
        std::vector<NQumir::NAst::TTypePtr> types,
        std::vector<TRowSet> batches)
        : Names(std::move(names))
        , Batches(std::move(batches))
    {
        BuildFromNames(types);
    }

    // Explicit column schema (+ optional batches); names are copied into Names so
    // the schema stays valid after the caller's columns go away.
    explicit TMockSource(
        TMockColumns,
        std::vector<TColumnSchema> columns,
        std::vector<TRowSet> batches = {})
        : Batches(std::move(batches))
    {
        Names.reserve(columns.size());
        for (const auto& column : columns) {
            Names.emplace_back(column.Name);
        }
        Cols.reserve(columns.size());
        for (size_t i = 0; i < columns.size(); ++i) {
            Cols.push_back({Names[i], columns[i].Type});
        }
    }

    TSchema Schema(NQumir::NAst::TTypeFactory& types) const override {
        TSchema schema;
        for (const auto& column : Cols) {
            schema.Columns.push_back({column.Name, ParseTypeText(types, PrintTypeText(column.Type))});
        }
        return schema;
    }
    const TStatsPtr Stats() const override { return nullptr; }
    const std::string& Path() const override { return Path_; }

    bool Next(TRowSet& rowSet) override {
        if (Index >= Batches.size()) {
            return false;
        }
        rowSet = Batches[Index++];
        return true;
    }

    std::unique_ptr<IRowLookupSource> MakeLookupSource() const override {
        return nullptr;
    }

    // Columns are ignored: the mock always emits its whole schema.
    std::unique_ptr<ISource> MakeSource(
        std::optional<std::vector<int>>,
        std::optional<std::unordered_set<std::string>>) const override
    {
        return std::make_unique<TMockReader>(const_cast<TMockSource&>(*this));
    }

    std::vector<NScheduler::TScanRowGroup> PruneRowGroups(
        NQumir::NAst::TTypeFactory&,
        const NQumir::NAst::TExprPtr&,
        std::string_view,
        std::ostream*,
        const std::unordered_set<std::string>*,
        const NQdb::NKernel::TAnnotationContext*) const override
    {
        // One group keeps the mock a single scan lane.
        int64_t rows = 0;
        for (const auto& batch : Batches) {
            rows += batch.RowCount;
        }
        return {{.RowGroup = 0, .RowCount = rows}};
    }

private:
    void BuildFromNames(const std::vector<NQumir::NAst::TTypePtr>& types) {
        Cols.reserve(Names.size());
        for (size_t i = 0; i < Names.size(); ++i) {
            Cols.push_back({
                Names[i],
                (i < types.size() && types[i])
                    ? types[i]
                    : TestTypes().MakeInteger(
                          NQumir::NAst::TIntegerType::I64),
            });
        }
    }
};

} // namespace NQdb
