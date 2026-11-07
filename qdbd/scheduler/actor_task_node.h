#pragma once

#include <qdb/scheduler/task.h>
#include <qdb/scheduler/graph.h>
#include <qdb/scheduler/utils/mpsc.h>

#include <coroio/notifier.hpp>

namespace NQdb {
namespace NScheduler {

class TActorTaskNode;
class TActorNotificationChannel {
public:
    TActorNotificationChannel(std::unique_ptr<NNet::INotifier> notifier)
        : Notifier(std::move(notifier))
        , ReadyQueue(1024) // TODO: right capacity?
    { }

    TMPSCQueue<TActorTaskNode*>& Queue() {
        return ReadyQueue;
    }

    bool Notify(TActorTaskNode* task) {
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
    TMPSCQueue<TActorTaskNode*> ReadyQueue;
};

class TActorTaskNode : public ITask {
public:
    TActorTaskNode(TActorNotificationChannel& actorChannel);

    ETaskResult Execute() override;
    void Mark() {
        Queued.store(false);
    }

    void SetNode(NScheduler::TTaskNode* node) {
        Node = node;
    }

    NScheduler::TTaskNode* Meta() const {
        return Node;
    }

private:
    TActorNotificationChannel& ActorChannel;
    std::atomic<bool> Queued = false;
    NScheduler::TTaskNode* Node = nullptr;
};

} // namespace NScheduler
} // namespace NQdb
