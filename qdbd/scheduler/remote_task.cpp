#include "remote_task.h"
#include "qdb/scheduler/connection.h"

#include <qdbd/messages.h>
#include <qdbd/serialization.h>

#include <coroio/actors/actorsystem.hpp>

namespace NQdb {
namespace NScheduler {

using namespace NRemote;

TRemoteSink::TRemoteSink(
    NScheduler::TInputPort input,
    NNet::NActors::TActorId actorId,
    size_t dstLaneId,
    size_t dstTaskId,
    const TValueLayout* descriptor,
    TActorNotificationChannel& actorChannel)
    : Input(std::move(input))
    , ActorId(actorId)
    , DstLaneId(dstLaneId)
    , DstTaskId(dstTaskId)
    , Descriptor(descriptor)
    , ActorChannel(actorChannel)
{ }

ETaskResult TRemoteSink::Execute() {
    if (Finished.load()) {
        return ETaskResult::FINISHED;
    }

    if (Queued.exchange(true) == false) {
        if (ActorChannel.Notify(this)) {
            return ETaskResult::WAITING;
        } else {
            // full queue, retry on next scheduler iteration
            return ETaskResult::OK;
        }
    }

    if (Finished.load()) {
        return ETaskResult::FINISHED;
    }
    return ETaskResult::WAITING;
}

void TRemoteSink::SendQueue(NNet::NActors::TActorContext::TPtr& ctx) {
    TRowSet rowSet;
    EFetchResult result;
    while ((result = Input.Fetch(rowSet)) == EFetchResult::OK) {
        ctx->Send<TDataChunk>(ActorId, TDataChunk {
            .DstLaneId = DstLaneId,
            .DstTaskId = DstTaskId,
            .RowSet = rowSet,
            .Descriptor = Descriptor,
        });

        if (--rowSet.RowCount == 0) {
            rowSet.Destroy(&rowSet);
        }
    }
    if (result == EFetchResult::FINISHED) {
        Finished.store(true);
    }
    Queued.store(false);
}

} // namespace NScheduler
} // namespace NQdb
