#include <qdb/io/parquet/selective_reader.h>

#include <arrow/buffer.h>
#include <arrow/memory_pool.h>
#include <arrow/type.h>
#include <parquet/column_reader.h>
#include <parquet/file_reader.h>
#include <parquet/metadata.h>
#include <parquet/schema.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace NQdb {

namespace {

// Values are decoded through a scratch buffer of this many rows, small enough
// to stay in cache, so that rejected rows cost no memory traffic beyond the
// page itself.
constexpr int64_t ChunkRows = 4096;

enum class EKind {
    // Parquet storage that is already the TColumn layout.
    Fixed,
    // INT32 storage of an 8- or 16-bit integer.
    Narrow,
    Bool,
    String,
};

struct TLayout {
    EKind Kind;
    int Width = 0;
};

std::optional<TLayout> Classify(
    const parquet::ColumnDescriptor& descriptor,
    const arrow::DataType& type)
{
    if (descriptor.max_repetition_level() != 0 ||
        descriptor.max_definition_level() > 1 ||
        descriptor.path()->ToDotVector().size() != 1)
    {
        return std::nullopt;
    }
    const auto id = type.id();
    switch (descriptor.physical_type()) {
        case parquet::Type::BOOLEAN:
            if (id == arrow::Type::BOOL) {
                return TLayout{.Kind = EKind::Bool};
            }
            break;
        case parquet::Type::INT32:
            if (id == arrow::Type::INT32 || id == arrow::Type::UINT32 ||
                id == arrow::Type::DATE32)
            {
                return TLayout{.Kind = EKind::Fixed, .Width = 4};
            }
            if (id == arrow::Type::INT16 || id == arrow::Type::UINT16) {
                return TLayout{.Kind = EKind::Narrow, .Width = 2};
            }
            if (id == arrow::Type::INT8 || id == arrow::Type::UINT8) {
                return TLayout{.Kind = EKind::Narrow, .Width = 1};
            }
            break;
        case parquet::Type::INT64:
            if (id == arrow::Type::INT64 || id == arrow::Type::UINT64) {
                return TLayout{.Kind = EKind::Fixed, .Width = 8};
            }
            break;
        case parquet::Type::DOUBLE:
            if (id == arrow::Type::DOUBLE) {
                return TLayout{.Kind = EKind::Fixed, .Width = 8};
            }
            break;
        case parquet::Type::BYTE_ARRAY:
            if (id == arrow::Type::STRING || id == arrow::Type::LARGE_STRING) {
                return TLayout{.Kind = EKind::String};
            }
            break;
        default:
            break;
    }
    return std::nullopt;
}

std::shared_ptr<arrow::Buffer> Allocate(int64_t bytes, arrow::MemoryPool* pool) {
    auto buffer = arrow::AllocateBuffer(bytes, pool);
    if (!buffer.ok()) {
        throw std::runtime_error(buffer.status().ToString());
    }
    return std::shared_ptr<arrow::Buffer>(std::move(*buffer));
}

void ClearBit(uint8_t* bits, int64_t i) {
    bits[i / 8] &= static_cast<uint8_t>(~(1u << (i % 8)));
}

void SetBit(uint8_t* bits, int64_t i) {
    bits[i / 8] |= static_cast<uint8_t>(1u << (i % 8));
}

// Validity of a nullable column. Starts all-valid and is dropped when no
// materialized row is null, as the Arrow path leaves Mask null then.
class TMask {
public:
    TMask(bool nullable, int64_t rows, arrow::MemoryPool* pool) {
        if (nullable) {
            Bits_ = Allocate((rows + 7) / 8, pool);
            std::memset(Bits_->mutable_data(), 0xff, static_cast<size_t>(Bits_->size()));
        }
    }

    void SetNull(int64_t row) {
        ClearBit(Bits_->mutable_data(), row);
        HasNulls_ = true;
    }

