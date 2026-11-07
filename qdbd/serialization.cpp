#include "serialization.h"

#include <bit>
#include <concepts>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace NNet {
namespace NActors {
namespace {

using NQdb::TColumn;
using NQdb::TRowSet;
using NQdb::TValueLayout;
using NQdb::NRemote::TDataChunk;
using NQdb::NRemote::TSpawnComputeActorResponse;

constexpr size_t MaxPayloadSize = std::numeric_limits<uint32_t>::max();

[[noreturn]] void Invalid(std::string_view reason) {
    throw std::runtime_error("invalid distributed message: " + std::string(reason));
}

size_t ByteSize(uint64_t count, size_t width) {
    if (width == 0 || count > MaxPayloadSize / width) {
        Invalid("buffer size overflow");
    }
    return static_cast<size_t>(count) * width;
}

size_t BitmapSize(uint64_t rows) {
    return ByteSize(rows / 8 + (rows % 8 != 0), 1);
}

void ValidateLayout(const TValueLayout& layout) {
    if (layout.Bitmap) {
        if (layout.Width != 0) {
            Invalid("bitmap column must have width 0");
        }
    } else if (layout.Width != 0 && layout.Width != 1 && layout.Width != 2 &&
        layout.Width != 4 && layout.Width != 8 && layout.Width != 16)
    {
        Invalid("unsupported value width");
    }
}

// The wire carries only layouts, so a layout with Width 0 and no Bitmap is the one string rule.
bool IsStringLayout(const TValueLayout& layout) {
    return layout.Width == 0 && !layout.Bitmap;
}

class TWriter {
public:
    explicit TWriter(std::ostringstream& output)
        : Output_(output)
    {}

    void Bytes(const void* data, size_t size) {
        if (size > MaxPayloadSize - Size_) {
            Invalid("payload size overflow");
        }
        if (size != 0) {
            if (!data) {
                Invalid("missing buffer");
            }
            Output_.write(static_cast<const char*>(data), size);
            if (!Output_) {
                Invalid("cannot write payload");
            }
        }
        Size_ += size;
    }

    template<std::unsigned_integral T>
    void Value(T value) {
        if constexpr (std::endian::native == std::endian::big) {
            value = std::byteswap(value);
        }
        Bytes(&value, sizeof(value));
    }

    void Bitmap(const void* data, int32_t bitOffset, uint64_t rows) {
        if (bitOffset < 0) {
            Invalid("negative bitmap offset");
        }
        const size_t size = BitmapSize(rows);
        if (size == 0) {
            return;
        }
        if (!data) {
            Invalid("missing bitmap");
        }
        const auto* source = static_cast<const uint8_t*>(data);
        const size_t firstByte = static_cast<uint32_t>(bitOffset) / 8;
        const unsigned shift = bitOffset % 8;
        const uint64_t sourceSize = (rows + static_cast<uint32_t>(bitOffset) + 7) / 8;
        std::vector<uint8_t> packed(size);
        for (size_t i = 0; i < size; ++i) {
            packed[i] = source[firstByte + i] >> shift;
            if (shift != 0 && firstByte + i + 1 < sourceSize) {
                packed[i] |= source[firstByte + i + 1] << (8 - shift);
            }
        }
        if (rows % 8 != 0) {
            packed.back() &= (1u << (rows % 8)) - 1;
        }
        Bytes(packed.data(), packed.size());
    }

private:
    std::ostringstream& Output_;
    size_t Size_ = 0;
};

class TReader {
public:
    explicit TReader(std::istringstream& input)
        : Data_(input.view())
    {
        if (Data_.size() > MaxPayloadSize) {
            Invalid("payload size overflow");
        }
    }

    std::string_view Bytes(size_t size) {
        if (size > Data_.size()) {
            Invalid("truncated payload");
        }
        const auto result = Data_.substr(0, size);
        Data_.remove_prefix(size);
        return result;
    }

    template<std::unsigned_integral T>
    T Value() {
        T value;
        const auto bytes = Bytes(sizeof(value));
        std::memcpy(&value, bytes.data(), sizeof(value));
        if constexpr (std::endian::native == std::endian::big) {
            value = std::byteswap(value);
        }
        return value;
    }

    bool Flag() {
        const auto flag = Value<uint8_t>();
        if (flag > 1) {
            Invalid("invalid presence flag");
        }
        return flag != 0;
    }

    size_t Remaining() const {
        return Data_.size();
    }

    void Finish() const {
        if (!Data_.empty()) {
            Invalid("trailing payload bytes");
        }
    }

private:
    std::string_view Data_;
};

struct TRowSetStorage {
    std::vector<TValueLayout> Layouts;
    std::vector<TColumn> Columns;
    std::vector<std::unique_ptr<char[]>> Buffers;

