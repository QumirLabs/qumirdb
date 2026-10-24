#include <qdb/io/parquet/kernel_scan.h>
#include <qdb/io/parquet/row_id.h>
#include <qdb/kernel/generated.h>
#include <qdb/modules/qumirdb_runtime.h>

#include <arrow/io/interfaces.h>
#include <arrow/type.h>
#include <arrow/util/compression.h>
#include <parquet/metadata.h>
#include <parquet/schema.h>
#include <parquet/types.h>

#include <snappy.h>
#include <zstd.h>

#include <algorithm>
#include <expected>
#include <mutex>
#include <stdexcept>

namespace NQdb {

namespace {

// Mirrors of the decoder kernel's structs (decode.oz); every field is eight
// bytes, so the layouts match without padding.
struct TParquetBytes {
    const uint8_t* Base;
    int64_t Pos;
    int64_t End;
    int64_t Error;
};

struct TParquetRle {
    const uint8_t* Base;
    int64_t Pos;
    int64_t End;
    int64_t Width;
    int64_t Left;
    int64_t Literal;
    int64_t Value;
    int64_t Bit;
};

struct TParquetColumnState {
    const uint8_t* Chunk;
    int64_t ChunkSize;
    int64_t Codec;
    TParquetCodecs* Codecs;
    int64_t Kind;
    int64_t Width;
    int64_t MaxDef;
    int64_t Pos;
    int64_t PageLeft;
    int64_t Encoding;
    TParquetBytes Plain;
    int64_t PlainBit;
    TParquetRle Levels;
    TParquetRle Indices;
    const uint8_t* Dict;
    int64_t DictCount;
    int64_t DictMaxLen;
    uint8_t* PageBuf;
    int64_t PageCap;
    uint8_t* DictBuf;
    int64_t DictCap;
    uint64_t* EntryBuf;
    int64_t EntryCap;
    uint32_t* LevelBuf;
    int64_t LevelCap;
    uint32_t* IndexBuf;
    int64_t IndexCap;
};

struct TParquetOutput {
    uint8_t* Data;
    int64_t DataCap;
    int64_t DataSize;
    int32_t* Offsets;
    uint8_t* Mask;
    int64_t NullCount;
    int64_t Row;
};

static_assert(sizeof(TParquetBytes) == 4 * 8);
static_assert(sizeof(TParquetRle) == 8 * 8);
static_assert(sizeof(TParquetColumnState) == 44 * 8);
static_assert(sizeof(TParquetOutput) == 7 * 8);

// Value representation the kernel writes; matches PqColumn.Kind.
enum class EKind : int64_t {
    Fixed = 1,
    Boolean = 2,
    ByteArray = 3,
};

struct TColumnLayout {
    EKind Kind;
    int64_t Width = 0;
    int64_t MaxDef = 0;
};

// Parallel to ParquetDecoderEntrypoints.
enum EEntry : size_t { EntryBeginChunk, EntryDecode, EntryRelease };

using TBeginChunk = int64_t (*)(TParquetColumnState*);
using TDecode = int64_t (*)(TParquetColumnState*, TParquetOutput*, int64_t);
using TRelease = int64_t (*)(TParquetColumnState*);

// Kernel buffers are read in whole 8- or 16-byte words past their end.
constexpr int64_t Padding = 64;

// parquet.thrift CompressionCodec.
constexpr int64_t CodecUncompressed = 0;
constexpr int64_t CodecSnappy = 1;
constexpr int64_t CodecZstd = 6;

std::optional<int64_t> ThriftCodec(arrow::Compression::type codec) {
    switch (codec) {
        case arrow::Compression::UNCOMPRESSED: return CodecUncompressed;
        case arrow::Compression::SNAPPY: return CodecSnappy;
        case arrow::Compression::ZSTD: return CodecZstd;
        default: return std::nullopt;
    }
}

const char* DecoderError(int64_t status) {
    switch (status) {
        case -1: return "corrupt page data";
        case -2: return "unsupported page type or encoding";
        case -3: return "decompression failed";
        case -4: return "dictionary index out of range";
        case -5: return "column chunk ended before its rows";
        case -6: return "allocation failed";
        case -7: return "string data of a batch exceeds 2 GiB";
        default: return "unknown error";
    }
}

std::expected<TColumnLayout, std::string> KernelLayout(
    const parquet::ColumnDescriptor& column,
    const arrow::Field& field)
{
    if (column.max_repetition_level() != 0 || column.path()->ToDotVector().size() != 1) {
        return std::unexpected("nested column " + column.path()->ToDotString());
    }
    const auto type = field.type()->id();
    const int64_t maxDef = column.max_definition_level();
    auto layout = [&](EKind kind, int64_t width) {
        return TColumnLayout{.Kind = kind, .Width = width, .MaxDef = maxDef};
    };
    switch (column.physical_type()) {
        case parquet::Type::INT32:
            if (type == arrow::Type::INT32 || type == arrow::Type::DATE32
                || type == arrow::Type::UINT32)
            {
                return layout(EKind::Fixed, 4);
            }
            break;
        case parquet::Type::INT64:
            if (type == arrow::Type::INT64 || type == arrow::Type::UINT64) {
                return layout(EKind::Fixed, 8);
            }
            break;
        case parquet::Type::DOUBLE:
            if (type == arrow::Type::DOUBLE) {
                return layout(EKind::Fixed, 8);
            }
            break;
        case parquet::Type::BOOLEAN:
            if (type == arrow::Type::BOOL) {
                return layout(EKind::Boolean, 0);
            }
            break;
        case parquet::Type::BYTE_ARRAY:
            if (type == arrow::Type::STRING) {
                return layout(EKind::ByteArray, 0);
            }
            break;
        default:
            break;
    }
    return std::unexpected("column " + field.name() + " of physical type "
        + parquet::TypeToString(column.physical_type()) + " read as "
        + field.type()->ToString());
}

bool KernelEncoding(parquet::Encoding::type encoding) {
    switch (encoding) {
        case parquet::Encoding::PLAIN:
        case parquet::Encoding::PLAIN_DICTIONARY:
        case parquet::Encoding::RLE:
        case parquet::Encoding::RLE_DICTIONARY:
            return true;
        default:
            return false;
    }
}

// Byte range of a column chunk: its first page header to the chunk end.
std::pair<int64_t, int64_t> ChunkRange(
    const parquet::FileMetaData& metadata,
    const parquet::ColumnChunkMetaData& chunk,
    int64_t fileSize)
{
    int64_t start = chunk.data_page_offset();
    if (chunk.has_dictionary_page() && chunk.dictionary_page_offset() > 0
        && chunk.dictionary_page_offset() < start)
    {
        start = chunk.dictionary_page_offset();
    }
    int64_t length = chunk.total_compressed_size();
    // PARQUET-816: old parquet-mr wrote a total size that misses up to 100
    // bytes of dictionary page header; read the slack as Arrow does.
    if (metadata.writer_version().VersionLt(
            parquet::ApplicationVersion::PARQUET_816_FIXED_VERSION()))
    {
        length += std::clamp<int64_t>(fileSize - (start + length), 0, 100);
    }
    return {start, length};
}

} // namespace

struct TParquetCodecs::TImpl {
    ZSTD_DCtx* Zstd = nullptr;
};

TParquetCodecs::TParquetCodecs()
    : Impl_(std::make_unique<TImpl>())
{}

TParquetCodecs::~TParquetCodecs() {
    if (Impl_->Zstd) {
        ZSTD_freeDCtx(Impl_->Zstd);
    }
}

int64_t TParquetCodecs::Decompress(
    int64_t codec, const uint8_t* src, int64_t srcSize,
    uint8_t* dst, int64_t dstSize)
{
    if (srcSize < 0 || dstSize < 0) {
        return -1;
    }
    switch (codec) {
        case CodecZstd: {
            if (!Impl_->Zstd && !(Impl_->Zstd = ZSTD_createDCtx())) {
                return -1;
            }
            const size_t produced = ZSTD_decompressDCtx(Impl_->Zstd,
                dst, static_cast<size_t>(dstSize), src, static_cast<size_t>(srcSize));
            return ZSTD_isError(produced) ? -1 : static_cast<int64_t>(produced);
        }
        case CodecSnappy: {
            const auto* input = reinterpret_cast<const char*>(src);
            size_t length = 0;
            if (!snappy::GetUncompressedLength(input, static_cast<size_t>(srcSize), &length)
                || length != static_cast<size_t>(dstSize)
                || !snappy::RawUncompress(input, static_cast<size_t>(srcSize),
                    reinterpret_cast<char*>(dst)))
            {
                return -1;
            }
            return dstSize;
        }
        default:
            return -1;
    }
}

struct TParquetKernelScan::TColumnReader {
    int Field = 0;
    TColumnLayout Layout;
    std::string Name;
    std::vector<uint8_t> Chunk;
    // Expected bytes per string value, for the first capacity of a batch.
    int64_t ValueBytes = 0;
    TParquetColumnState State{};
};

namespace {

// Storage of one output column; all buffers come from qdb_alloc because the
// kernel replaces string data it outgrows with a larger qdb_alloc buffer.
struct TColumnBuffers {
    uint8_t* Data = nullptr;
    int64_t DataCap = 0;
    int32_t* Offsets = nullptr;
    int64_t OffsetsCap = 0;
    uint8_t* Mask = nullptr;
    int64_t MaskCap = 0;
};

struct TBatchBuffers {
    std::vector<TColumnBuffers> Columns;
    std::vector<TColumn> View;
};

void FreeBuffers(TBatchBuffers& batch) {
    for (auto& column : batch.Columns) {
        qdb_free(column.Data);
        qdb_free(column.Offsets);
        qdb_free(column.Mask);
    }
}

// Reserves bytes without keeping the old contents.
template <typename T>
void Reserve(T*& buffer, int64_t& capacity, int64_t bytes) {
    if (capacity >= bytes) {
        return;
    }
    qdb_free(buffer);
    buffer = static_cast<T*>(qdb_alloc(bytes + Padding));
    if (!buffer) {
        capacity = 0;
        throw std::bad_alloc();
    }
    capacity = bytes;
}

} // namespace

// Recycles batch storage: a batch returns its buffers when its last reference
// is released, possibly on another worker, so the next batch writes into
// memory that is already mapped instead of faulting in fresh pages.
class TParquetKernelScan::TBatchPool {
public:
    ~TBatchPool() {
        for (auto& batch : Free_) {
            FreeBuffers(*batch);
        }
    }

