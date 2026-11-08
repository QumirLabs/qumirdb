#include <qdbd/serialization.h>
#include <qdbd/actors.h>
#include <qdbd/actors/compute.h>
#include <qdbd/actors/collector.h>
#include <qdbd/actors/pinger.h>
#include <qdbd/actors/local_scheduler.h>
#include <qdbd/actors/scheduler.h>
#include <qdbd/http/router.h>
#include <qdbd/scheduler/remote_task.h>
#include <qdb/scheduler/impl/threaded_scheduler.h>
#include <qdb/scheduler/channel.h>
#include <qdb/scheduler/connections/local.h>

#include <coroio/all.hpp>
#include <coroio/actors/actorsystem.hpp>

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <chrono>
#include <cstring>
#include <deque>
#include <limits>
#include <optional>
#include <map>
#include <mutex>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <thread>
#include <unordered_set>

namespace {

using namespace NQdb;
using namespace NQdb::NRemote;
using namespace NNet::NActors;

template<class T>
std::string Encode(const T& message) {
    std::ostringstream output;
    SerializeToStream(message, output);
    return output.str();
}

struct TDecodedChunk {
    TDataChunk Message;

    TDecodedChunk() = default;

    explicit TDecodedChunk(const std::string& bytes) {
        std::istringstream input(bytes);
        DeserializeFromStream(Message, input);
    }

    ~TDecodedChunk() {
        if (Message.RowSet.RefCount > 0) {
            Release(&Message.RowSet);
        }
    }

    TDecodedChunk(const TDecodedChunk&) = delete;
    TDecodedChunk& operator=(const TDecodedChunk&) = delete;
};

template<class T>
void Replace(std::string& bytes, size_t offset, T value) {
    if constexpr (std::endian::native == std::endian::big) {
        value = std::byteswap(value);
    }
    ASSERT_LE(offset + sizeof(value), bytes.size());
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

struct TMixedBatch {
    std::array<char, 48> Fixed;
    std::array<uint8_t, 2> Booleans{0x80, 0x02};
    std::array<uint8_t, 1> Mask{0x40};
    std::string Chars{"xxa\0bcde", 8};
    std::array<int32_t, 4> Offsets32{2, 2, 5, 8};
    std::array<int64_t, 4> Offsets64{2, 2, 5, 8};
    std::array<uint64_t, 3> Hash{0, 123, std::numeric_limits<uint64_t>::max()};
    std::array<TValueLayout, 4> Layouts{{{.Width = 16}, {.Bitmap = true}, {}, {}}};
    std::array<TColumn, 4> Columns{};
    TDataChunk Message;

    TMixedBatch() {
        for (size_t i = 0; i < Fixed.size(); ++i) {
            Fixed[i] = static_cast<char>(i);
        }
        Columns[0].Data = Fixed.data();
        Columns[1] = TColumn{
            .Data = reinterpret_cast<char*>(Booleans.data()),
            .DataBitOffset = 7,
            .Mask = Mask.data(),
            .MaskBitOffset = 5,
        };
        Columns[2] = TColumn{
            .Data = Chars.data(),
            .Offsets = Offsets32.data(),
            .OffsetWidth = 4,
        };
        Columns[3] = TColumn{
            .Data = Chars.data(),
            .Mask = Mask.data(),
            .MaskBitOffset = 5,
            .Offsets = Offsets64.data(),
            .OffsetWidth = 8,
        };
        Message.DstTaskId = 123;
        Message.Seq = 7;
        Message.Descriptor = Layouts.data();
        Message.RowSet = TRowSet{
            .Columns = Columns.data(),
            .ColumnCount = static_cast<int64_t>(Columns.size()),
            .RowCount = 3,
            .Hash = Hash.data(),
        };
    }
};

void CheckMixed(const TDataChunk& message) {
    EXPECT_EQ(message.DstTaskId, 123);
    EXPECT_EQ(message.Seq, 7);
    const auto& rows = message.RowSet;
    ASSERT_EQ(rows.ColumnCount, 4);
    ASSERT_EQ(rows.RowCount, 3);
    EXPECT_EQ(rows.Selection, nullptr);
    EXPECT_EQ(rows.RefCount, 1);
    ASSERT_NE(rows.Destroy, nullptr);
    ASSERT_NE(message.Descriptor, nullptr);
    EXPECT_EQ(message.Descriptor[0].Width, 16);
    EXPECT_TRUE(message.Descriptor[1].Bitmap);
    for (int i = 0; i < 48; ++i) {
        EXPECT_EQ(rows.Columns[0].Data[i], static_cast<char>(i));
    }
    EXPECT_EQ(rows.Columns[0].Mask, nullptr);
    EXPECT_EQ(reinterpret_cast<const uint8_t*>(rows.Columns[1].Data)[0], 5);
    EXPECT_EQ(rows.Columns[1].Mask[0], 2);
    for (const auto& column : std::span(rows.Columns, rows.ColumnCount)) {
        EXPECT_EQ(column.DataBitOffset, 0);
        EXPECT_EQ(column.MaskBitOffset, 0);
    }
    EXPECT_EQ(rows.Columns[0].OffsetWidth, 0);
    EXPECT_EQ(rows.Columns[0].Offsets, nullptr);
    EXPECT_EQ(rows.Columns[1].OffsetWidth, 0);
    EXPECT_EQ(rows.Columns[1].Offsets, nullptr);
    const std::array<int32_t, 4> offsets32{0, 0, 3, 6};
    const std::array<int64_t, 4> offsets64{0, 0, 3, 6};
    EXPECT_EQ(rows.Columns[2].OffsetWidth, 4);
    EXPECT_EQ(rows.Columns[3].OffsetWidth, 8);
    EXPECT_EQ(std::memcmp(rows.Columns[2].Offsets, offsets32.data(), sizeof(offsets32)), 0);
    EXPECT_EQ(std::memcmp(rows.Columns[3].Offsets, offsets64.data(), sizeof(offsets64)), 0);
    EXPECT_EQ(std::string(rows.Columns[2].Data, 6), std::string("a\0bcde", 6));
    EXPECT_EQ(std::string(rows.Columns[3].Data, 6), std::string("a\0bcde", 6));
    EXPECT_EQ(rows.Columns[2].Mask, nullptr);
    EXPECT_EQ(rows.Columns[3].Mask[0], 2);
    ASSERT_NE(rows.Hash, nullptr);
    EXPECT_EQ(rows.Hash[0], 0);
    EXPECT_EQ(rows.Hash[1], 123);
    EXPECT_EQ(rows.Hash[2], std::numeric_limits<uint64_t>::max());
}

struct TAllocator {
    void* Acquire(size_t size) {
        return ::operator new(size);
    }

    void Release(void* data) {
        ::operator delete(data);
    }
};

} // namespace

TEST(DistributedSerialization, ActorIds) {
    for (size_t count : {0, 1, 2, 10}) {
        SCOPED_TRACE(count);
        TSpawnComputeActorResponse original;
        for (size_t i = 0; i < count; ++i) {
            original.ActorIds.emplace_back(i + 1, 100 + i, 20 + i);
        }
        const auto bytes = Encode(original);
        EXPECT_EQ(bytes.size(), 8 + 8 * count);
        std::istringstream input(bytes);
        TSpawnComputeActorResponse decoded;
        DeserializeFromStream(decoded, input);
        ASSERT_EQ(decoded.ActorIds.size(), count);
        for (size_t i = 0; i < count; ++i) {
            EXPECT_EQ(decoded.ActorIds[i].NodeId(), i + 1);
            EXPECT_EQ(decoded.ActorIds[i].ActorId(), 100 + i);
            EXPECT_EQ(decoded.ActorIds[i].Cookie(), 20 + i);
        }
    }
}

TEST(DistributedSerialization, ActorListRejectsTruncationAndTrailingBytes) {
    TSpawnComputeActorResponse original{{TActorId(2, 123, 4), TActorId(5, 0, 6)}};
    const auto bytes = Encode(original);
    for (size_t size = 0; size < bytes.size(); ++size) {
        SCOPED_TRACE(size);
        std::istringstream input(bytes.substr(0, size));
        TSpawnComputeActorResponse decoded;
        EXPECT_THROW(DeserializeFromStream(decoded, input), std::runtime_error);
    }
    std::istringstream trailing(bytes + 'x');
    TSpawnComputeActorResponse decoded;
    EXPECT_THROW(DeserializeFromStream(decoded, trailing), std::runtime_error);
    auto invalid = bytes;
    Replace(invalid, 0, std::numeric_limits<uint32_t>::max());
    std::istringstream input(invalid);
    EXPECT_THROW(DeserializeFromStream(decoded, input), std::runtime_error);
}

TEST(DistributedSerialization, MixedColumnsAndSlicedBuffers) {
    TMixedBatch original;
    TDecodedChunk decoded(Encode(original.Message));
    CheckMixed(decoded.Message);
    EXPECT_EQ(original.Offsets32.front(), 2);
    EXPECT_EQ(original.Columns[1].DataBitOffset, 7);
}

TEST(DistributedSerialization, UsesCodecThroughCoroioFarPath) {
    EXPECT_FALSE(is_pod_v<TDataChunk>);
    TMixedBatch original;
    TAllocator allocator;
    auto near = SerializeNear<TDataChunk>(allocator, original.Message);
    auto far = SerializeFar<TDataChunk>(std::move(near));
    TDecodedChunk decoded;
    decoded.Message = DeserializeFar<TDataChunk>(far);
    CheckMixed(decoded.Message);
}

TEST(DistributedSerialization, UsesCodecForDirectRemoteSend) {
    struct TStream {
        std::vector<char> Buffer;
        size_t Committed = 0;

        std::span<char> Acquire(size_t size) {
            Buffer.resize(size);
            return Buffer;
        }

        void Commit(size_t size) {
            Committed = size;
        }
    } stream;
    TMixedBatch original;
    SerializeFarInplace<TDataChunk>(stream, TActorId(1, 2, 3), TActorId(4, 5, 6), original.Message);
    ASSERT_EQ(stream.Committed, stream.Buffer.size());
    ASSERT_GE(stream.Buffer.size(), sizeof(THeader));
    THeader header;
    std::memcpy(&header, stream.Buffer.data(), sizeof(header));
    EXPECT_EQ(header.MessageId, TDataChunk::MessageId);
    EXPECT_EQ(header.Size, stream.Buffer.size() - sizeof(THeader));
    TDecodedChunk decoded(std::string(stream.Buffer.data() + sizeof(THeader), header.Size));
    CheckMixed(decoded.Message);
}

TEST(DistributedSerialization, FixedWidthsAndNullMasks) {
    for (int width : {1, 2, 4, 8, 16}) {
        for (bool nullable : {false, true}) {
            SCOPED_TRACE(std::to_string(width) + "/" + std::to_string(nullable));
            std::vector<char> data(3 * width);
            for (size_t i = 0; i < data.size(); ++i) {
                data[i] = static_cast<char>(i + 1);
            }
            uint8_t mask = 0;
            TColumn column{.Data = data.data(), .Mask = nullable ? &mask : nullptr};
            TValueLayout layout{.Width = width};
            TDataChunk original;
            original.Descriptor = &layout;
            original.RowSet = {.Columns = &column, .ColumnCount = 1, .RowCount = 3};
            TDecodedChunk decoded(Encode(original));
            EXPECT_EQ(std::memcmp(decoded.Message.RowSet.Columns[0].Data, data.data(), data.size()), 0);
            const auto* validity = decoded.Message.RowSet.Columns[0].Mask;
            if (nullable) {
                ASSERT_NE(validity, nullptr);
                EXPECT_EQ(*validity, 0);
            } else {
                EXPECT_EQ(validity, nullptr);
            }
            EXPECT_EQ(decoded.Message.RowSet.Hash, nullptr);
        }
    }
}

TEST(DistributedSerialization, BitmapSlicesAcrossByteBoundaries) {
    for (int offset = 0; offset < 16; ++offset) {
        for (int rows : {1, 7, 8, 9, 16}) {
            SCOPED_TRACE(std::to_string(offset) + "/" + std::to_string(rows));
            std::array<uint8_t, 4> bits{0x69, 0xa5, 0x3c, 0xff};
            TColumn column{
                .Data = reinterpret_cast<char*>(bits.data()),
                .DataBitOffset = offset,
                .Mask = bits.data(),
                .MaskBitOffset = offset,
            };
            TValueLayout layout{.Bitmap = true};
            TDataChunk original;
            original.Descriptor = &layout;
            original.RowSet = {.Columns = &column, .ColumnCount = 1, .RowCount = rows};
            TDecodedChunk decoded(Encode(original));
            const auto& result = decoded.Message.RowSet.Columns[0];
            for (int row = 0; row < rows; ++row) {
                const auto expected = (bits[(row + offset) / 8] >> ((row + offset) % 8)) & 1;
                EXPECT_EQ((static_cast<uint8_t>(result.Data[row / 8]) >> (row % 8)) & 1, expected);
                EXPECT_EQ((result.Mask[row / 8] >> (row % 8)) & 1, expected);
            }
        }
    }
}

TEST(DistributedSerialization, SelectedRowsAreWrittenWithoutChangingTheSource) {
    // Rows 0, 2, 3, 6 of 7 are selected; every column kind, with and without a mask.
    std::array<uint8_t, 7> selection{1, 0, 1, 1, 0, 0, 1};
    const std::array<int, 4> kept{0, 2, 3, 6};
    for (bool nullable : {false, true}) {
        for (bool hashed : {false, true}) {
            SCOPED_TRACE(std::to_string(nullable) + "/" + std::to_string(hashed));
            std::array<int32_t, 7> fixed{10, 11, 12, 13, 14, 15, 16};
            std::array<uint8_t, 3> bits{0xb5, 0x6a, 0x93};
            std::array<uint8_t, 3> mask{0x5c, 0xe1, 0x27};
            std::string chars = "aaabbbbcdddeeef";
            // Row lengths 1, 0, 3, 2, 4, 0, 1 -> 7 strings, offsets start from 2.
            std::array<int32_t, 8> offsets32{2, 3, 3, 6, 8, 12, 12, 13};
            std::array<int64_t, 8> offsets64{2, 3, 3, 6, 8, 12, 12, 13};
            std::array<uint64_t, 7> hash{100, 101, 102, 103, 104, 105, 106};
            std::array<TValueLayout, 4> layouts{{{.Width = 4}, {.Bitmap = true}, {}, {}}};
            std::array<TColumn, 4> columns{};
            columns[0] = TColumn{.Data = reinterpret_cast<char*>(fixed.data()),
                .Mask = nullable ? mask.data() : nullptr, .MaskBitOffset = 3};
            columns[1] = TColumn{.Data = reinterpret_cast<char*>(bits.data()), .DataBitOffset = 5,
                .Mask = nullable ? mask.data() : nullptr, .MaskBitOffset = 9};
            columns[2] = TColumn{.Data = chars.data(), .Mask = nullable ? mask.data() : nullptr,
                .MaskBitOffset = 1, .Offsets = offsets32.data(), .OffsetWidth = 4};
            columns[3] = TColumn{.Data = chars.data(), .Offsets = offsets64.data(), .OffsetWidth = 8};
            TDataChunk original;
            original.Descriptor = layouts.data();
            original.RowSet = {.Columns = columns.data(), .ColumnCount = 4, .RowCount = 7,
                .Selection = selection.data(), .Hash = hashed ? hash.data() : nullptr};

            const auto fixedBefore = fixed;
            const auto bitsBefore = bits;
            const auto maskBefore = mask;
            const auto offsets32Before = offsets32;
            const auto offsets64Before = offsets64;
            const auto hashBefore = hash;
            const auto charsBefore = chars;
            const auto columnsBefore = columns;
            TDecodedChunk decoded(Encode(original));
            EXPECT_EQ(fixed, fixedBefore);
            EXPECT_EQ(bits, bitsBefore);
            EXPECT_EQ(mask, maskBefore);
            EXPECT_EQ(offsets32, offsets32Before);
            EXPECT_EQ(offsets64, offsets64Before);
            EXPECT_EQ(hash, hashBefore);
            EXPECT_EQ(chars, charsBefore);
            EXPECT_EQ(std::memcmp(columns.data(), columnsBefore.data(), sizeof(columns)), 0);
            EXPECT_EQ(original.RowSet.RowCount, 7);
            EXPECT_EQ(original.RowSet.Selection, selection.data());

            const auto& result = decoded.Message.RowSet;
            ASSERT_EQ(result.RowCount, 4);
            EXPECT_EQ(result.Selection, nullptr);
            const auto bit = [](const void* data, int offset, int row) {
                const int index = row + offset;
                return (static_cast<const uint8_t*>(data)[index / 8] >> (index % 8)) & 1;
            };
            for (int i = 0; i < 4; ++i) {
                const int row = kept[i];
                int32_t value;
                std::memcpy(&value, result.Columns[0].Data + i * 4, 4);
                EXPECT_EQ(value, fixed[row]);
                EXPECT_EQ(bit(result.Columns[1].Data, 0, i), bit(bits.data(), 5, row));
                for (int c : {2, 3}) {
                    const auto& column = result.Columns[c];
                    const int64_t from = c == 2 ? offsets32[row] : offsets64[row];
                    const int64_t to = c == 2 ? offsets32[row + 1] : offsets64[row + 1];
                    int64_t begin = 0;
                    int64_t end = 0;
                    if (c == 2) {
                        ASSERT_EQ(column.OffsetWidth, 4);
                        int32_t b, e;
                        std::memcpy(&b, static_cast<const char*>(column.Offsets) + i * 4, 4);
                        std::memcpy(&e, static_cast<const char*>(column.Offsets) + (i + 1) * 4, 4);
                        begin = b;
                        end = e;
                    } else {
                        ASSERT_EQ(column.OffsetWidth, 8);
                        std::memcpy(&begin, static_cast<const char*>(column.Offsets) + i * 8, 8);
                        std::memcpy(&end, static_cast<const char*>(column.Offsets) + (i + 1) * 8, 8);
                    }
                    EXPECT_EQ(std::string(column.Data + begin, end - begin), chars.substr(from, to - from));
                }
                if (nullable) {
                    EXPECT_EQ(bit(result.Columns[0].Mask, 0, i), bit(mask.data(), 3, row));
                    EXPECT_EQ(bit(result.Columns[1].Mask, 0, i), bit(mask.data(), 9, row));
                    EXPECT_EQ(bit(result.Columns[2].Mask, 0, i), bit(mask.data(), 1, row));
                }
                if (hashed) {
                    EXPECT_EQ(result.Hash[i], hash[row]);
                }
            }
            if (!nullable) {
                for (const auto& column : std::span(result.Columns, 4)) {
                    EXPECT_EQ(column.Mask, nullptr);
                }
            }
            EXPECT_EQ(result.Columns[3].Mask, nullptr);
            EXPECT_EQ(result.Hash == nullptr, !hashed);
        }
    }
}

TEST(DistributedSerialization, EmptyBatches) {
    TDataChunk original;
    TDecodedChunk zeroColumns(Encode(original));
    EXPECT_EQ(zeroColumns.Message.RowSet.ColumnCount, 0);
    EXPECT_EQ(zeroColumns.Message.RowSet.RowCount, 0);
    std::array<TColumn, 3> columns{};
    columns[2].OffsetWidth = 4;
    std::array<TValueLayout, 3> layouts{{{.Width = 8}, {.Bitmap = true}, {}}};
    original.Descriptor = layouts.data();
    original.RowSet.Columns = columns.data();
    original.RowSet.ColumnCount = columns.size();
    TDecodedChunk empty(Encode(original));
    EXPECT_EQ(empty.Message.RowSet.RowCount, 0);
    ASSERT_EQ(empty.Message.RowSet.ColumnCount, 3);
    EXPECT_EQ(*static_cast<const int32_t*>(empty.Message.RowSet.Columns[2].Offsets), 0);
}

TEST(DistributedSerialization, RejectsEveryTruncatedPrefixWithoutChangingOutput) {
    TMixedBatch original;
    const auto bytes = Encode(original.Message);
    for (size_t size = 0; size < bytes.size(); ++size) {
        SCOPED_TRACE(size);
        std::istringstream input(bytes.substr(0, size));
        TDataChunk output;
        output.DstTaskId = 999;
        EXPECT_THROW(DeserializeFromStream(output, input), std::runtime_error);
        EXPECT_EQ(output.DstTaskId, 999);
        EXPECT_EQ(output.RowSet.Private, nullptr);
    }
    EXPECT_THROW(TDecodedChunk{bytes + 'x'}, std::runtime_error);
}

TEST(DistributedSerialization, RejectsInvalidDimensionsAndLayouts) {
    TMixedBatch original;
    const auto bytes = Encode(original.Message);
    for (size_t offset : {16, 24}) {
        auto invalid = bytes;
        Replace(invalid, offset, std::numeric_limits<uint64_t>::max());
        EXPECT_THROW(TDecodedChunk{invalid}, std::runtime_error);
    }
    for (const auto [offset, value] : std::array<std::pair<size_t, uint8_t>, 3>{{{32, 3}, {33, 2}, {33, 1}}}) {
        auto invalid = bytes;
        Replace(invalid, offset, value);
        EXPECT_THROW(TDecodedChunk{invalid}, std::runtime_error);
    }
    auto huge = bytes;
    Replace(huge, 24, static_cast<uint64_t>(std::numeric_limits<int64_t>::max()));
    EXPECT_THROW(TDecodedChunk{huge}, std::runtime_error);
    TDataChunk empty;
    auto flag = Encode(empty);
    flag.back() = 2;
    EXPECT_THROW(TDecodedChunk{flag}, std::runtime_error);
}

TEST(DistributedSerialization, RejectsInvalidStringOffsets) {
    std::array<int32_t, 4> offsets{0, 0, 3, 6};
    std::string data = "abcdef";
    TValueLayout layout{};
    TColumn column{.Data = data.data(), .Offsets = offsets.data(), .OffsetWidth = 4};
    TDataChunk message;
    message.Descriptor = &layout;
    message.RowSet = {.Columns = &column, .ColumnCount = 1, .RowCount = 3};
    const auto bytes = Encode(message);
    auto invalid = bytes;
    Replace(invalid, 34, uint8_t{3});
    EXPECT_THROW(TDecodedChunk{invalid}, std::runtime_error);
    for (uint32_t first : {1u, std::numeric_limits<uint32_t>::max()}) {
        invalid = bytes;
        Replace(invalid, 35, first);
        EXPECT_THROW(TDecodedChunk{invalid}, std::runtime_error);
    }
    for (uint32_t last : {2u, 1000u}) {
        invalid = bytes;
        Replace(invalid, 47, last);
        EXPECT_THROW(TDecodedChunk{invalid}, std::runtime_error);
    }
}

TEST(DistributedSerialization, RejectsInvalidSourceBatches) {
    TMixedBatch batch;
    batch.Message.RowSet.RowCount = -1;
    EXPECT_THROW(Encode(batch.Message), std::runtime_error);
    batch.Message.RowSet.RowCount = 3;
    batch.Message.RowSet.ColumnCount = -1;
    EXPECT_THROW(Encode(batch.Message), std::runtime_error);
    batch.Message.RowSet.ColumnCount = 4;
    batch.Message.Descriptor = nullptr;
    EXPECT_THROW(Encode(batch.Message), std::runtime_error);
    batch.Message.Descriptor = batch.Layouts.data();
    batch.Columns[0].Data = nullptr;
    EXPECT_THROW(Encode(batch.Message), std::runtime_error);
    batch.Columns[0].Data = batch.Fixed.data();
    // A fixed-width layout over a column that carries string offsets.
    batch.Columns[0].Offsets = batch.Offsets32.data();
    EXPECT_THROW(Encode(batch.Message), std::runtime_error);
    batch.Columns[0].Offsets = nullptr;
    batch.Offsets32[2] = -1;
    EXPECT_THROW(Encode(batch.Message), std::runtime_error);
}

namespace {

struct TFakeTimeSource : ITimeSource {
    NNet::TTime Now{};

