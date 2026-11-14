#pragma once

#include <qdb/scheduler/task.h>
#include <qdb/scheduler/tasks/cte_spool.h>
#include <qdb/scheduler/tasks/runtime_adapter.h>
#include <qdb/scheduler/graph.h>
#include <qdb/scheduler/port.h>
#include <qdb/scheduler/utils/mpsc.h>
#include <qdb/io/rowset_ops.h>

#include <qdbd/messages.h>

#include <algorithm>
#include <cassert>
#include <deque>
#include <span>
#include <string>
#include <vector>

#include <coroio/notifier.hpp>
#include <coroio/actors/actor.hpp>

namespace NQdb {
namespace NScheduler {

struct TRemoteSinkState;
struct TRemoteSourceState;
struct TRuntimeFilterSinkState;

// Wakes the ComputeActor; carries the sink states that have rows in their outbox,
// the source states that have consumed chunks to confirm and the runtime filter
// sink states that have their partial. A state is queued at most once, so queues
// sized by the number of states never fill up.
class TActorNotificationChannel {
public:
    TActorNotificationChannel(
        std::unique_ptr<NNet::INotifier> notifier, size_t sinks, size_t sources, size_t filterSinks = 0)
        : Notifier(std::move(notifier))
        , SinkReady(std::max<size_t>(sinks, 2)) // 2 is the queue minimum
        , SourceReady(std::max<size_t>(sources, 2))
        , FilterSinkReady(std::max<size_t>(filterSinks, 2))
    { }

    TMPSCQueue<TRemoteSinkState*>& SinkQueue() {
        return SinkReady;
    }

    TMPSCQueue<TRemoteSourceState*>& SourceQueue() {
        return SourceReady;
    }

    TMPSCQueue<TRuntimeFilterSinkState*>& FilterSinkQueue() {
        return FilterSinkReady;
    }

    void Notify(TRemoteSinkState* state) {
        Push(SinkReady, state);
    }

    void Notify(TRemoteSourceState* state) {
        Push(SourceReady, state);
    }

    void Notify(TRuntimeFilterSinkState* state) {
        Push(FilterSinkReady, state);
    }

    NNet::TFuture<void> Wait() {
        co_return co_await Notifier->Wait();
    }

private:
    template <class TState>
    void Push(TMPSCQueue<TState*>& queue, TState* state) {
        [[maybe_unused]] const bool pushed = queue.TryPush(std::move(state));
        assert(pushed);
        Notifier->Notify();
    }

    std::unique_ptr<NNet::INotifier> Notifier;
    TMPSCQueue<TRemoteSinkState*> SinkReady;
    TMPSCQueue<TRemoteSourceState*> SourceReady;
    TMPSCQueue<TRuntimeFilterSinkState*> FilterSinkReady;
};

// Flow control of one remote stream, in serialized bytes of row sets. A sink
// queues at most OutboxBytes (and OutboxCapacity row sets) for the actor, which
// packs them into chunks and keeps at most WindowBytes unconfirmed. A chunk
// closes once it holds ChunkBytes, so its last row set may overshoot it; so
// may the last row set the outbox takes. The peer's inbox holds WindowChunks
// chunks, so that many may be unconfirmed too.
struct TRemoteStreamLimits {
    size_t OutboxCapacity = 64;
    size_t OutboxBytes = 32 << 20;
    size_t ChunkBytes = 256 << 10;
    size_t WindowBytes = 2 << 20;
    size_t WindowChunks = 16;
};

// Worker -> ComputeActor. The sink task writes into a bounded outbox; the actor
// is its only consumer. The external id of the sink node is kept here so the
// actor can wake the node with Graph.ExternalNode(Id). The actor numbers the
// chunks it sends from 1.
struct TRemoteSinkState {
    TRemoteSinkState(
        uint64_t id,
        NNet::NActors::TActorId actorId,
        uint64_t dstTaskId,
        std::span<const TValueLayout> descriptor,
        TActorNotificationChannel& actorChannel,
        TRemoteStreamLimits limits = {});
    ~TRemoteSinkState();

