#pragma once

#include <qdb/io/parquet/row_id.h>

#include <arrow/result.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace arrow {
class Array;
class MemoryPool;
class RecordBatch;
} // namespace arrow

namespace parquet {
class ParquetFileReader;
} // namespace parquet

namespace parquet::arrow {
class FileReader;
struct SchemaManifest;
} // namespace parquet::arrow

namespace NQdb {

class TParquetRowReader {
public:
    // Column names are resolved once against the full file schema.
    explicit TParquetRowReader(
        std::shared_ptr<parquet::arrow::FileReader> reader,
        std::span<const std::string> columnNames,
        arrow::MemoryPool* memoryPool = nullptr);
    ~TParquetRowReader();

    TParquetRowReader(const TParquetRowReader&) = delete;
    TParquetRowReader& operator=(const TParquetRowReader&) = delete;

    arrow::Result<std::shared_ptr<arrow::RecordBatch>> ReadRows(
        std::span<const TPhysicalRowId> rowIds) const;

    // False when a column type forces ReadRows to read whole row groups.
    bool SupportsPageLookup() const;

private:
    struct TImpl;
    std::unique_ptr<TImpl> Impl_;
};

// Rows [Begin, End) of one row group.
struct TRowRange {
    int64_t Begin = 0;
    int64_t End = 0;

    bool operator==(const TRowRange&) const = default;
};

// Rows [FirstRow, FirstRow + RowCount) of the row group, one array per column.
struct TRowRangeBatch {
    int64_t FirstRow = 0;
    int64_t RowCount = 0;
    std::vector<std::shared_ptr<arrow::Array>> Columns;
};

// Reads columns of one row group over sorted, disjoint row ranges with the
// Parquet record reader. A data page outside every range is skipped before it
// is decompressed, and the rows of a kept page outside the ranges are skipped
// rather than materialized.
class TParquetRangeReader {
public:
    // Columns are file field indices of a flat schema. Null when a column needs
    // Arrow's own reader: nested, or of a type the record reader path lacks.
    // A file opened with a buffered stream reads only the pages a range keeps.
    static std::unique_ptr<TParquetRangeReader> Make(
        const parquet::arrow::SchemaManifest& manifest,
        parquet::ParquetFileReader& file,
        int rowGroup,
        std::span<const int> columns,
        std::vector<TRowRange> ranges,
        arrow::MemoryPool* memoryPool = nullptr);
    ~TParquetRangeReader();

    TParquetRangeReader(const TParquetRangeReader&) = delete;
    TParquetRangeReader& operator=(const TParquetRangeReader&) = delete;

    // At most `rows` next rows, all from one range; nullopt after the last.
    arrow::Result<std::optional<TRowRangeBatch>> Next(int64_t rows);

private:
    struct TImpl;
    explicit TParquetRangeReader(std::unique_ptr<TImpl> impl);

    std::unique_ptr<TImpl> Impl_;
};

} // namespace NQdb