    NNet::TTime CurrentTime() const override {
        return Now;
    }
};

class TMemoryNode : public INode {
public:
    explicit TMemoryNode(TActorSystem& peer)
        : Peer_(peer)
    {
        RegisterMessages(Factory_);
    }

    void Send(TEnvelope&& envelope) override {
        auto blob = std::move(envelope.Blob);
        if (blob.Type == TBlob::PointerType::Near) {
            blob = Factory_.SerializeFar(envelope.MessageId, std::move(blob));
        }
        Peer_.Send(envelope.Sender, envelope.Recipient, envelope.MessageId, std::move(blob));
    }

    std::span<char> Acquire(size_t size) override {
        Buffer_.resize(size);
        return Buffer_;
    }

    void Commit(size_t size) override {
        ASSERT_GE(size, sizeof(THeader));
        THeader header;
        std::memcpy(&header, Buffer_.data(), sizeof(header));
        ASSERT_EQ(size, sizeof(header) + header.Size);
        auto* data = ::operator new(header.Size);
        std::memcpy(data, Buffer_.data() + sizeof(header), header.Size);
        TBlob blob{
            .Data = TBlob::TRawPtr(data, TBlobDeleter{[](void* pointer) {
                ::operator delete(pointer);
            }}),
            .Size = header.Size,
            .Type = TBlob::PointerType::Far,
        };
        Peer_.Send(header.Sender, header.Recipient, header.MessageId, std::move(blob));
    }

    void StartConnect() override {}
    void Drain() override {}

    NNet::THostPort GetHostPort() const override {
        return NNet::THostPort("memory", 0);
    }

private:
    TActorSystem& Peer_;
    TMessagesFactory Factory_;
    std::vector<char> Buffer_;
};

template<class T>
T ReadMessage(const TBlob& blob) {
    if (blob.Type == TBlob::PointerType::Near) {
        return DeserializeNear<T>(blob);
    }
    return DeserializeFar<T>(blob);
}

class TResponseProbe : public IActor {
public:
    explicit TResponseProbe(std::vector<TSpawnComputeActorResponse>& responses)
        : Responses_(responses)
    {}

    void Receive(TMessageId messageId, TBlob blob, TActorContext::TPtr) override {
        ASSERT_EQ(messageId, TSpawnComputeActorResponse::MessageId);
        Responses_.push_back(ReadMessage<TSpawnComputeActorResponse>(blob));
    }

private:
    std::vector<TSpawnComputeActorResponse>& Responses_;
};

enum class EReply { Result, QueryError, Unavailable };

class TQueryReplyProbe : public IActor {
public:
    explicit TQueryReplyProbe(EReply mode)
        : Mode_(mode)
    {}

