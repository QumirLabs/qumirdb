#include <qdb/exec/diagnostics.h>

#include <qdb/plan/types/decimal.h>
#include <qdb/plan/types/nullable.h>

#include <algorithm>
#include <stdexcept>

namespace NQdb {

std::vector<int64_t> DiagnosticColumnWidths(const NQumir::NAst::TTypePtr& type) {
    using namespace NQumir::NAst;
    auto structure = TMaybeType<TStructType>(UnwrapNamedType(type));
    if (!structure) {
        throw std::runtime_error("diagnostic rowset schema must be a struct");
    }
    std::vector<int64_t> widths;
    for (const auto& [name, field] : structure.Cast()->Fields) {
        const auto value = UnwrapNamedType(UnwrapNullableType(field));
        if (IsDecimalType(field)) {
            widths.push_back(16);
        } else if (TMaybeType<TStringType>(value)) {
            widths.push_back(-1);
        } else if (auto integer = TMaybeType<TIntegerType>(value)) {
            widths.push_back(integer.Cast()->BitWidth() / 8);
        } else if (TMaybeType<TBoolType>(value)) {
            widths.push_back(1);
        } else if (TMaybeType<TFloatType>(value)) {
            widths.push_back(8);
        } else {
            throw std::runtime_error("unsupported diagnostic column: " + field->ToString());
        }
    }
    return widths;
}

void TRowSetDiagnostics::Add(const TRowSet& rowSet) {
    if (rowSet.ColumnCount != static_cast<int64_t>(Widths.size())) {
        throw std::runtime_error("diagnostic rowset/schema column count mismatch");
    }
    using TMeasure = void(*)(const TRowSet*, const int64_t*, int64_t*);
    int64_t values[2] = {};
    reinterpret_cast<TMeasure>(Kernel->Fns[0])(&rowSet, Widths.data(), values);
    Rows += values[0];
    Bytes += values[1];
}

void TOperatorDiagnostics::ObserveHash(
    void* table,
    size_t side,
    bool join,
    std::span<const int64_t> stringBuffers)
{
    using TMeasure = void(*)(void*, int64_t, const int64_t*, int64_t, int64_t*);
    int64_t values[3] = {};
    reinterpret_cast<TMeasure>(Kernel->Fns[1])(
        table, join, stringBuffers.data(), stringBuffers.size(), values);
    HasHash = true;
    const uint64_t capacity = values[1];
    if (Capacities_[side]) {
        // Tables only grow by doubling, including multiple growths inside one
        // batch. Initial reservation is not a rehash.
        for (auto old = Capacities_[side]; old < capacity; old *= 2) {
            ++HashGrowthEvents;
            ++Rehashes;
        }
    }
    Capacities_[side] = capacity;
    Groups_[side] = values[0];
    Memory_[side] = values[2];
    Groups = Groups_[0] + Groups_[1];
    HashCapacity = Capacities_[0] + Capacities_[1];
    HashMemoryPeakBytes = std::max(HashMemoryPeakBytes, Memory_[0] + Memory_[1]);
}

} // namespace NQdb
