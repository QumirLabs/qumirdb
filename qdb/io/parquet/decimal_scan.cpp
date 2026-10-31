#include <qdb/io/parquet/decimal_scan.h>

#include <parquet/column_reader.h>
#include <parquet/schema.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace NQdb {

namespace {

// Values per ReadBatch: the levels and raw values of a chunk stay in cache until stored.
constexpr int64_t ChunkRows = 4096;

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

// The same for a length known at compile time: whole loads and a byte swap.
template <int Length>
auto BigEndianUnscaled(const uint8_t* bytes) {
    if constexpr (Length <= 8) {
        uint64_t raw = 0;
        std::memcpy(&raw, bytes, Length);
        return static_cast<int64_t>(std::byteswap(raw)) >> (64 - 8 * Length);
    } else {
        uint64_t low;
        std::memcpy(&low, bytes + Length - 8, sizeof(low));
        const auto high = static_cast<unsigned __int128>(BigEndianUnscaled<Length - 8>(bytes));
        return static_cast<__int128>(high << 64 | std::byteswap(low));
    }
}

// Calls `func` with std::integral_constant<int, length> for a FIXED_LEN_BYTE_ARRAY length.
// ReadRows is instantiated per length (32 instantiations) so the byte swap has a
// compile-time size; accepted for the measured gain over a runtime-length loop.
template <int Length = 1, typename TFunc>
void WithLength(int length, TFunc&& func) {
    if constexpr (Length > 16) {
        throw std::runtime_error("parquet decimal scan: unsupported FIXED_LEN_BYTE_ARRAY length");
    } else if (length == Length) {
        func(std::integral_constant<int, Length>{});
    } else {
        WithLength<Length + 1>(length, std::forward<TFunc>(func));
    }
}

std::shared_ptr<arrow::Buffer> Allocate(int64_t bytes, arrow::MemoryPool* memoryPool) {
    auto allocated = arrow::AllocateBuffer(bytes, memoryPool);
    if (!allocated.ok()) {
        throw std::runtime_error(allocated.status().ToString());
    }
    return std::move(*allocated);
}

// The validity bitmap of one Read, allocated at its first NULL.
struct TNulls {
    void Mark(int64_t row, const int16_t* defLevels, int16_t maxDef, int64_t levels) {
        if (!Bitmap) {
            Bitmap = Allocate(arrow::bit_util::BytesForBits(Rows), MemoryPool);
            std::memset(Bitmap->mutable_data(), 0xff, Bitmap->size());
        }
        for (int64_t i = 0; i < levels; ++i) {
            if (defLevels[i] != maxDef) {
                arrow::bit_util::ClearBit(Bitmap->mutable_data(), row + i);
                ++Count;
            }
        }
    }