    void Receive(TMessageId messageId, TBlob, TActorContext::TPtr ctx) override {
        ASSERT_EQ(messageId, TExecuteQuery::MessageId);
        TExecuteQueryResponse response{
            .Response = Mode_ != EReply::Result
                ? std::expected<std::string, NQumir::TError>(std::unexpected(NQumir::TError("query failed")))
                : std::expected<std::string, NQumir::TError>("query result"),
            .Unavailable = Mode_ == EReply::Unavailable,
        };
        ctx->Send<TExecuteQueryResponse>(ctx->Sender(), std::move(response));
    }

private:
    EReply Mode_;
};

struct TWorkerState {
    std::string Error;
    bool DropReply = false;
    bool DuplicateReply = false;
    bool MalformedCount = false;
    TActorId Collector;
    std::optional<TSpawnComputeActor> LastRequest;
    std::optional<TSpawnComputeActorResponse> LastResponse;
    std::shared_ptr<TRuntimeResources> Resources = std::make_shared<TRuntimeResources>();
};

class TSpawnProbe : public IActor {
public:
    TSpawnProbe(std::vector<TActorId>& requests, std::shared_ptr<TWorkerState> state)
        : Requests_(requests)
        , State_(std::move(state))
    {}

    void Receive(TMessageId messageId, TBlob blob, TActorContext::TPtr ctx) override {
        ASSERT_EQ(messageId, TSpawnComputeActor::MessageId);
        const auto request = ReadMessage<TSpawnComputeActor>(blob);
        Requests_.push_back(ctx->Self());
        State_->LastRequest = request;
        State_->Collector = ctx->Sender();
        TSpawnComputeActorResponse response{
            .Error = State_->Error,
        };
        if (response.Error.empty()) {
            const uint32_t count = request.Count - (State_->MalformedCount && request.Count != 0);
            for (uint32_t i = 0; i < count; ++i) {
                response.ActorIds.push_back(ctx->Register(
                    std::unique_ptr<IActor>(CreateComputeActor(State_->Resources))));
            }
        }
        State_->LastResponse = response;
        if (!State_->DropReply) {
            ctx->Send<TSpawnComputeActorResponse>(ctx->Sender(), response);
            if (State_->DuplicateReply) {
                ctx->Send<TSpawnComputeActorResponse>(ctx->Sender(), response);
            }
        }
    }

private:
    std::vector<TActorId>& Requests_;
    std::shared_ptr<TWorkerState> State_;
};

struct TTestCluster {
    NNet::TLoop<NNet::TDefaultPoller> Loop;
    TFakeTimeSource Clock;
    std::vector<TSpawnComputeActorResponse> Responses;
    std::vector<TActorId> Requests;
    std::map<std::string, std::shared_ptr<TWorkerState>> Workers;
    TActorSystem Leader;
    TActorSystem Follower;
    TActorId LeaderScheduler;
    TActorId FollowerScheduler;
    TActorId Client;

    explicit TTestCluster(TNodeId leaderId = 2)
        : Leader(&Loop.Poller(), leaderId)
        , Follower(&Loop.Poller(), leaderId == 1 ? 2 : 1)
    {
        Loop.Poller().SetMaxDuration(std::chrono::milliseconds(0));
        Leader.AddNode(leaderId == 1 ? 2 : 1, std::make_unique<TMemoryNode>(Follower));
        Follower.AddNode(leaderId, std::make_unique<TMemoryNode>(Leader));
        Leader.Serve();
        Follower.Serve();
        auto logger = [](const std::string&) {};
        LeaderScheduler = Leader.RegisterWellKnown(
            static_cast<TLocalActorId>(EWellKnownActors::Scheduler),
            std::unique_ptr<IActor>(CreateSchedulerActor(&Clock, leaderId, logger)));
        FollowerScheduler = Follower.RegisterWellKnown(
            static_cast<TLocalActorId>(EWellKnownActors::Scheduler),
            std::unique_ptr<IActor>(CreateSchedulerActor(&Clock, leaderId, logger)));
        Client = Follower.Register(std::make_unique<TResponseProbe>(Responses));
    }

    void Pump() {
        for (int i = 0; i < 4; ++i) {
            Loop.Step();
        }
    }

    TActorId AddWorker(bool onLeader, TLocalActorId id) {
        auto& system = onLeader ? Leader : Follower;
        auto state = std::make_shared<TWorkerState>();
        const auto actor = system.RegisterWellKnown(id, std::make_unique<TSpawnProbe>(Requests, state));
        Workers.emplace(actor.ToString(), std::move(state));
        return actor;
    }

    TWorkerState& Worker(TActorId actor) {
        return *Workers.at(actor.ToString());
    }

    void Wake(TActorId actor) {
        auto& system = actor.NodeId() == LeaderScheduler.NodeId() ? Leader : Follower;
        system.Send<TWakeUp>(Client, actor, TWakeUp{});
        Pump();
    }

    void Reply(TActorId worker, const TSpawnComputeActorResponse& response) {
        auto& system = worker.NodeId() == LeaderScheduler.NodeId() ? Leader : Follower;
        system.Send<TSpawnComputeActorResponse>(worker, Worker(worker).Collector, response);
        Pump();
    }

    void Ping(TActorId worker) {
        if (worker.NodeId() == LeaderScheduler.NodeId()) {
            Leader.Send<TPing>(worker, LeaderScheduler, TPing{});
        } else {
            Follower.Send<TPing>(worker, FollowerScheduler, TPing{});
        }
        Pump();
    }

    void Evict() {
        Leader.Send<TWakeUp>(Client, LeaderScheduler, TWakeUp{});
        Pump();
    }

    TActorId BeginAllocation(uint32_t count) {
        Spawn(count);
        auto placement = std::move(Responses.back());
        Responses.pop_back();
        if (!placement.Error.empty()) {
            Responses.push_back(std::move(placement));
            return {};
        }
        const auto collector = Follower.Register(std::unique_ptr<IActor>(CreateActorsCollector(
            Client, std::move(placement.ActorIds), count, &Clock)));
        Pump();
        return collector;
    }