    void Finish(TColumn& column, std::vector<std::shared_ptr<arrow::Buffer>>& owners) {
        column.Mask = nullptr;
        column.MaskBitOffset = 0;
        if (HasNulls_) {
            column.Mask = Bits_->data();
            owners.push_back(std::move(Bits_));
        }
    }

private:
    std::shared_ptr<arrow::Buffer> Bits_;
    bool HasNulls_ = false;
};

// String offsets start 32-bit and widen once if a batch's kept bytes do not
// fit, so the common case keeps the layout the Arrow path produces.
class TOffsets {
public:
    TOffsets(int64_t rows, arrow::MemoryPool* pool)
        : Rows_(rows)
        , Pool_(pool)
        , Buffer_(Allocate((rows + 1) * sizeof(int32_t), pool))
    {
        reinterpret_cast<int32_t*>(Buffer_->mutable_data())[0] = 0;
    }

    // Calls write with the offsets array, first widened if maxOffset needs
    // it; the entries up to written are already set.
    template <typename TWrite>
    void Write(int64_t written, int64_t maxOffset, TWrite&& write) {
        if (Width_ == 4 && maxOffset > std::numeric_limits<int32_t>::max()) {
            auto wide = Allocate((Rows_ + 1) * sizeof(int64_t), Pool_);
            std::copy_n(
                reinterpret_cast<const int32_t*>(Buffer_->data()),
                written + 1,
                reinterpret_cast<int64_t*>(wide->mutable_data()));
            Buffer_ = std::move(wide);
            Width_ = 8;
        }
        if (Width_ == 4) {
            write(reinterpret_cast<int32_t*>(Buffer_->mutable_data()));
        } else {
            write(reinterpret_cast<int64_t*>(Buffer_->mutable_data()));
        }
    }

    void Finish(TColumn& column, std::vector<std::shared_ptr<arrow::Buffer>>& owners) {
        column.Offsets = Buffer_->mutable_data();
        column.OffsetWidth = static_cast<uint8_t>(Width_);
        owners.push_back(std::move(Buffer_));
    }

private:
    int64_t Rows_;
    arrow::MemoryPool* Pool_;
    std::shared_ptr<arrow::Buffer> Buffer_;
    int Width_ = 4;
};

} // namespace

struct TSelectiveColumnReader::TImpl {
    std::shared_ptr<parquet::RowGroupReader> RowGroup;
    std::shared_ptr<parquet::ColumnReader> Reader;
    TLayout Layout;
    bool Nullable = false;
    arrow::MemoryPool* Pool = nullptr;
    std::vector<int16_t> DefLevels;
    std::vector<int32_t> ValueIndex;
    std::vector<uint8_t> Scratch;

    template <typename TDType>
    parquet::TypedColumnReader<TDType>& Typed() {
        return static_cast<parquet::TypedColumnReader<TDType>&>(*Reader);
    }

    // Decodes rows values chunk by chunk. A ReadBatch call stays within one
    // page and writes non-null values densely, so each chunk is consumed
    // before the next call may release the page its byte arrays point into.
    // consume(row, levels, values) sees the chunk's first row.
    template <typename TDType, typename TConsume>
    void Decode(int64_t rows, TConsume&& consume) {
        using T = typename TDType::c_type;
        const int64_t chunk = std::min(rows, ChunkRows);
        Scratch.resize(static_cast<size_t>(chunk) * sizeof(T));
        if (Nullable) {
            DefLevels.resize(static_cast<size_t>(chunk));
        }
        auto* values = reinterpret_cast<T*>(Scratch.data());
        auto& reader = Typed<TDType>();
        int64_t row = 0;
        while (row < rows) {
            int64_t valuesRead = 0;
            const int64_t levels = reader.ReadBatch(
                std::min(chunk, rows - row),
                Nullable ? DefLevels.data() : nullptr,
                nullptr,
                values,
                &valuesRead);
            if (levels <= 0) {
                throw std::runtime_error(
                    "parquet column chunk ended before its row group");
            }
            consume(row, levels, static_cast<const T*>(values));
            row += levels;
        }
    }