    // TSinkCode::TWrite: OK or WAITING (the outbox is full).
    ETaskResult Write(const TRowSet& rowSet);
    // TSinkCode::TFinish
    ETaskResult Finish();

    // ComputeActor side. While the window is open, packs the queued row sets
    // into chunks and passes each to `send`; a chunk takes what is queued and
    // never waits for more. Calls `finish` once after the last row of a
    // finished sink. Queued stays set while the actor owns the state: from the
    // queue pop until the outbox is empty, or parked on a closed window until
    // Confirm resumes the drain.
    template <class TSend, class TSendFinish>
    void Drain(TSend&& send, TSendFinish&& finish) {
        for (;;) {
            while (WindowOpen() && SendQueued(send)) {
            }
            // Finish follows the last Write, so Written is final once InputFinished is set.
            if (InputFinished.load() && Popped == Written) {
                if (!FinishSent) {
                    FinishSent = true;
                    finish();
                }
                return;
            }
            if (!WindowOpen()) {
                Parked = true;
                return;
            }
            Queued.store(false);
            // Store-buffering with the worker's push: the pop must not run before the store.
            std::atomic_thread_fence(std::memory_order_seq_cst);
            // A Write or Finish that saw Queued set did not notify: look again.
            if (!SendQueued(send) && !InputFinished.load()) {
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
        for (; Confirmed < lastApplied; ++Confirmed) {
            InFlightBytes -= InFlight.front();
            InFlight.pop_front();
        }
        if (Parked && WindowOpen()) {
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
    const TRemoteStreamLimits Limits;
    // A row set and its bytes, counted once by the worker.
    struct TQueued {
        TRowSet RowSet{};
        uint64_t Bytes = 0;
    };
    TMPSCQueue<TQueued> Outbox;
    std::atomic<uint64_t> QueuedBytes = 0;
    std::atomic<bool> Queued = false;
    std::atomic<bool> InputFinished = false;
    uint64_t Written = 0; // worker only until InputFinished: rows pushed into the outbox
    // Actor only: row sets taken from the outbox, the last sent and the last
    // confirmed Seq, and the bytes of each unconfirmed chunk.
    uint64_t Popped = 0;
    uint64_t Sent = 0;
    uint64_t Confirmed = 0;
    std::deque<uint64_t> InFlight;
    uint64_t InFlightBytes = 0;
    bool Parked = false;
    bool FinishSent = false;

private:
    bool WindowOpen() const {
        return InFlightBytes < Limits.WindowBytes && Sent - Confirmed < Limits.WindowChunks;
    }

    // Packs queued row sets up to ChunkBytes into the next chunk; false if none is queued.
    template <class TSend>
    bool SendQueued(TSend& send) {
        std::vector<TRowSet> rowSets;
        uint64_t bytes = 0;
        TQueued queued;
        while (bytes < Limits.ChunkBytes && Outbox.TryPop(queued)) {
            QueuedBytes.fetch_sub(queued.Bytes);
            rowSets.push_back(queued.RowSet);
            bytes += queued.Bytes;
            ++Popped;
        }
        if (rowSets.empty()) {
            return false;
        }
        InFlight.push_back(bytes);
        InFlightBytes += bytes;
        send(NRemote::TDataChunk(DstTaskId, ++Sent, std::move(rowSets), Descriptor.data()));
        return true;
    }

    void SendChunk(NNet::NActors::TActorContext::TPtr& ctx, NRemote::TDataChunk&& chunk);
    void SendFinish(NNet::NActors::TActorContext::TPtr& ctx);
};

// ComputeActor -> worker. The actor pushes received chunks into a bounded
// inbox and then wakes the source node with Graph.ExternalNode(Id). A chunk is
// applied once Next takes it from the inbox; the worker then wakes the actor,
// which confirms the last applied Seq to the peer sink (PeerActor, PeerTaskId).
struct TRemoteSourceState {
    TRemoteSourceState(
        uint64_t id,
        NNet::NActors::TActorId peerActor,
        uint64_t peerTaskId,
        TActorNotificationChannel& actorChannel,
        size_t inboxCapacity = TRemoteStreamLimits{}.WindowChunks);

    // ComputeActor side. False if the chunk's Seq is not the next one or the
    // inbox is full; the chunk is then left untouched. The peer keeps at most
    // its window of chunks unconfirmed, so an inbox that holds the window never
    // fills up.
    bool Push(NRemote::TDataChunk&& chunk);
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
    // The chunks release the rows they still hold.
    TMPSCQueue<NRemote::TDataChunk> Inbox;
    std::atomic<bool> Finished = false;
    std::atomic<uint64_t> Applied = 0; // worker: the Seq of the last chunk taken from the inbox
    std::atomic<bool> Queued = false;
    // Actor only: the last received and the last confirmed Seq.
    uint64_t Received = 0;
    uint64_t Confirmed = 0;

private:
    bool Pop(TRowSet& rowSet);

    // Worker only: the chunk being handed out and its next row set.
    NRemote::TDataChunk Current;
    size_t CurrentPos = 0;
};

// Worker -> ComputeActor: a runtime filter sink task hands its partial over
// once and queues the state; the actor then sends it to the source PeerTaskId
// of PeerActor.
struct TRuntimeFilterSinkState {
    TRuntimeFilterSinkState(
        uint64_t id,
        NNet::NActors::TActorId peerActor,
        uint64_t peerTaskId,
        TActorNotificationChannel& actorChannel);

    // TRuntimeFilterSinkCode::TSend
    void Send(TRuntimeFilterPartial&& partial);
    // ComputeActor side, once the state is popped from the queue.
    void SendPartial(NNet::NActors::TActorContext::TPtr& ctx);

    const uint64_t Id;
    NNet::NActors::TActorId PeerActor;
    uint64_t PeerTaskId;
    TActorNotificationChannel& ActorChannel;
    // Written before the state is queued, read after it is popped.
    TRuntimeFilterPartial Partial;
};

// ComputeActor -> worker: the actor puts the received partial here and wakes
// the source node, whose task takes it once.
struct TRuntimeFilterSourceState {
    explicit TRuntimeFilterSourceState(uint64_t id);

    // ComputeActor side. False if a partial already came.
    bool Push(TRuntimeFilterPartial&& partial);
    // TRuntimeFilterSourceCode::TTake
    bool Take(TRuntimeFilterPartial& partial);

    const uint64_t Id;
    // Written before Arrived is set.
    TRuntimeFilterPartial Partial;
    std::atomic<bool> Arrived = false;
    // Actor only.
    bool Received = false;
};

// Empty if every endpoint names its own node of the matching task type
// (TSourceTask for Source, TSinkTask for Sink, TRuntimeFilterSourceTask and
// TRuntimeFilterSinkTask for the filter kinds, TCteSourceTask and TCteSinkTask
// for the CTE kinds) built with empty code, and every
// such task with empty code has an endpoint, and every sink task has an input
// connection with row layouts; otherwise the first problem.
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
    TRemoteStreamLimits limits = {});

std::pair<std::shared_ptr<const TSourceCode>, std::shared_ptr<TRemoteSourceState>> MakeRemoteSource(
    uint64_t id,
    NNet::NActors::TActorId peerActor,
    uint64_t peerTaskId,
    TActorNotificationChannel& actorChannel,
    size_t inboxCapacity = TRemoteStreamLimits{}.WindowChunks);

// Code and state for TRuntimeFilterSinkTask / TRuntimeFilterSourceTask.
std::pair<std::shared_ptr<const TRuntimeFilterSinkCode>, std::shared_ptr<TRuntimeFilterSinkState>>
MakeRuntimeFilterSink(
    uint64_t id,
    NNet::NActors::TActorId peerActor,
    uint64_t peerTaskId,
    TActorNotificationChannel& actorChannel);

std::pair<std::shared_ptr<const TRuntimeFilterSourceCode>, std::shared_ptr<TRuntimeFilterSourceState>>
MakeRuntimeFilterSource(uint64_t id);

} // namespace NScheduler
} // namespace NQdb