    void Spawn(uint32_t count, TActorId destination = {}) {
        const size_t before = Responses.size();
        Follower.Send<TSpawnComputeActor>(Client,
            destination ? destination : FollowerScheduler, TSpawnComputeActor{.Count = count});
        const auto deadline = NNet::TClock::now() + std::chrono::seconds(2);
        while (Responses.size() == before && NNet::TClock::now() < deadline) {
            Loop.Step();
        }
        ASSERT_EQ(Responses.size(), before + 1);
        Pump();
    }
};

} // namespace

TEST(DistributedScheduler, ForwardsToLeaderAndPreservesRequester) {
    for (TNodeId leaderId : {1, 2}) {
        SCOPED_TRACE(leaderId);
        TTestCluster cluster(leaderId);
        const auto first = cluster.AddWorker(true, 10);
        const auto second = cluster.AddWorker(false, 9);
        cluster.Ping(first);
        cluster.Ping(second);
        cluster.Spawn(4);
        ASSERT_EQ(cluster.Responses.size(), 1);
        ASSERT_EQ(cluster.Responses.back().ActorIds.size(), 2);
        ASSERT_TRUE(cluster.Requests.empty());
        EXPECT_EQ(cluster.Worker(first).Resources->UsedSlots.load(), 0);
        EXPECT_EQ(cluster.Worker(second).Resources->UsedSlots.load(), 0);
        size_t firstCount = 0;
        size_t secondCount = 0;
        for (const auto& actor : cluster.Responses.back().ActorIds) {
            firstCount += actor.NodeId() == first.NodeId();
            secondCount += actor.NodeId() == second.NodeId();
        }
        EXPECT_EQ(firstCount, 1);
        EXPECT_EQ(secondCount, 1);
        cluster.Clock.Now += std::chrono::seconds(10);
        cluster.Evict();
        cluster.Spawn(1);
        ASSERT_EQ(cluster.Responses.size(), 2);
        EXPECT_TRUE(cluster.Responses.back().ActorIds.empty());
    }
}

TEST(DistributedScheduler, RepliesWithErrorWhenRequestReachesNonLeaderTwice) {
    TTestCluster cluster;
    cluster.Follower.Send<TSpawnComputeActor>(cluster.Client, cluster.FollowerScheduler,
        TSpawnComputeActor{.Count = 1, .Forwarded = true});
    const auto deadline = NNet::TClock::now() + std::chrono::seconds(2);
    while (cluster.Responses.empty() && NNet::TClock::now() < deadline) {
        cluster.Loop.Step();
    }
    ASSERT_EQ(cluster.Responses.size(), 1);
    EXPECT_FALSE(cluster.Responses.back().Error.empty());
    EXPECT_TRUE(cluster.Responses.back().ActorIds.empty());
}

TEST(DistributedScheduler, KeepsDistinctSendersAndRepeatedPingsAtTimeZero) {
    TTestCluster cluster;
    const auto first = cluster.AddWorker(true, 10);
    const auto second = cluster.AddWorker(false, 9);
    cluster.Ping(first);
    cluster.Ping(first);
    cluster.Ping(second);
    for (int i = 0; i < 4; ++i) {
        cluster.Spawn(1);
    }
    ASSERT_TRUE(cluster.Requests.empty());
    ASSERT_EQ(cluster.Responses.size(), 4);
    EXPECT_EQ(cluster.Responses[0].ActorIds, std::vector<TActorId>{first});
    EXPECT_EQ(cluster.Responses[1].ActorIds, std::vector<TActorId>{second});
    EXPECT_EQ(cluster.Responses[2].ActorIds, std::vector<TActorId>{first});
    EXPECT_EQ(cluster.Responses[3].ActorIds, std::vector<TActorId>{second});
}

TEST(DistributedScheduler, RetainsHealthyNodesAndExpiresAtTenSeconds) {
    TTestCluster cluster;
    const auto worker = cluster.AddWorker(true, 10);
    cluster.Ping(worker);
    cluster.Clock.Now += std::chrono::seconds(9);
    cluster.Evict();
    cluster.Spawn(1);
    ASSERT_EQ(cluster.Responses.size(), 1);
    ASSERT_EQ(cluster.Responses.back().ActorIds.size(), 1);
    EXPECT_EQ(cluster.Responses.back().ActorIds[0].NodeId(), worker.NodeId());
    cluster.Clock.Now += std::chrono::seconds(1);
    cluster.Evict();
    cluster.Spawn(1);
    ASSERT_EQ(cluster.Responses.size(), 2);
    EXPECT_TRUE(cluster.Responses.back().ActorIds.empty());
}

TEST(DistributedScheduler, RefreshesPingAndRecoversAfterEvictingEveryNode) {
    TTestCluster cluster;
    const auto first = cluster.AddWorker(true, 10);
    const auto second = cluster.AddWorker(false, 9);
    cluster.Ping(first);
    cluster.Ping(second);
    cluster.Clock.Now += std::chrono::seconds(5);
    cluster.Ping(first);
    cluster.Clock.Now += std::chrono::seconds(5);
    cluster.Evict();
    cluster.Requests.clear();
    cluster.Spawn(2);
    ASSERT_TRUE(cluster.Requests.empty());
    EXPECT_EQ(cluster.Responses.back().ActorIds, std::vector<TActorId>{first});
    cluster.Clock.Now += std::chrono::seconds(5);
    cluster.Evict();
    cluster.Spawn(1);
    ASSERT_EQ(cluster.Responses.size(), 2);
    EXPECT_TRUE(cluster.Responses.back().ActorIds.empty());
    cluster.Ping(second);
    cluster.Spawn(1);
    ASSERT_EQ(cluster.Responses.size(), 3);
    ASSERT_EQ(cluster.Responses.back().ActorIds.size(), 1);
    EXPECT_EQ(cluster.Responses.back().ActorIds[0].NodeId(), second.NodeId());
}

TEST(DistributedScheduler, DoesNotAllocateOnExpiredNodesBetweenPeriodicChecks) {
    TTestCluster cluster;
    cluster.Ping(cluster.AddWorker(true, 10));
    cluster.Clock.Now += std::chrono::seconds(10);
    cluster.Spawn(1);
    ASSERT_EQ(cluster.Responses.size(), 1);
    EXPECT_TRUE(cluster.Responses.back().ActorIds.empty());
    EXPECT_TRUE(cluster.Requests.empty());
}

TEST(DistributedScheduler, EmptyAndZeroCountRequestsDoNotLeakCollectors) {
    TTestCluster cluster;
    const auto before = cluster.Leader.ActorsSize();
    cluster.Spawn(1);
    ASSERT_EQ(cluster.Responses.size(), 1);
    EXPECT_TRUE(cluster.Responses.back().ActorIds.empty());
    EXPECT_EQ(cluster.Leader.ActorsSize(), before);
    cluster.Ping(cluster.AddWorker(true, 10));
    const auto withWorker = cluster.Leader.ActorsSize();
    cluster.Spawn(0);
    ASSERT_EQ(cluster.Responses.size(), 2);
    EXPECT_TRUE(cluster.Responses.back().ActorIds.empty());
    EXPECT_TRUE(cluster.Requests.empty());
    EXPECT_EQ(cluster.Leader.ActorsSize(), withWorker);
}

TEST(DistributedScheduler, HeartbeatsAdvertiseLocalSchedulersAndAllowAllocation) {
    for (TNodeId leaderId : {1, 2}) {
        SCOPED_TRACE(leaderId);
        TTestCluster cluster(leaderId);
        cluster.Loop.Poller().SetMaxDuration(std::chrono::milliseconds(1));
        const auto first = cluster.Leader.RegisterWellKnown(
            static_cast<TLocalActorId>(EWellKnownActors::LocalScheduler),
            std::unique_ptr<IActor>(CreateLocalScheduler(leaderId, 1)));
        const auto second = cluster.Follower.RegisterWellKnown(
            static_cast<TLocalActorId>(EWellKnownActors::LocalScheduler),
            std::unique_ptr<IActor>(CreateLocalScheduler(leaderId, 1)));
        const auto leaderActors = cluster.Leader.ActorsSize();
        const auto followerActors = cluster.Follower.ActorsSize();
        const auto deadline = NNet::TClock::now() + std::chrono::seconds(2);
        do {
            cluster.Spawn(5);
        } while (cluster.Responses.back().ActorIds.size() < 2 && NNet::TClock::now() < deadline);
        ASSERT_TRUE(cluster.Responses.back().Error.empty());
        const auto& placement = cluster.Responses.back().ActorIds;
        EXPECT_EQ((std::unordered_set<TActorId>(placement.begin(), placement.end())),
            (std::unordered_set<TActorId>{first, second}));
        EXPECT_EQ(cluster.Leader.ActorsSize(), leaderActors);
        EXPECT_EQ(cluster.Follower.ActorsSize(), followerActors);

        cluster.Responses.clear();
        cluster.BeginAllocation(5);
        ASSERT_EQ(cluster.Responses.size(), 1);
        ASSERT_TRUE(cluster.Responses.back().Error.empty());
        const auto& actors = cluster.Responses.back().ActorIds;
        ASSERT_EQ(actors.size(), 5);
        EXPECT_EQ((std::unordered_set<TActorId>(actors.begin(), actors.end())).size(), 5);
        size_t onLeader = 0;
        size_t onFollower = 0;
        for (const auto actor : actors) {
            onLeader += actor.NodeId() == first.NodeId();
            onFollower += actor.NodeId() == second.NodeId();
            EXPECT_NE(actor, first);
            EXPECT_NE(actor, second);
            cluster.Follower.Send<TPoison>(cluster.Client, actor, TPoison{});
        }
        EXPECT_GE(onLeader, 2);
        EXPECT_GE(onFollower, 2);
        cluster.Pump();
        EXPECT_EQ(cluster.Leader.ActorsSize(), leaderActors);
        EXPECT_EQ(cluster.Follower.ActorsSize(), followerActors);
    }
}

TEST(DistributedScheduler, LocalSchedulerCreatesIndependentActors) {
    TTestCluster cluster;
    const auto local = cluster.Follower.RegisterWellKnown(
        static_cast<TLocalActorId>(EWellKnownActors::LocalScheduler),
        std::unique_ptr<IActor>(CreateLocalScheduler(
            cluster.LeaderScheduler.NodeId(), 1000)));
    const auto before = cluster.Follower.ActorsSize();
    for (uint32_t count : {0, 1, 3}) {
        SCOPED_TRACE(count);
        cluster.Spawn(count, local);
        ASSERT_FALSE(cluster.Responses.empty());
        const auto& actors = cluster.Responses.back().ActorIds;
        ASSERT_EQ(actors.size(), count);
        EXPECT_EQ(cluster.Follower.ActorsSize(), before + count);
        std::set<std::string> unique;
        for (const auto& actor : actors) {
            EXPECT_TRUE(unique.insert(actor.ToString()).second);
            cluster.Follower.Send<TPoison>(cluster.Client, actor, TPoison{});
        }
        cluster.Pump();
        EXPECT_EQ(cluster.Follower.ActorsSize(), before);
    }
}

TEST(DistributedSerialization, AllocationErrorRoundTrip) {
    TSpawnComputeActorResponse original{
        .Error = std::string("failure\0details", 15),
    };
    std::istringstream input(Encode(original));
    TSpawnComputeActorResponse decoded;
    DeserializeFromStream(decoded, input);
    EXPECT_EQ(decoded.Error, original.Error);
    auto invalid = Encode(original);
    Replace(invalid, 4, std::numeric_limits<uint32_t>::max());
    std::istringstream badLength(invalid);
    EXPECT_THROW(DeserializeFromStream(decoded, badLength), std::runtime_error);
}

TEST(DistributedSerialization, ComputeErrorRoundTrip) {
    TComputeError original{.Message = std::string("bad\0graph", 9)};
    const auto bytes = Encode(original);
    std::istringstream input(bytes);
    TComputeError decoded;
    DeserializeFromStream(decoded, input);
    EXPECT_EQ(decoded.Message, original.Message);
    for (size_t size = 0; size < bytes.size(); ++size) {
        SCOPED_TRACE(size);
        std::istringstream truncated(bytes.substr(0, size));
        EXPECT_THROW(DeserializeFromStream(decoded, truncated), std::runtime_error);
    }
    std::istringstream trailing(bytes + 'x');
    EXPECT_THROW(DeserializeFromStream(decoded, trailing), std::runtime_error);
}

TEST(DistributedAllocation, NoNodesIsAnExplicitErrorAndZeroCountSucceeds) {
    TTestCluster cluster;
    cluster.Spawn(1);
    ASSERT_EQ(cluster.Responses.size(), 1);
    EXPECT_FALSE(cluster.Responses.back().Error.empty());
    EXPECT_TRUE(cluster.Responses.back().ActorIds.empty());
    cluster.Spawn(0);
    ASSERT_EQ(cluster.Responses.size(), 2);
    EXPECT_TRUE(cluster.Responses.back().Error.empty());
    EXPECT_TRUE(cluster.Responses.back().ActorIds.empty());
}

TEST(DistributedAllocation, FailureStopsAllActorsAlreadyReported) {
    TTestCluster cluster;
    const auto first = cluster.AddWorker(true, 10);
    const auto second = cluster.AddWorker(false, 9);
    cluster.Worker(second).Error = "worker failed";
    cluster.Ping(first);
    cluster.Ping(second);
    cluster.BeginAllocation(4);
    ASSERT_EQ(cluster.Responses.size(), 1);
    EXPECT_EQ(cluster.Responses.back().Error, "worker failed");
    EXPECT_TRUE(cluster.Responses.back().ActorIds.empty());
    EXPECT_EQ(cluster.Worker(first).Resources->UsedSlots.load(), 0);
}

TEST(DistributedAllocation, CollectorExitsOnErrorWithoutWaitingForLateReplies) {
    TTestCluster cluster;
    const auto first = cluster.AddWorker(true, 10);
    const auto second = cluster.AddWorker(false, 9);
    cluster.Worker(first).Error = "worker failed";
    cluster.Worker(second).DropReply = true;
    cluster.Ping(first);
    cluster.Ping(second);
    const auto before = cluster.Follower.ActorsSize();
    cluster.BeginAllocation(2);
    ASSERT_EQ(cluster.Responses.size(), 1);
    EXPECT_FALSE(cluster.Responses.back().Error.empty());
    EXPECT_EQ(cluster.Worker(second).Resources->UsedSlots.load(), 1);
    EXPECT_EQ(cluster.Follower.ActorsSize(), before + 1);
    cluster.Reply(second, *cluster.Worker(second).LastResponse);
    EXPECT_EQ(cluster.Worker(second).Resources->UsedSlots.load(), 1);
    EXPECT_EQ(cluster.Follower.ActorsSize(), before + 1);
    EXPECT_EQ(cluster.Responses.size(), 1);
}

TEST(DistributedAllocation, DuplicateRepliesDoNotReplaceAMissingNode) {
    TTestCluster cluster;
    const auto first = cluster.AddWorker(true, 10);
    const auto second = cluster.AddWorker(false, 9);
    cluster.Worker(first).DuplicateReply = true;
    cluster.Worker(second).DropReply = true;
    cluster.Ping(first);
    cluster.Ping(second);
    const auto collector = cluster.BeginAllocation(2);
    EXPECT_TRUE(cluster.Responses.empty());
    ASSERT_TRUE(cluster.Worker(first).LastRequest.has_value());
    cluster.Clock.Now += std::chrono::seconds(5);
    cluster.Wake(collector);
    ASSERT_EQ(cluster.Responses.size(), 1);
    EXPECT_EQ(cluster.Responses.back().Error, "compute actor allocation timed out");
    EXPECT_EQ(cluster.Worker(first).Resources->UsedSlots.load(), 0);
    cluster.Reply(second, *cluster.Worker(second).LastResponse);
    EXPECT_EQ(cluster.Worker(second).Resources->UsedSlots.load(), 1);
    EXPECT_EQ(cluster.Responses.size(), 1);
}

TEST(DistributedAllocation, DuplicateSuccessfulReplyDoesNotStopAllocatedActors) {
    TTestCluster cluster;
    const auto worker = cluster.AddWorker(true, 10);
    cluster.Ping(worker);
    cluster.BeginAllocation(2);
    ASSERT_EQ(cluster.Responses.size(), 1);
    EXPECT_TRUE(cluster.Responses.back().Error.empty());
    cluster.Reply(worker, *cluster.Worker(worker).LastResponse);
    EXPECT_EQ(cluster.Responses.size(), 1);
    EXPECT_EQ(cluster.Worker(worker).Resources->UsedSlots.load(), 2);
    for (const auto actor : cluster.Responses.back().ActorIds) {
        cluster.Follower.Send<TPoison>(cluster.Client, actor, TPoison{});
    }
    cluster.Pump();
    EXPECT_EQ(cluster.Worker(worker).Resources->UsedSlots.load(), 0);
}

TEST(DistributedAllocation, MalformedReplyIsAnErrorAndReleasesItsActors) {
    TTestCluster cluster;
    const auto worker = cluster.AddWorker(true, 10);
    cluster.Worker(worker).MalformedCount = true;
    cluster.Ping(worker);
    cluster.BeginAllocation(3);
    ASSERT_EQ(cluster.Responses.size(), 1);
    EXPECT_EQ(cluster.Responses.back().Error, "invalid compute actor allocation response");
    EXPECT_TRUE(cluster.Responses.back().ActorIds.empty());
    EXPECT_EQ(cluster.Worker(worker).Resources->UsedSlots.load(), 0);
}

TEST(DistributedAllocation, DeadlineIsCheckedWhenTheReplyArrives) {
    TTestCluster cluster;
    const auto worker = cluster.AddWorker(true, 10);
    cluster.Worker(worker).DropReply = true;
    cluster.Ping(worker);
    cluster.BeginAllocation(1);
    EXPECT_TRUE(cluster.Responses.empty());
    cluster.Clock.Now += std::chrono::seconds(5);
    cluster.Reply(worker, *cluster.Worker(worker).LastResponse);
    ASSERT_EQ(cluster.Responses.size(), 1);
    EXPECT_EQ(cluster.Responses.back().Error, "compute actor allocation timed out");
    EXPECT_EQ(cluster.Worker(worker).Resources->UsedSlots.load(), 0);
}

TEST(DistributedAllocation, CollectorCreatesActorsWithoutSchedulerParticipation) {
    TTestCluster cluster;
    const auto first = cluster.AddWorker(true, 10);
    const auto second = cluster.AddWorker(false, 9);
    cluster.Follower.Register(std::unique_ptr<IActor>(CreateActorsCollector(
        cluster.Client, {first, second}, 5, &cluster.Clock)));
    cluster.Pump();
    ASSERT_EQ(cluster.Responses.size(), 1);
    EXPECT_TRUE(cluster.Responses.back().Error.empty());
    EXPECT_EQ(cluster.Responses.back().ActorIds.size(), 5);
    EXPECT_EQ(cluster.Worker(first).Resources->UsedSlots.load(), 3);
    EXPECT_EQ(cluster.Worker(second).Resources->UsedSlots.load(), 2);
    EXPECT_EQ(cluster.Requests, (std::vector<TActorId>{first, second}));
}

TEST(DistributedAllocation, CollectorExitsOnTimeoutWithAMissingReply) {
    TTestCluster cluster;
    const auto first = cluster.AddWorker(true, 10);
    const auto second = cluster.AddWorker(false, 9);
    cluster.Worker(second).DropReply = true;
    cluster.Ping(first);
    cluster.Ping(second);
    const auto before = cluster.Follower.ActorsSize();
    const auto collector = cluster.BeginAllocation(2);
    EXPECT_TRUE(cluster.Responses.empty());
    EXPECT_EQ(cluster.Follower.ActorsSize(), before + 2);
    cluster.Clock.Now += std::chrono::seconds(5);
    cluster.Wake(collector);
    ASSERT_EQ(cluster.Responses.size(), 1);
    EXPECT_EQ(cluster.Responses.back().Error, "compute actor allocation timed out");
    EXPECT_EQ(cluster.Follower.ActorsSize(), before + 1);
    EXPECT_EQ(cluster.Worker(first).Resources->UsedSlots.load(), 0);
    EXPECT_EQ(cluster.Worker(second).Resources->UsedSlots.load(), 1);
}

TEST(DistributedAllocation, EmptyAndInvalidPlacementsDoNotCreateActors) {
    for (uint32_t count : {0, 1}) {
        TTestCluster cluster;
        const auto before = cluster.Follower.ActorsSize();
        cluster.Follower.Register(std::unique_ptr<IActor>(CreateActorsCollector(
            cluster.Client, {}, count, &cluster.Clock)));
        cluster.Pump();
        ASSERT_EQ(cluster.Responses.size(), 1);
        EXPECT_EQ(cluster.Responses.back().Error.empty(), count == 0);
        EXPECT_EQ(cluster.Follower.ActorsSize(), before);
    }
    TTestCluster cluster;
    const auto worker = cluster.AddWorker(true, 10);
    const auto before = cluster.Follower.ActorsSize();
    cluster.Follower.Register(std::unique_ptr<IActor>(CreateActorsCollector(
        cluster.Client, {worker, worker}, 2, &cluster.Clock)));
    cluster.Pump();
    ASSERT_EQ(cluster.Responses.size(), 1);
    EXPECT_EQ(cluster.Responses.back().Error, "invalid local scheduler placement");
    EXPECT_TRUE(cluster.Requests.empty());
    EXPECT_EQ(cluster.Follower.ActorsSize(), before);
}

TEST(DistributedAllocation, LocalSchedulerAllowsOversubscription) {
    TTestCluster cluster;
    const auto local = cluster.Follower.RegisterWellKnown(
        static_cast<TLocalActorId>(EWellKnownActors::LocalScheduler),
        std::unique_ptr<IActor>(CreateLocalScheduler(
            cluster.LeaderScheduler.NodeId(), 1000)));
    const auto before = cluster.Follower.ActorsSize();
    cluster.Spawn(32, local);
    ASSERT_EQ(cluster.Responses.size(), 1);
    EXPECT_TRUE(cluster.Responses.back().Error.empty());
    EXPECT_EQ(cluster.Responses.back().ActorIds.size(), 32);
    EXPECT_EQ(cluster.Follower.ActorsSize(), before + 32);
    for (const auto actor : cluster.Responses.back().ActorIds) {
        cluster.Follower.Send<TPoison>(cluster.Client, actor, TPoison{});
    }
    cluster.Pump();
    EXPECT_EQ(cluster.Follower.ActorsSize(), before);
}

namespace {

void CountDestroy(TRowSet* rowSet) {
    static_cast<std::atomic<int>*>(rowSet->Private)->fetch_add(1, std::memory_order_relaxed);
}

// One column, like the Layouts of the sink endpoints in these tests.
TRowSet CountedRows(int64_t rows, std::atomic<int>* destroyed) {
    static TColumn column{};
    return TRowSet{
        .Columns = &column,
        .ColumnCount = 1,
        .RowCount = rows,
        .Destroy = CountDestroy,
        .Private = destroyed,
        .RefCount = 1,
    };
}

// source -> sink, both external: ids 2 and 1, like the pipeline tests above.
struct TTwoEndpointGraph {
    // The sealed graph points into the task graph.
    NQdb::NScheduler::TTaskGraph Owner;
    NQdb::NScheduler::TSealedTaskGraph Graph;
    std::vector<TRemoteEndpoint> Endpoints{
        {.Id = 1, .Kind = ERemoteEndpointKind::Sink, .Layouts = {TValueLayout{.Width = 8}}},
        {.Id = 2, .Kind = ERemoteEndpointKind::Source},
    };