    // Calls visit(row, value) for every row of a chunk; value is null for a
    // null row.
    template <typename T, typename TVisit>
    void VisitAll(int64_t row, int64_t levels, const T* values, TVisit&& visit) {
        if (!Nullable) {
            for (int64_t i = 0; i < levels; ++i) {
                visit(row + i, values + i);
            }
            return;
        }
        for (int64_t i = 0; i < levels; ++i) {
            if (DefLevels[static_cast<size_t>(i)] != 0) {
                visit(row + i, values++);
            } else {
                visit(row + i, static_cast<const T*>(nullptr));
            }
        }
    }

    // Calls visit for the kept rows of a chunk; next is the first kept row
    // not visited yet and advances past the chunk.
    template <typename T, typename TVisit>
    void VisitKept(
        int64_t row,
        int64_t levels,
        const T* values,
        std::span<const int32_t> kept,
        size_t& next,
        TVisit&& visit)
    {
        const int64_t end = row + levels;
        if (!Nullable) {
            for (; next < kept.size() && kept[next] < end; ++next) {
                visit(kept[next], values + (kept[next] - row));
            }
            return;
        }
        // Positions of the dense non-null values.
        ValueIndex.resize(static_cast<size_t>(levels));
        int32_t valid = 0;
        for (int64_t i = 0; i < levels; ++i) {
            ValueIndex[static_cast<size_t>(i)] = valid;
            valid += DefLevels[static_cast<size_t>(i)] != 0;
        }
        for (; next < kept.size() && kept[next] < end; ++next) {
            const auto i = static_cast<size_t>(kept[next] - row);
            visit(kept[next], DefLevels[i] != 0 ? values + ValueIndex[i] : nullptr);
        }
    }

    // Decodes rows values and stores those of the kept rows, or of every row
    // without kept, with store(out, row, value); a null row stores T{}. The
    // other rows hold zeros: kernels may evaluate unselected rows, so they
    // must not see arbitrary bytes. TOut is the storage unit of bytes per
    // row, or uint8_t for a bitmap.
    template <typename TDType, typename TOut, typename TStore>
    void ReadValues(
        int64_t rows,
        const std::span<const int32_t>* kept,
        TColumn& column,
        std::vector<std::shared_ptr<arrow::Buffer>>& owners,
        int64_t bytes,
        TStore&& store)
    {
        using T = typename TDType::c_type;
        auto data = Allocate(bytes, Pool);
        auto* out = reinterpret_cast<TOut*>(data->mutable_data());
        if (kept) {
            std::memset(out, 0, static_cast<size_t>(bytes));
        }
        TMask mask(Nullable, rows, Pool);
        auto visit = [&](int64_t row, const T* value) {
            if (value) {
                store(out, row, *value);
            } else {
                store(out, row, T{});
                mask.SetNull(row);
            }
        };
        size_t next = 0;
        Decode<TDType>(rows, [&](int64_t row, int64_t levels, const T* values) {
            if (kept) {
                VisitKept(row, levels, values, *kept, next, visit);
            } else {
                VisitAll(row, levels, values, visit);
            }
        });
        mask.Finish(column, owners);
        column.Data = reinterpret_cast<char*>(out);
        owners.push_back(std::move(data));
    }

    template <typename TDType>
    void ReadFixed(
        int64_t rows,
        const std::span<const int32_t>* kept,
        TColumn& column,
        std::vector<std::shared_ptr<arrow::Buffer>>& owners)
    {
        using T = typename TDType::c_type;
        // Scattering kept values costs a random store each; past this share
        // of kept rows, decoding every value into place costs less.
        constexpr size_t scatterMaxShare = 16;
        if (kept && kept->size() * scatterMaxShare > static_cast<size_t>(rows)) {
            kept = nullptr;
        }
        if (kept || Nullable) {
            ReadValues<TDType, T>(rows, kept, column, owners, rows * sizeof(T),
                [](T* out, int64_t row, T value) { out[row] = value; });
            return;
        }
        // Every row, none null: decode in place, as the Arrow reader does.
        auto data = Allocate(rows * static_cast<int64_t>(sizeof(T)), Pool);
        auto* out = reinterpret_cast<T*>(data->mutable_data());
        auto& reader = Typed<TDType>();
        for (int64_t row = 0; row < rows;) {
            int64_t valuesRead = 0;
            const int64_t levels = reader.ReadBatch(
                rows - row, nullptr, nullptr, out + row, &valuesRead);
            if (levels <= 0) {
                throw std::runtime_error(
                    "parquet column chunk ended before its row group");
            }
            row += levels;
        }
        column.Data = reinterpret_cast<char*>(out);
        owners.push_back(std::move(data));
    }

