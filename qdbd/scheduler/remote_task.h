#pragma once

#include <qdb/scheduler/task.h>
#include <qdb/scheduler/tasks/runtime_adapter.h>
#include <qdb/scheduler/graph.h>
#include <qdb/scheduler/port.h>
#include <qdb/scheduler/utils/mpsc.h>
#include <qdb/io/rowset_ops.h>

#include <qdbd/messages.h>

#include <algorithm>
#include <span>
#include <string>

#include <coroio/notifier.hpp>
#include <coroio/actors/actor.hpp>

namespace NQdb {
namespace NScheduler {

struct TRemoteSinkState;
struct TRemoteSourceState;

// Wakes the ComputeActor; carries the sink states that have rows in their outbox
// and the source states that have consumed chunks to confirm.
class TActorNotificationChannel {
public:
    TActorNotificationChannel(std::unique_ptr<NNet::INotifier> notifier)
        : Notifier(std::move(notifier))
        , SinkReady(1024) // TODO: right capacity?
        , SourceReady(1024)
    { }

    TMPSCQueue<TRemoteSinkState*>& SinkQueue() {
        return SinkReady;
    }

    TMPSCQueue<TRemoteSourceState*>& SourceQueue() {
        return SourceReady;
    }

    bool Notify(TRemoteSinkState* state) {
        return Push(SinkReady, state);
    }

    bool Notify(TRemoteSourceState* state) {
        return Push(SourceReady, state);
    }

    NNet::TFuture<void> Wait() {
        co_return co_await Notifier->Wait();
    }

private:
    template <class TState>
    bool Push(TMPSCQueue<TState*>& queue, TState* state) {
        if (queue.TryPush(std::move(state))) {
            Notifier->Notify();
            return true;
        }
        return false;
    }

    std::unique_ptr<NNet::INotifier> Notifier;
    TMPSCQueue<TRemoteSinkState*> SinkReady;
    TMPSCQueue<TRemoteSourceState*> SourceReady;
};

// Worker -> ComputeActor. The sink task writes into a bounded outbox; the actor
// is its only consumer. The external id of the sink node is kept here so the
// actor can wake the node with Graph.ExternalNode(Id). The actor numbers the
// chunks it sends from 1 and keeps at most Window of them unconfirmed.
struct TRemoteSinkState {
    TRemoteSinkState(
        uint64_t id,
        NNet::NActors::TActorId actorId,
        uint64_t dstTaskId,
        std::span<const TValueLayout> descriptor,
        TActorNotificationChannel& actorChannel,
        size_t outboxCapacity = 16,
        size_t window = 2);

    // TSinkCode::TWrite: OK or WAITING (outbox or notification channel is full).
    ETaskResult Write(const TRowSet& rowSet);
    // TSinkCode::TFinish: OK or WAITING (notification channel is full).
    ETaskResult Finish();

    // ComputeActor side. Passes queued row sets and their Seq to `send`, which
    // takes ownership, while fewer than Window are unconfirmed; calls `finish`
    // once after the last row of a finished sink. Queued stays set while the
    // actor owns the state: from the queue pop until the outbox is empty, or
    // parked on a closed window until Confirm resumes the drain.
    template <class TSend, class TSendFinish>
    void Drain(TSend&& send, TSendFinish&& finish) {
        TRowSet rowSet;
        for (;;) {
            while (Sent - Confirmed < Window && Outbox.TryPop(rowSet)) {
                send(++Sent, std::move(rowSet));
            }
            // Finish follows the last Write, so Written is final once InputFinished is set.
            if (InputFinished.load() && Sent == Written) {
                if (!FinishSent) {
                    FinishSent = true;
                    finish();
                }
                return;
            }
            if (Sent - Confirmed >= Window) {
                Parked = true;
                return;
            }
            Queued.store(false);
            // A Write or Finish that saw Queued set did not notify: look again.
            if (Outbox.TryPop(rowSet)) {
                send(++Sent, std::move(rowSet));
            } else if (!InputFinished.load()) {
                return;
            }
            if (Queued.exchange(true)) {
                return; // a worker queued this state again
            }
        }
    }

