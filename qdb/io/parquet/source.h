#pragma once

#include <qdb/io/io.h>
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

namespace arrow { class RecordBatchReader; class Schema; }

namespace NQdb {

class TDecimalColumnScan;
struct TParquetFileData;

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
        std::optional<std::unordered_set<std::string>> restrictedColumns = std::nullopt) const override;

    TSchema Schema(NQumir::NAst::TTypeFactory& types) const override;
    const TStatsPtr Stats() const override;
    const std::string& Path() const override;
    std::unique_ptr<IRowLookupSource> MakeLookupSource() const override;
    std::vector<NScheduler::TScanRowGroup> ScanRowGroups() const;
    // Estimate a planned projection without opening a reader.
    std::vector<NScheduler::TScanRowGroup> ScanRowGroups(
        const std::unordered_set<std::string>& columnNames) const;
    std::vector<NScheduler::TScanRowGroup> PruneRowGroups(
        NQumir::NAst::TTypeFactory& types,
        const NQumir::NAst::TExprPtr& predicate,
        std::string_view sourceAlias,
        std::ostream* diagnostics = nullptr,
        const std::unordered_set<std::string>* columnNames = nullptr,
        const NKernel::TAnnotationContext* annotation = nullptr) const override;

private:
    std::vector<NScheduler::TScanRowGroup> ScanRowGroupsForColumns(
        const std::unordered_set<std::string>* columnNames) const;

    std::shared_ptr<TParquetFileData> Data_;
};

// A reader of one scan: the chosen row groups and columns of a parquet file.
class TParquetSource : public ISource {
public:
    ~TParquetSource();

    TSchema Schema(NQumir::NAst::TTypeFactory& types) const override;
    const TStatsPtr Stats() const override;
    bool Next(TRowSet& rowSet) override;

private:
    friend class TParquetFile;
    TParquetSource(
        std::shared_ptr<TParquetFileData> file,
        std::optional<std::vector<int>> rowGroups,
        std::optional<std::unordered_set<std::string>> restrictedColumns);

    void ResetReader();
    std::vector<int> EffectiveRowGroups() const;
    std::vector<int> EffectiveFileColumnIndexes() const;

    void SelectColumns();
    void RefreshSchema();
    bool RowIdRequested() const;
    void EnsureOpen();

    std::shared_ptr<TParquetFileData> File_;
    // nullopt means the full file; an engaged empty vector is an empty scan.
    std::optional<std::vector<int>> RowGroups_;
    std::optional<std::unordered_set<std::string>> RestrictedColumns_;
    std::shared_ptr<arrow::RecordBatchReader> Reader_;
    // File fields in output order; the decimal ones are read by DecimalScans_.
    std::vector<int> SelectedColumns_;
    struct TDecimalScan {
        size_t Position = 0;
        std::unique_ptr<TDecimalColumnScan> Scan;
    };
    std::vector<TDecimalScan> DecimalScans_;
    std::shared_ptr<arrow::Schema> BatchSchema_;
    std::vector<int> ReaderRowGroups_;
    size_t ReaderRowGroupIndex_ = 0;
    int64_t ReaderRowOffset_ = 0;
};

} // namespace NQdb