    void ReadNarrow(
        int64_t rows,
        const std::span<const int32_t>* kept,
        TColumn& column,
        std::vector<std::shared_ptr<arrow::Buffer>>& owners)
    {
        if (Layout.Width == 1) {
            ReadValues<parquet::Int32Type, int8_t>(rows, kept, column, owners, rows,
                [](int8_t* out, int64_t row, int32_t value) {
                    out[row] = static_cast<int8_t>(value);
                });
        } else {
            ReadValues<parquet::Int32Type, int16_t>(rows, kept, column, owners, rows * 2,
                [](int16_t* out, int64_t row, int32_t value) {
                    out[row] = static_cast<int16_t>(value);
                });
        }
    }

    void ReadBool(
        int64_t rows,
        const std::span<const int32_t>* kept,
        TColumn& column,
        std::vector<std::shared_ptr<arrow::Buffer>>& owners)
    {
        ReadValues<parquet::BooleanType, uint8_t>(rows, kept, column, owners, (rows + 7) / 8,
            [](uint8_t* out, int64_t row, bool value) {
                if (value) {
                    SetBit(out, row);
                } else {
                    ClearBit(out, row);
                }
            });
    }

    void ReadString(
        int64_t rows,
        const std::span<const int32_t>* kept,
        TColumn& column,
        std::vector<std::shared_ptr<arrow::Buffer>>& owners)
    {
        auto resizable = arrow::AllocateResizableBuffer(0, Pool);
        if (!resizable.ok()) {
            throw std::runtime_error(resizable.status().ToString());
        }
        std::shared_ptr<arrow::ResizableBuffer> data = std::move(*resizable);
        TOffsets offsets(rows, Pool);
        TMask mask(Nullable, rows, Pool);
        int64_t size = 0;
        size_t next = 0;
        Decode<parquet::ByteArrayType>(rows, [&](int64_t row, int64_t levels, const parquet::ByteArray* values) {
            // The byte arrays point into the dictionary or the current page:
            // copy the kept ones now, sizing them first to grow data once.
            auto visit = [&](auto&& each) {
                if (kept) {
                    size_t cursor = next;
                    VisitKept(row, levels, values, *kept, cursor, each);
                    return cursor;
                }
                VisitAll(row, levels, values, each);
                return next;
            };
            int64_t bytes = 0;
            visit([&](int64_t, const parquet::ByteArray* value) {
                bytes += value ? value->len : 0;
            });
            if (size + bytes > data->capacity()) {
                auto status = data->Reserve(std::max(size + bytes, data->capacity() * 2));
                if (!status.ok()) {
                    throw std::runtime_error(status.ToString());
                }
            }
            auto* to = data->mutable_data();
            offsets.Write(row, size + bytes, [&](auto* offset) {
                using TOffset = std::remove_reference_t<decltype(*offset)>;
                int64_t filled = row;
                next = visit([&](int64_t i, const parquet::ByteArray* value) {
                    // A row that is not kept is an empty string.
                    for (; filled < i; ++filled) {
                        offset[filled + 1] = static_cast<TOffset>(size);
                    }
                    if (value) {
                        std::memcpy(to + size, value->ptr, value->len);
                        size += value->len;
                    } else {
                        mask.SetNull(i);
                    }
                    offset[i + 1] = static_cast<TOffset>(size);
                    filled = i + 1;
                });
                for (; filled < row + levels; ++filled) {
                    offset[filled + 1] = static_cast<TOffset>(size);
                }
            });
        });
        auto status = data->Resize(size, false);
        if (!status.ok()) {
            throw std::runtime_error(status.ToString());
        }
        offsets.Finish(column, owners);
        mask.Finish(column, owners);
        column.Data = reinterpret_cast<char*>(data->mutable_data());
        owners.push_back(std::move(data));
    }

