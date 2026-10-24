#pragma once

#include <qdb/io/io.h>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace arrow {
class Buffer;
class DataType;
class MemoryPool;
} // namespace arrow

namespace parquet {
class ColumnDescriptor;
class ColumnReader;
class RowGroupReader;
} // namespace parquet

namespace NQdb {

// Decodes one flat Parquet column chunk batch by batch and materializes only
// the rows a selection keeps. Parquet decoders cannot decode part of a page,
// so values still pass through a small scratch buffer; rejected rows are not
// copied into the batch, which saves string copies and memory traffic. A
// skip still decompresses the pages it passes, but decodes the values of
// none it passes entirely.
class TSelectiveColumnReader {
public:
    // Whether the reader produces exactly the TColumn the Arrow reader would
    // for this column. Other columns stay on the Arrow path.
    static bool Supports(
        const parquet::ColumnDescriptor& descriptor,
        const arrow::DataType& type);

    TSelectiveColumnReader(
        std::shared_ptr<parquet::RowGroupReader> rowGroup,
        int column,
        const arrow::DataType& type,
        arrow::MemoryPool* memoryPool);
    ~TSelectiveColumnReader();

    TSelectiveColumnReader(TSelectiveColumnReader&&) noexcept;
    TSelectiveColumnReader& operator=(TSelectiveColumnReader&&) noexcept;

    // Reads the next rows of the chunk into column, which points into
    // buffers appended to owners.
    void ReadAll(
        int64_t rows,
        TColumn& column,
        std::vector<std::shared_ptr<arrow::Buffer>>& owners);
    // Like ReadAll, but only the kept rows, ascending positions among the
    // next rows, hold their values; the others hold zeros or empty strings.
    void ReadKept(
        int64_t rows,
        std::span<const int32_t> kept,
        TColumn& column,
        std::vector<std::shared_ptr<arrow::Buffer>>& owners);
    void Skip(int64_t rows);

private:
    struct TImpl;
    std::unique_ptr<TImpl> Impl_;
};

} // namespace NQdb