    TTwoEndpointGraph() {
        using namespace NQdb::NScheduler;
        auto connection = std::make_unique<TOneToOneConnection>();
        auto* connectionPtr = connection.get();
        auto& source = Owner.AddOwnedNode(std::make_unique<TSourceTask>(
            nullptr, nullptr, TOutputPort{.Connection = connectionPtr}));
        auto& sink = Owner.AddOwnedNode(std::make_unique<TSinkTask>(
            nullptr, nullptr, TInputPort{.Connection = connectionPtr}));
        Owner.AddOwnedEdge(source, sink, std::move(connection));
        Owner.MarkExternal(sink, 1);
        Owner.MarkExternal(source, 2);
        Graph = Owner.Seal();
    }
};

// Plays the query actor: counts what the compute actor reports.
class TOrchestratorProbe : public IActor {
public:
    TOrchestratorProbe(int& statuses, std::vector<std::string>& errors)
        : Statuses_(statuses)
        , Errors_(errors)
    {}

    void Receive(TMessageId messageId, TBlob blob, TActorContext::TPtr) override {
        if (messageId == TComputeStatus::MessageId) {
            ++Statuses_;
        } else {
            ASSERT_EQ(messageId, TComputeError::MessageId);
            Errors_.push_back(ReadMessage<TComputeError>(blob).Message);
        }
    }

private:
    int& Statuses_;
    std::vector<std::string>& Errors_;
};

} // namespace

TEST(DistributedAllocation, ComputeActorSendsPeriodicStatus) {
    auto resources = std::make_shared<TRuntimeResources>();
    resources->Scheduler.StatusPeriodMs = 5;
    // Declared first: the compute actor's graph copy points into it until the cluster is gone.
    TTwoEndpointGraph graph;
    TTestCluster cluster;
    int statuses = 0;
    std::vector<std::string> errors;
    const auto probe = cluster.Follower.Register(std::make_unique<TOrchestratorProbe>(statuses, errors));
    const auto actor = cluster.Follower.Register(std::unique_ptr<IActor>(CreateComputeActor(resources)));
    for (int i = 0; i < 8; ++i) {
        cluster.Pump();
    }
    // No graph, no heartbeat.
    EXPECT_EQ(statuses, 0);

    cluster.Follower.Send<TTaskGraphMessage>(probe, actor, TTaskGraphMessage{
        .Graph = graph.Graph, .Endpoints = graph.Endpoints});
    const auto deadline = NNet::TClock::now() + std::chrono::seconds(5);
    while (statuses < 3 && NNet::TClock::now() < deadline) {
        cluster.Pump();
    }
    EXPECT_GE(statuses, 3);

    cluster.Follower.Send<TPoison>(probe, actor, TPoison{});
    cluster.Pump();
    const int stopped = statuses;
    const auto quiet = NNet::TClock::now() + std::chrono::milliseconds(50);
    while (NNet::TClock::now() < quiet) {
        cluster.Pump();
    }
    EXPECT_EQ(statuses, stopped);
    EXPECT_TRUE(errors.empty());
}

TEST(DistributedAllocation, ComputeActorReportsErrorsToTheQueryActor) {
    auto resources = std::make_shared<TRuntimeResources>();
    // Declared first: the compute actor's graph copy points into it until the cluster is gone.
    TTwoEndpointGraph graph;
    TTestCluster cluster;
    int statuses = 0;
    std::vector<std::string> errors;
    const auto probe = cluster.Follower.Register(std::make_unique<TOrchestratorProbe>(statuses, errors));
    const auto actor = cluster.Follower.Register(std::unique_ptr<IActor>(CreateComputeActor(resources)));

    auto bad = graph.Endpoints;
    bad.push_back({.Id = 7, .Kind = ERemoteEndpointKind::Sink, .Layouts = {TValueLayout{.Width = 8}}});
    cluster.Follower.Send<TTaskGraphMessage>(probe, actor, TTaskGraphMessage{
        .Graph = graph.Graph, .Endpoints = bad});
    cluster.Pump();
    ASSERT_EQ(errors.size(), 1);
    EXPECT_NE(errors[0].find("no external node with id 7"), std::string::npos);

    cluster.Follower.Send<TTaskGraphMessage>(probe, actor, TTaskGraphMessage{
        .Graph = graph.Graph, .Endpoints = graph.Endpoints});
    cluster.Pump();
    EXPECT_EQ(errors.size(), 1);

    // Unknown tasks: a chunk (its row set must be released), a finish and a confirm.
    std::atomic<int> destroyed = 0;
    cluster.Follower.Send<TDataChunk>(probe, actor, TDataChunk{
        .DstTaskId = 99, .Seq = 1, .RowSet = CountedRows(3, &destroyed)});
    cluster.Follower.Send<TFinish>(probe, actor, TFinish{.DstTaskId = 99});
    cluster.Follower.Send<TDataConfirm>(probe, actor, TDataConfirm{.SinkId = 99, .LastApplied = 1});
    cluster.Pump();
    ASSERT_EQ(errors.size(), 4);
    EXPECT_NE(errors[1].find("unknown task 99"), std::string::npos);
    EXPECT_NE(errors[2].find("finish"), std::string::npos);
    EXPECT_NE(errors[3].find("confirm"), std::string::npos);
    EXPECT_EQ(destroyed.load(), 1);

    // A started graph keeps a context in the actor: stop it before the system goes.
    cluster.Follower.Send<TPoison>(probe, actor, TPoison{});
    cluster.Pump();
}

TEST(DistributedAllocation, ComputeActorRejectsASecondGraph) {
    auto resources = std::make_shared<TRuntimeResources>();
    TTwoEndpointGraph first;
    TTwoEndpointGraph second;
    TTestCluster cluster;
    int statuses = 0;
    std::vector<std::string> errors;
    const auto probe = cluster.Follower.Register(std::make_unique<TOrchestratorProbe>(statuses, errors));
    const auto actor = cluster.Follower.Register(std::unique_ptr<IActor>(CreateComputeActor(resources)));
    cluster.Follower.Send<TTaskGraphMessage>(probe, actor, TTaskGraphMessage{
        .Graph = first.Graph, .Endpoints = first.Endpoints});
    cluster.Pump();
    cluster.Follower.Send<TTaskGraphMessage>(probe, actor, TTaskGraphMessage{
        .Graph = second.Graph, .Endpoints = second.Endpoints});
    cluster.Pump();
    ASSERT_EQ(errors.size(), 1u);
    EXPECT_NE(errors[0].find("already started"), std::string::npos);

    cluster.Follower.Send<TPoison>(probe, actor, TPoison{});
    cluster.Pump();
}

TEST(DistributedAllocation, ComputeActorReleasesItsCounterExactlyOnce) {
    auto resources = std::make_shared<TRuntimeResources>();
    {
        TTestCluster cluster;
        auto actor = cluster.Follower.Register(std::unique_ptr<IActor>(CreateComputeActor(resources)));
        EXPECT_EQ(resources->UsedSlots.load(), 1);
        cluster.Follower.Send<TPoison>(cluster.Client, actor, TPoison{});
        cluster.Pump();
        EXPECT_EQ(resources->UsedSlots.load(), 0);
    }
    EXPECT_EQ(resources->UsedSlots.load(), 0);
    {
        TTestCluster cluster;
        cluster.Follower.Register(std::unique_ptr<IActor>(CreateComputeActor(resources)));
        EXPECT_EQ(resources->UsedSlots.load(), 1);
    }
    EXPECT_EQ(resources->UsedSlots.load(), 0);
}

TEST(DistributedHttp, UnknownRoutesReturnNotFound) {
    auto router = NHttp::CreateRouter(nullptr, {});
    for (const std::string method : {"GET", "POST"}) {
        SCOPED_TRACE(method);
        NNet::TRequest request(method + " /missing HTTP/1.1\r\nContent-Length: 0\r\n\r\n",
            [](char*, size_t) -> NNet::TFuture<ssize_t> { co_return 0; });
        std::string output;
        NNet::TResponse response([&](const void* data, size_t size) -> NNet::TFuture<ssize_t> {
            output.append(static_cast<const char*>(data), size);
            co_return size;
        });
        auto result = router->HandleRequest(request, response);
        ASSERT_TRUE(result.done());
        ASSERT_NO_THROW(result.await_resume());
        EXPECT_EQ(response.StatusCode(), 404);
        EXPECT_TRUE(output.starts_with("HTTP/1.1 404 Not Found\r\n"));
        EXPECT_TRUE(output.ends_with("\r\n\r\nNot Found"));
    }
}

TEST(DistributedHttp, QueryResultAndErrorKeepTheirStatusCodes) {
    for (const auto mode : {EReply::Result, EReply::QueryError, EReply::Unavailable}) {
        SCOPED_TRACE(static_cast<int>(mode));
        TTestCluster cluster;
        const auto gateway = cluster.Follower.Register(std::make_unique<TQueryReplyProbe>(mode));
        auto router = NHttp::CreateRouter(&cluster.Follower, gateway);
        NNet::TRequest request("POST /api/run HTTP/1.1\r\nContent-Length: 0\r\n\r\n",
            [](char*, size_t) -> NNet::TFuture<ssize_t> { co_return 0; });
        std::string output;
        NNet::TResponse response([&](const void* data, size_t size) -> NNet::TFuture<ssize_t> {
            output.append(static_cast<const char*>(data), size);
            co_return size;
        });
        auto result = router->HandleRequest(request, response);
        const auto deadline = NNet::TClock::now() + std::chrono::seconds(2);
        while (!result.done() && NNet::TClock::now() < deadline) {
            cluster.Pump();
        }
        ASSERT_TRUE(result.done());
        ASSERT_NO_THROW(result.await_resume());
        const std::array<int, 3> statuses{200, 400, 503};
        const std::array<const char*, 3> lines{"HTTP/1.1 200 OK\r\n", "HTTP/1.1 400 Bad Request\r\n",
            "HTTP/1.1 503 Service Unavailable\r\n"};
        const auto index = static_cast<size_t>(mode);
        EXPECT_EQ(response.StatusCode(), statuses[index]);
        EXPECT_TRUE(output.starts_with(lines[index]));
        EXPECT_NE(output.find(mode == EReply::Result ? "query result" : "query failed"), std::string::npos);
        EXPECT_NE(output.find("Access-Control-Allow-Origin: *\r\n"), std::string::npos);
    }
}

namespace {

class TFakeNotifier : public NNet::INotifier {
public:
    void Notify() override {}

