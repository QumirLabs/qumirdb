#include "remote_task.h"
#include "qdb/scheduler/connection.h"

#include <qdbd/messages.h>
#include <qdbd/serialization.h>

#include <coroio/actors/actorsystem.hpp>

namespace NQdb {
namespace NScheduler {

using namespace NRemote;

TRemoteSinkState::TRemoteSinkState(
    uint64_t id,
    NNet::NActors::TActorId actorId,
    uint64_t dstTaskId,
    const TValueLayout* descriptor,
    TActorNotificationChannel& actorChannel,
    size_t outboxCapacity)
    : Id(id)
    , ActorId(actorId)
    , DstTaskId(dstTaskId)
    , Descriptor(descriptor)
    , ActorChannel(actorChannel)
    , Outbox(outboxCapacity)
{ }

ETaskResult TRemoteSinkState::Write(const TRowSet& rowSet) {
    if (!NotifyPending) {
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
    }
    if (!Queued.exchange(true)) {
        if (!ActorChannel.Notify(this)) {
            // The same row set comes back; do not push it twice.
            Queued.store(false);
            NotifyPending = true;
            return ETaskResult::WAITING;
        }
    }
    NotifyPending = false;
    return ETaskResult::OK;
}

void TRemoteSinkState::SendQueue(NNet::NActors::TActorContext::TPtr& ctx) {
    Drain([&](TRowSet&& rowSet) {
        ctx->Send<TDataChunk>(ActorId, TDataChunk {
            .DstTaskId = DstTaskId,
            .RowSet = rowSet,
            .Descriptor = Descriptor,
        });
        // Send has serialized the payload, so the reference can go here. Row
        // sets own detached buffers, which any thread may free.
        Release(&rowSet);
    });
}

TRemoteSourceState::TRemoteSourceState(uint64_t id, size_t inboxCapacity)
    : Id(id)
    , Inbox(inboxCapacity)
{ }

bool TRemoteSourceState::Push(TRowSet&& rowSet) {
    return Inbox.TryPush(std::move(rowSet));
}

void TRemoteSourceState::Finish() {
    Finished.store(true, std::memory_order_release);
}

ETaskResult TRemoteSourceState::Next(TRowSet& rowSet) {
    if (Inbox.TryPop(rowSet)) {
        return ETaskResult::OK;
    }
    if (!Finished.load(std::memory_order_acquire)) {
        return ETaskResult::WAITING;
    }
    // Rows pushed before Finish are visible now.
    return Inbox.TryPop(rowSet) ? ETaskResult::OK : ETaskResult::FINISHED;
}

std::pair<std::shared_ptr<const TSinkCode>, std::shared_ptr<TRemoteSinkState>> MakeRemoteSink(
    uint64_t id,
    NNet::NActors::TActorId actorId,
    uint64_t dstTaskId,
    const TValueLayout* descriptor,
    TActorNotificationChannel& actorChannel,
    size_t outboxCapacity)
{
    auto code = std::make_shared<const TSinkCode>([](void* state, const TRowSet& rowSet) {
        return static_cast<TRemoteSinkState*>(state)->Write(rowSet);
    });
    auto state = std::make_shared<TRemoteSinkState>(
        id, actorId, dstTaskId, descriptor, actorChannel, outboxCapacity);
    return {std::move(code), std::move(state)};
}

std::pair<std::shared_ptr<const TSourceCode>, std::shared_ptr<TRemoteSourceState>> MakeRemoteSource(
    uint64_t id,
    size_t inboxCapacity)
{
    auto code = std::make_shared<const TSourceCode>([](void* state, TRowSet& rowSet) {
        return static_cast<TRemoteSourceState*>(state)->Next(rowSet);
    });
    return {std::move(code), std::make_shared<TRemoteSourceState>(id, inboxCapacity)};
}

} // namespace NScheduler
} // namespace NQdb
