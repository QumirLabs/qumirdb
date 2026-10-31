#include <qdb/io/parquet/column_scan.h>

#include <qdb/plan/types/decimal.h>

#include <parquet/column_reader.h>
#include <parquet/schema.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace NQdb {

// The values of one read, decoded from the current page of the column reader.
class IColumnValues {
public:
    virtual ~IColumnValues() = default;

    virtual const std::shared_ptr<arrow::DataType>& Type() const = 0;

    // Starts a read of `rows` rows; a sparse read leaves the rows it does not decode zero.
    virtual void Begin(int64_t rows, bool sparse) = 0;

    // Decodes rows [row, row + count) of the read; a selection keeps the strings it marks.
    virtual void Read(
        parquet::ColumnReader& reader,
        int64_t row,
        int64_t count,
        const uint8_t* selection) = 0;

    virtual void Skip(parquet::ColumnReader& reader, int64_t count) = 0;

    virtual std::shared_ptr<arrow::Array> Finish() = 0;
};

namespace {

static_assert(std::endian::native == std::endian::little);

// Values per ReadBatch: the levels and raw values of a chunk stay in cache until stored.
constexpr int64_t ChunkRows = 4096;

// A Skip inside a page decodes into a scratch buffer, so it pays off only for a run
// long enough to outweigh the extra call.
constexpr int64_t MinSkipRows = 64;

// The first row in [row, end) that the selection marks, or `end`.
int64_t NextSelected(const uint8_t* selection, int64_t row, int64_t end) {
    for (; row + 8 <= end; row += 8) {
        uint64_t word;
        std::memcpy(&word, selection + row, sizeof(word));
        if (word != 0) {
            return row + std::countr_zero(word) / 8;
        }
    }
    for (; row < end; ++row) {
        if (selection[row] != 0) {
            return row;
        }
    }
    return end;
}

// The first row in [row, end) that the selection does not mark, or `end`.
int64_t NextUnselected(const uint8_t* selection, int64_t row, int64_t end) {
    for (; row + 8 <= end; row += 8) {
        uint64_t word;
        std::memcpy(&word, selection + row, sizeof(word));
        // The lowest flagged byte is the first zero byte.
        const uint64_t zero = (word - 0x0101010101010101ULL) & ~word & 0x8080808080808080ULL;
        if (zero != 0) {
            return row + std::countr_zero(zero) / 8;
        }
    }
    for (; row < end; ++row) {
        if (selection[row] == 0) {
            return row;
        }
    }
    return end;
}

// The end of the run of selected rows from `row`, a selected one, through gaps too short to skip.
int64_t RunEnd(const uint8_t* selection, int64_t row, int64_t end) {
    while (row < end) {
        const int64_t gap = NextUnselected(selection, row, end);
        const int64_t next = NextSelected(selection, gap, end);
        if (next == end || next - gap >= MinSkipRows) {
            return gap;
        }
        row = next;
    }
    return end;
}

std::shared_ptr<arrow::ResizableBuffer> Allocate(int64_t bytes, arrow::MemoryPool* memoryPool) {
    auto allocated = arrow::AllocateResizableBuffer(bytes, memoryPool);
    if (!allocated.ok()) {
        throw std::runtime_error(allocated.status().ToString());
    }
    return std::move(*allocated);
}

void ThrowIfEmpty(int64_t levels) {
    if (levels == 0) {
        throw std::runtime_error("parquet column scan: a page ended before its values");
    }
}

// The validity bitmap of one read, allocated at its first NULL.
class TNulls {
public:
    TNulls(int16_t maxDef, arrow::MemoryPool* memoryPool)
        : MaxDef_(maxDef)
        , MemoryPool_(memoryPool)
    {}

    void Begin(int64_t rows) {
        Rows_ = rows;
        Bitmap_.reset();
        Count_ = 0;
    }

    int16_t* Levels() {
        if (MaxDef_ == 0) {
            return nullptr;
        }
        Levels_.resize(ChunkRows);
        return Levels_.data();
    }

    bool Valid(int64_t level) const {
        return Levels_[level] == MaxDef_;
    }

    void Mark(int64_t row, int64_t levels) {
        if (!Bitmap_) {
            Bitmap_ = Allocate(arrow::bit_util::BytesForBits(Rows_), MemoryPool_);
            std::memset(Bitmap_->mutable_data(), 0xff, Bitmap_->size());
        }
        for (int64_t i = 0; i < levels; ++i) {
            if (!Valid(i)) {
                arrow::bit_util::ClearBit(Bitmap_->mutable_data(), row + i);
                ++Count_;
            }
        }
    }

