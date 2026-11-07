#pragma once

#include <qdb/scheduler/task.h>
#include <qdb/scheduler/tasks/runtime_adapter.h>
#include <qdb/scheduler/graph.h>
#include <qdb/scheduler/port.h>
#include <qdb/scheduler/utils/mpsc.h>
#include <qdb/io/rowset_ops.h>

#include <qdbd/messages.h>

#include <coroio/notifier.hpp>
#include <coroio/actors/actor.hpp>

namespace NQdb {
namespace NScheduler {

struct TRemoteSinkState;

// Wakes the ComputeActor; carries the sink states that have rows in their outbox.
class TActorNotificationChannel {
public:
    TActorNotificationChannel(std::unique_ptr<NNet::INotifier> notifier)
        : Notifier(std::move(notifier))
        , ReadyQueue(1024) // TODO: right capacity?
    { }

    TMPSCQueue<TRemoteSinkState*>& Queue() {
        return ReadyQueue;
    }

    bool Notify(TRemoteSinkState* state) {
        if (ReadyQueue.TryPush(std::move(state))) {
            Notifier->Notify();
            return true;
        }
        return false;
    }

    NNet::TFuture<void> Wait() {
        co_return co_await Notifier->Wait();
    }

private:
    std::unique_ptr<NNet::INotifier> Notifier;
    TMPSCQueue<TRemoteSinkState*> ReadyQueue;
};

// Worker -> ComputeActor. The sink task writes into a bounded outbox; the actor
// is its only consumer. The external id of the sink node is kept here so the
// actor can wake the node with Graph.ExternalNode(Id).
struct TRemoteSinkState {
    TRemoteSinkState(
        uint64_t id,
        NNet::NActors::TActorId actorId,
        uint64_t dstLaneId,
        uint64_t dstTaskId,
        const TValueLayout* descriptor,
        TActorNotificationChannel& actorChannel,
        size_t outboxCapacity = 16);

    // TSinkCode::TWrite: OK or WAITING (outbox or notification channel is full).
    ETaskResult Write(const TRowSet& rowSet);

    // ComputeActor side. Passes every queued row set to `send`, which takes
    // ownership, then lets workers queue this state again.
    template <class TSend>
    void Drain(TSend&& send) {
        TRowSet rowSet;
        for (;;) {
            while (Outbox.TryPop(rowSet)) {
                send(std::move(rowSet));
            }
            Queued.store(false);
            if (!Outbox.TryPop(rowSet)) {
                return;
            }
            send(std::move(rowSet));
            if (Queued.exchange(true)) {
                return; // a worker queued this state again
            }
        }
    }

    void SendQueue(NNet::NActors::TActorContext::TPtr& ctx);

    const uint64_t Id;
    NNet::NActors::TActorId ActorId;
    uint64_t DstLaneId;
    uint64_t DstTaskId;
    const TValueLayout* Descriptor;
    TActorNotificationChannel& ActorChannel;
    TMPSCQueue<TRowSet> Outbox;
    std::atomic<bool> Queued = false;
    bool NotifyPending = false; // worker only: the row is in the outbox, the notification failed
};

// ComputeActor -> worker. The actor pushes received row sets into a bounded
// inbox and then wakes the source node with Graph.ExternalNode(Id).
struct TRemoteSourceState {
    explicit TRemoteSourceState(uint64_t id, size_t inboxCapacity = 16);

    // ComputeActor side. False if the inbox is full; rowSet is then left untouched.
    bool Push(TRowSet&& rowSet);
    void Finish();

    // TSourceCode::TNext
    ETaskResult Next(TRowSet& rowSet);

    const uint64_t Id;
    TMPSCQueue<TRowSet> Inbox;
    std::atomic<bool> Finished = false;
};

// Code and state for TSinkTask / TSourceTask.
std::pair<std::shared_ptr<const TSinkCode>, std::shared_ptr<TRemoteSinkState>> MakeRemoteSink(
    uint64_t id,
    NNet::NActors::TActorId actorId,
    uint64_t dstLaneId,
    uint64_t dstTaskId,
    const TValueLayout* descriptor,
    TActorNotificationChannel& actorChannel,
    size_t outboxCapacity = 16);

std::pair<std::shared_ptr<const TSourceCode>, std::shared_ptr<TRemoteSourceState>> MakeRemoteSource(
    uint64_t id,
    size_t inboxCapacity = 16);

} // namespace NScheduler
} // namespace NQdb
