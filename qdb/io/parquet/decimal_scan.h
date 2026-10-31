#pragma once

#include <qdb/plan/types/decimal.h>

#include <arrow/api.h>
#include <parquet/file_reader.h>

#include <memory>
#include <vector>

namespace NQdb {

// Reads one flat DECIMAL column of a scan with the Parquet column reader instead of
// Arrow's FileReader, which converts every decimal value to Decimal128 one by one. The
// values come out in the storage of the column's precision (DecimalStorageOf): the page
// decoder writes an INT64 column at i64 storage straight into the output, INT32 is
// widened, and (FIXED_LEN_)BYTE_ARRAY is decoded from the big-endian bytes the page
// decoder points at, in the page or its dictionary, without an Arrow builder between.
class TDecimalColumnScan {
public:
    TDecimalColumnScan(
        parquet::ParquetFileReader& file,
        int column,
        std::vector<int> rowGroups,
        arrow::MemoryPool* memoryPool);
    ~TDecimalColumnScan();

    // Whether the scan reads this column: a flat decimal of a physical type it decodes.
    static bool Supports(const parquet::ColumnDescriptor& descriptor);

    // The Arrow type of the arrays Read returns: Decimal64 or Decimal128 by storage.
    const std::shared_ptr<arrow::DataType>& Type() const;

    // The next `rows` values of the column, continuing across row groups.
    std::shared_ptr<arrow::Array> Read(int64_t rows);

private:
    parquet::ParquetFileReader& File_;
    const int Column_;
    const std::vector<int> RowGroups_;
    size_t NextRowGroup_ = 0;
    arrow::MemoryPool* MemoryPool_;
    const parquet::ColumnDescriptor* Descriptor_;
    TDecimalSpec Spec_;
    std::shared_ptr<arrow::DataType> Type_;
    // The reader of the current row group.
    std::shared_ptr<parquet::ColumnReader> Reader_;
};

} // namespace NQdb
