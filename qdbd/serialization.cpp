#include "serialization.h"

#include <bit>
#include <concepts>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace NNet {
namespace NActors {
namespace {

using NQdb::TColumn;
using NQdb::TRowSet;
using NQdb::TValueLayout;
using NQdb::NRemote::ERemoteEndpointKind;
using NQdb::NRemote::TComputeError;
using NQdb::NRemote::TDataChunk;
using NQdb::NRemote::TRemoteEndpoint;
using NQdb::NRemote::TSpawnComputeActorResponse;
using NQdb::NRemote::TTaskGraphMessage;

static_assert(std::endian::native == std::endian::little, "the chunk layout on the wire is little-endian");

constexpr size_t MaxPayloadSize = std::numeric_limits<uint32_t>::max();
// Id, kind, peer actor (node, actor, cookie) and peer task.
constexpr size_t EndpointSize = 8 + 1 + 2 + 4 + 2 + 8;

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

// Counts the bytes when it has no buffer, otherwise writes them into the reserved region.
class TWriter {
public:
    TWriter() = default;

    explicit TWriter(std::span<char> out)
        : Out_(out)
        , Counting_(false)
    {}

    void Bytes(const void* data, size_t size) {
        if (size > MaxPayloadSize - Size_) {
            Invalid("payload size overflow");
        }
        if (size != 0) {
            if (!data) {
                Invalid("missing buffer");
            }
            if (!Counting_) {
                if (size > Out_.size() - Size_) {
                    Invalid("payload outgrew its size");
                }
                std::memcpy(Out_.data() + Size_, data, size);
            }
        }
        Size_ += size;
    }

    template<std::unsigned_integral T>
    void Value(T value) {
        Bytes(&value, sizeof(value));
    }

