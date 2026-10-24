#pragma once

#include <qdb/io/io.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace arrow {
class Field;
namespace io { class RandomAccessFile; }
} // namespace arrow

namespace parquet { class FileMetaData; }

namespace NQdb {

struct TKernelSlot;

// Entry points of the parquet decoder kernel (qdb/kernel/parquet/decode.oz),
// in slot order.
inline constexpr std::array<const char*, 3> ParquetDecoderEntrypoints = {
    "pq_begin_chunk",
    "pq_decode",
    "pq_release",
};

// Codec state for the decoder kernel's one host primitive,
// qdb_parquet_decompress. One instance per scan, so a decompression context
// is reused across pages without thread-local state.
class TParquetCodecs {
public:
    TParquetCodecs();
    ~TParquetCodecs();
    TParquetCodecs(const TParquetCodecs&) = delete;
    TParquetCodecs& operator=(const TParquetCodecs&) = delete;

    // Decompresses src into exactly dstSize bytes of dst; codec uses the
    // parquet.thrift CompressionCodec numbering. Returns the number of bytes
    // produced, or -1.
    int64_t Decompress(
        int64_t codec, const uint8_t* src, int64_t srcSize,
        uint8_t* dst, int64_t dstSize);

private:
    struct TImpl;
    std::unique_ptr<TImpl> Impl_;
};

// The parquet file a kernel scan reads: its footer and a positional reader.
struct TParquetScanFile {
    std::shared_ptr<arrow::io::RandomAccessFile> Input;
    std::shared_ptr<parquet::FileMetaData> Metadata;
    // Arrow fields of the file schema, one per top-level column.
    std::vector<std::shared_ptr<arrow::Field>> Fields;
};

// Reads row groups of a parquet file into TRowSets: the host reads column
// chunks and owns buffers, the decoder kernel decodes pages straight into the
// TColumn buffers of a batch. Batches never span row groups.
class TParquetKernelScan {
public:
    // Why the kernel cannot decode these fields of these row groups, or
    // nullopt when it can. Fields index TParquetScanFile::Fields.
    static std::optional<std::string> Unsupported(
        const TParquetScanFile& file,
        std::span<const int> rowGroups,
        std::span<const int> fields);

    TParquetKernelScan(
        std::shared_ptr<const TParquetScanFile> file,
        std::vector<int> rowGroups,
        std::vector<int> fields,
        bool rowIds,
        std::shared_ptr<TKernelSlot> decoder,
        int64_t batchRows);
    ~TParquetKernelScan();

    bool Next(TRowSet& rowSet);

private:
    struct TColumnReader;
    class TBatchPool;
    struct TPooledBatch;

    void BeginRowGroup();
    static void DestroyBatch(TRowSet* rowSet);

    std::shared_ptr<const TParquetScanFile> File_;
    std::vector<int> RowGroups_;
    bool RowIds_;
    std::shared_ptr<TKernelSlot> Decoder_;
    int64_t BatchRows_;
    std::unique_ptr<TParquetCodecs> Codecs_;
    std::vector<std::unique_ptr<TColumnReader>> Columns_;
    std::shared_ptr<TBatchPool> Pool_;
    size_t RowGroupIndex_ = 0;
    int64_t RowGroupRows_ = 0;
    int64_t RowGroupOffset_ = 0;
    bool InRowGroup_ = false;
};

} // namespace NQdb
