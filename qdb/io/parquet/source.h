#pragma once

#include <qdb/io/io.h>
#include <qdb/io/parquet/row_reader.h>
#include <qdb/io/table.h>
#include <qdb/plan/ops/stats.h>
#include <qdb/scheduler/lowering/scan_split.h>

#include <qumir/parser/ast.h>

#include <memory>
#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace arrow { class Array; class RecordBatch; class RecordBatchReader; class Schema; }

namespace NQdb {

class TDecimalColumnScan;
struct TParquetFileData;
struct TSharedRowGroupPredicate;

// Allocator behind Arrow's parquet reads. Auto prefers jemalloc and falls back
// to Arrow's default pool; the others fall back to it when not compiled in.
enum class EArrowMemoryPool {
    Auto,
    System,
    Default,
    Jemalloc,
    Mimalloc,
};

// The names the config accepts.
inline constexpr std::string_view ArrowMemoryPoolNames = "auto, system, default, jemalloc, mimalloc";

std::optional<EArrowMemoryPool> ParseArrowMemoryPool(std::string_view name);

// Arrow memory pools are process singletons, so the choice is process-wide.
// Call it once at startup, before the first parquet file is opened.
void SetArrowMemoryPool(EArrowMemoryPool pool);

// A parquet table: the opened file shared by all of its scan sources. The file
// schema and statistics are loaded once; each source creates its own
// record-batch reader.
class TParquetFile : public ITable {
public:
    explicit TParquetFile(const std::string& path);
    ~TParquetFile();

    // nullopt row groups read the whole file and an empty list nothing;
    // nullopt columns read every file column.
    std::unique_ptr<ISource> MakeSource(
        std::optional<std::vector<int>> rowGroups = std::nullopt,
        std::optional<std::unordered_set<std::string>> restrictedColumns = std::nullopt,
        std::optional<TRowGroupPredicateProgram> blockPredicate = std::nullopt) const override;

    const TSchema& Schema() const override;
    const TStatsPtr Stats() const override;
    const std::string& Path() const override;
    std::unique_ptr<IRowLookupSource> MakeLookupSource() const override;
    std::vector<NScheduler::TScanRowGroup> ScanRowGroups() const;
    // Estimate a planned projection without opening a reader.
    std::vector<NScheduler::TScanRowGroup> ScanRowGroups(
        const std::unordered_set<std::string>& columnNames) const;
    std::vector<NScheduler::TScanRowGroup> PruneRowGroups(
        const NQumir::NAst::TExprPtr& predicate,
        std::string_view sourceAlias,
        std::ostream* diagnostics = nullptr,
        const std::unordered_set<std::string>* columnNames = nullptr) const override;

private:
    std::vector<NScheduler::TScanRowGroup> ScanRowGroupsForColumns(
        const std::unordered_set<std::string>* columnNames) const;

    std::shared_ptr<TParquetFileData> Data_;
};

// A reader of one scan: the chosen rows and columns of a parquet file.
class TParquetSource : public ISource {
public:
    ~TParquetSource();

    const TSchema& Schema() const override;
    const TStatsPtr Stats() const override;
    bool Next(TRowSet& rowSet) override;

private:
    friend class TParquetFile;

    // One row group of the scan and its rows to read.
    struct TPart {
        int RowGroup = 0;
        // Sorted and disjoint.
        std::vector<TRowRange> Ranges;
    };

    TParquetSource(
        std::shared_ptr<TParquetFileData> file,
        std::vector<TPart> parts,
        std::optional<std::unordered_set<std::string>> restrictedColumns,
        std::optional<TRowGroupPredicateProgram> blockPredicate);

    void SelectColumns();
    void RefreshSchema();
    bool RowIdRequested() const;
    std::vector<int> EffectiveFileColumnIndexes() const;

    void OpenPart(const TPart& part);
    // Block selection reads the predicate columns of a window of the part and
    // gives the other columns the ranges of its blocks the predicate may keep.
    bool StartBlockSelection(const TPart& part);
    void SelectNextBlocks();
    void ClosePart();
    // The next rows of the open part, all from one of its ranges.
    std::optional<TRowRangeBatch> ReadPart();
    std::optional<TRowRangeBatch> ReadArrowPart();

    std::shared_ptr<TParquetFileData> File_;
    std::vector<TPart> Parts_;
    size_t NextPart_ = 0;
    std::optional<std::unordered_set<std::string>> RestrictedColumns_;
    // File fields in output order; the decimal ones are read by DecimalScans_,
    // the others, ArrowColumns_, by Arrow or a range reader.
    std::vector<int> SelectedColumns_;
    std::vector<int> ArrowColumns_;
    struct TDecimalScan {
        size_t Position = 0;
        std::unique_ptr<TDecimalColumnScan> Scan;
    };
    std::vector<TDecimalScan> DecimalScans_;
    // Block selection: the positions in ArrowColumns_ of the predicate columns
    // with value bounds, and the other arrow columns.
    std::optional<TRowGroupPredicateProgram> BlockProgram_;
    std::shared_ptr<TSharedRowGroupPredicate> BlockPredicate_;
    std::vector<size_t> BlockColumns_;
    std::vector<int> RestColumns_;
    std::shared_ptr<arrow::Schema> BatchSchema_;
    std::vector<std::string> Names_;
    std::vector<TColumnSchema> Columns_;
    TSchema Schema_;

    // The open part and the rows and columns its reader reads. A whole row
    // group, or ranges a range reader cannot read, go through Arrow's reader
    // sliced to the ranges. Under block selection BlockReader_ reads the
    // predicate columns: BlockArrays_ holds a window of them from row
    // BlockArraysBegin_, and PartRanges_ grows window by window.
    const TPart* Part_ = nullptr;
    std::vector<TRowRange> PartRanges_;
    std::vector<int> PartColumns_;
    std::unique_ptr<TParquetRangeReader> BlockReader_;
    std::vector<std::shared_ptr<arrow::Array>> BlockArrays_;
    int64_t BlockArraysBegin_ = 0;
    std::unique_ptr<TParquetRangeReader> RangeReader_;
    std::shared_ptr<arrow::RecordBatchReader> ArrowReader_;
    std::shared_ptr<arrow::RecordBatch> ArrowBatch_;
    // Rows [ArrowBatchBegin_, ArrowBatchEnd_) of ArrowBatch_; ArrowCursor_ is
    // the first of them not yet returned.
    int64_t ArrowBatchBegin_ = 0;
    int64_t ArrowBatchEnd_ = 0;
    int64_t ArrowCursor_ = 0;
    size_t ArrowRange_ = 0;
};

} // namespace NQdb