    std::shared_ptr<arrow::Buffer> Bitmap() {
        return std::move(Bitmap_);
    }

    int64_t Count() const {
        return Count_;
    }

private:
    const int16_t MaxDef_;
    arrow::MemoryPool* const MemoryPool_;
    std::vector<int16_t> Levels_;
    int64_t Rows_ = 0;
    std::shared_ptr<arrow::ResizableBuffer> Bitmap_;
    int64_t Count_ = 0;
};

template <typename TDType>
auto& Typed(parquet::ColumnReader& reader) {
    return static_cast<parquet::TypedColumnReader<TDType>&>(reader);
}

// Skips values of the current page only: a new page needs the page filter's context.
template <typename TDType>
void SkipValues(parquet::ColumnReader& reader, int64_t count) {
    if (Typed<TDType>(reader).Skip(count) != count) {
        throw std::runtime_error("parquet column scan: a page ended before its values");
    }
}

// A fixed-width column: each raw value is decoded to its storage. Every row a read reaches
// keeps its value; a NULL row stores zero.
template <typename TDType, typename TStorage, typename TDecode>
class TFixedValues final : public IColumnValues {
    using TValue = typename TDType::c_type;
    // The page decoder writes straight into the output.
    static constexpr bool Direct = std::is_same_v<TValue, TStorage>;

public:
    TFixedValues(
        std::shared_ptr<arrow::DataType> type,
        int16_t maxDef,
        arrow::MemoryPool* memoryPool,
        TDecode decode)
        : Type_(std::move(type))
        , MemoryPool_(memoryPool)
        , Decode_(std::move(decode))
        , Nulls_(maxDef, memoryPool)
    {}

    const std::shared_ptr<arrow::DataType>& Type() const override {
        return Type_;
    }

    void Begin(int64_t rows, bool sparse) override {
        Rows_ = rows;
        Values_ = Allocate(rows * static_cast<int64_t>(sizeof(TStorage)), MemoryPool_);
        if (sparse) {
            std::memset(Values_->mutable_data(), 0, Values_->size());
        }
        Nulls_.Begin(rows);
    }

    void Read(
        parquet::ColumnReader& reader,
        int64_t row,
        int64_t count,
        const uint8_t* /*selection*/) override
    {
        auto* out = reinterpret_cast<TStorage*>(Values_->mutable_data());
        if (!Direct) {
            Chunk_.resize(ChunkRows);
        }
        for (int64_t done = 0; done < count;) {
            TStorage* target = out + row + done;
            TValue* values = Chunk_.data();
            if constexpr (Direct) {
                values = target;
            }
            int64_t valuesRead = 0;
            const int64_t levels = Typed<TDType>(reader).ReadBatch(
                std::min(count - done, ChunkRows), Nulls_.Levels(), nullptr, values, &valuesRead);
            ThrowIfEmpty(levels);
            if (valuesRead == levels) {
                if constexpr (!Direct) {
                    for (int64_t i = 0; i < levels; ++i) {
                        target[i] = static_cast<TStorage>(Decode_(values[i]));
                    }
                }
            } else {
                Nulls_.Mark(row + done, levels);
                // Dense values move to their rows; from the back, so a direct read spreads in place.
                int64_t next = valuesRead;
                for (int64_t i = levels; i-- > 0;) {
                    target[i] = Nulls_.Valid(i)
                        ? static_cast<TStorage>(Decode_(values[--next]))
                        : TStorage{0};
                }
            }
            done += levels;
        }
    }

    void Skip(parquet::ColumnReader& reader, int64_t count) override {
        SkipValues<TDType>(reader, count);
    }

