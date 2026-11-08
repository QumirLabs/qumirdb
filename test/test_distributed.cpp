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

TEST(DistributedSerialization, RejectsUncompactedAndInvalidSourceBatches) {
    TMixedBatch batch;
    batch.Message.RowSet.RowCount = -1;
    EXPECT_THROW(Encode(batch.Message), std::runtime_error);
    batch.Message.RowSet.RowCount = 3;
    batch.Message.RowSet.ColumnCount = -1;
    EXPECT_THROW(Encode(batch.Message), std::runtime_error);
    batch.Message.RowSet.ColumnCount = 4;
    uint8_t selected = 1;
    batch.Message.RowSet.Selection = &selected;
    EXPECT_THROW(Encode(batch.Message), std::runtime_error);
    batch.Message.RowSet.Selection = nullptr;
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

void CountDestroy(TRowSet* rowSet) {
    static_cast<std::atomic<int>*>(rowSet->Private)->fetch_add(1, std::memory_order_relaxed);
}

TRowSet CountedRows(int64_t rows, std::atomic<int>* destroyed) {
    return TRowSet{
        .RowCount = rows,
        .Destroy = CountDestroy,
        .Private = destroyed,
        .RefCount = 1,
    };
}

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
// Both fake actors share one thread; FIFO queues stand for the transport.
void RunRemotePipeline(const TRemotePipeline& options) {
    using namespace NQdb::NScheduler;
    std::atomic<int> destroyed = 0;
    TActorNotificationChannel channelA(std::make_unique<TFakeNotifier>());
    TActorNotificationChannel channelB(std::make_unique<TFakeNotifier>());

    auto [sinkCode, sinkState] = MakeRemoteSink(
        1, TActorId{}, 2, nullptr, channelA, options.OutboxCapacity, options.Window);
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
        int64_t delivered = 0;
        while (!stop.load(std::memory_order_acquire)) {
            bool idle = true;
            TRemoteSinkState* sinkReady = nullptr;
            while (channelA.SinkQueue().TryPop(sinkReady)) {
                idle = false;
                sinkReady->Drain(send);
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
                if (!sourceState->Push(seq, std::move(rowSet))) {
                    ADD_FAILURE() << "chunk " << seq << " does not fit";
                    Release(&rowSet);
                }
                if (++delivered == options.Total) {
                    sourceState->Finish();
                }
                wakeB.Notify(sealedB.ExternalNode(sourceState->Id));
            }
            if (!confirms.empty()) {
                idle = false;
                EXPECT_TRUE(sinkState->Confirm(confirms.front(), send));
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
    TActorNotificationChannel channel(std::make_unique<TFakeNotifier>());
    auto [code, state] = MakeRemoteSink(1, TActorId{}, 2, nullptr, channel, 8, 2);
    std::vector<uint64_t> sent;
    auto send = [&](uint64_t seq, TRowSet&& rowSet) {
        sent.push_back(seq);
        Release(&rowSet);
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
    queued->Drain(send);
    EXPECT_EQ(sent, (std::vector<uint64_t>{1, 2}));
    // A parked state stays with the actor: a new row does not queue it again.
    write(6);
    EXPECT_FALSE(channel.SinkQueue().TryPop(queued));

    EXPECT_TRUE(state->Confirm(1, send));
    EXPECT_EQ(sent, (std::vector<uint64_t>{1, 2, 3}));
    EXPECT_TRUE(state->Confirm(3, send));
    EXPECT_EQ(sent, (std::vector<uint64_t>{1, 2, 3, 4, 5}));
    EXPECT_FALSE(state->Confirm(6, send));
    EXPECT_TRUE(state->Confirm(5, send));
    EXPECT_EQ(sent, (std::vector<uint64_t>{1, 2, 3, 4, 5, 6}));
    EXPECT_FALSE(channel.SinkQueue().TryPop(queued));

    // The outbox ran empty inside the window, so the next row queues the state.
    write(7);
    ASSERT_TRUE(channel.SinkQueue().TryPop(queued));
    queued->Drain(send);
    EXPECT_EQ(sent.back(), 7);
    EXPECT_EQ(destroyed.load(), 7);
}

TEST(DistributedScheduler, RemoteSourceConfirmsConsumedChunks) {
    using namespace NQdb::NScheduler;
    std::atomic<int> destroyed = 0;
    TActorNotificationChannel channel(std::make_unique<TFakeNotifier>());
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
    TActorNotificationChannel channel(std::make_unique<TFakeNotifier>());
    auto [sinkCode, sinkState] = MakeRemoteSink(1, TActorId{}, 2, nullptr, channel);
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
        return TRemoteEndpoint{.Id = id, .Kind = kind};
    };
    const std::vector<TRemoteEndpoint> good{
        endpoint(1, ERemoteEndpointKind::Sink), endpoint(2, ERemoteEndpointKind::Source)};
    EXPECT_EQ(ValidateEndpoints(sealed, good), "");
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

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
