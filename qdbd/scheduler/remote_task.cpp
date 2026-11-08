#include "remote_task.h"
#include "qdb/scheduler/connection.h"

#include <qdbd/messages.h>
#include <qdbd/serialization.h>

#include <coroio/actors/actorsystem.hpp>

#include <stdexcept>
#include <unordered_set>

namespace NQdb {
namespace NScheduler {

using namespace NRemote;

TRemoteSinkState::TRemoteSinkState(
    uint64_t id,
    NNet::NActors::TActorId actorId,
    uint64_t dstTaskId,
    std::span<const TValueLayout> descriptor,
    TActorNotificationChannel& actorChannel,
    size_t outboxCapacity,
    size_t window)
    : Id(id)
    , ActorId(actorId)
    , DstTaskId(dstTaskId)
    , Descriptor(descriptor)
    , ActorChannel(actorChannel)
    , Outbox(outboxCapacity)
    , Window(window)
{ }

TRemoteSinkState::~TRemoteSinkState() {
    TRowSet rowSet;
    while (Outbox.TryPop(rowSet)) {
        Release(&rowSet);
    }
}

ETaskResult TRemoteSinkState::Write(const TRowSet& rowSet) {
    // TSinkTask releases its reference after Write. The outbox copy owns the
    // one taken here and is released by whoever drains it.
    auto& task = const_cast<TRowSet&>(rowSet);
    Retain(&task);
    TRowSet copy = task;
    copy.RefCount = 1;
    if (!Outbox.TryPush(std::move(copy))) {
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
        [&](uint64_t seq, TRowSet&& rowSet) {
            SendChunk(ctx, seq, std::move(rowSet));
        },
        [&] {
            SendFinish(ctx);
        });
}

bool TRemoteSinkState::OnConfirm(uint64_t lastApplied, NNet::NActors::TActorContext::TPtr& ctx) {
    return Confirm(
        lastApplied,
        [&](uint64_t seq, TRowSet&& rowSet) {
            SendChunk(ctx, seq, std::move(rowSet));
        },
        [&] {
            SendFinish(ctx);
        });
}

void TRemoteSinkState::SendChunk(NNet::NActors::TActorContext::TPtr& ctx, uint64_t seq, TRowSet&& rowSet) {
    // TODO: a Selection rowset is refused by serialization; compact or reject is still open.
    try {
        if (static_cast<size_t>(rowSet.ColumnCount) > Descriptor.size()) {
            throw std::runtime_error("remote sink " + std::to_string(Id) + ": fewer layouts than columns");
        }
        ctx->Send<TDataChunk>(ActorId, TDataChunk {
            .DstTaskId = DstTaskId,
            .Seq = seq,
            .RowSet = rowSet,
            .Descriptor = Descriptor.data(),
        });
    } catch (...) {
        Release(&rowSet);
        throw;
    }
    // Send has serialized the payload, so the reference can go here. Row
    // sets own detached buffers, which any thread may free.
    Release(&rowSet);
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

TRemoteSourceState::~TRemoteSourceState() {
    TRowSet rowSet;
    while (Inbox.TryPop(rowSet)) {
        Release(&rowSet);
    }
}

bool TRemoteSourceState::Push(uint64_t seq, TRowSet&& rowSet) {
    if (seq != Received + 1 || !Inbox.TryPush(std::move(rowSet))) {
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
    if (!Inbox.TryPop(rowSet)) {
        return false;
    }
    // Seqs arrive without gaps, so the pop count is the Seq of this chunk.
    Applied.fetch_add(1);
    if (!Queued.exchange(true)) {
        ActorChannel.Notify(this);
    }
    return true;
}

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
        const auto* source = dynamic_cast<const TSourceTask*>(node->Task);
        const auto* sink = dynamic_cast<const TSinkTask*>(node->Task);
        const bool isSource = endpoint.Kind == ERemoteEndpointKind::Source;
        if (isSource ? !source : !sink) {
            return "external node " + std::to_string(endpoint.Id)
                + (isSource ? " is not a source task" : " is not a sink task");
        }
        if (isSource ? source->Code() != nullptr : sink->Code() != nullptr) {
            return "external node " + std::to_string(endpoint.Id) + " already has code";
        }
        covered.insert(node);
    }
    // A source or sink left without code would dereference a null Code_.
    std::vector<const TTaskNode*> stack{&graph.Root()};
    std::unordered_set<const TTaskNode*> visited{&graph.Root()};
    while (!stack.empty()) {
        const auto* node = stack.back();
        stack.pop_back();
        const auto* source = dynamic_cast<const TSourceTask*>(node->Task);
        const auto* sink = dynamic_cast<const TSinkTask*>(node->Task);
        if ((source ? source->Code() == nullptr : sink && sink->Code() == nullptr) && !covered.contains(node)) {
            return std::string("a ") + (source ? "source" : "sink") + " task without code has no endpoint";
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
    size_t outboxCapacity,
    size_t window)
{
    auto code = std::make_shared<const TSinkCode>(
        [](void* state, const TRowSet& rowSet) {
            return static_cast<TRemoteSinkState*>(state)->Write(rowSet);
        },
        [](void* state) {
            return static_cast<TRemoteSinkState*>(state)->Finish();
        });
    auto state = std::make_shared<TRemoteSinkState>(
        id, actorId, dstTaskId, descriptor, actorChannel, outboxCapacity, window);
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

} // namespace NScheduler
} // namespace NQdb