    std::shared_ptr<arrow::Array> Finish() override {
        const int64_t nulls = Nulls_.Count();
        return arrow::MakeArray(arrow::ArrayData::Make(
            Type_, Rows_, {Nulls_.Bitmap(), std::move(Values_)}, nulls));
    }

private:
    const std::shared_ptr<arrow::DataType> Type_;
    arrow::MemoryPool* const MemoryPool_;
    const TDecode Decode_;
    TNulls Nulls_;
    std::vector<TValue> Chunk_;
    int64_t Rows_ = 0;
    std::shared_ptr<arrow::ResizableBuffer> Values_;
};

template <typename TDType, typename TStorage, typename TDecode>
std::unique_ptr<IColumnValues> MakeFixed(
    std::shared_ptr<arrow::DataType> type,
    int16_t maxDef,
    arrow::MemoryPool* memoryPool,
    TDecode decode)
{
    return std::make_unique<TFixedValues<TDType, TStorage, TDecode>>(
        std::move(type), maxDef, memoryPool, std::move(decode));
}

template <typename TDType, typename TStorage>
std::unique_ptr<IColumnValues> MakeFixed(
    std::shared_ptr<arrow::DataType> type,
    int16_t maxDef,
    arrow::MemoryPool* memoryPool)
{
    return MakeFixed<TDType, TStorage>(
        std::move(type), maxDef, memoryPool, [](typename TDType::c_type value) {
            return value;
        });
}

// A BOOLEAN column as an Arrow bitmap.
class TBoolValues final : public IColumnValues {
public:
    TBoolValues(int16_t maxDef, arrow::MemoryPool* memoryPool)
        : MemoryPool_(memoryPool)
        , Nulls_(maxDef, memoryPool)
        , Chunk_(std::make_unique<bool[]>(ChunkRows))
    {}

    const std::shared_ptr<arrow::DataType>& Type() const override {
        return Type_;
    }

    void Begin(int64_t rows, bool /*sparse*/) override {
        Rows_ = rows;
        Bits_ = Allocate(arrow::bit_util::BytesForBits(rows), MemoryPool_);
        std::memset(Bits_->mutable_data(), 0, Bits_->size());
        Nulls_.Begin(rows);
    }

    void Read(
        parquet::ColumnReader& reader,
        int64_t row,
        int64_t count,
        const uint8_t* /*selection*/) override
    {
        for (int64_t done = 0; done < count;) {
            int64_t valuesRead = 0;
            const int64_t levels = Typed<parquet::BooleanType>(reader).ReadBatch(
                std::min(count - done, ChunkRows), Nulls_.Levels(), nullptr, Chunk_.get(), &valuesRead);
            ThrowIfEmpty(levels);
            if (valuesRead != levels) {
                Nulls_.Mark(row + done, levels);
            }
            int64_t next = 0;
            for (int64_t i = 0; i < levels; ++i) {
                if (valuesRead != levels && !Nulls_.Valid(i)) {
                    continue;
                }
                if (Chunk_[next++]) {
                    arrow::bit_util::SetBit(Bits_->mutable_data(), row + done + i);
                }
            }
            done += levels;
        }
    }

    void Skip(parquet::ColumnReader& reader, int64_t count) override {
        SkipValues<parquet::BooleanType>(reader, count);
    }

    std::shared_ptr<arrow::Array> Finish() override {
        const int64_t nulls = Nulls_.Count();
        return arrow::MakeArray(arrow::ArrayData::Make(
            Type_, Rows_, {Nulls_.Bitmap(), std::move(Bits_)}, nulls));
    }

private:
    const std::shared_ptr<arrow::DataType> Type_ = arrow::boolean();
    arrow::MemoryPool* const MemoryPool_;
    TNulls Nulls_;
    // Not std::vector<bool>: ReadBatch writes an array of bool.
    std::unique_ptr<bool[]> Chunk_;
    int64_t Rows_ = 0;
    std::shared_ptr<arrow::ResizableBuffer> Bits_;
};

// A BYTE_ARRAY string column. Only the selected rows copy their bytes; the others are empty.
class TStringValues final : public IColumnValues {
public:
    TStringValues(int16_t maxDef, arrow::MemoryPool* memoryPool)
        : MemoryPool_(memoryPool)
        , Nulls_(maxDef, memoryPool)
        , Data_(memoryPool)
    {}

    const std::shared_ptr<arrow::DataType>& Type() const override {
        return Type_;
    }

    void Begin(int64_t rows, bool /*sparse*/) override {
        Rows_ = rows;
        Offsets_ = Allocate((rows + 1) * static_cast<int64_t>(sizeof(int64_t)), MemoryPool_);
        OffsetData()[0] = 0;
        Filled_ = 0;
        Data_.Reset();
        Nulls_.Begin(rows);
    }

