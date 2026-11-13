#include "remote_task.h"
#include "qdb/scheduler/connection.h"

#include <qdbd/messages.h>
#include <qdbd/serialization.h>

#include <coroio/actors/actorsystem.hpp>

#include <optional>
#include <stdexcept>
#include <unordered_set>

namespace NQdb {
namespace NScheduler {

using namespace NRemote;

namespace {

uint64_t StringOffset(const TColumn& column, int64_t row) {
    if (column.OffsetWidth == 4) {
        return static_cast<uint32_t>(static_cast<const int32_t*>(column.Offsets)[row]);
    }
    return static_cast<uint64_t>(static_cast<const int64_t*>(column.Offsets)[row]);
}

// The bytes of a row set's values, masks, strings and hashes, unselected rows
// included: a bound on its size in a chunk that needs no pass over the rows.
// Checking the row set is left to the serializer on the actor.
uint64_t RowSetBytes(const TRowSet& rowSet, std::span<const TValueLayout> layouts) {
    const auto rows = static_cast<uint64_t>(std::max<int64_t>(rowSet.RowCount, 0));
    const uint64_t bitmap = (rows + 7) / 8;
    uint64_t bytes = rowSet.Hash ? rows * sizeof(uint64_t) : 0;
    const size_t columns = rowSet.Columns
        ? std::min(static_cast<size_t>(std::max<int64_t>(rowSet.ColumnCount, 0)), layouts.size())
        : 0;
    for (size_t i = 0; i < columns; ++i) {
        const auto& layout = layouts[i];
        const auto& column = rowSet.Columns[i];
        if (layout.Bitmap) {
            bytes += bitmap;
        } else if (layout.Width != 0) {
            bytes += rows * static_cast<uint64_t>(layout.Width);
        } else if (column.Offsets && (column.OffsetWidth == 4 || column.OffsetWidth == 8)) {
            const auto first = StringOffset(column, 0);
            const auto last = StringOffset(column, rowSet.RowCount);
            bytes += (rows + 1) * column.OffsetWidth + (last > first ? last - first : 0);
        }
        if (column.Mask) {
            bytes += bitmap;
        }
    }
    return bytes;
}

} // namespace

TRemoteSinkState::TRemoteSinkState(
    uint64_t id,
    NNet::NActors::TActorId actorId,
    uint64_t dstTaskId,
    std::span<const TValueLayout> descriptor,
    TActorNotificationChannel& actorChannel,
    TRemoteStreamLimits limits)
    : Id(id)
    , ActorId(actorId)
    , DstTaskId(dstTaskId)
    , Descriptor(descriptor)
    , ActorChannel(actorChannel)
    , Limits(limits)
    , Outbox(limits.OutboxCapacity)
{ }

TRemoteSinkState::~TRemoteSinkState() {
    TQueued queued;
    while (Outbox.TryPop(queued)) {
        Release(&queued.RowSet);
    }
}

ETaskResult TRemoteSinkState::Write(const TRowSet& rowSet) {
    if (OutboxBytes.load() >= Limits.ChunkBytes) {
        return ETaskResult::WAITING;
    }
    // TSinkTask releases its reference after Write. The outbox copy owns the
    // one taken here and is released by whoever drains it.
    auto& task = const_cast<TRowSet&>(rowSet);
    Retain(&task);
    TQueued queued{.RowSet = task, .Bytes = RowSetBytes(task, Descriptor)};
    queued.RowSet.RefCount = 1;
    // Counted before the push, so the actor's pop never takes the count below zero.
    OutboxBytes.fetch_add(queued.Bytes);
    if (!Outbox.TryPush(std::move(queued))) {
        OutboxBytes.fetch_sub(queued.Bytes);
        --task.RefCount;
        return ETaskResult::WAITING;
    }
    ++Written;
    // Pairs with the fence in Drain: the push must be visible before Queued is read.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (!Queued.exchange(true)) {
        ActorChannel.Notify(this);
    }
    return ETaskResult::OK;
}

ETaskResult TRemoteSinkState::Finish() {
    InputFinished.store(true);
    if (!Queued.exchange(true)) {
        ActorChannel.Notify(this);
    }
    return ETaskResult::OK;
}

void TRemoteSinkState::SendQueue(NNet::NActors::TActorContext::TPtr& ctx) {
    Drain(
        [&](TDataChunk&& chunk) {
            SendChunk(ctx, std::move(chunk));
        },
        [&] {
            SendFinish(ctx);
        });
}

bool TRemoteSinkState::OnConfirm(uint64_t lastApplied, NNet::NActors::TActorContext::TPtr& ctx) {
    return Confirm(
        lastApplied,
        [&](TDataChunk&& chunk) {
            SendChunk(ctx, std::move(chunk));
        },
        [&] {
            SendFinish(ctx);
        });
}

// The chunk releases its rows also when the check throws.
void TRemoteSinkState::SendChunk(NNet::NActors::TActorContext::TPtr& ctx, TDataChunk&& chunk) {
    for (const auto& rowSet : chunk.RowSets) {
        if (static_cast<size_t>(rowSet.ColumnCount) != Descriptor.size()) {
            throw std::runtime_error("remote sink " + std::to_string(Id) + ": row set columns do not match layouts");
        }
    }
    ctx->Send<TDataChunk>(ActorId, std::move(chunk));
}

void TRemoteSinkState::SendFinish(NNet::NActors::TActorContext::TPtr& ctx) {
    ctx->Send<TFinish>(ActorId, TFinish {
        .DstTaskId = DstTaskId,
    });
}

TRemoteSourceState::TRemoteSourceState(
    uint64_t id,
    NNet::NActors::TActorId peerActor,
    uint64_t peerTaskId,
    TActorNotificationChannel& actorChannel,
    size_t inboxCapacity)
    : Id(id)
    , PeerActor(peerActor)
    , PeerTaskId(peerTaskId)
    , ActorChannel(actorChannel)
    , Inbox(inboxCapacity)
{ }

bool TRemoteSourceState::Push(TDataChunk&& chunk) {
    const auto seq = chunk.Seq;
    if (seq != Received + 1 || !Inbox.TryPush(std::move(chunk))) {
        return false;
    }
    Received = seq;
    return true;
}

void TRemoteSourceState::Finish() {
    Finished.store(true, std::memory_order_release);
}

uint64_t TRemoteSourceState::TakeApplied() {
    // Cleared before the read: a later pop queues the state again.
    Queued.store(false);
    const auto applied = Applied.load();
    if (applied == Confirmed) {
        return 0;
    }
    Confirmed = applied;
    return applied;
}

void TRemoteSourceState::SendConfirm(NNet::NActors::TActorContext::TPtr& ctx) {
    if (const auto applied = TakeApplied()) {
        ctx->Send<TDataConfirm>(PeerActor, TDataConfirm {
            .SinkId = PeerTaskId,
            .LastApplied = applied,
        });
    }
}

ETaskResult TRemoteSourceState::Next(TRowSet& rowSet) {
    if (Pop(rowSet)) {
        return ETaskResult::OK;
    }
    if (!Finished.load(std::memory_order_acquire)) {
        return ETaskResult::WAITING;
    }
    // Rows pushed before Finish are visible now.
    return Pop(rowSet) ? ETaskResult::OK : ETaskResult::FINISHED;
}

bool TRemoteSourceState::Pop(TRowSet& rowSet) {
    while (CurrentPos == Current.RowSets.size()) {
        Current = {};
        CurrentPos = 0;
        if (!Inbox.TryPop(Current)) {
            return false;
        }
        // Seqs arrive without gaps, so the pop count is the Seq of this chunk.
        Applied.fetch_add(1);
        if (!Queued.exchange(true)) {
            ActorChannel.Notify(this);
        }
    }
    // The row set handed out belongs to the task now.
    rowSet = std::exchange(Current.RowSets[CurrentPos++], TRowSet{});
    return true;
}

TRuntimeFilterSinkState::TRuntimeFilterSinkState(
    uint64_t id,
    NNet::NActors::TActorId peerActor,
    uint64_t peerTaskId,
    TActorNotificationChannel& actorChannel)
    : Id(id)
    , PeerActor(peerActor)
    , PeerTaskId(peerTaskId)
    , ActorChannel(actorChannel)
{ }

void TRuntimeFilterSinkState::Send(TRuntimeFilterPartial&& partial) {
    Partial = std::move(partial);
    ActorChannel.Notify(this);
}

void TRuntimeFilterSinkState::SendPartial(NNet::NActors::TActorContext::TPtr& ctx) {
    ctx->Send<TFilterPartial>(PeerActor, TFilterPartial {
        .DstTaskId = PeerTaskId,
        .Partial = std::move(Partial),
    });
}

TRuntimeFilterSourceState::TRuntimeFilterSourceState(uint64_t id)
    : Id(id)
{ }

bool TRuntimeFilterSourceState::Push(TRuntimeFilterPartial&& partial) {
    if (Received) {
        return false;
    }
    Received = true;
    Partial = std::move(partial);
    Arrived.store(true, std::memory_order_release);
    return true;
}

bool TRuntimeFilterSourceState::Take(TRuntimeFilterPartial& partial) {
    if (!Arrived.load(std::memory_order_acquire)) {
        return false;
    }
    partial = std::move(Partial);
    return true;
}

namespace {

// A task whose code the compute actor binds, the endpoint kind it takes.
struct TRemoteTask {
    ERemoteEndpointKind Kind = ERemoteEndpointKind::Source;
    bool HasCode = false;
};

std::optional<TRemoteTask> RemoteTask(const ITask* task) {
    if (const auto* source = dynamic_cast<const TSourceTask*>(task)) {
        return TRemoteTask{ERemoteEndpointKind::Source, source->Code() != nullptr};
    }
    if (const auto* sink = dynamic_cast<const TSinkTask*>(task)) {
        return TRemoteTask{ERemoteEndpointKind::Sink, sink->Code() != nullptr};
    }
    if (const auto* source = dynamic_cast<const TRuntimeFilterSourceTask*>(task)) {
        return TRemoteTask{ERemoteEndpointKind::FilterSource, source->Code() != nullptr};
    }
    if (const auto* sink = dynamic_cast<const TRuntimeFilterSinkTask*>(task)) {
        return TRemoteTask{ERemoteEndpointKind::FilterSink, sink->Code() != nullptr};
    }
    if (const auto* source = dynamic_cast<const TCteSourceTask*>(task)) {
        return TRemoteTask{ERemoteEndpointKind::CteSource, source->Code() != nullptr};
    }
    if (const auto* sink = dynamic_cast<const TCteSinkTask*>(task)) {
        return TRemoteTask{ERemoteEndpointKind::CteSink, sink->Code() != nullptr};
    }
    return std::nullopt;
}

std::string KindName(ERemoteEndpointKind kind) {
    switch (kind) {
        case ERemoteEndpointKind::Source:
            return "source";
        case ERemoteEndpointKind::Sink:
            return "sink";
        case ERemoteEndpointKind::FilterSource:
            return "runtime filter source";
        case ERemoteEndpointKind::FilterSink:
            return "runtime filter sink";
        case ERemoteEndpointKind::CteSource:
            return "cte source";
        case ERemoteEndpointKind::CteSink:
            return "cte sink";
    }
    return "unknown";
}

} // namespace

std::string ValidateEndpoints(
    const TSealedTaskGraph& graph,
    std::span<const TRemoteEndpoint> endpoints)
{
    std::unordered_set<uint64_t> seen;
    std::unordered_set<const TTaskNode*> covered;
    for (const auto& endpoint : endpoints) {
        const auto* node = graph.ExternalNode(endpoint.Id);
        if (!node) {
            return "no external node with id " + std::to_string(endpoint.Id);
        }
        if (!seen.insert(endpoint.Id).second) {
            return "external node " + std::to_string(endpoint.Id) + " has two endpoints";
        }
        const auto remote = RemoteTask(node->Task);
        if (!remote || remote->Kind != endpoint.Kind) {
            return "external node " + std::to_string(endpoint.Id) + " is not a " + KindName(endpoint.Kind) + " task";
        }
        if (remote->HasCode) {
            return "external node " + std::to_string(endpoint.Id) + " already has code";
        }
        const auto* sink = dynamic_cast<const TSinkTask*>(node->Task);
        if (sink && (!sink->InputConnection() || sink->InputConnection()->RowLayouts().empty())) {
            return "sink " + std::to_string(endpoint.Id) + " has no input row layouts";
        }
        covered.insert(node);
    }
    // A remote task left without code would dereference a null Code_.
    std::vector<const TTaskNode*> stack;
    for (const auto& roots : graph.Graphs()) {
        stack.insert(stack.end(), roots.begin(), roots.end());
    }
    std::unordered_set<const TTaskNode*> visited(stack.begin(), stack.end());
    while (!stack.empty()) {
        const auto* node = stack.back();
        stack.pop_back();
        const auto remote = RemoteTask(node->Task);
        if (remote && !remote->HasCode && !covered.contains(node)) {
            return "a " + KindName(remote->Kind) + " task without code has no endpoint";
        }
        for (const auto* edges : {&node->Inbound, &node->Outbound}) {
            for (const auto* edge : *edges) {
                for (const auto* next : {edge->Src, edge->Dst}) {
                    if (visited.insert(next).second) {
                        stack.push_back(next);
                    }
                }
            }
        }
    }
    return {};
}

std::pair<std::shared_ptr<const TSinkCode>, std::shared_ptr<TRemoteSinkState>> MakeRemoteSink(
    uint64_t id,
    NNet::NActors::TActorId actorId,
    uint64_t dstTaskId,
    std::span<const TValueLayout> descriptor,
    TActorNotificationChannel& actorChannel,
    TRemoteStreamLimits limits)
{
    auto code = std::make_shared<const TSinkCode>(
        [](void* state, const TRowSet& rowSet) {
            return static_cast<TRemoteSinkState*>(state)->Write(rowSet);
        },
        [](void* state) {
            return static_cast<TRemoteSinkState*>(state)->Finish();
        });
    auto state = std::make_shared<TRemoteSinkState>(id, actorId, dstTaskId, descriptor, actorChannel, limits);
    return {std::move(code), std::move(state)};
}

std::pair<std::shared_ptr<const TSourceCode>, std::shared_ptr<TRemoteSourceState>> MakeRemoteSource(
    uint64_t id,
    NNet::NActors::TActorId peerActor,
    uint64_t peerTaskId,
    TActorNotificationChannel& actorChannel,
    size_t inboxCapacity)
{
    auto code = std::make_shared<const TSourceCode>([](void* state, TRowSet& rowSet) {
        return static_cast<TRemoteSourceState*>(state)->Next(rowSet);
    });
    auto state = std::make_shared<TRemoteSourceState>(
        id, peerActor, peerTaskId, actorChannel, inboxCapacity);
    return {std::move(code), std::move(state)};
}

std::pair<std::shared_ptr<const TRuntimeFilterSinkCode>, std::shared_ptr<TRuntimeFilterSinkState>>
MakeRuntimeFilterSink(
    uint64_t id,
    NNet::NActors::TActorId peerActor,
    uint64_t peerTaskId,
    TActorNotificationChannel& actorChannel)
{
    auto code = std::make_shared<const TRuntimeFilterSinkCode>([](void* state, TRuntimeFilterPartial&& partial) {
        static_cast<TRuntimeFilterSinkState*>(state)->Send(std::move(partial));
    });
    auto state = std::make_shared<TRuntimeFilterSinkState>(id, peerActor, peerTaskId, actorChannel);
    return {std::move(code), std::move(state)};
}

std::pair<std::shared_ptr<const TRuntimeFilterSourceCode>, std::shared_ptr<TRuntimeFilterSourceState>>
MakeRuntimeFilterSource(uint64_t id)
{
    auto code = std::make_shared<const TRuntimeFilterSourceCode>([](void* state, TRuntimeFilterPartial& partial) {
        return static_cast<TRuntimeFilterSourceState*>(state)->Take(partial);
    });
    return {std::move(code), std::make_shared<TRuntimeFilterSourceState>(id)};
}

} // namespace NScheduler
} // namespace NQdb
