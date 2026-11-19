#pragma once

#include <qdb/plan/types/decimal.h>

#include <arrow/api.h>
#include <parquet/file_reader.h>

#include <memory>

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
        arrow::MemoryPool* memoryPool);
    ~TDecimalColumnScan();

    // Whether the scan reads this column: a flat decimal of a physical type it decodes.
    static bool Supports(const parquet::ColumnDescriptor& descriptor);

    // The Arrow type of the arrays Read returns: Decimal64 or Decimal128 by storage.
    const std::shared_ptr<arrow::DataType>& Type() const;

    // Rows [firstRow, firstRow + rows) of a row group. Reads move forward: a
    // read starts at or after the end of the previous one in the same group.
    std::shared_ptr<arrow::Array> Read(int rowGroup, int64_t firstRow, int64_t rows);

private:
    void Seek(int rowGroup, int64_t row);

    parquet::ParquetFileReader& File_;
    const int Column_;
    arrow::MemoryPool* MemoryPool_;
    const parquet::ColumnDescriptor* Descriptor_;
    TDecimalSpec Spec_;
    std::shared_ptr<arrow::DataType> Type_;
    // The reader of the current row group and the row it reads next.
    std::shared_ptr<parquet::ColumnReader> Reader_;
    int RowGroup_ = -1;
    int64_t Position_ = 0;
};

} // namespace NQdb