    void Read(
        parquet::ColumnReader& reader,
        int64_t row,
        int64_t count,
        const uint8_t* selection) override
    {
        Chunk_.resize(ChunkRows);
        for (int64_t done = 0; done < count;) {
            int64_t valuesRead = 0;
            const int64_t levels = Typed<parquet::ByteArrayType>(reader).ReadBatch(
                std::min(count - done, ChunkRows), Nulls_.Levels(), nullptr, Chunk_.data(), &valuesRead);
            ThrowIfEmpty(levels);
            const bool dense = valuesRead == levels;
            if (!dense) {
                Nulls_.Mark(row + done, levels);
            }
            int64_t next = 0;
            for (int64_t i = 0; i < levels; ++i) {
                if (!dense && !Nulls_.Valid(i)) {
                    continue;
                }
                const auto& value = Chunk_[next++];
                const int64_t at = row + done + i;
                if (!selection || selection[at] != 0) {
                    Store(at, value);
                }
            }
            done += levels;
        }
    }

    void Skip(parquet::ColumnReader& reader, int64_t count) override {
        SkipValues<parquet::ByteArrayType>(reader, count);
    }

    std::shared_ptr<arrow::Array> Finish() override {
        FillTo(Rows_);
        std::shared_ptr<arrow::Buffer> data;
        if (auto status = Data_.Finish(&data); !status.ok()) {
            throw std::runtime_error(status.ToString());
        }
        const int64_t nulls = Nulls_.Count();
        return arrow::MakeArray(arrow::ArrayData::Make(
            Type_, Rows_, {Nulls_.Bitmap(), std::move(Offsets_), std::move(data)}, nulls));
    }

private:
    int64_t* OffsetData() {
        return reinterpret_cast<int64_t*>(Offsets_->mutable_data());
    }

    // Rows before `row` that stored nothing are empty strings.
    void FillTo(int64_t row) {
        int64_t* offsets = OffsetData();
        const int64_t size = Data_.length();
        for (; Filled_ < row; ++Filled_) {
            offsets[Filled_ + 1] = size;
        }
    }

    void Store(int64_t row, const parquet::ByteArray& value) {
        FillTo(row);
        if (auto status = Data_.Append(value.ptr, value.len); !status.ok()) {
            throw std::runtime_error(status.ToString());
        }
        OffsetData()[row + 1] = Data_.length();
        Filled_ = row + 1;
    }