    size_t Size() const {
        return Size_;
    }

private:
    std::span<char> Out_;
    bool Counting_ = true;
    size_t Size_ = 0;
};

class TReader {
public:
    explicit TReader(const TBlob& blob)
        : Data_(static_cast<const char*>(blob.Data.get()), blob.Size)
    {}

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
        return value;
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

void Write(TWriter& writer, const TSpawnComputeActorResponse& message) {
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

void Write(TWriter& writer, const TComputeError& message) {
    writer.Value<uint32_t>(ByteSize(message.Message.size(), 1));
    writer.Bytes(message.Message.data(), message.Message.size());
}

void Write(TWriter& writer, const TTaskGraphMessage& message) {
    writer.Value<uint32_t>(ByteSize(message.Spec.size(), 1));
    writer.Bytes(message.Spec.data(), message.Spec.size());
    writer.Value<uint32_t>(ByteSize(message.KernelImage.size(), 1));
    writer.Bytes(message.KernelImage.data(), message.KernelImage.size());
    if (message.Endpoints.size() > MaxPayloadSize / EndpointSize) {
        Invalid("too many endpoints");
    }
    writer.Value<uint32_t>(message.Endpoints.size());
    for (const auto& endpoint : message.Endpoints) {
        writer.Value<uint64_t>(endpoint.Id);
        writer.Value<uint8_t>(static_cast<uint8_t>(endpoint.Kind));
        writer.Value<uint16_t>(endpoint.PeerActor.NodeId());
        writer.Value<uint32_t>(endpoint.PeerActor.ActorId());
        writer.Value<uint16_t>(endpoint.PeerActor.Cookie());
        writer.Value<uint64_t>(endpoint.PeerTaskId);
    }
}

// The small messages are written by one function twice: to count, then to fill.
template<class T>
size_t CountedSize(const T& message) {
    TWriter writer;
    Write(writer, message);
    return writer.Size();
}

template<class T>
void WriteExactly(const T& message, std::span<char> out) {
    TWriter writer(out);
    Write(writer, message);
    if (writer.Size() != out.size()) {
        Invalid("payload does not match its size");
    }
}

// A chunk on the wire: a header, one entry per column, then 16-byte aligned regions
// per column (values or string offsets and bytes, then the mask) and the hashes.
// The received blob is 16-byte aligned, so the row set points into it.
constexpr size_t ChunkAlign = 16;

size_t AlignUp(size_t value) {
    return (value + ChunkAlign - 1) & ~(ChunkAlign - 1);
}

struct TChunkHeader {
    uint64_t DstTaskId;
    uint64_t Seq;
    uint64_t Columns;
    uint64_t Rows;
    uint64_t HasHash;
};

struct TChunkColumn {
    uint8_t Width;
    uint8_t Bitmap;
    uint8_t HasMask;
    uint8_t OffsetWidth;
};

struct TChunkStorage {
    std::vector<TValueLayout> Layouts;
    std::vector<TColumn> Columns;
    TBlob::TRawPtr Bytes;
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

void PutOffset(char* offsets, uint64_t index, uint8_t width, uint64_t value) {
    if (width == 4) {
        const auto narrow = static_cast<uint32_t>(value);
        std::memcpy(offsets + index * 4, &narrow, 4);
    } else {
        std::memcpy(offsets + index * 8, &value, 8);
    }
}

uint64_t SelectedRows(const TRowSet& rowSet) {
    if (!rowSet.Selection) {
        return static_cast<uint64_t>(rowSet.RowCount);
    }
    uint64_t rows = 0;
    for (int64_t row = 0; row < rowSet.RowCount; ++row) {
        rows += rowSet.Selection[row] != 0;
    }
    return rows;
}

// The bytes of the selected strings; checks the offsets the writer then trusts.
uint64_t StringBytes(const TColumn& column, const TRowSet& rowSet) {
    if (column.OffsetWidth != 4 && column.OffsetWidth != 8) {
        Invalid("unsupported string offset width");
    }
    const auto source = static_cast<uint64_t>(rowSet.RowCount);
    if (!column.Offsets) {
        if (source != 0) {
            Invalid("missing string offsets");
        }
        return 0;
    }
    uint64_t total = 0;
    uint64_t previous = OffsetAt(column, 0);
    for (uint64_t row = 0; row < source; ++row) {
        const uint64_t next = OffsetAt(column, row + 1);
        if (next < previous) {
            Invalid("decreasing string offsets");
        }
        if (!rowSet.Selection || rowSet.Selection[row]) {
            total += next - previous;
        }
        previous = next;
    }
    if (total != 0 && !column.Data) {
        Invalid("missing string data");
    }
    return total;
}

size_t ChunkSize(const TDataChunk& message) {
    const auto& rowSet = message.RowSet;
    if (rowSet.RowCount < 0 || rowSet.ColumnCount < 0) {
        Invalid("row set must have nonnegative dimensions");
    }
    if (rowSet.ColumnCount != 0 && (!rowSet.Columns || !message.Descriptor)) {
        Invalid("missing columns or value layouts");
    }
    const uint64_t rows = SelectedRows(rowSet);
    size_t size = AlignUp(sizeof(TChunkHeader) + ByteSize(rowSet.ColumnCount, sizeof(TChunkColumn)));
    for (int64_t i = 0; i < rowSet.ColumnCount; ++i) {
        const auto& layout = message.Descriptor[i];
        const auto& column = rowSet.Columns[i];
        ValidateLayout(layout);
        if (column.Offsets && !IsStringLayout(layout)) {
            Invalid("string offsets do not match the value layout");
        }
        if (layout.Bitmap) {
            if (rows != 0 && !column.Data) {
                Invalid("missing bitmap");
            }
            size += AlignUp(BitmapSize(rows));
        } else if (IsStringLayout(layout)) {
            size = AlignUp(size + ByteSize(rows + 1, column.OffsetWidth == 8 ? 8 : 4));
            size += AlignUp(ByteSize(StringBytes(column, rowSet), 1));
        } else {
            if (rows != 0 && !column.Data) {
                Invalid("missing buffer");
            }
            size += AlignUp(ByteSize(rows, layout.Width));
        }
        if (column.Mask) {
            size += AlignUp(BitmapSize(rows));
        }
        if (size > MaxPayloadSize) {
            Invalid("payload size overflow");
        }
    }
    if (rowSet.Hash) {
        size += AlignUp(ByteSize(rows, sizeof(uint64_t)));
    }
    if (size > MaxPayloadSize) {
        Invalid("payload size overflow");
    }
    return size;
}

void WriteBits(char* out, const void* source, int32_t bitOffset, const uint8_t* selection,
    uint64_t sourceRows, uint64_t rows)
{
    const size_t bytes = BitmapSize(rows);
    if (bytes == 0) {
        return;
    }
    if (bitOffset < 0) {
        Invalid("negative bitmap offset");
    }
    auto* target = reinterpret_cast<uint8_t*>(out);
    const auto* bits = static_cast<const uint8_t*>(source);
    if (!selection && bitOffset % 8 == 0) {
        std::memcpy(target, bits + bitOffset / 8, bytes);
        if (rows % 8 != 0) {
            target[bytes - 1] &= (1u << (rows % 8)) - 1;
        }
        return;
    }
    std::memset(target, 0, bytes);
    uint64_t next = 0;
    for (uint64_t row = 0; row < sourceRows; ++row) {
        if (selection && !selection[row]) {
            continue;
        }
        const uint64_t bit = row + static_cast<uint32_t>(bitOffset);
        target[next / 8] |= ((bits[bit / 8] >> (bit % 8)) & 1) << (next % 8);
        ++next;
    }
}

// Writes the offsets from 0 at `pos` and the bytes in the next aligned region; returns their end.
size_t WriteStrings(char* out, size_t pos, const TColumn& column, const TRowSet& rowSet, uint64_t rows) {
    const uint8_t width = column.OffsetWidth;
    char* offsets = out + pos;
    const size_t bytesStart = AlignUp(pos + (rows + 1) * width);
    char* bytes = out + bytesStart;
    const auto source = static_cast<uint64_t>(rowSet.RowCount);
    if (source == 0) {
        PutOffset(offsets, 0, width, 0);
        return bytesStart;
    }
    if (!rowSet.Selection) {
        const uint64_t first = OffsetAt(column, 0);
        const uint64_t last = OffsetAt(column, source);
        if (first == 0) {
            std::memcpy(offsets, column.Offsets, (rows + 1) * width);
        } else {
            for (uint64_t row = 0; row <= rows; ++row) {
                PutOffset(offsets, row, width, OffsetAt(column, row) - first);
            }
        }
        if (last != first) {
            std::memcpy(bytes, column.Data + first, last - first);
        }
        return bytesStart + (last - first);
    }
    uint64_t next = 0;
    uint64_t total = 0;
    PutOffset(offsets, next++, width, 0);
    for (uint64_t row = 0; row < source; ++row) {
        if (!rowSet.Selection[row]) {
            continue;
        }
        const uint64_t begin = OffsetAt(column, row);
        const uint64_t end = OffsetAt(column, row + 1);
        if (end != begin) {
            std::memcpy(bytes + total, column.Data + begin, end - begin);
        }
        total += end - begin;
        PutOffset(offsets, next++, width, total);
    }
    return bytesStart + total;
}

void WriteValues(char* out, const TColumn& column, const TRowSet& rowSet, uint64_t rows, size_t width) {
    if (rows == 0) {
        return;
    }
    if (!rowSet.Selection) {
        std::memcpy(out, column.Data, rows * width);
        return;
    }
    size_t next = 0;
    for (int64_t row = 0; row < rowSet.RowCount; ++row) {
        if (rowSet.Selection[row]) {
            std::memcpy(out + next * width, column.Data + row * width, width);
            ++next;
        }
    }
}

// The caller has checked the message with ChunkSize, which `out` matches.
void WriteChunk(const TDataChunk& message, std::span<char> out) {
    const auto& rowSet = message.RowSet;
    const uint64_t rows = SelectedRows(rowSet);
    const auto source = static_cast<uint64_t>(rowSet.RowCount);
    char* base = out.data();
    const TChunkHeader header{
        .DstTaskId = message.DstTaskId,
        .Seq = message.Seq,
        .Columns = static_cast<uint64_t>(rowSet.ColumnCount),
        .Rows = rows,
        .HasHash = rowSet.Hash != nullptr,
    };
    std::memcpy(base, &header, sizeof(header));
    size_t pos = sizeof(header);
    for (int64_t i = 0; i < rowSet.ColumnCount; ++i) {
        const auto& layout = message.Descriptor[i];
        const auto& column = rowSet.Columns[i];
        const TChunkColumn entry{
            .Width = static_cast<uint8_t>(layout.Width),
            .Bitmap = layout.Bitmap,
            .HasMask = column.Mask != nullptr,
            .OffsetWidth = IsStringLayout(layout) ? column.OffsetWidth : uint8_t{0},
        };
        std::memcpy(base + pos, &entry, sizeof(entry));
        pos += sizeof(entry);
    }
    // Padding is zeroed: the buffer may hold earlier messages' bytes.
    std::memset(base + pos, 0, AlignUp(pos) - pos);
    pos = AlignUp(pos);
    auto pad = [&](size_t end) {
        std::memset(base + end, 0, AlignUp(end) - end);
        pos = AlignUp(end);
    };
    for (int64_t i = 0; i < rowSet.ColumnCount; ++i) {
        const auto& layout = message.Descriptor[i];
        const auto& column = rowSet.Columns[i];
        if (layout.Bitmap) {
            WriteBits(base + pos, column.Data, column.DataBitOffset, rowSet.Selection, source, rows);
            pad(pos + BitmapSize(rows));
        } else if (IsStringLayout(layout)) {
            const size_t offsetsEnd = pos + (rows + 1) * column.OffsetWidth;
            std::memset(base + offsetsEnd, 0, AlignUp(offsetsEnd) - offsetsEnd);
            pad(WriteStrings(base, pos, column, rowSet, rows));
        } else {
            WriteValues(base + pos, column, rowSet, rows, layout.Width);
            pad(pos + rows * layout.Width);
        }
        if (column.Mask) {
            WriteBits(base + pos, column.Mask, column.MaskBitOffset, rowSet.Selection, source, rows);
            pad(pos + BitmapSize(rows));
        }
    }
    if (rowSet.Hash) {
        if (!rowSet.Selection) {
            std::memcpy(base + pos, rowSet.Hash, rows * sizeof(uint64_t));
        } else {
            size_t next = 0;
            for (uint64_t row = 0; row < source; ++row) {
                if (rowSet.Selection[row]) {
                    std::memcpy(base + pos + next * sizeof(uint64_t), &rowSet.Hash[row], sizeof(uint64_t));
                    ++next;
                }
            }
        }
        pad(pos + rows * sizeof(uint64_t));
    }
    if (pos != out.size()) {
        Invalid("payload does not match its size");
    }
}

bool Flag(uint64_t value) {
    if (value > 1) {
        Invalid("invalid presence flag");
    }
    return value != 0;
}

void CheckStringOffsets(const char* offsets, uint8_t width, uint64_t rows, uint64_t bytes) {
    const uint64_t maxOffset = width == 4
        ? std::numeric_limits<int32_t>::max()
        : std::numeric_limits<int64_t>::max();
    uint64_t last = 0;
    for (uint64_t row = 0; row <= rows; ++row) {
        uint64_t offset = 0;
        if (width == 4) {
            uint32_t value;
            std::memcpy(&value, offsets + row * 4, 4);
            offset = value;
        } else {
            std::memcpy(&offset, offsets + row * 8, 8);
        }
        if (offset > maxOffset || offset < last || (row == 0 && offset != 0) || offset > bytes) {
            Invalid("invalid string offsets");
        }
        last = offset;
    }
}

uint64_t LastOffset(const char* offsets, uint8_t width, uint64_t rows) {
    if (width == 4) {
        uint32_t value;
        std::memcpy(&value, offsets + rows * 4, 4);
        return value;
    }
    uint64_t value;
    std::memcpy(&value, offsets + rows * 8, 8);
    return value;
}

void ReadChunk(TDataChunk& message, TBlob& blob) {
    const size_t size = blob.Size;
    if (size < sizeof(TChunkHeader)) {
        Invalid("truncated payload");
    }
    TChunkHeader header;
    std::memcpy(&header, blob.Data.get(), sizeof(header));
    const uint64_t rows = header.Rows;
    const uint64_t columns = header.Columns;
    if (rows > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
        || columns > (size - sizeof(header)) / sizeof(TChunkColumn))
    {
        Invalid("invalid row set dimensions");
    }
    const bool hasHash = Flag(header.HasHash);
    // An envelope reader may hand out an unaligned blob: the row set then gets an aligned copy.
    TBlob::TRawPtr copy;
    char* base = static_cast<char*>(blob.Data.get());
    if (reinterpret_cast<uintptr_t>(base) % ChunkAlign != 0) {
        copy = TBlob::TRawPtr(::operator new(size), TBlobDeleter{[](void* ptr) {
            ::operator delete(ptr);
        }});
        std::memcpy(copy.get(), base, size);
        base = static_cast<char*>(copy.get());
    }
    auto storage = std::make_unique<TChunkStorage>();
    storage->Layouts.resize(columns);
    storage->Columns.resize(columns);
    size_t pos = AlignUp(sizeof(header) + columns * sizeof(TChunkColumn));
    auto take = [&](size_t length) {
        if (pos > size || length > size - pos) {
            Invalid("truncated payload");
        }
        char* region = base + pos;
        pos += AlignUp(length);
        return region;
    };
    for (uint64_t i = 0; i < columns; ++i) {
        TChunkColumn entry;
        std::memcpy(&entry, base + sizeof(header) + i * sizeof(entry), sizeof(entry));
        auto& layout = storage->Layouts[i];
        auto& column = storage->Columns[i];
        layout = TValueLayout{.Width = entry.Width, .Bitmap = Flag(entry.Bitmap)};
        ValidateLayout(layout);
        column = TColumn{};
        if (IsStringLayout(layout)) {
            const uint8_t width = entry.OffsetWidth;
            if (width != 4 && width != 8) {
                Invalid("unsupported string offset width");
            }
            char* offsets = take(ByteSize(rows + 1, width));
            const uint64_t length = LastOffset(offsets, width, rows);
            if (pos > size || length > size - pos) {
                Invalid("invalid string offsets");
            }
            CheckStringOffsets(offsets, width, rows, length);
            column.Offsets = offsets;
            column.OffsetWidth = width;
            column.Data = take(length);
        } else {
            if (entry.OffsetWidth != 0) {
                Invalid("string offsets do not match the value layout");
            }
            column.Data = take(layout.Bitmap ? BitmapSize(rows) : ByteSize(rows, layout.Width));
        }
        if (Flag(entry.HasMask)) {
            column.Mask = reinterpret_cast<const uint8_t*>(take(BitmapSize(rows)));
        }
    }
    uint64_t* hash = nullptr;
    if (hasHash) {
        hash = reinterpret_cast<uint64_t*>(take(ByteSize(rows, sizeof(uint64_t))));
    }
    if (pos != size) {
        Invalid("trailing payload bytes");
    }
    // The row set keeps the bytes it points into.
    if (copy) {
        storage->Bytes = std::move(copy);
    } else {
        storage->Bytes = std::move(blob.Data);
        blob.Size = 0;
    }
    message.DstTaskId = header.DstTaskId;
    message.Seq = header.Seq;
    message.Descriptor = storage->Layouts.data();
    message.RowSet = TRowSet{
        .Columns = storage->Columns.data(),
        .ColumnCount = static_cast<int64_t>(columns),
        .RowCount = static_cast<int64_t>(rows),
        .Selection = nullptr,
        .Hash = hash,
        .Destroy = [](TRowSet* rowSet) {
            delete static_cast<TChunkStorage*>(rowSet->Private);
        },
        .Private = storage.release(),
        .RefCount = 1,
    };
}

} // namespace

template<>
size_t SerializedSize(const TSpawnComputeActorResponse& message) {
    return CountedSize(message);
}

template<>
void SerializeTo(const TSpawnComputeActorResponse& message, std::span<char> out) {
    WriteExactly(message, out);
}

template<>
void DeserializeFrom(TSpawnComputeActorResponse& message, TBlob& blob) {
    TReader reader(blob);
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
size_t SerializedSize(const TComputeError& message) {
    return CountedSize(message);
}

template<>
void SerializeTo(const TComputeError& message, std::span<char> out) {
    WriteExactly(message, out);
}

template<>
void DeserializeFrom(TComputeError& message, TBlob& blob) {
    TReader reader(blob);
    TComputeError result;
    result.Message = reader.Bytes(reader.Value<uint32_t>());
    reader.Finish();
    message = std::move(result);
}

template<>
size_t SerializedSize(const TTaskGraphMessage& message) {
    return CountedSize(message);
}

template<>
void SerializeTo(const TTaskGraphMessage& message, std::span<char> out) {
    WriteExactly(message, out);
}

template<>
void DeserializeFrom(TTaskGraphMessage& message, TBlob& blob) {
    TReader reader(blob);
    TTaskGraphMessage result;
    result.Spec = reader.Bytes(reader.Value<uint32_t>());
    result.KernelImage = reader.Bytes(reader.Value<uint32_t>());
    const auto count = reader.Value<uint32_t>();
    if (count > reader.Remaining() / EndpointSize) {
        Invalid("truncated endpoint list");
    }
    result.Endpoints.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        TRemoteEndpoint endpoint{.Id = reader.Value<uint64_t>()};
        const auto kind = reader.Value<uint8_t>();
        if (kind > static_cast<uint8_t>(ERemoteEndpointKind::Sink)) {
            Invalid("invalid endpoint kind");
        }
        endpoint.Kind = static_cast<ERemoteEndpointKind>(kind);
        const auto node = reader.Value<uint16_t>();
        const auto actor = reader.Value<uint32_t>();
        const auto cookie = reader.Value<uint16_t>();
        endpoint.PeerActor = TActorId(node, actor, cookie);
        endpoint.PeerTaskId = reader.Value<uint64_t>();
        result.Endpoints.push_back(endpoint);
    }
    reader.Finish();
    message = std::move(result);
}

template<>
size_t SerializedSize(const TDataChunk& message) {
    return ChunkSize(message);
}

template<>
void SerializeTo(const TDataChunk& message, std::span<char> out) {
    WriteChunk(message, out);
}

template<>
void DeserializeFrom(TDataChunk& message, TBlob& blob) {
    ReadChunk(message, blob);
}

} // namespace NActors
} // namespace NNet
