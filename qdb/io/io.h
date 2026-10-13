#pragma once

#include <qdb/io/physical_row_id.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>

#include <qdb/io/schema.h>
#include <qdb/plan/ops/stats.h>

namespace NQdb {

struct TColumn {
    char* Data;
    int32_t DataBitOffset = 0; // bit offset for bitmap-backed columns (bool)
    const uint8_t* Mask; // Arrow null bitmap; nullptr if all valid
    int32_t MaskBitOffset = 0;
    void* Offsets; // raw offsets buffer for variable-length types; null for fixed-width
    uint8_t OffsetWidth; // 4 for STRING, 8 for LARGE_STRING, 0 otherwise
};

struct TRowSet {
    TColumn* Columns;
    int64_t ColumnCount;
    int64_t RowCount;
    uint8_t* Selection; // nullptr = all rows selected; Selection[i]==0 skips row i
    // nullptr = not precomputed; otherwise Hash[i] is a shuffle-consistent
    // rh_hash of row i's key for every selected row. The slot of an unselected
    // row holds no hash and must not be read.
    uint64_t* Hash;

    void (*Destroy)(TRowSet*);
    void* Private;
    int64_t RefCount;
};

inline void Retain(TRowSet* rs) {
    ++rs->RefCount;
}

inline void Release(TRowSet* rs) {
    if (--rs->RefCount == 0 && rs->Destroy) {
        rs->Destroy(rs);
    }
}

struct ISource {
    virtual ~ISource() = default;
    virtual const TSchema& Schema() const = 0;
    virtual const TStatsPtr Stats() const = 0;
    virtual bool Next(TRowSet& rowSet) = 0;
    virtual void RestrictColumns(const std::unordered_set<std::string>& names) {}
};

class IPhysicalRowReader {
public:
    virtual ~IPhysicalRowReader() = default;

    virtual bool ReadRows(
        std::span<const TPhysicalRowId> rowIds,
        TRowSet& output,
        std::string* error) const = 0;
};

inline constexpr std::string_view InternalRowIdColumnName = "__row_id__";

// Conservative byte estimates used by the logical late-materialization rule.
// Sources that cannot provide stable physical row locators simply do not
// implement IRowLookupSource.
struct TLateMaterializationCost {
    uint64_t EagerBytes = 0;
    uint64_t NarrowBytes = 0;
    uint64_t LookupBytes = 0;
};

class IRowLookupSource {
public:
    virtual ~IRowLookupSource() = default;

    virtual std::optional<TLateMaterializationCost> EstimateLookup(
        std::span<const std::string> earlyColumns,
        std::span<const std::string> fetchColumns,
        uint64_t maxRows) const = 0;

    virtual std::shared_ptr<const IPhysicalRowReader> CompileReader(
        std::span<const std::string> columnNames) const = 0;
};

struct ISink {
    virtual ~ISink() = default;
    virtual void Write(const TRowSet& rowSet) = 0;
    virtual void Flush() {}
};

} // namespace NQdb