    const std::shared_ptr<arrow::DataType> Type_ = arrow::large_utf8();
    arrow::MemoryPool* const MemoryPool_;
    TNulls Nulls_;
    std::vector<parquet::ByteArray> Chunk_;
    int64_t Rows_ = 0;
    std::shared_ptr<arrow::ResizableBuffer> Offsets_;
    // Offsets [0, Filled_] are written.
    int64_t Filled_ = 0;
    arrow::BufferBuilder Data_;
};

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
// The decoder is instantiated per length (32 instantiations) so the byte swap has a
// compile-time size; accepted for the measured gain over a runtime-length loop.
template <int Length = 1, typename TFunc>
auto WithLength(int length, TFunc&& func) {
    if constexpr (Length > 16) {
        return decltype(func(std::integral_constant<int, 1>{})){};
    } else if (length == Length) {
        return func(std::integral_constant<int, Length>{});
    } else {
        return WithLength<Length + 1>(length, std::forward<TFunc>(func));
    }
}

// A decimal in the storage of its precision.
template <typename TStorage>
std::unique_ptr<IColumnValues> MakeDecimalValues(
    const parquet::ColumnDescriptor& descriptor,
    std::shared_ptr<arrow::DataType> type,
    arrow::MemoryPool* memoryPool)
{
    const int16_t maxDef = descriptor.max_definition_level();
    switch (descriptor.physical_type()) {
        case parquet::Type::INT32:
            return MakeFixed<parquet::Int32Type, TStorage>(std::move(type), maxDef, memoryPool);
        case parquet::Type::INT64:
            return MakeFixed<parquet::Int64Type, TStorage>(std::move(type), maxDef, memoryPool);
        case parquet::Type::FIXED_LEN_BYTE_ARRAY:
            return WithLength(descriptor.type_length(), [&](auto length) {
                return MakeFixed<parquet::FLBAType, TStorage>(
                    std::move(type), maxDef, memoryPool, [](parquet::FixedLenByteArray value) {
                        return BigEndianUnscaled<decltype(length)::value>(value.ptr);
                    });
            });
        case parquet::Type::BYTE_ARRAY:
            return MakeFixed<parquet::ByteArrayType, TStorage>(
                std::move(type), maxDef, memoryPool, [](parquet::ByteArray value) {
                    const auto length = static_cast<int32_t>(value.len);
                    return length <= 8
                        ? static_cast<__int128>(BigEndianUnscaled<int64_t, uint64_t>(value.ptr, length))
                        : BigEndianUnscaled<__int128, unsigned __int128>(value.ptr, length);
                });
        default:
            return nullptr;
    }
}

// How the scan decodes a column it reads.
enum class EValues {
    Bool,
    Int8,
    Int16,
    Int32,
    UInt8,
    UInt16,
    UInt32,
    Int64,
    UInt64,
    Float,
    Double,
    String,
    Decimal,
};

// The decoding of a flat column read as `type`, or nullopt when the scan does not read it.
std::optional<EValues> ValuesOf(const parquet::ColumnDescriptor& descriptor, const arrow::DataType& type) {
    if (descriptor.max_repetition_level() != 0 || descriptor.max_definition_level() > 1) {
        return std::nullopt;
    }
    const auto physical = descriptor.physical_type();
    if (descriptor.converted_type() == parquet::ConvertedType::DECIMAL) {
        const bool decodes = physical == parquet::Type::INT32 || physical == parquet::Type::INT64 ||
            physical == parquet::Type::BYTE_ARRAY ||
            (physical == parquet::Type::FIXED_LEN_BYTE_ARRAY && descriptor.type_length() <= 16);
        if (descriptor.type_precision() <= 0 || descriptor.type_precision() > MaxDecimalPrecision || !decodes) {
            return std::nullopt;
        }
        return EValues::Decimal;
    }
    const auto id = type.id();
    if (physical == parquet::Type::INT32) {
        switch (id) {
            case arrow::Type::INT8:
                return EValues::Int8;
            case arrow::Type::INT16:
                return EValues::Int16;
            case arrow::Type::INT32:
            case arrow::Type::DATE32:
                return EValues::Int32;
            case arrow::Type::UINT8:
                return EValues::UInt8;
            case arrow::Type::UINT16:
                return EValues::UInt16;
            case arrow::Type::UINT32:
                return EValues::UInt32;
            default:
                return std::nullopt;
        }
    }
    if (physical == parquet::Type::INT64 && (id == arrow::Type::INT64 || id == arrow::Type::UINT64)) {
        return id == arrow::Type::INT64
            ? EValues::Int64
            : EValues::UInt64;
    }
    if (physical == parquet::Type::BOOLEAN && id == arrow::Type::BOOL) {
        return EValues::Bool;
    }
    if (physical == parquet::Type::FLOAT && id == arrow::Type::FLOAT) {
        return EValues::Float;
    }
    if (physical == parquet::Type::DOUBLE && id == arrow::Type::DOUBLE) {
        return EValues::Double;
    }
    if (physical == parquet::Type::BYTE_ARRAY && (id == arrow::Type::STRING || id == arrow::Type::LARGE_STRING)) {
        return EValues::String;
    }
    return std::nullopt;
}

std::unique_ptr<IColumnValues> MakeValues(
    const parquet::ColumnDescriptor& descriptor,
    EValues values,
    const std::shared_ptr<arrow::DataType>& type,
    arrow::MemoryPool* memoryPool)
{
    const int16_t maxDef = descriptor.max_definition_level();
    switch (values) {
        case EValues::Bool:
            return std::make_unique<TBoolValues>(maxDef, memoryPool);
        case EValues::Int8:
            return MakeFixed<parquet::Int32Type, int8_t>(type, maxDef, memoryPool);
        case EValues::Int16:
            return MakeFixed<parquet::Int32Type, int16_t>(type, maxDef, memoryPool);
        case EValues::Int32:
            return MakeFixed<parquet::Int32Type, int32_t>(type, maxDef, memoryPool);
        case EValues::UInt8:
            return MakeFixed<parquet::Int32Type, uint8_t>(type, maxDef, memoryPool);
        case EValues::UInt16:
            return MakeFixed<parquet::Int32Type, uint16_t>(type, maxDef, memoryPool);
        case EValues::UInt32:
            return MakeFixed<parquet::Int32Type, uint32_t>(type, maxDef, memoryPool);
        case EValues::Int64:
            return MakeFixed<parquet::Int64Type, int64_t>(type, maxDef, memoryPool);
        case EValues::UInt64:
            return MakeFixed<parquet::Int64Type, uint64_t>(type, maxDef, memoryPool);
        case EValues::Float:
            return MakeFixed<parquet::FloatType, float>(type, maxDef, memoryPool);
        case EValues::Double:
            return MakeFixed<parquet::DoubleType, double>(type, maxDef, memoryPool);
        case EValues::String:
            return std::make_unique<TStringValues>(maxDef, memoryPool);
        case EValues::Decimal: {
            const TDecimalSpec spec{.Precision = descriptor.type_precision(), .Scale = descriptor.type_scale()};
            if (DecimalStorageOf(spec) == EDecimalStorage::I64) {
                return MakeDecimalValues<int64_t>(
                    descriptor, arrow::decimal64(spec.Precision, spec.Scale), memoryPool);
            }
            return MakeDecimalValues<__int128>(
                descriptor, arrow::decimal128(spec.Precision, spec.Scale), memoryPool);
        }
    }
    return nullptr;
}

} // namespace