    void Read(
        int64_t rows,
        const std::span<const int32_t>* kept,
        TColumn& column,
        std::vector<std::shared_ptr<arrow::Buffer>>& owners)
    {
        column = TColumn{};
        switch (Layout.Kind) {
            case EKind::Fixed:
                switch (Reader->type()) {
                    case parquet::Type::INT32:
                        ReadFixed<parquet::Int32Type>(rows, kept, column, owners);
                        return;
                    case parquet::Type::INT64:
                        ReadFixed<parquet::Int64Type>(rows, kept, column, owners);
                        return;
                    case parquet::Type::DOUBLE:
                        ReadFixed<parquet::DoubleType>(rows, kept, column, owners);
                        return;
                    default:
                        break;
                }
                break;
            case EKind::Narrow:
                ReadNarrow(rows, kept, column, owners);
                return;
            case EKind::Bool:
                ReadBool(rows, kept, column, owners);
                return;
            case EKind::String:
                ReadString(rows, kept, column, owners);
                return;
        }
        throw std::logic_error("selective reader: unexpected column layout");
    }
};

bool TSelectiveColumnReader::Supports(
    const parquet::ColumnDescriptor& descriptor,
    const arrow::DataType& type)
{
    return Classify(descriptor, type).has_value();
}

TSelectiveColumnReader::TSelectiveColumnReader(
    std::shared_ptr<parquet::RowGroupReader> rowGroup,
    int column,
    const arrow::DataType& type,
    arrow::MemoryPool* memoryPool)
    : Impl_(std::make_unique<TImpl>())
{
    const auto* descriptor = rowGroup->metadata()->schema()->Column(column);
    auto layout = Classify(*descriptor, type);
    if (!layout) {
        throw std::invalid_argument(
            "parquet column '" + descriptor->name() +
            "' is not supported by the selective reader");
    }
    Impl_->Layout = *layout;
    Impl_->Nullable = descriptor->max_definition_level() > 0;
    Impl_->Pool = memoryPool;
    Impl_->Reader = rowGroup->Column(column);
    Impl_->RowGroup = std::move(rowGroup);
}

TSelectiveColumnReader::~TSelectiveColumnReader() = default;
TSelectiveColumnReader::TSelectiveColumnReader(TSelectiveColumnReader&&) noexcept = default;
TSelectiveColumnReader& TSelectiveColumnReader::operator=(TSelectiveColumnReader&&) noexcept = default;

void TSelectiveColumnReader::ReadAll(
    int64_t rows,
    TColumn& column,
    std::vector<std::shared_ptr<arrow::Buffer>>& owners)
{
    Impl_->Read(rows, nullptr, column, owners);
}

void TSelectiveColumnReader::ReadKept(
    int64_t rows,
    std::span<const int32_t> kept,
    TColumn& column,
    std::vector<std::shared_ptr<arrow::Buffer>>& owners)
{
    Impl_->Read(rows, &kept, column, owners);
}

void TSelectiveColumnReader::Skip(int64_t rows) {
    auto& impl = *Impl_;
    int64_t skipped = 0;
    switch (impl.Reader->type()) {
        case parquet::Type::BOOLEAN:
            skipped = impl.Typed<parquet::BooleanType>().Skip(rows);
            break;
        case parquet::Type::INT32:
            skipped = impl.Typed<parquet::Int32Type>().Skip(rows);
            break;
        case parquet::Type::INT64:
            skipped = impl.Typed<parquet::Int64Type>().Skip(rows);
            break;
        case parquet::Type::DOUBLE:
            skipped = impl.Typed<parquet::DoubleType>().Skip(rows);
            break;
        case parquet::Type::BYTE_ARRAY:
            skipped = impl.Typed<parquet::ByteArrayType>().Skip(rows);
            break;
        default:
            throw std::logic_error("selective reader: unexpected physical type");
    }
    if (skipped != rows) {
        throw std::runtime_error("parquet column chunk ended before its row group");
    }
}

} // namespace NQdb