    NNet::TFuture<void> Wait() override {
        throw std::logic_error("unused");
    }
};

struct TProducerState {
    int64_t Next = 1;
    int64_t Total = 0;
};

struct TCollectorState {
    std::vector<int64_t> Rows;
};

struct TRemotePipeline {
    int64_t Total = 300;
    size_t Workers = 2;
    size_t OutboxCapacity = 2;
    size_t InboxCapacity = 2;
    size_t Window = 2;
};

// producer -> remote sink | fake compute actors | remote source -> collector.
// Both fake actors share one thread; FIFO queues stand for the transport, and
// a chunk without a Seq stands for TFinish.
void RunRemotePipeline(const TRemotePipeline& options) {
    using namespace NQdb::NScheduler;
    std::atomic<int> destroyed = 0;
    TActorNotificationChannel channelA(std::make_unique<TFakeNotifier>(), 1, 1);
    TActorNotificationChannel channelB(std::make_unique<TFakeNotifier>(), 1, 1);

    auto [sinkCode, sinkState] = MakeRemoteSink(
        1, TActorId{}, 2, {}, channelA, options.OutboxCapacity, options.Window);
    auto producerCode = std::make_shared<TSourceCode>([&](void* state, TRowSet& rowSet) {
        auto* producer = static_cast<TProducerState*>(state);
        if (producer->Next > producer->Total) {
            return ETaskResult::FINISHED;
        }
        rowSet = CountedRows(producer->Next++, &destroyed);
        return ETaskResult::OK;
    });
    auto producerState = std::make_shared<TProducerState>(TProducerState{.Total = options.Total});

    // Remote endpoints arrive with empty code; the compute actor binds its own.
    auto producerToSink = std::make_unique<TOneToOneConnection>();
    auto* producerToSinkPtr = producerToSink.get();
    TTaskGraph graphA;
    auto& producer = graphA.AddOwnedNode(std::make_unique<TSourceTask>(
        producerCode, producerState, TOutputPort{.Connection = producerToSinkPtr}));
    auto& sink = graphA.AddOwnedNode(std::make_unique<TSinkTask>(
        nullptr, nullptr, TInputPort{.Connection = producerToSinkPtr}));
    graphA.AddOwnedEdge(producer, sink, std::move(producerToSink));
    graphA.MarkExternal(sink, sinkState->Id);
    const auto sealedA = graphA.Seal();
    ASSERT_TRUE(sealedA);
    static_cast<TSinkTask*>(sink.Task)->BindCode(sinkCode, sinkState);

    auto [sourceCode, sourceState] = MakeRemoteSource(2, TActorId{}, 1, channelB, options.InboxCapacity);
    auto collectorCode = std::make_shared<TSinkCode>([](void* state, const TRowSet& rowSet) {
        static_cast<TCollectorState*>(state)->Rows.push_back(rowSet.RowCount);
        return ETaskResult::OK;
    });
    auto collectorState = std::make_shared<TCollectorState>();

    auto sourceToCollector = std::make_unique<TOneToOneConnection>();
    auto* sourceToCollectorPtr = sourceToCollector.get();
    TTaskGraph graphB;
    auto& source = graphB.AddOwnedNode(std::make_unique<TSourceTask>(
        nullptr, nullptr, TOutputPort{.Connection = sourceToCollectorPtr}));
    auto& collector = graphB.AddOwnedNode(std::make_unique<TSinkTask>(
        collectorCode, collectorState, TInputPort{.Connection = sourceToCollectorPtr}));
    graphB.AddOwnedEdge(source, collector, std::move(sourceToCollector));
    graphB.MarkExternal(source, sourceState->Id);
    const auto sealedB = graphB.Seal();
    ASSERT_TRUE(sealedB);
    static_cast<TSourceTask*>(source.Task)->BindCode(sourceCode, sourceState);

    TThreadedScheduler schedulerA(sealedA, options.Workers);
    TThreadedScheduler schedulerB(sealedB, options.Workers);
    TSchedulerChannel wakeA(schedulerA);
    TSchedulerChannel wakeB(schedulerB);

    std::atomic<bool> stop = false;
    std::thread actors([&] {
        std::deque<std::pair<uint64_t, TRowSet>> chunks;
        std::deque<uint64_t> confirms;
        auto send = [&](uint64_t seq, TRowSet&& rowSet) {
            EXPECT_LE(sinkState->Sent - sinkState->Confirmed, options.Window);
            chunks.emplace_back(seq, rowSet);
        };
        auto finish = [&] {
            chunks.emplace_back(0, TRowSet{});
        };
        int64_t delivered = 0;
        while (!stop.load(std::memory_order_acquire)) {
            bool idle = true;
            TRemoteSinkState* sinkReady = nullptr;
            while (channelA.SinkQueue().TryPop(sinkReady)) {
                idle = false;
                sinkReady->Drain(send, finish);
                wakeA.Notify(sealedA.ExternalNode(sinkReady->Id));
            }
            TRemoteSourceState* sourceReady = nullptr;
            while (channelB.SourceQueue().TryPop(sourceReady)) {
                idle = false;
                if (const auto applied = sourceReady->TakeApplied()) {
                    confirms.push_back(applied);
                }
            }
            if (!chunks.empty()) {
                idle = false;
                auto [seq, rowSet] = chunks.front();
                chunks.pop_front();
                if (seq == 0) {
                    EXPECT_EQ(delivered, options.Total);
                    sourceState->Finish();
                } else if (sourceState->Push(seq, std::move(rowSet))) {
                    ++delivered;
                } else {
                    ADD_FAILURE() << "chunk " << seq << " does not fit";
                    Release(&rowSet);
                }
                wakeB.Notify(sealedB.ExternalNode(sourceState->Id));
            }
            if (!confirms.empty()) {
                idle = false;
                EXPECT_TRUE(sinkState->Confirm(confirms.front(), send, finish));
                confirms.pop_front();
                wakeA.Notify(sealedA.ExternalNode(sinkState->Id));
            }
            if (idle) {
                std::this_thread::yield();
            }
        }
    });

    std::string errorA;
    std::string errorB;
    bool okA = false;
    bool okB = false;
    std::thread runA([&] { okA = schedulerA.Run(&errorA); });
    std::thread runB([&] { okB = schedulerB.Run(&errorB); });
    runA.join();
    runB.join();
    stop.store(true, std::memory_order_release);
    actors.join();

    EXPECT_TRUE(okA) << errorA;
    EXPECT_TRUE(okB) << errorB;
    ASSERT_EQ(collectorState->Rows.size(), static_cast<size_t>(options.Total));
    for (int64_t i = 0; i < options.Total; ++i) {
        ASSERT_EQ(collectorState->Rows[i], i + 1);
    }
    EXPECT_EQ(sinkState->Sent, static_cast<uint64_t>(options.Total));
    EXPECT_TRUE(sinkState->FinishSent);
    EXPECT_EQ(source.State.load(), ETaskState::Finished);
    EXPECT_EQ(destroyed.load(), options.Total);
}

} // namespace

TEST(DistributedScheduler, RemoteTasksMoveRowSetsInOrder) {
    for (int iteration = 0; iteration < 20; ++iteration) {
        SCOPED_TRACE(iteration);
        RunRemotePipeline({});
    }
}

TEST(DistributedScheduler, RemoteTasksDeliverEverythingWithSmallWindows) {
    for (size_t window : {1, 2, 3}) {
        for (size_t outbox : {1, 4}) {
            for (int iteration = 0; iteration < 3; ++iteration) {
                SCOPED_TRACE(std::to_string(window) + "/" + std::to_string(outbox) + "/" + std::to_string(iteration));
                // An inbox of exactly the window must never overflow.
                RunRemotePipeline({
                    .Total = 1000,
                    .Workers = 4,
                    .OutboxCapacity = outbox,
                    .InboxCapacity = window,
                    .Window = window,
                });
            }
        }
    }
}

TEST(DistributedScheduler, RemoteSinkKeepsAtMostWindowChunksUnconfirmed) {
    using namespace NQdb::NScheduler;
    std::atomic<int> destroyed = 0;
    TActorNotificationChannel channel(std::make_unique<TFakeNotifier>(), 2, 2);
    auto [code, state] = MakeRemoteSink(1, TActorId{}, 2, {}, channel, 8, 2);
    std::vector<uint64_t> sent;
    auto send = [&](uint64_t seq, TRowSet&& rowSet) {
        sent.push_back(seq);
        Release(&rowSet);
    };
    auto finish = [] {
        ADD_FAILURE() << "the sink has not finished";
    };
    auto write = [&](int64_t rows) {
        auto rowSet = CountedRows(rows, &destroyed);
        EXPECT_EQ(code->Write(state.get(), rowSet), ETaskResult::OK);
        Release(&rowSet);
    };

    for (int64_t i = 1; i <= 5; ++i) {
        write(i);
    }
    TRemoteSinkState* queued = nullptr;
    ASSERT_TRUE(channel.SinkQueue().TryPop(queued));
    EXPECT_FALSE(channel.SinkQueue().TryPop(queued));
    queued->Drain(send, finish);
    EXPECT_EQ(sent, (std::vector<uint64_t>{1, 2}));
    // A parked state stays with the actor: a new row does not queue it again.
    write(6);
    EXPECT_FALSE(channel.SinkQueue().TryPop(queued));

    EXPECT_TRUE(state->Confirm(1, send, finish));
    EXPECT_EQ(sent, (std::vector<uint64_t>{1, 2, 3}));
    EXPECT_TRUE(state->Confirm(3, send, finish));
    EXPECT_EQ(sent, (std::vector<uint64_t>{1, 2, 3, 4, 5}));
    EXPECT_FALSE(state->Confirm(6, send, finish));
    EXPECT_TRUE(state->Confirm(5, send, finish));
    EXPECT_EQ(sent, (std::vector<uint64_t>{1, 2, 3, 4, 5, 6}));
    EXPECT_FALSE(channel.SinkQueue().TryPop(queued));

    // The outbox ran empty inside the window, so the next row queues the state.
    write(7);
    ASSERT_TRUE(channel.SinkQueue().TryPop(queued));
    queued->Drain(send, finish);
    EXPECT_EQ(sent.back(), 7);
    EXPECT_EQ(destroyed.load(), 7);
}

TEST(DistributedScheduler, RemoteSinkFinishesAfterItsLastChunk) {
    using namespace NQdb::NScheduler;
    std::atomic<int> destroyed = 0;
    TActorNotificationChannel channel(std::make_unique<TFakeNotifier>(), 2, 2);
    auto [code, state] = MakeRemoteSink(1, TActorId{}, 2, {}, channel, 4, 1);
    std::vector<uint64_t> events; // 0 is the finish
    auto send = [&](uint64_t seq, TRowSet&& rowSet) {
        events.push_back(seq);
        Release(&rowSet);
    };
    auto finish = [&] {
        events.push_back(0);
    };
    for (int64_t rows = 1; rows <= 3; ++rows) {
        auto rowSet = CountedRows(rows, &destroyed);
        EXPECT_EQ(code->Write(state.get(), rowSet), ETaskResult::OK);
        Release(&rowSet);
    }

    // Finish comes while the window holds back two rows.
    EXPECT_EQ(code->Finish(state.get()), ETaskResult::OK);
    TRemoteSinkState* queued = nullptr;
    ASSERT_TRUE(channel.SinkQueue().TryPop(queued));
    queued->Drain(send, finish);
    EXPECT_EQ(events, (std::vector<uint64_t>{1}));
    EXPECT_TRUE(state->Confirm(1, send, finish));
    EXPECT_EQ(events, (std::vector<uint64_t>{1, 2}));
    EXPECT_TRUE(state->Confirm(2, send, finish));
    EXPECT_EQ(events, (std::vector<uint64_t>{1, 2, 3, 0}));
    EXPECT_TRUE(state->Confirm(3, send, finish));
    EXPECT_EQ(events, (std::vector<uint64_t>{1, 2, 3, 0}));
    EXPECT_FALSE(channel.SinkQueue().TryPop(queued));
    EXPECT_EQ(destroyed.load(), 3);

    // An empty stream finishes at once.
    auto [emptyCode, emptyState] = MakeRemoteSink(3, TActorId{}, 4, {}, channel, 4, 1);
    events.clear();
    EXPECT_EQ(emptyCode->Finish(emptyState.get()), ETaskResult::OK);
    ASSERT_TRUE(channel.SinkQueue().TryPop(queued));
    queued->Drain(send, finish);
    EXPECT_EQ(events, (std::vector<uint64_t>{0}));
}

// A row pushed while the actor leaves Drain must still be picked up or announced.
TEST(DistributedScheduler, RemoteSinkNeverStrandsARowAgainstConcurrentDrain) {
    using namespace NQdb::NScheduler;
    constexpr uint64_t total = 20000;
    std::atomic<int> destroyed = 0;
    TActorNotificationChannel channel(std::make_unique<TFakeNotifier>(), 2, 2);
    auto [code, state] = MakeRemoteSink(1, TActorId{}, 2, {}, channel, 4, 1 << 20);
    std::thread worker([&] {
        for (uint64_t i = 0; i < total; ++i) {
            auto rowSet = CountedRows(1, &destroyed);
            while (code->Write(state.get(), rowSet) != ETaskResult::OK) {
                std::this_thread::yield();
            }
            Release(&rowSet);
        }
        while (code->Finish(state.get()) != ETaskResult::OK) {
            std::this_thread::yield();
        }
    });
    uint64_t sent = 0;
    bool finished = false;
    auto send = [&](uint64_t, TRowSet&& rowSet) {
        ++sent;
        Release(&rowSet);
    };
    auto finish = [&] {
        finished = true;
    };
    const auto deadline = NNet::TClock::now() + std::chrono::seconds(30);
    TRemoteSinkState* queued = nullptr;
    while (!finished && NNet::TClock::now() < deadline) {
        if (channel.SinkQueue().TryPop(queued)) {
            queued->Drain(send, finish);
        }
    }
    worker.join();
    EXPECT_TRUE(finished);
    EXPECT_EQ(sent, total);
}

// The queues are sized by the number of states, so a Write or Finish never fails to wake the actor.
TEST(DistributedScheduler, NotificationChannelHoldsEveryState) {
    using namespace NQdb::NScheduler;
    std::atomic<int> destroyed = 0;
    TActorNotificationChannel channel(std::make_unique<TFakeNotifier>(), 3, 3);
    std::vector<std::shared_ptr<TRemoteSinkState>> sinks;
    std::vector<std::shared_ptr<TRemoteSourceState>> sources;
    for (uint64_t id = 1; id <= 3; ++id) {
        auto [sinkCode, sinkState] = MakeRemoteSink(id, TActorId{}, 0, {}, channel);
        auto rowSet = CountedRows(1, &destroyed);
        EXPECT_EQ(sinkCode->Write(sinkState.get(), rowSet), ETaskResult::OK);
        Release(&rowSet);
        sinks.push_back(sinkState);
        auto [sourceCode, sourceState] = MakeRemoteSource(id, TActorId{}, 0, channel);
        sourceState->Push(1, CountedRows(1, &destroyed));
        TRowSet out;
        EXPECT_EQ(sourceCode->Next(sourceState.get(), out), ETaskResult::OK);
        Release(&out);
        sources.push_back(sourceState);
    }
    TRemoteSinkState* sink = nullptr;
    TRemoteSourceState* source = nullptr;
    for (int i = 0; i < 3; ++i) {
        EXPECT_TRUE(channel.SinkQueue().TryPop(sink));
        EXPECT_TRUE(channel.SourceQueue().TryPop(source));
    }
    EXPECT_FALSE(channel.SinkQueue().TryPop(sink));
    EXPECT_FALSE(channel.SourceQueue().TryPop(source));
}

TEST(DistributedScheduler, RemoteSourceConfirmsConsumedChunks) {
    using namespace NQdb::NScheduler;
    std::atomic<int> destroyed = 0;
    TActorNotificationChannel channel(std::make_unique<TFakeNotifier>(), 2, 2);
    auto [code, state] = MakeRemoteSource(2, TActorId{}, 1, channel, 2);
    auto push = [&](uint64_t seq) {
        auto rowSet = CountedRows(seq, &destroyed);
        if (state->Push(seq, std::move(rowSet))) {
            return true;
        }
        Release(&rowSet);
        return false;
    };
    TRowSet rowSet{};
    auto next = [&] {
        const auto result = code->Next(state.get(), rowSet);
        if (result == ETaskResult::OK) {
            Release(&rowSet);
        }
        return result;
    };

    EXPECT_TRUE(push(1));
    EXPECT_FALSE(push(3));
    EXPECT_TRUE(push(2));
    EXPECT_FALSE(push(3));
    // Received chunks are not confirmed before the source task takes them.
    TRemoteSourceState* queued = nullptr;
    EXPECT_FALSE(channel.SourceQueue().TryPop(queued));
    EXPECT_EQ(state->TakeApplied(), 0);

    ASSERT_EQ(next(), ETaskResult::OK);
    EXPECT_EQ(rowSet.RowCount, 1);
    ASSERT_TRUE(channel.SourceQueue().TryPop(queued));
    EXPECT_EQ(queued->TakeApplied(), 1);
    EXPECT_EQ(queued->TakeApplied(), 0);

    // Pops between two wakes are confirmed once, with the last Seq.
    EXPECT_TRUE(push(3));
    ASSERT_EQ(next(), ETaskResult::OK);
    ASSERT_EQ(next(), ETaskResult::OK);
    EXPECT_EQ(rowSet.RowCount, 3);
    EXPECT_EQ(next(), ETaskResult::WAITING);
    ASSERT_TRUE(channel.SourceQueue().TryPop(queued));
    EXPECT_FALSE(channel.SourceQueue().TryPop(queued));
    EXPECT_EQ(queued->TakeApplied(), 3);
    EXPECT_EQ(destroyed.load(), 5);
}

TEST(DistributedScheduler, ValidatesRemoteEndpoints) {
    using namespace NQdb::NScheduler;
    TActorNotificationChannel channel(std::make_unique<TFakeNotifier>(), 2, 2);
    auto [sinkCode, sinkState] = MakeRemoteSink(1, TActorId{}, 2, {}, channel);
    auto [sourceCode, sourceState] = MakeRemoteSource(2, TActorId{}, 1, channel);
    auto connection = std::make_unique<TOneToOneConnection>();
    auto* connectionPtr = connection.get();
    TTaskGraph graph;
    auto& source = graph.AddOwnedNode(std::make_unique<TSourceTask>(
        nullptr, nullptr, TOutputPort{.Connection = connectionPtr}));
    auto& sink = graph.AddOwnedNode(std::make_unique<TSinkTask>(
        nullptr, nullptr, TInputPort{.Connection = connectionPtr}));
    graph.AddOwnedEdge(source, sink, std::move(connection));
    graph.MarkExternal(sink, 1);
    graph.MarkExternal(source, 2);
    const auto sealed = graph.Seal();
    ASSERT_TRUE(sealed);

    auto endpoint = [](uint64_t id, ERemoteEndpointKind kind) {
        TRemoteEndpoint result{.Id = id, .Kind = kind};
        if (kind == ERemoteEndpointKind::Sink) {
            result.Layouts = {TValueLayout{.Width = 8}};
        }
        return result;
    };
    const std::vector<TRemoteEndpoint> good{
        endpoint(1, ERemoteEndpointKind::Sink), endpoint(2, ERemoteEndpointKind::Source)};
    EXPECT_EQ(ValidateEndpoints(sealed, good), "");
    // Every source or sink without code needs an endpoint: a null Code would crash the scheduler.
    const std::vector<TRemoteEndpoint> sinkOnly{endpoint(1, ERemoteEndpointKind::Sink)};
    EXPECT_NE(ValidateEndpoints(sealed, sinkOnly), "");
    EXPECT_NE(ValidateEndpoints(sealed, {}), "");
    auto noLayouts = endpoint(1, ERemoteEndpointKind::Sink);
    noLayouts.Layouts.clear();
    EXPECT_NE(ValidateEndpoints(sealed, std::vector{noLayouts, endpoint(2, ERemoteEndpointKind::Source)}), "");
    const std::vector<TRemoteEndpoint> missing{endpoint(3, ERemoteEndpointKind::Sink)};
    EXPECT_NE(ValidateEndpoints(sealed, missing), "");
    const std::vector<TRemoteEndpoint> swapped{
        endpoint(1, ERemoteEndpointKind::Source), endpoint(2, ERemoteEndpointKind::Sink)};
    EXPECT_NE(ValidateEndpoints(sealed, swapped), "");
    const std::vector<TRemoteEndpoint> twice{
        endpoint(1, ERemoteEndpointKind::Sink), endpoint(1, ERemoteEndpointKind::Sink)};
    EXPECT_NE(ValidateEndpoints(sealed, twice), "");

    // A node that already has code is not a remote endpoint.
    static_cast<TSinkTask*>(sink.Task)->BindCode(sinkCode, sinkState);
    EXPECT_NE(ValidateEndpoints(sealed, good), "");
    const std::vector<TRemoteEndpoint> sourceOnly{endpoint(2, ERemoteEndpointKind::Source)};
    EXPECT_EQ(ValidateEndpoints(sealed, sourceOnly), "");
    static_cast<TSourceTask*>(source.Task)->BindCode(sourceCode, sourceState);
    EXPECT_NE(ValidateEndpoints(sealed, sourceOnly), "");
}

namespace {

// Counts what each compute actor reports to the query actor.
class TReportProbe : public IActor {
public:
    TReportProbe(std::map<std::string, int>& statuses, std::vector<std::string>& errors)
        : Statuses_(statuses)
        , Errors_(errors)
    {}