    std::unique_ptr<TBatchBuffers> Take(size_t columns) {
        {
            std::lock_guard lock(Mutex_);
            if (!Free_.empty()) {
                auto batch = std::move(Free_.back());
                Free_.pop_back();
                return batch;
            }
        }
        auto batch = std::make_unique<TBatchBuffers>();
        batch->Columns.resize(columns);
        batch->View.resize(columns);
        return batch;
    }

    void Put(std::unique_ptr<TBatchBuffers> batch) {
        {
            std::lock_guard lock(Mutex_);
            if (Free_.size() < MaxFree) {
                Free_.push_back(std::move(batch));
                return;
            }
        }
        FreeBuffers(*batch);
    }

private:
    // A streaming consumer returns a batch about when the scan needs the next
    // one, so the free list rarely holds more than one set; keeping more would
    // only hold memory, most of all for large batches.
    static constexpr size_t MaxFree = 2;

    std::mutex Mutex_;
    std::vector<std::unique_ptr<TBatchBuffers>> Free_;
};

struct TParquetKernelScan::TPooledBatch {
    std::shared_ptr<TBatchPool> Pool;
    std::unique_ptr<TBatchBuffers> Buffers;
};

void TParquetKernelScan::DestroyBatch(TRowSet* rowSet) {
    auto* batch = static_cast<TPooledBatch*>(rowSet->Private);
    batch->Pool->Put(std::move(batch->Buffers));
    delete batch;
}

std::optional<std::string> TParquetKernelScan::Unsupported(
    const TParquetScanFile& file,
    std::span<const int> rowGroups,
    std::span<const int> fields)
{
    const auto& metadata = *file.Metadata;
    if (metadata.is_encryption_algorithm_set()) {
        return "encrypted file";
    }
    if (metadata.num_columns() != static_cast<int>(file.Fields.size())) {
        return "nested schema";
    }
    for (int field : fields) {
        const auto* column = metadata.schema()->Column(field);
        if (column->name() != file.Fields[static_cast<size_t>(field)]->name()) {
            return "nested schema";
        }
        if (auto layout = KernelLayout(*column, *file.Fields[static_cast<size_t>(field)]);
            !layout)
        {
            return layout.error();
        }
        for (int rowGroup : rowGroups) {
            const auto chunk = metadata.RowGroup(rowGroup)->ColumnChunk(field);
            if (!ThriftCodec(chunk->compression())) {
                return "column " + column->name() + " compressed with "
                    + arrow::util::Codec::GetCodecAsString(chunk->compression());
            }
            if (chunk->crypto_metadata()) {
                return "encrypted column " + column->name();
            }
            for (auto encoding : chunk->encodings()) {
                if (!KernelEncoding(encoding)) {
                    return "column " + column->name() + " encoded with "
                        + parquet::EncodingToString(encoding);
                }
            }
        }
    }
    return std::nullopt;
}

TParquetKernelScan::TParquetKernelScan(
    std::shared_ptr<const TParquetScanFile> file,
    std::vector<int> rowGroups,
    std::vector<int> fields,
    bool rowIds,
    std::shared_ptr<TKernelSlot> decoder,
    int64_t batchRows)
    : File_(std::move(file))
    , RowGroups_(std::move(rowGroups))
    , RowIds_(rowIds)
    , Decoder_(std::move(decoder))
    , BatchRows_(batchRows)
    , Codecs_(std::make_unique<TParquetCodecs>())
    , Pool_(std::make_shared<TBatchPool>())
{
    if (!Decoder_ || Decoder_->Fns.size() != ParquetDecoderEntrypoints.size()) {
        throw std::invalid_argument("parquet kernel scan requires the decoder kernel");
    }
    if (BatchRows_ <= 0) {
        throw std::invalid_argument("parquet kernel scan batch size must be positive");
    }
    Columns_.reserve(fields.size());
    for (int field : fields) {
        auto column = std::make_unique<TColumnReader>();
        const auto* descriptor = File_->Metadata->schema()->Column(field);
        auto layout = KernelLayout(*descriptor, *File_->Fields[static_cast<size_t>(field)]);
        if (!layout) {
            throw std::invalid_argument("parquet kernel scan: " + layout.error());
        }
        column->Field = field;
        column->Layout = *layout;
        column->Name = descriptor->name();
        column->State.Codecs = Codecs_.get();
        column->State.Kind = static_cast<int64_t>(layout->Kind);
        column->State.Width = layout->Width;
        column->State.MaxDef = layout->MaxDef;
        Columns_.push_back(std::move(column));
    }
}

TParquetKernelScan::~TParquetKernelScan() {
    // A scan is created while the query is lowered and its kernel bound
    // later; if the query never ran, the decoder owns no buffers.
    auto release = reinterpret_cast<TRelease>(Decoder_->Fns[EntryRelease]);
    if (!release) {
        return;
    }
    for (auto& column : Columns_) {
        release(&column->State);
    }
}

void TParquetKernelScan::BeginRowGroup() {
    const int rowGroup = RowGroups_[RowGroupIndex_];
    const auto& metadata = *File_->Metadata;
    const auto group = metadata.RowGroup(rowGroup);
    RowGroupRows_ = group->num_rows();
    RowGroupOffset_ = 0;
    InRowGroup_ = true;
    if (RowGroupRows_ == 0) {
        return;
    }
    auto fileSize = File_->Input->GetSize();
    if (!fileSize.ok()) {
        throw std::runtime_error(fileSize.status().ToString());
    }
    auto begin = reinterpret_cast<TBeginChunk>(Decoder_->Fns[EntryBeginChunk]);
    for (auto& column : Columns_) {
        const auto chunk = group->ColumnChunk(column->Field);
        const auto [start, length] = ChunkRange(metadata, *chunk, *fileSize);
        if (start < 0 || length < 0 || start + length > *fileSize) {
            throw std::runtime_error("parquet column chunk " + column->Name
                + " lies outside the file");
        }
        // Grow only, so a smaller chunk does not refill the tail.
        if (static_cast<int64_t>(column->Chunk.size()) < length + Padding) {
            column->Chunk.resize(static_cast<size_t>(length + Padding));
        }
        auto read = File_->Input->ReadAt(start, length, column->Chunk.data());
        if (!read.ok()) {
            throw std::runtime_error(read.status().ToString());
        }
        if (*read != length) {
            throw std::runtime_error("short read of parquet column chunk " + column->Name);
        }
        auto& state = column->State;
        state.Chunk = column->Chunk.data();
        state.ChunkSize = length;
        state.Codec = *ThriftCodec(chunk->compression());
        begin(&state);
        if (column->Layout.Kind == EKind::ByteArray && chunk->num_values() > 0) {
            column->ValueBytes = std::max<int64_t>(1,
                chunk->total_uncompressed_size() / chunk->num_values());
        }
    }
}

bool TParquetKernelScan::Next(TRowSet& rowSet) {
    if (!Decoder_->Fns[EntryDecode]) {
        throw std::logic_error("parquet kernel scan: the decoder kernel is not bound");
    }
    while (!InRowGroup_ || RowGroupOffset_ == RowGroupRows_) {
        if (InRowGroup_) {
            ++RowGroupIndex_;
            InRowGroup_ = false;
        }
        if (RowGroupIndex_ >= RowGroups_.size()) {
            return false;
        }
        BeginRowGroup();
    }
    const int64_t rows = std::min(BatchRows_, RowGroupRows_ - RowGroupOffset_);
    const size_t width = Columns_.size() + (RowIds_ ? 1 : 0);
    auto buffers = Pool_->Take(width);
    auto decode = reinterpret_cast<TDecode>(Decoder_->Fns[EntryDecode]);

    for (size_t c = 0; c < Columns_.size(); ++c) {
        auto& column = *Columns_[c];
        auto& storage = buffers->Columns[c];
        auto& view = buffers->View[c];
        view = TColumn{
            .Data = nullptr,
            .DataBitOffset = 0,
            .Mask = nullptr,
            .MaskBitOffset = 0,
            .Offsets = nullptr,
            .OffsetWidth = 0,
        };
        TParquetOutput output{};
        switch (column.Layout.Kind) {
            case EKind::Fixed:
                Reserve(storage.Data, storage.DataCap, rows * column.Layout.Width);
                break;
            case EKind::Boolean:
                Reserve(storage.Data, storage.DataCap, (rows + 7) / 8);
                break;
            case EKind::ByteArray:
                Reserve(storage.Data, storage.DataCap, rows * column.ValueBytes);
                Reserve(storage.Offsets, storage.OffsetsCap,
                    (rows + 1) * static_cast<int64_t>(sizeof(int32_t)));
                output.Offsets = storage.Offsets;
                break;
        }
        if (column.Layout.MaxDef > 0) {
            Reserve(storage.Mask, storage.MaskCap, (rows + 7) / 8);
            output.Mask = storage.Mask;
        }
        output.Data = storage.Data;
        output.DataCap = storage.DataCap;
        const int64_t status = decode(&column.State, &output, rows);
        // The kernel may have moved string data even when it then failed.
        storage.Data = output.Data;
        storage.DataCap = output.DataCap;
        if (status != 0) {
            throw std::runtime_error("parquet decoder: column " + column.Name
                + ", row group " + std::to_string(RowGroups_[RowGroupIndex_]) + ": "
                + DecoderError(status));
        }
        view.Data = reinterpret_cast<char*>(storage.Data);
        if (output.NullCount > 0) {
            view.Mask = storage.Mask;
        }
        if (column.Layout.Kind == EKind::ByteArray) {
            view.Offsets = storage.Offsets;
            view.OffsetWidth = 4;
            // Keep the next batch from growing in small steps.
            column.ValueBytes = std::max(column.ValueBytes,
                (output.DataSize + rows - 1) / rows);
        }
    }

    if (RowIds_) {
        auto& storage = buffers->Columns.back();
        Reserve(storage.Data, storage.DataCap, rows * static_cast<int64_t>(sizeof(uint64_t)));
        auto* ids = reinterpret_cast<uint64_t*>(storage.Data);
        const auto rowGroup = static_cast<uint32_t>(RowGroups_[RowGroupIndex_]);
        for (int64_t i = 0; i < rows; ++i) {
            // TParquetFileData checks that row-group ids and offsets fit.
            ids[i] = MakeParquetRowIdUnchecked(
                rowGroup, static_cast<uint32_t>(RowGroupOffset_ + i));
        }
        buffers->View.back() = TColumn{
            .Data = reinterpret_cast<char*>(storage.Data),
            .DataBitOffset = 0,
            .Mask = nullptr,
            .MaskBitOffset = 0,
            .Offsets = nullptr,
            .OffsetWidth = 0,
        };
    }
    RowGroupOffset_ += rows;

    auto* batch = new TPooledBatch{
        .Pool = Pool_,
        .Buffers = std::move(buffers),
    };
    rowSet = {
        .Columns = batch->Buffers->View.data(),
        .ColumnCount = static_cast<int64_t>(width),
        .RowCount = rows,
        .Selection = nullptr,
        .Hash = nullptr,
        .Destroy = DestroyBatch,
        .Private = batch,
        .RefCount = 1,
    };
    return true;
}

} // namespace NQdb

extern "C" int64_t qdb_parquet_decompress(
    void* codecs, int64_t codec, const uint8_t* src, int64_t srcSize,
    uint8_t* dst, int64_t dstSize)
{
    return static_cast<NQdb::TParquetCodecs*>(codecs)->Decompress(
        codec, src, srcSize, dst, dstSize);
}
