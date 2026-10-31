#pragma once

#include <arrow/api.h>
#include <parquet/file_reader.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace NQdb {

class IColumnValues;

// One flat column of a scan read with the Parquet column reader. A read can keep only the rows
// a selection marks: pages without them are not decompressed, and the other rows hold zero or
// an empty string. Decimals come in their storage width, strings with 64-bit offsets.
class TParquetColumnScan {
public:
    TParquetColumnScan(
        parquet::ParquetFileReader& file,
        int column,
        const std::shared_ptr<arrow::DataType>& type,
        std::vector<int> rowGroups,
        arrow::MemoryPool* memoryPool);
    ~TParquetColumnScan();

    // Whether the scan reads this flat column as `type`, the Arrow type of its field.
    static bool Supports(
        const parquet::ColumnDescriptor& descriptor,
        const std::shared_ptr<arrow::DataType>& type);

    // The type of the arrays Read returns.
    const std::shared_ptr<arrow::DataType>& Type() const;

    // The next `rows` values; with a selection, only of the rows where it is not zero.
    std::shared_ptr<arrow::Array> Read(int64_t rows, const uint8_t* selection = nullptr);

    // Moves past the next `rows` values; the next read skips their pages.
    void Skip(int64_t rows);

private:
    void LoadPage();
    bool SkipPage(int64_t values);

    parquet::ParquetFileReader& File_;
    const int Column_;
    const std::vector<int> RowGroups_;
    size_t NextRowGroup_ = 0;
    arrow::MemoryPool* MemoryPool_;
    const parquet::ColumnDescriptor* Descriptor_;
    std::unique_ptr<IColumnValues> Values_;
    // The reader of the current row group.
    std::shared_ptr<parquet::ColumnReader> Reader_;
    // Rows counted from the first row of the scan: the next row Read returns, the next
    // value of the reader, the end of its page, and the first row of the next page.
    int64_t Position_ = 0;
    int64_t ReaderRow_ = 0;
    int64_t PageEnd_ = 0;
    int64_t NextPage_ = 0;
    // The rows of the read in progress and its selection, for the page filter.
    int64_t ReadBegin_ = 0;
    int64_t ReadEnd_ = 0;
    const uint8_t* Selection_ = nullptr;
};

} // namespace NQdb
