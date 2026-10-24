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

namespace arrow {
class Field;
class RecordBatchReader;
} // namespace arrow

namespace NQdb {

class TParquetSource;
class TParquetKernelScan;
struct TParquetFileData;
struct TKernelSlot;

// How a source decodes pages: through the parquet decoder kernel
// (qdb/kernel/parquet/decode.oz) when Kernel is set and supports every column
// and row group the source reads, otherwise through Arrow.
struct TParquetDecoding {
    std::shared_ptr<TKernelSlot> Kernel;
    // Rows per batch of the kernel path.
    int64_t BatchRows = 2048;
};

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
        const std::vector<size_t>& rowGroups,
        TParquetDecoding decoding = {}) const;
    // Why the decoder kernel cannot read this source, or nullopt.
    std::optional<std::string> KernelDecodingUnsupported() const;
    bool UsesKernelDecoding() const { return KernelScan_ != nullptr; }
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
        std::optional<std::unordered_set<std::string>> restrictedColumns,
        TParquetDecoding decoding = {});

    void ResetReader();
    std::vector<int> ScanFields() const;
    std::vector<int> EffectiveRowGroups() const;
    std::vector<int> EffectiveFileColumnIndexes(
        const std::unordered_set<std::string>* columnNames = nullptr) const;
    std::vector<NScheduler::TScanRowGroup> ScanRowGroupsForColumns(
        const std::unordered_set<std::string>* columnNames) const;
    void RefreshSchema(const std::vector<std::shared_ptr<arrow::Field>>& fields);
    bool RowIdRequested() const;

    std::shared_ptr<TParquetFileData> File_;
    // nullopt means the full file; an engaged empty vector is an empty scan.
    std::optional<std::vector<int>> RowGroups_;
    std::optional<std::unordered_set<std::string>> RestrictedColumns_;
    TParquetDecoding Decoding_;
    // Exactly one of the two readers is set.
    std::unique_ptr<TParquetKernelScan> KernelScan_;
    std::shared_ptr<arrow::RecordBatchReader> Reader_;
    std::vector<int> ReaderRowGroups_;
    size_t ReaderRowGroupIndex_ = 0;
    int64_t ReaderRowOffset_ = 0;
    std::vector<std::string> Names_;
    std::vector<TColumnSchema> Columns_;
    TSchema Schema_;
};

} // namespace NQdb