TParquetColumnScan::TParquetColumnScan(
    parquet::ParquetFileReader& file,
    int column,
    const std::shared_ptr<arrow::DataType>& type,
    std::vector<int> rowGroups,
    arrow::MemoryPool* memoryPool)
    : File_(file)
    , Column_(column)
    , RowGroups_(std::move(rowGroups))
    , MemoryPool_(memoryPool)
    , Descriptor_(File_.metadata()->schema()->Column(Column_))
{
    const auto values = ValuesOf(*Descriptor_, *type);
    if (!values) {
        throw std::runtime_error("parquet column scan: unsupported column " + Descriptor_->name());
    }
    Values_ = MakeValues(*Descriptor_, *values, type, memoryPool);
}

TParquetColumnScan::~TParquetColumnScan() = default;

bool TParquetColumnScan::Supports(
    const parquet::ColumnDescriptor& descriptor,
    const std::shared_ptr<arrow::DataType>& type)
{
    return ValuesOf(descriptor, *type).has_value();
}

const std::shared_ptr<arrow::DataType>& TParquetColumnScan::Type() const {
    return Values_->Type();
}

std::shared_ptr<arrow::Array> TParquetColumnScan::Read(int64_t rows, const uint8_t* selection) {
    ReadBegin_ = Position_;
    ReadEnd_ = Position_ + rows;
    Selection_ = selection;
    Values_->Begin(rows, selection != nullptr);
    for (int64_t row = 0;;) {
        if (selection) {
            row = NextSelected(selection, row, rows);
        }
        if (row == rows) {
            break;
        }
        const int64_t target = ReadBegin_ + row;
        while (PageEnd_ <= target) {
            // The rest of a page is consumed without decoding.
            if (ReaderRow_ < PageEnd_) {
                Values_->Skip(*Reader_, PageEnd_ - ReaderRow_);
            }
            LoadPage();
        }
        if (ReaderRow_ < target) {
            Values_->Skip(*Reader_, target - ReaderRow_);
            ReaderRow_ = target;
        }
        const int64_t pageEnd = std::min(PageEnd_, ReadEnd_) - ReadBegin_;
        const int64_t end = selection
            ? RunEnd(selection, row, pageEnd)
            : pageEnd;
        Values_->Read(*Reader_, row, end - row, selection);
        ReaderRow_ += end - row;
        row = end;
    }
    Position_ = ReadEnd_;
    Selection_ = nullptr;
    return Values_->Finish();
}

void TParquetColumnScan::Skip(int64_t rows) {
    Position_ += rows;
}

// Loads the next page the page filter keeps; SkipPage positions it.
void TParquetColumnScan::LoadPage() {
    while (!Reader_ || !Reader_->HasNext()) {
        if (NextRowGroup_ == RowGroups_.size()) {
            throw std::runtime_error("parquet column scan ran past its row groups");
        }
        auto rowGroup = File_.RowGroup(RowGroups_[NextRowGroup_++]);
        auto pages = rowGroup->GetColumnPageReader(Column_);
        pages->set_data_page_filter([this](const parquet::DataPageStats& stats) {
            return SkipPage(stats.num_values);
        });
        Reader_ = parquet::ColumnReader::Make(Descriptor_, std::move(pages), MemoryPool_);
    }
}

// A page holds the next `values` rows of the flat column. It is needed when it reaches past
// the read in progress, or holds a row of the read that the selection marks.
bool TParquetColumnScan::SkipPage(int64_t values) {
    const int64_t begin = NextPage_;
    const int64_t end = begin + values;
    NextPage_ = end;
    // An empty page would end the column reader.
    bool skip = values == 0 || end <= ReadBegin_;
    if (!skip && end <= ReadEnd_ && Selection_) {
        const int64_t first = std::max(begin, ReadBegin_) - ReadBegin_;
        skip = NextSelected(Selection_, first, end - ReadBegin_) == end - ReadBegin_;
    }
    if (!skip) {
        ReaderRow_ = begin;
        PageEnd_ = end;
    }
    return skip;
}

} // namespace NQdb
