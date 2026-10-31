#include <qdb/io/parquet/decimal_scan.h>

#include <parquet/column_reader.h>
#include <parquet/level_conversion.h>
#include <parquet/schema.h>

#include <cstring>
#include <stdexcept>
#include <string>

namespace NQdb {

namespace {

// Big-endian two's complement bytes, sign-extended.
template <typename TInt, typename TUInt>
TInt BigEndianUnscaled(const uint8_t* bytes, int32_t length) {
    TUInt value = length > 0 && (bytes[0] & 0x80)
        ? ~TUInt{0}
        : TUInt{0};
    for (int32_t i = 0; i < length; ++i) {
        value = (value << 8) | bytes[i];
    }
    return static_cast<TInt>(value);
}

void StoreUnscaled(uint8_t* data, int64_t row, int32_t width, __int128 value) {
    if (width == 8) {
        const int64_t narrow = static_cast<int64_t>(value);
        std::memcpy(data + row * 8, &narrow, sizeof(narrow));
    } else {
        std::memcpy(data + row * 16, &value, sizeof(value));
    }
}

std::shared_ptr<arrow::Buffer> Allocate(int64_t bytes, arrow::MemoryPool* memoryPool) {
    auto allocated = arrow::AllocateBuffer(bytes, memoryPool);
    if (!allocated.ok()) {
        throw std::runtime_error(allocated.status().ToString());
    }
    return std::move(*allocated);
}

std::shared_ptr<arrow::DataType> ReaderType(const parquet::ColumnDescriptor& descriptor) {
    switch (descriptor.physical_type()) {
        case parquet::Type::FIXED_LEN_BYTE_ARRAY:
            return arrow::fixed_size_binary(descriptor.type_length());
        case parquet::Type::BYTE_ARRAY:
            return arrow::binary();
        default:
            return nullptr;
    }
}

} // namespace

TDecimalColumnScan::TDecimalColumnScan(
    parquet::ParquetFileReader& file,
    int column,
    std::vector<int> rowGroups,
    arrow::MemoryPool* memoryPool)
    : File_(file)
    , Column_(column)
    , RowGroups_(std::move(rowGroups))
    , MemoryPool_(memoryPool)
{
    const auto* descriptor = File_.metadata()->schema()->Column(Column_);
    Spec_ = {.Precision = descriptor->type_precision(), .Scale = descriptor->type_scale()};
    Type_ = DecimalStorageOf(Spec_) == EDecimalStorage::I64
        ? arrow::decimal64(Spec_.Precision, Spec_.Scale)
        : arrow::decimal128(Spec_.Precision, Spec_.Scale);
    Reader_ = parquet::internal::RecordReader::Make(
        descriptor,
        parquet::internal::LevelInfo::ComputeLevelInfo(descriptor),
        MemoryPool_,
        false,
        false,
        ReaderType(*descriptor));
}

TDecimalColumnScan::~TDecimalColumnScan() = default;

bool TDecimalColumnScan::Supports(const parquet::ColumnDescriptor& descriptor) {
    if (descriptor.max_repetition_level() != 0 ||
        descriptor.converted_type() != parquet::ConvertedType::DECIMAL ||
        descriptor.type_precision() <= 0 ||
        descriptor.type_precision() > MaxDecimalPrecision)
    {
        return false;
    }
    switch (descriptor.physical_type()) {
        case parquet::Type::INT32:
        case parquet::Type::INT64:
        case parquet::Type::BYTE_ARRAY:
            return true;
        case parquet::Type::FIXED_LEN_BYTE_ARRAY:
            return descriptor.type_length() <= 16;
        default:
            return false;
    }
}

const std::shared_ptr<arrow::DataType>& TDecimalColumnScan::Type() const {
    return Type_;
}

std::shared_ptr<arrow::Array> TDecimalColumnScan::Read(int64_t rows) {
    Reader_->Reset();
    Reader_->Reserve(rows);
    int64_t read = 0;
    bool started = NextRowGroup_ > 0;
    while (read < rows) {
        // A row group is done when the reader runs out of data or reads no record.
        const int64_t records = started && Reader_->HasMoreData()
            ? Reader_->ReadRecords(rows - read)
            : 0;
        if (records > 0) {
            read += records;
            continue;
        }
        if (NextRowGroup_ == RowGroups_.size()) {
            throw std::runtime_error("parquet decimal scan ran past its row groups");
        }
        auto rowGroup = File_.RowGroup(RowGroups_[NextRowGroup_++]);
        Reader_->SetPageReader(rowGroup->GetColumnPageReader(Column_));
        started = true;
    }

    const int32_t width = DecimalStorageBytes(Spec_);
    const auto physical = Reader_->descr()->physical_type();
    std::shared_ptr<arrow::Buffer> validity;
    std::shared_ptr<arrow::Buffer> values;
    int64_t nullCount = 0;
    if (physical == parquet::Type::INT32 || physical == parquet::Type::INT64) {
        nullCount = Reader_->null_count();
        auto decoded = Reader_->ReleaseValues();
        auto isValid = Reader_->ReleaseIsValid();
        if (nullCount > 0) {
            validity = std::move(isValid);
        }
        if (physical == parquet::Type::INT64 && width == 8) {
            values = std::move(decoded);
        } else {
            values = Allocate(rows * width, MemoryPool_);
            for (int64_t row = 0; row < rows; ++row) {
                const __int128 value = physical == parquet::Type::INT64
                    ? static_cast<__int128>(reinterpret_cast<const int64_t*>(decoded->data())[row])
                    : static_cast<__int128>(reinterpret_cast<const int32_t*>(decoded->data())[row]);
                StoreUnscaled(values->mutable_data(), row, width, value);
            }
        }
    } else {
        auto* binary = dynamic_cast<parquet::internal::BinaryRecordReader*>(Reader_.get());
        if (!binary) {
            throw std::runtime_error("parquet decimal scan: binary record reader is unavailable");
        }
        values = Allocate(rows * width, MemoryPool_);
        std::shared_ptr<arrow::Buffer> bitmap;
        int64_t row = 0;
        for (const auto& chunk : binary->GetBuilderChunks()) {
            for (int64_t i = 0; i < chunk->length(); ++i, ++row) {
                if (chunk->IsNull(i)) {
                    if (!bitmap) {
                        bitmap = Allocate(arrow::bit_util::BytesForBits(rows), MemoryPool_);
                        std::memset(bitmap->mutable_data(), 0xff, bitmap->size());
                    }
                    arrow::bit_util::ClearBit(bitmap->mutable_data(), row);
                    StoreUnscaled(values->mutable_data(), row, width, 0);
                    ++nullCount;
                    continue;
                }
                std::string_view bytes = physical == parquet::Type::FIXED_LEN_BYTE_ARRAY
                    ? static_cast<const arrow::FixedSizeBinaryArray&>(*chunk).GetView(i)
                    : static_cast<const arrow::BinaryArray&>(*chunk).GetView(i);
                const auto* data = reinterpret_cast<const uint8_t*>(bytes.data());
                const auto length = static_cast<int32_t>(bytes.size());
                const __int128 value = length <= 8
                    ? static_cast<__int128>(BigEndianUnscaled<int64_t, uint64_t>(data, length))
                    : BigEndianUnscaled<__int128, unsigned __int128>(data, length);
                StoreUnscaled(values->mutable_data(), row, width, value);
            }
        }
        if (row != rows) {
            throw std::runtime_error("parquet decimal scan: decoded value count mismatch");
        }
        validity = std::move(bitmap);
    }
    return arrow::MakeArray(arrow::ArrayData::Make(
        Type_, rows, {std::move(validity), std::move(values)}, nullCount));
}

} // namespace NQdb