    const int64_t Rows;
    arrow::MemoryPool* const MemoryPool;
    std::shared_ptr<arrow::Buffer> Bitmap;
    int64_t Count = 0;
};

// Reads `rows` values into `out`, decoding each raw value with `decode`; a NULL row stores zero.
// `reader` returns the column reader of the current row group with values left.
template <typename TDType, typename TStorage, typename TReader, typename TDecode>
void ReadRows(
    int64_t rows,
    int16_t maxDef,
    TReader&& reader,
    TDecode decode,
    TStorage* out,
    TNulls& nulls)
{
    using TValue = typename TDType::c_type;
    // INT64 at i64 storage: the page decoder writes straight into the output.
    constexpr bool direct = std::is_same_v<TValue, TStorage>;
    std::vector<TValue> chunk(direct ? 0 : ChunkRows);
    std::vector<int16_t> defLevels(maxDef > 0 ? ChunkRows : 0);
    for (int64_t row = 0; row < rows;) {
        auto& typed = static_cast<parquet::TypedColumnReader<TDType>&>(reader());
        TValue* values = chunk.data();
        if constexpr (direct) {
            values = out + row;
        }
        int64_t valuesRead = 0;
        const int64_t levels = typed.ReadBatch(
            std::min(rows - row, ChunkRows),
            maxDef > 0 ? defLevels.data() : nullptr,
            nullptr,
            values,
            &valuesRead);
        TStorage* target = out + row;
        if (valuesRead == levels) {
            if constexpr (!direct) {
                for (int64_t i = 0; i < levels; ++i) {
                    target[i] = static_cast<TStorage>(decode(values[i]));
                }
            }
        } else {
            nulls.Mark(row, defLevels.data(), maxDef, levels);
            // Dense values move to their rows; from the back, so a direct read is spread in place.
            int64_t next = valuesRead;
            for (int64_t i = levels; i-- > 0;) {
                target[i] = defLevels[i] == maxDef
                    ? static_cast<TStorage>(decode(values[--next]))
                    : TStorage{0};
            }
        }
        row += levels;
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
    , Descriptor_(File_.metadata()->schema()->Column(Column_))
{
    Spec_ = {.Precision = Descriptor_->type_precision(), .Scale = Descriptor_->type_scale()};
    Type_ = DecimalStorageOf(Spec_) == EDecimalStorage::I64
        ? arrow::decimal64(Spec_.Precision, Spec_.Scale)
        : arrow::decimal128(Spec_.Precision, Spec_.Scale);
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
    auto reader = [this]() -> parquet::ColumnReader& {
        while (!Reader_ || !Reader_->HasNext()) {
            if (NextRowGroup_ == RowGroups_.size()) {
                throw std::runtime_error("parquet decimal scan ran past its row groups");
            }
            auto rowGroup = File_.RowGroup(RowGroups_[NextRowGroup_++]);
            Reader_ = parquet::ColumnReader::Make(
                Descriptor_, rowGroup->GetColumnPageReader(Column_), MemoryPool_);
        }
        return *Reader_;
    };
    const int16_t maxDef = Descriptor_->max_definition_level();
    auto values = Allocate(rows * DecimalStorageBytes(Spec_), MemoryPool_);
    TNulls nulls{.Rows = rows, .MemoryPool = MemoryPool_};
    auto read = [&]<typename TStorage>(TStorage* out) {
        auto rowsOf = [&]<typename TDType>(TDType, auto decode) {
            ReadRows<TDType>(rows, maxDef, reader, decode, out, nulls);
        };
        switch (Descriptor_->physical_type()) {
            case parquet::Type::INT32:
                rowsOf(parquet::Int32Type{}, [](int32_t value) { return value; });
                break;
            case parquet::Type::INT64:
                rowsOf(parquet::Int64Type{}, [](int64_t value) { return value; });
                break;
            case parquet::Type::FIXED_LEN_BYTE_ARRAY:
                WithLength(Descriptor_->type_length(), [&](auto length) {
                    rowsOf(parquet::FLBAType{}, [](parquet::FixedLenByteArray value) {
                        return BigEndianUnscaled<decltype(length)::value>(value.ptr);
                    });
                });
                break;
            case parquet::Type::BYTE_ARRAY:
                rowsOf(parquet::ByteArrayType{}, [](parquet::ByteArray value) {
                    const auto length = static_cast<int32_t>(value.len);
                    return length <= 8
                        ? static_cast<__int128>(BigEndianUnscaled<int64_t, uint64_t>(value.ptr, length))
                        : BigEndianUnscaled<__int128, unsigned __int128>(value.ptr, length);
                });
                break;
            default:
                throw std::runtime_error("parquet decimal scan: unsupported physical type");
        }
    };
    if (DecimalStorageOf(Spec_) == EDecimalStorage::I64) {
        read(reinterpret_cast<int64_t*>(values->mutable_data()));
    } else {
        read(reinterpret_cast<__int128*>(values->mutable_data()));
    }
    return arrow::MakeArray(arrow::ArrayData::Make(
        Type_, rows, {std::move(nulls.Bitmap), std::move(values)}, nulls.Count));
}

} // namespace NQdb
