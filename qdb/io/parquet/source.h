#pragma once

#include <qdb/io/io.h>
#include <qdb/plan/ops/stats.h>
#include <qdb/scheduler/scan_split.h>

#include <qumir/parser/ast.h>

#include <memory>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace arrow { class RecordBatchReader; class Schema; }

namespace NQdb {

class TParquetColumnScan;
class TParquetSource;
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

// Opened parquet file shared by all of its scan sources. The file schema and
// statistics are loaded once; each source creates its own record-batch reader.
class TParquetFile {
public:
    explicit TParquetFile(const std::string& path);
    ~TParquetFile();

    std::unique_ptr<TParquetSource> MakeSource() const;

private:
    std::shared_ptr<TParquetFileData> Data_;
};

class TParquetSource :
    public ISource,
    public IRowLookupSource,
    public NScheduler::IScanMetadataSource
{
public:
    explicit TParquetSource(const std::string& path);
    ~TParquetSource();

    const TSchema& Schema() const override;
    bool Next(TRowSet& rowSet) override;
    void RestrictColumns(const std::unordered_set<std::string>& names) override;
    std::vector<NScheduler::TScanRowGroup> ScanRowGroups() const override;
    // Estimate a planned projection without changing the source's reader.
    std::vector<NScheduler::TScanRowGroup> ScanRowGroups(
        const std::unordered_set<std::string>& columnNames) const;
    std::vector<NScheduler::TScanRowGroup> PruneRowGroups(
        const NQumir::NAst::TExprPtr& predicate,
        std::string_view sourceAlias,
        std::ostream* diagnostics = nullptr,
        const std::unordered_set<std::string>* columnNames = nullptr) const;
    std::unique_ptr<TParquetSource> MakeRowGroupsSource(
        const std::vector<size_t>& rowGroups) const;
    std::shared_ptr<const IPhysicalRowReader> CompileReader(
        std::span<const std::string> columnNames) const override;

    std::optional<TLateMaterializationCost> EstimateLookup(
        std::span<const std::string> earlyColumns,
        std::span<const std::string> fetchColumns,
        uint64_t maxRows) const override;
    const TStatsPtr Stats() const override;

private:
    friend class TParquetFile;
    TParquetSource(
        std::shared_ptr<TParquetFileData> file,
        std::optional<std::vector<int>> rowGroups,
        std::optional<std::unordered_set<std::string>> restrictedColumns);

    void ResetReader();
    std::vector<int> EffectiveRowGroups() const;
    std::vector<int> EffectiveFileColumnIndexes(
        const std::unordered_set<std::string>* columnNames = nullptr) const;
    std::vector<NScheduler::TScanRowGroup> ScanRowGroupsForColumns(
        const std::unordered_set<std::string>* columnNames) const;
    void RefreshSchema();
    bool RowIdRequested() const;

    std::shared_ptr<TParquetFileData> File_;
    // nullopt means the full file; an engaged empty vector is an empty scan.
    std::optional<std::vector<int>> RowGroups_;
    std::optional<std::unordered_set<std::string>> RestrictedColumns_;
    std::shared_ptr<arrow::RecordBatchReader> Reader_;
    // File fields in output order; the decimal ones are read by ColumnScans_.
    std::vector<int> SelectedColumns_;
    struct TColumnScan {
        size_t Position = 0;
        std::unique_ptr<TParquetColumnScan> Scan;
    };
    std::vector<TColumnScan> ColumnScans_;
    std::shared_ptr<arrow::Schema> BatchSchema_;
    std::vector<int> ReaderRowGroups_;
    size_t ReaderRowGroupIndex_ = 0;
    int64_t ReaderRowOffset_ = 0;
    std::vector<std::string> Names_;
    std::vector<TColumnSchema> Columns_;
    TSchema Schema_;
};

} // namespace NQdb
