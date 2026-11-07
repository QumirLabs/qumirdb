#pragma once

#include <qdb/scheduler/task.h>
#include <qdb/scheduler/graph.h>
#include <qdb/scheduler/port.h>
#include <qdb/scheduler/utils/mpsc.h>
#include <qdb/io/rowset_ops.h>

#include <qdbd/messages.h>

#include <coroio/notifier.hpp>
#include <coroio/actors/actor.hpp>

namespace NQdb {
namespace NScheduler {

class TRemoteSink;
class TActorNotificationChannel {
public:
    TActorNotificationChannel(std::unique_ptr<NNet::INotifier> notifier)
        : Notifier(std::move(notifier))
        , ReadyQueue(1024) // TODO: right capacity?
    { }

    TMPSCQueue<TRemoteSink*>& Queue() {
        return ReadyQueue;
    }

    bool Notify(TRemoteSink* task) {
        if (ReadyQueue.TryPush(std::move(task))) {
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
    TMPSCQueue<TRemoteSink*> ReadyQueue;
};

class TRemoteSource : public ITask {
public:
    TRemoteSource(NScheduler::TOutputPort output, size_t srcLaneId);

    void Receive(NQdb::NRemote::TDataChunk&& data, NNet::NActors::TActorContext::TPtr& ctx);

    ETaskResult Execute() override;

private:
    NScheduler::TOutputPort Output;
};

class TRemoteSink : public ITask {
public:
    TRemoteSink(
        NScheduler::TInputPort input, // sink consumes input
        NNet::NActors::TActorId actorId,
        size_t dstLaneId,
        size_t dstTaskId,
        const TValueLayout* descriptor,
        TActorNotificationChannel& actorChannel);

    ETaskResult Execute() override;

    void SendQueue(NNet::NActors::TActorContext::TPtr& ctx);

    void SetNode(NScheduler::TTaskNode* node) {
        Node = node;
    }

    NScheduler::TTaskNode* Meta() const {
        return Node;
    }

private:
    NScheduler::TInputPort Input;
    NNet::NActors::TActorId ActorId;
    size_t DstLaneId;
    size_t DstTaskId;
    const TValueLayout* Descriptor;
    TActorNotificationChannel& ActorChannel;
    std::atomic<bool> Queued = false;
    std::atomic<bool> Finished = false;
    NScheduler::TTaskNode* Node = nullptr;
};

} // namespace NScheduler
} // namespace NQdb