    char* Allocate(size_t size) {
        if (size == 0) {
            return nullptr;
        }
        auto buffer = std::make_unique<char[]>(size);
        auto* data = buffer.get();
        Buffers.push_back(std::move(buffer));
        return data;
    }

    char* Copy(std::string_view bytes) {
        auto* data = Allocate(bytes.size());
        if (!bytes.empty()) {
            std::memcpy(data, bytes.data(), bytes.size());
        }
        return data;
    }
};

uint64_t OffsetAt(const TColumn& column, uint64_t row) {
    int64_t value = 0;
    const auto* offsets = static_cast<const char*>(column.Offsets);
    if (column.OffsetWidth == 4) {
        int32_t offset;
        std::memcpy(&offset, offsets + row * 4, 4);
        value = offset;
    } else {
        std::memcpy(&value, offsets + row * 8, 8);
    }
    if (value < 0) {
        Invalid("negative string offset");
    }
    return static_cast<uint64_t>(value);
}

void WriteStrings(TWriter& writer, const TColumn& column, uint64_t rows) {
    const auto width = column.OffsetWidth;
    if (width != 4 && width != 8) {
        Invalid("unsupported string offset width");
    }
    ByteSize(rows + 1, width);
    if (!column.Offsets && rows != 0) {
        Invalid("missing string offsets");
    }
    writer.Value<uint8_t>(width);
    const uint64_t first = column.Offsets ? OffsetAt(column, 0) : 0;
    uint64_t last = first;
    for (uint64_t row = 0; row <= rows; ++row) {
        const uint64_t offset = column.Offsets ? OffsetAt(column, row) : 0;
        if (offset < last) {
            Invalid("decreasing string offsets");
        }
        if (width == 4) {
            writer.Value<uint32_t>(offset - first);
        } else {
            writer.Value<uint64_t>(offset - first);
        }
        last = offset;
    }
    const auto bytes = ByteSize(last - first, 1);
    if (bytes != 0 && !column.Data) {
        Invalid("missing string data");
    }
    writer.Bytes(bytes == 0 ? nullptr : column.Data + first, bytes);
}

void ReadStrings(TReader& reader, TRowSetStorage& storage, TColumn& column, uint64_t rows) {
    column.OffsetWidth = reader.Value<uint8_t>();
    const auto width = column.OffsetWidth;
    if (width != 4 && width != 8) {
        Invalid("unsupported string offset width");
    }
    const auto encoded = reader.Bytes(ByteSize(rows + 1, width));
    auto* offsets = storage.Allocate(encoded.size());
    column.Offsets = offsets;
    uint64_t last = 0;
    for (uint64_t row = 0; row <= rows; ++row) {
        uint64_t offset;
        if (width == 4) {
            uint32_t value;
            std::memcpy(&value, encoded.data() + row * 4, 4);
            if constexpr (std::endian::native == std::endian::big) {
                value = std::byteswap(value);
            }
            offset = value;
            std::memcpy(offsets + row * 4, &value, 4);
        } else {
            std::memcpy(&offset, encoded.data() + row * 8, 8);
            if constexpr (std::endian::native == std::endian::big) {
                offset = std::byteswap(offset);
            }
            std::memcpy(offsets + row * 8, &offset, 8);
        }
        const uint64_t maxOffset = width == 4
            ? std::numeric_limits<int32_t>::max()
            : std::numeric_limits<int64_t>::max();
        if (offset > maxOffset || offset < last || (row == 0 && offset != 0)) {
            Invalid("invalid string offsets");
        }
        last = offset;
    }
    column.Data = storage.Copy(reader.Bytes(ByteSize(last, 1)));
}

} // namespace

template<>
void SerializeToStream(const TSpawnComputeActorResponse& message, std::ostringstream& output) {
    TWriter writer(output);
    if (message.ActorIds.size() > MaxPayloadSize / 8) {
        Invalid("too many actors");
    }
    writer.Value<uint32_t>(message.ActorIds.size());
    for (const auto& actor : message.ActorIds) {
        writer.Value<uint16_t>(actor.NodeId());
        writer.Value<uint32_t>(actor.ActorId());
        writer.Value<uint16_t>(actor.Cookie());
    }
    writer.Value<uint32_t>(ByteSize(message.Error.size(), 1));
    writer.Bytes(message.Error.data(), message.Error.size());
}

template<>
void DeserializeFromStream(TSpawnComputeActorResponse& message, std::istringstream& input) {
    TReader reader(input);
    const auto count = reader.Value<uint32_t>();
    if (count > reader.Remaining() / 8) {
        Invalid("truncated actor list");
    }
    TSpawnComputeActorResponse result;
    result.ActorIds.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        const auto node = reader.Value<uint16_t>();
        const auto actor = reader.Value<uint32_t>();
        const auto cookie = reader.Value<uint16_t>();
        result.ActorIds.emplace_back(node, actor, cookie);
    }
    result.Error = reader.Bytes(reader.Value<uint32_t>());
    reader.Finish();
    message = std::move(result);
}