    // ComputeActor side, on TDataConfirm. False if lastApplied was never sent.
    template <class TSend, class TSendFinish>
    bool Confirm(uint64_t lastApplied, TSend&& send, TSendFinish&& finish) {
        if (lastApplied > Sent) {
            return false;
        }
        Confirmed = std::max(Confirmed, lastApplied);
        if (Parked && Sent - Confirmed < Window) {
            Parked = false;
            Drain(send, finish);
        }
        return true;
    }

    void SendQueue(NNet::NActors::TActorContext::TPtr& ctx);
    bool OnConfirm(uint64_t lastApplied, NNet::NActors::TActorContext::TPtr& ctx);

    const uint64_t Id;
    NNet::NActors::TActorId ActorId;
    uint64_t DstTaskId;
    std::span<const TValueLayout> Descriptor;
    TActorNotificationChannel& ActorChannel;
    TMPSCQueue<TRowSet> Outbox;
    const size_t Window;
    std::atomic<bool> Queued = false;
    std::atomic<bool> InputFinished = false;
    bool NotifyPending = false; // worker only: the row is in the outbox, the notification failed
    uint64_t Written = 0; // worker only until InputFinished: rows pushed into the outbox
    // Actor only: the last sent and the last confirmed Seq.
    uint64_t Sent = 0;
    uint64_t Confirmed = 0;
    bool Parked = false;
    bool FinishSent = false;

private:
    void SendChunk(NNet::NActors::TActorContext::TPtr& ctx, uint64_t seq, TRowSet&& rowSet);
    void SendFinish(NNet::NActors::TActorContext::TPtr& ctx);
};

// ComputeActor -> worker. The actor pushes received row sets into a bounded
// inbox and then wakes the source node with Graph.ExternalNode(Id). A chunk is
// applied once Next pops it; the worker then wakes the actor, which confirms
// the last applied Seq to the peer sink (PeerActor, PeerTaskId).
struct TRemoteSourceState {
    TRemoteSourceState(
        uint64_t id,
        NNet::NActors::TActorId peerActor,
        uint64_t peerTaskId,
        TActorNotificationChannel& actorChannel,
        size_t inboxCapacity = 16);

    // ComputeActor side. False if seq is not the next one or the inbox is full;
    // rowSet is then left untouched. The peer keeps at most its window of chunks
    // unconfirmed, so an inbox that holds the window never fills up.
    bool Push(uint64_t seq, TRowSet&& rowSet);
    void Finish();

    // ComputeActor side, on a wake: the last applied Seq if it moved since the
    // previous call, else 0.
    uint64_t TakeApplied();
    void SendConfirm(NNet::NActors::TActorContext::TPtr& ctx);

    // TSourceCode::TNext
    ETaskResult Next(TRowSet& rowSet);

    const uint64_t Id;
    NNet::NActors::TActorId PeerActor;
    uint64_t PeerTaskId;
    TActorNotificationChannel& ActorChannel;
    TMPSCQueue<TRowSet> Inbox;
    std::atomic<bool> Finished = false;
    std::atomic<uint64_t> Applied = 0; // worker: the Seq of the last popped chunk
    std::atomic<bool> Queued = false;
    // Actor only: the last received and the last confirmed Seq.
    uint64_t Received = 0;
    uint64_t Confirmed = 0;

private:
    bool Pop(TRowSet& rowSet);
};

// Empty if every endpoint names its own node of the matching task type
// (TSourceTask for Source, TSinkTask for Sink) built with empty code;
// otherwise the first problem.
std::string ValidateEndpoints(
    const TSealedTaskGraph& graph,
    std::span<const NRemote::TRemoteEndpoint> endpoints);

// Code and state for TSinkTask / TSourceTask.
std::pair<std::shared_ptr<const TSinkCode>, std::shared_ptr<TRemoteSinkState>> MakeRemoteSink(
    uint64_t id,
    NNet::NActors::TActorId actorId,
    uint64_t dstTaskId,
    std::span<const TValueLayout> descriptor,
    TActorNotificationChannel& actorChannel,
    size_t outboxCapacity = 16,
    size_t window = 2);

std::pair<std::shared_ptr<const TSourceCode>, std::shared_ptr<TRemoteSourceState>> MakeRemoteSource(
    uint64_t id,
    NNet::NActors::TActorId peerActor,
    uint64_t peerTaskId,
    TActorNotificationChannel& actorChannel,
    size_t inboxCapacity = 16);

} // namespace NScheduler
} // namespace NQdb