    void Receive(TMessageId messageId, TBlob blob, TActorContext::TPtr ctx) override {
        if (messageId == TComputeStatus::MessageId) {
            ++Statuses_[ctx->Sender().ToString()];
        } else {
            ASSERT_EQ(messageId, TComputeError::MessageId);
            Errors_.push_back(ReadMessage<TComputeError>(blob).Message);
        }
    }

private:
    std::map<std::string, int>& Statuses_;
    std::vector<std::string>& Errors_;
};

struct TSmokeCollector {
    std::mutex Mutex;
    std::vector<int64_t> Rows;
    std::atomic<bool> Finished = false;
};

} // namespace

// CA1: [local source -> remote sink] -> CA2: [remote source -> local sink], one actor system.
TEST(DistributedScheduler, TwoComputeActorsMoveRowsThroughRemoteTasks) {
    using namespace NQdb::NScheduler;
    constexpr int64_t total = 500;
    std::atomic<int> destroyed = 0;
    TProducerState producerState{.Total = total};
    TSmokeCollector collector;

    // The graphs outlive the actors: their graph copies point into these.
    auto producerCode = std::make_shared<TSourceCode>([&](void* state, TRowSet& rowSet) {
        auto* producer = static_cast<TProducerState*>(state);
        if (producer->Next > producer->Total) {
            return ETaskResult::FINISHED;
        }
        rowSet = CountedRows(producer->Next++, &destroyed);
        return ETaskResult::OK;
    });
    auto collectorCode = std::make_shared<TSinkCode>(
        [](void* state, const TRowSet& rowSet) {
            auto* result = static_cast<TSmokeCollector*>(state);
            std::lock_guard lock(result->Mutex);
            result->Rows.push_back(rowSet.RowCount);
            return ETaskResult::OK;
        },
        [](void* state) {
            static_cast<TSmokeCollector*>(state)->Finished.store(true);
            return ETaskResult::OK;
        });

    auto toRemoteSink = std::make_unique<TOneToOneConnection>();
    auto* toRemoteSinkPtr = toRemoteSink.get();
    TTaskGraph graph1;
    auto& producer = graph1.AddOwnedNode(std::make_unique<TSourceTask>(
        producerCode, std::shared_ptr<void>(&producerState, [](void*) {}),
        TOutputPort{.Connection = toRemoteSinkPtr}));
    auto& remoteSink = graph1.AddOwnedNode(std::make_unique<TSinkTask>(
        nullptr, nullptr, TInputPort{.Connection = toRemoteSinkPtr}));
    graph1.AddOwnedEdge(producer, remoteSink, std::move(toRemoteSink));
    graph1.MarkExternal(remoteSink, 1);
    const auto sealed1 = graph1.Seal();
    ASSERT_TRUE(sealed1);

    auto fromRemoteSource = std::make_unique<TOneToOneConnection>();
    auto* fromRemoteSourcePtr = fromRemoteSource.get();
    TTaskGraph graph2;
    auto& remoteSource = graph2.AddOwnedNode(std::make_unique<TSourceTask>(
        nullptr, nullptr, TOutputPort{.Connection = fromRemoteSourcePtr}));
    auto& localSink = graph2.AddOwnedNode(std::make_unique<TSinkTask>(
        collectorCode, std::shared_ptr<void>(&collector, [](void*) {}),
        TInputPort{.Connection = fromRemoteSourcePtr}));
    graph2.AddOwnedEdge(remoteSource, localSink, std::move(fromRemoteSource));
    graph2.MarkExternal(remoteSource, 2);
    const auto sealed2 = graph2.Seal();
    ASSERT_TRUE(sealed2);

    auto resources = std::make_shared<TRuntimeResources>();
    resources->Scheduler.WorkerCount = 2;
    resources->Scheduler.StatusPeriodMs = 5;
    TTestCluster cluster;
    std::map<std::string, int> statuses;
    std::vector<std::string> errors;
    const auto query = cluster.Follower.Register(std::make_unique<TReportProbe>(statuses, errors));
    const auto ca1 = cluster.Follower.Register(std::unique_ptr<IActor>(CreateComputeActor(resources)));
    const auto ca2 = cluster.Follower.Register(std::unique_ptr<IActor>(CreateComputeActor(resources)));

    cluster.Follower.Send<TTaskGraphMessage>(query, ca1, TTaskGraphMessage{
        .Graph = sealed1,
        .Endpoints = {{.Id = 1, .Kind = ERemoteEndpointKind::Sink, .PeerActor = ca2, .PeerTaskId = 2,
            .Layouts = {TValueLayout{.Width = 8}}}}});
    cluster.Follower.Send<TTaskGraphMessage>(query, ca2, TTaskGraphMessage{
        .Graph = sealed2,
        .Endpoints = {{.Id = 2, .Kind = ERemoteEndpointKind::Source, .PeerActor = ca1, .PeerTaskId = 1}}});

    // The deadline only guards against a hang.
    const auto deadline = NNet::TClock::now() + std::chrono::seconds(60);
    auto reported = [&] {
        return statuses.contains(ca1.ToString()) && statuses.contains(ca2.ToString());
    };
    while ((!collector.Finished.load() || !reported()) && NNet::TClock::now() < deadline) {
        cluster.Pump();
    }
    EXPECT_TRUE(collector.Finished.load());
    EXPECT_TRUE(reported());
    EXPECT_TRUE(errors.empty());

    cluster.Follower.Send<TPoison>(query, ca1, TPoison{});
    cluster.Follower.Send<TPoison>(query, ca2, TPoison{});
    cluster.Pump();

    std::lock_guard lock(collector.Mutex);
    ASSERT_EQ(collector.Rows.size(), static_cast<size_t>(total));
    for (int64_t i = 0; i < total; ++i) {
        ASSERT_EQ(collector.Rows[i], i + 1);
    }
    // In one system the chunk is a shallow copy, so both sides release the same row set.
    EXPECT_GE(destroyed.load(), total);
}

namespace {

class TBlackHole : public IActor {
public:
    void Receive(TMessageId, TBlob, TActorContext::TPtr) override { }
};

// Rows with `columns` columns for a sink whose layout count differs.
TRowSet UnsendableRows(int64_t columns, bool selection, std::atomic<int>* destroyed) {
    static TColumn storage[2] = {};
    static uint8_t selected[1] = {1};
    auto rows = CountedRows(1, destroyed);
    rows.Columns = storage;
    rows.ColumnCount = columns;
    rows.Selection = selection ? selected : nullptr;
    return rows;
}

} // namespace

// The sink is on another node, so a failing Send must reach the query actor as TComputeError.
TEST(DistributedScheduler, ComputeActorReportsAFailedSendAndReleasesTheRowSet) {
    using namespace NQdb::NScheduler;
    struct TCase {
        int64_t Columns;
        bool Selection;
        size_t Layouts;
    };
    for (const auto& c : {TCase{2, false, 1}, TCase{1, false, 2}}) {
        std::atomic<int> destroyed = 0;
        TProducerState producerState{.Total = 1000};
        auto producerCode = std::make_shared<TSourceCode>([&](void* state, TRowSet& rowSet) {
            auto* producer = static_cast<TProducerState*>(state);
            if (producer->Next > producer->Total) {
                return ETaskResult::FINISHED;
            }
            ++producer->Next;
            rowSet = UnsendableRows(c.Columns, c.Selection, &destroyed);
            return ETaskResult::OK;
        });
        auto connection = std::make_unique<TOneToOneConnection>();
        auto* connectionPtr = connection.get();
        TTaskGraph graph;
        auto& producer = graph.AddOwnedNode(std::make_unique<TSourceTask>(
            producerCode, std::shared_ptr<void>(&producerState, [](void*) {}),
            TOutputPort{.Connection = connectionPtr}));
        auto& remoteSink = graph.AddOwnedNode(std::make_unique<TSinkTask>(
            nullptr, nullptr, TInputPort{.Connection = connectionPtr}));
        graph.AddOwnedEdge(producer, remoteSink, std::move(connection));
        graph.MarkExternal(remoteSink, 1);
        const auto sealed = graph.Seal();
        ASSERT_TRUE(sealed);

        auto resources = std::make_shared<TRuntimeResources>();
        resources->Scheduler.WorkerCount = 2;
        TTestCluster cluster;
        std::map<std::string, int> statuses;
        std::vector<std::string> errors;
        const auto query = cluster.Follower.Register(std::make_unique<TReportProbe>(statuses, errors));
        // The peer is on the other node, so the chunk goes through serialization.
        const auto peer = cluster.Leader.Register(std::make_unique<TBlackHole>());
        const auto actor = cluster.Follower.Register(std::unique_ptr<IActor>(CreateComputeActor(resources)));
        TRemoteEndpoint sink{.Id = 1, .Kind = ERemoteEndpointKind::Sink, .PeerActor = peer, .PeerTaskId = 2};
        sink.Layouts.assign(c.Layouts, TValueLayout{.Width = 8});
        cluster.Follower.Send<TTaskGraphMessage>(query, actor, TTaskGraphMessage{
            .Graph = sealed, .Endpoints = {sink}});
        const auto deadline = NNet::TClock::now() + std::chrono::seconds(10);
        while (errors.empty() && NNet::TClock::now() < deadline) {
            cluster.Pump();
        }
        ASSERT_EQ(errors.size(), 1u);
        EXPECT_NE(errors[0].find("compute actor failed"), std::string::npos);
        EXPECT_GE(destroyed.load(), 1);
        cluster.Follower.Send<TPoison>(query, actor, TPoison{});
        cluster.Pump();
    }
}

// The peer never confirms, so the window stays closed and the outbox fills up.
TEST(DistributedScheduler, RemoteSinkReleasesItsOutboxOnTeardown) {
    using namespace NQdb::NScheduler;
    std::atomic<int> destroyed = 0;
    std::atomic<int> produced = 0;
    TProducerState producerState{.Total = 1000};
    auto producerCode = std::make_shared<TSourceCode>([&](void* state, TRowSet& rowSet) {
        auto* producer = static_cast<TProducerState*>(state);
        if (producer->Next > producer->Total) {
            return ETaskResult::FINISHED;
        }
        rowSet = CountedRows(producer->Next++, &destroyed);
        ++produced;
        return ETaskResult::OK;
    });
    {
        auto connection = std::make_unique<TOneToOneConnection>();
        auto* connectionPtr = connection.get();
        TTaskGraph graph;
        auto& producer = graph.AddOwnedNode(std::make_unique<TSourceTask>(
            producerCode, std::shared_ptr<void>(&producerState, [](void*) {}),
            TOutputPort{.Connection = connectionPtr}));
        auto& remoteSink = graph.AddOwnedNode(std::make_unique<TSinkTask>(
            nullptr, nullptr, TInputPort{.Connection = connectionPtr}));
        graph.AddOwnedEdge(producer, remoteSink, std::move(connection));
        graph.MarkExternal(remoteSink, 1);
        const auto sealed = graph.Seal();
        ASSERT_TRUE(sealed);

        auto resources = std::make_shared<TRuntimeResources>();
        resources->Scheduler.WorkerCount = 2;
        TTestCluster cluster;
        std::map<std::string, int> statuses;
        std::vector<std::string> errors;
        const auto query = cluster.Follower.Register(std::make_unique<TReportProbe>(statuses, errors));
        const auto peer = cluster.Follower.Register(std::make_unique<TBlackHole>());
        const auto actor = cluster.Follower.Register(std::unique_ptr<IActor>(CreateComputeActor(resources)));
        cluster.Follower.Send<TTaskGraphMessage>(query, actor, TTaskGraphMessage{
            .Graph = sealed,
            .Endpoints = {{.Id = 1, .Kind = ERemoteEndpointKind::Sink, .PeerActor = peer, .PeerTaskId = 2,
            .Layouts = {TValueLayout{.Width = 8}}}}});
        const auto until = NNet::TClock::now() + std::chrono::milliseconds(200);
        while (NNet::TClock::now() < until) {
            cluster.Pump();
        }
        cluster.Follower.Send<TPoison>(query, actor, TPoison{});
        cluster.Pump();
    }
    EXPECT_GT(produced.load(), 2);
    EXPECT_EQ(destroyed.load(), produced.load());
}

namespace {

// One int64 column with `values`; only the rows with selection[i] != 0 are real.
struct TSelectedInts {
    std::vector<int64_t> Values;
    std::vector<uint8_t> Selection;
    std::vector<int64_t> OriginalValues;
    std::vector<uint8_t> OriginalSelection;
    std::atomic<bool>* Intact = nullptr;
    TColumn Column{};
};

// `intact` is set on destruction: true if the sender's buffers were not modified.
TRowSet SelectedInts(std::vector<int64_t> values, std::vector<uint8_t> selection, std::atomic<bool>* intact) {
    auto* data = new TSelectedInts{.Values = values, .Selection = selection,
        .OriginalValues = values, .OriginalSelection = selection, .Intact = intact};
    data->Column.Data = reinterpret_cast<char*>(data->Values.data());
    return TRowSet{
        .Columns = &data->Column,
        .ColumnCount = 1,
        .RowCount = static_cast<int64_t>(data->Values.size()),
        .Selection = data->Selection.data(),
        .Destroy = [](TRowSet* rowSet) {
            auto* data = static_cast<TSelectedInts*>(rowSet->Private);
            data->Intact->store(data->Values == data->OriginalValues &&
                data->Selection == data->OriginalSelection && rowSet->RowCount == 4);
            delete data;
        },
        .Private = data,
        .RefCount = 1,
    };
}

} // namespace

// CA1 (follower): producer -> remote sink; CA2 (leader): remote source -> collector.
TEST(DistributedScheduler, SelectedRowsCrossNodesCompacted) {
    using namespace NQdb::NScheduler;
    TProducerState producerState{.Total = 1};
    std::atomic<bool> senderIntact = false;
    auto producerCode = std::make_shared<TSourceCode>([&](void* state, TRowSet& rowSet) {
        auto* producer = static_cast<TProducerState*>(state);
        if (producer->Next > producer->Total) {
            return ETaskResult::FINISHED;
        }
        ++producer->Next;
        rowSet = SelectedInts({10, 20, 30, 40}, {1, 0, 1, 0}, &senderIntact);
        return ETaskResult::OK;
    });
    std::mutex mutex;
    std::vector<int64_t> received;
    std::atomic<bool> finished = false;
    auto collectorCode = std::make_shared<TSinkCode>(
        [&](void*, const TRowSet& rowSet) {
            std::lock_guard lock(mutex);
            EXPECT_EQ(rowSet.Selection, nullptr);
            const auto* data = reinterpret_cast<const int64_t*>(rowSet.Columns[0].Data);
            received.assign(data, data + rowSet.RowCount);
            return ETaskResult::OK;
        },
        [&](void*) {
            finished.store(true);
            return ETaskResult::OK;
        });

    auto toRemoteSink = std::make_unique<TOneToOneConnection>();
    auto* toRemoteSinkPtr = toRemoteSink.get();
    TTaskGraph graph1;
    auto& producer = graph1.AddOwnedNode(std::make_unique<TSourceTask>(
        producerCode, std::shared_ptr<void>(&producerState, [](void*) {}),
        TOutputPort{.Connection = toRemoteSinkPtr}));
    auto& remoteSink = graph1.AddOwnedNode(std::make_unique<TSinkTask>(
        nullptr, nullptr, TInputPort{.Connection = toRemoteSinkPtr}));
    graph1.AddOwnedEdge(producer, remoteSink, std::move(toRemoteSink));
    graph1.MarkExternal(remoteSink, 1);
    const auto sealed1 = graph1.Seal();
    ASSERT_TRUE(sealed1);

    auto fromRemoteSource = std::make_unique<TOneToOneConnection>();
    auto* fromRemoteSourcePtr = fromRemoteSource.get();
    TTaskGraph graph2;
    auto& remoteSource = graph2.AddOwnedNode(std::make_unique<TSourceTask>(
        nullptr, nullptr, TOutputPort{.Connection = fromRemoteSourcePtr}));
    auto& localSink = graph2.AddOwnedNode(std::make_unique<TSinkTask>(
        collectorCode, nullptr, TInputPort{.Connection = fromRemoteSourcePtr}));
    graph2.AddOwnedEdge(remoteSource, localSink, std::move(fromRemoteSource));
    graph2.MarkExternal(remoteSource, 2);
    const auto sealed2 = graph2.Seal();
    ASSERT_TRUE(sealed2);

    auto resources = std::make_shared<TRuntimeResources>();
    resources->Scheduler.WorkerCount = 2;
    TTestCluster cluster;
    std::map<std::string, int> statuses;
    std::vector<std::string> errors;
    const auto queryF = cluster.Follower.Register(std::make_unique<TReportProbe>(statuses, errors));
    const auto queryL = cluster.Leader.Register(std::make_unique<TReportProbe>(statuses, errors));
    const auto ca1 = cluster.Follower.Register(std::unique_ptr<IActor>(CreateComputeActor(resources)));
    const auto ca2 = cluster.Leader.Register(std::unique_ptr<IActor>(CreateComputeActor(resources)));
    cluster.Follower.Send<TTaskGraphMessage>(queryF, ca1, TTaskGraphMessage{
        .Graph = sealed1,
        .Endpoints = {{.Id = 1, .Kind = ERemoteEndpointKind::Sink, .PeerActor = ca2, .PeerTaskId = 2,
            .Layouts = {TValueLayout{.Width = 8}}}}});
    cluster.Leader.Send<TTaskGraphMessage>(queryL, ca2, TTaskGraphMessage{
        .Graph = sealed2,
        .Endpoints = {{.Id = 2, .Kind = ERemoteEndpointKind::Source, .PeerActor = ca1, .PeerTaskId = 1}}});
    const auto deadline = NNet::TClock::now() + std::chrono::seconds(10);
    while (!finished.load() && errors.empty() && NNet::TClock::now() < deadline) {
        cluster.Pump();
    }
    EXPECT_TRUE(finished.load());
    EXPECT_TRUE(errors.empty());
    cluster.Follower.Send<TPoison>(queryF, ca1, TPoison{});
    cluster.Leader.Send<TPoison>(queryL, ca2, TPoison{});
    cluster.Pump();
    std::lock_guard lock(mutex);
    EXPECT_EQ(received, (std::vector<int64_t>{10, 30}));
    EXPECT_TRUE(senderIntact.load());
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
