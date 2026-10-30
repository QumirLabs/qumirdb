#include <qdb/exec/instrumentation/operator.h>

#include <qdb/plan/types/decimal.h>
#include <qdb/plan/types/nullable.h>

#include <algorithm>
#include <stdexcept>

namespace NQdb {

std::vector<int64_t> InstrumentationColumnWidths(const NQumir::NAst::TTypePtr& type) {
    using namespace NQumir::NAst;
    auto structure = TMaybeType<TStructType>(UnwrapNamedType(type));
    if (!structure) {
        throw std::runtime_error("instrumentation rowset schema must be a struct");
    }
    std::vector<int64_t> widths;
    for (const auto& [name, field] : structure.Cast()->Fields) {
        const auto value = UnwrapNamedType(UnwrapNullableType(field));
        if (IsDecimalType(field) || IsBinIntStorageType(field)) {
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
            throw std::runtime_error("unsupported instrumentation column: " + field->ToString());
        }
    }
    return widths;
}

void TRowSetInstrumentation::Add(const TRowSet& rowSet) {
    if (rowSet.ColumnCount != static_cast<int64_t>(Widths.size())) {
        throw std::runtime_error("instrumentation rowset/schema column count mismatch");
    }
    using TMeasure = void(*)(const TRowSet*, const int64_t*, int64_t*);
    int64_t values[2] = {};
    reinterpret_cast<TMeasure>(Kernel->Fns[0])(&rowSet, Widths.data(), values);
    Rows += values[0];
    Bytes += values[1];
}

uint64_t TOperatorInstrumentation::InputPayloadBytes(const TRowSet& rowSet, size_t input) const {
    auto counter = Inputs.at(input);
    counter.Rows = 0;
    counter.Bytes = 0;
    auto allRows = rowSet;
    allRows.Selection = nullptr;
    counter.Add(allRows);
    return counter.Bytes;
}

void TOperatorInstrumentation::SetRetainedInputBytes(uint64_t bytes) {
    RetainedInputBytes = bytes;
    RetainedInputPeakBytes = std::max(RetainedInputPeakBytes, bytes);
    HasBuildMemory = true;
    BuildMemoryPeakBytes = std::max(BuildMemoryPeakBytes, bytes + Memory_[0] + Memory_[1]);
}

void TOperatorInstrumentation::RetainInput(const TRowSet& rowSet, size_t input, bool append) {
    const auto bytes = InputPayloadBytes(rowSet, input);
    SetRetainedInputBytes(append ? RetainedInputBytes + bytes : bytes);
}

void TOperatorInstrumentation::ObserveHash(
    void* table,
    size_t side,
    bool join,
    std::span<const int64_t> stringBuffers)
{
    using TMeasure = void(*)(void*, int64_t, const int64_t*, int64_t, int64_t*);
    int64_t values[6] = {};
    reinterpret_cast<TMeasure>(Kernel->Fns[1])(
        table, join, stringBuffers.data(), stringBuffers.size(), values);
    HasHash = true;
    HasBuildMemory = true;
    RowLayout = values[3] != 0;
    const uint64_t capacity = values[1];
    const uint64_t rowRebuilds = values[4];
    if (Capacities_[side]) {
        // A swiss table only grows by doubling, so the doublings between two
        // observations are its rehashes, several inside one batch included.
        // A table that moved to the row layout since the last observation
        // doubled up to the slots it moved from. The row layout counts its
        // own rebuilds: the move, then each growth of its entries. Initial
        // reservation is not a rehash.
        const bool wasSwiss = RowRebuilds_[side] == 0;
        const uint64_t swissEnd = !RowLayout ? capacity
            : (wasSwiss ? static_cast<uint64_t>(values[5]) : 0);
        for (auto old = Capacities_[side]; old < swissEnd; old *= 2) {
            ++HashGrowthEvents;
            ++Rehashes;
        }
        HashGrowthEvents += rowRebuilds - RowRebuilds_[side];
        Rehashes += rowRebuilds - RowRebuilds_[side];
    }
    RowRebuilds_[side] = rowRebuilds;
    Capacities_[side] = capacity;
    Groups_[side] = values[0];
    Memory_[side] = values[2];
    Groups = Groups_[0] + Groups_[1];
    HashCapacity = Capacities_[0] + Capacities_[1];
    HashMemoryPeakBytes = std::max(HashMemoryPeakBytes, Memory_[0] + Memory_[1]);
    BuildMemoryPeakBytes = std::max(BuildMemoryPeakBytes, RetainedInputBytes + Memory_[0] + Memory_[1]);
}

void TOperatorInstrumentation::ObserveBuildTable(
    size_t side,
    uint64_t keys,
    uint64_t capacity,
    uint64_t bytes)
{
    HasHash = true;
    HasBuildMemory = true;
    RowLayout = true;
    Capacities_[side] = capacity;
    Groups_[side] = keys;
    Memory_[side] = bytes;
    Groups = Groups_[0] + Groups_[1];
    HashCapacity = Capacities_[0] + Capacities_[1];
    HashMemoryPeakBytes = std::max(HashMemoryPeakBytes, Memory_[0] + Memory_[1]);
    BuildMemoryPeakBytes = std::max(BuildMemoryPeakBytes, RetainedInputBytes + Memory_[0] + Memory_[1]);
}

} // namespace NQdb