template<>
void SerializeToStream(const TDataChunk& message, std::ostringstream& output) {
    const auto& rowSet = message.RowSet;
    if (rowSet.RowCount < 0 || rowSet.ColumnCount < 0 || rowSet.Selection) {
        Invalid("row set must have nonnegative dimensions and be compacted");
    }
    const uint64_t rows = rowSet.RowCount;
    const uint64_t columns = rowSet.ColumnCount;
    if (columns != 0 && (!rowSet.Columns || !message.Descriptor)) {
        Invalid("missing columns or value layouts");
    }
    ByteSize(columns, 2);
    TWriter writer(output);
    writer.Value<uint64_t>(message.DstTaskId);
    writer.Value<uint64_t>(message.DstLaneId);
    writer.Value<uint64_t>(columns);
    writer.Value<uint64_t>(rows);
    for (uint64_t i = 0; i < columns; ++i) {
        const auto& layout = message.Descriptor[i];
        ValidateLayout(layout);
        if (rowSet.Columns && rowSet.Columns[i].Offsets && !IsStringLayout(layout)) {
            Invalid("string offsets do not match the value layout");
        }
        writer.Value<uint8_t>(layout.Width);
        writer.Value<uint8_t>(layout.Bitmap);
    }
    for (uint64_t i = 0; i < columns; ++i) {
        const auto& column = rowSet.Columns[i];
        const auto& layout = message.Descriptor[i];
        if (layout.Bitmap) {
            writer.Bitmap(column.Data, column.DataBitOffset, rows);
        } else if (IsStringLayout(layout)) {
            WriteStrings(writer, column, rows);
        } else {
            writer.Bytes(column.Data, ByteSize(rows, layout.Width));
        }
        writer.Value<uint8_t>(column.Mask != nullptr);
        if (column.Mask) {
            writer.Bitmap(column.Mask, column.MaskBitOffset, rows);
        }
    }
    writer.Value<uint8_t>(rowSet.Hash != nullptr);
    if (rowSet.Hash) {
        writer.Bytes(rowSet.Hash, ByteSize(rows, sizeof(uint64_t)));
    }
}

template<>
void DeserializeFromStream(TDataChunk& message, std::istringstream& input) {
    TReader reader(input);
    const auto task = reader.Value<uint64_t>();
    const auto lane = reader.Value<uint64_t>();
    const auto columns = reader.Value<uint64_t>();
    const auto rows = reader.Value<uint64_t>();
    if (rows > std::numeric_limits<int64_t>::max() || columns > reader.Remaining() / 2) {
        Invalid("invalid row set dimensions");
    }
    auto storage = std::make_unique<TRowSetStorage>();
    storage->Layouts.reserve(columns);
    for (uint64_t i = 0; i < columns; ++i) {
        TValueLayout layout{
            .Width = reader.Value<uint8_t>(),
            .Bitmap = reader.Flag(),
        };
        ValidateLayout(layout);
        storage->Layouts.push_back(layout);
    }
    storage->Columns.resize(columns);
    for (uint64_t i = 0; i < columns; ++i) {
        auto& column = storage->Columns[i];
        const auto& layout = storage->Layouts[i];
        if (layout.Bitmap) {
            column.Data = storage->Copy(reader.Bytes(BitmapSize(rows)));
        } else if (IsStringLayout(layout)) {
            ReadStrings(reader, *storage, column, rows);
        } else {
            column.Data = storage->Copy(reader.Bytes(ByteSize(rows, layout.Width)));
        }
        if (reader.Flag()) {
            column.Mask = reinterpret_cast<const uint8_t*>(
                storage->Copy(reader.Bytes(BitmapSize(rows))));
        }
    }
    uint64_t* hash = nullptr;
    if (reader.Flag()) {
        hash = reinterpret_cast<uint64_t*>(storage->Copy(reader.Bytes(ByteSize(rows, sizeof(uint64_t)))));
    }
    reader.Finish();
    message.DstTaskId = task;
    message.DstLaneId = lane;
    message.Descriptor = storage->Layouts.data();
    message.RowSet = TRowSet{
        .Columns = storage->Columns.data(),
        .ColumnCount = static_cast<int64_t>(columns),
        .RowCount = static_cast<int64_t>(rows),
        .Selection = nullptr,
        .Hash = hash,
        .Destroy = [](TRowSet* rowSet) {
            delete static_cast<TRowSetStorage*>(rowSet->Private);
        },
        .Private = storage.release(),
        .RefCount = 1,
    };
}

} // namespace NActors
} // namespace NNet
