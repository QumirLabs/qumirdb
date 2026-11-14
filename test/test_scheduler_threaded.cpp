#include <gtest/gtest.h>

#include <qdb/scheduler/channel.h>
#include <qdb/scheduler/connection.h>
#include <qdb/scheduler/connections/local.h>
#include <qdb/scheduler/graph.h>
#include <qdb/scheduler/impl/single_threaded_scheduler.h>
#include <qdb/scheduler/impl/threaded_scheduler.h>
#include <qdb/scheduler/port.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <latch>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace NQdb::NScheduler;

namespace {

class TRepeatTask : public ITask {
public:
    explicit TRepeatTask(int repeats)
        : Remaining_(repeats)
    {}

    ETaskResult Execute() override {
        ++Calls_;
        if (Remaining_ > 0) {
            --Remaining_;
            return ETaskResult::OK;
        }
        return ETaskResult::FINISHED;
    }

    int Calls() const {
        return Calls_;
    }

private:
    int Remaining_;
    int Calls_ = 0;
};

class TStressSourceTask : public ITask {
public:
    explicit TStressSourceTask(int total)
        : Total_(total)
    {}

    ETaskResult Execute() override {
        auto produced = Produced.load(std::memory_order_relaxed);
        if (produced < Total_) {
            Produced.store(produced + 1, std::memory_order_release);
            return ETaskResult::OK;
        }
        Finished.store(true, std::memory_order_release);
        return ETaskResult::FINISHED;
    }

    std::atomic<int> Produced = 0;
    std::atomic<bool> Finished = false;

private:
    int Total_;
};

class TStressSinkTask : public ITask {
public:
    TStressSinkTask(TStressSourceTask& source, int total)
        : Source_(source)
        , Total_(total)
    {}

    ETaskResult Execute() override {
        auto produced = Source_.Produced.load(std::memory_order_acquire);
        if (Consumed_ < produced) {
            ++Consumed_;
            return ETaskResult::OK;
        }
        if (
            Source_.Finished.load(std::memory_order_acquire) &&
            Consumed_ == Total_)
        {
            return ETaskResult::FINISHED;
        }
        return ETaskResult::NEED_DATA;
    }

    int Consumed() const {
        return Consumed_;
    }

private:
    TStressSourceTask& Source_;
    int Total_;
    int Consumed_ = 0;
};

class TGuardedTask : public ITask {
public:
    explicit TGuardedTask(int repeats)
        : Remaining_(repeats)
    {}

    ETaskResult Execute() override {
        if (Running_.exchange(true, std::memory_order_acq_rel)) {
            Violations.fetch_add(1, std::memory_order_relaxed);
        }
        std::this_thread::yield();

        ETaskResult result = ETaskResult::FINISHED;
        if (Remaining_.fetch_sub(1, std::memory_order_acq_rel) > 0) {
            result = ETaskResult::OK;
        }

        Running_.store(false, std::memory_order_release);
        return result;
    }

    std::atomic<int> Violations = 0;

private:
    std::atomic<int> Remaining_;
    std::atomic<bool> Running_ = false;
};

class TFinishTask : public ITask {
public:
    ETaskResult Execute() override {
        return ETaskResult::FINISHED;
    }
};

class TThrowTask : public ITask {
public:
    ETaskResult Execute() override {
        throw std::runtime_error("expected task failure");
    }
};

class TManyInputSinkTask : public ITask {
public:
    ETaskResult Execute() override {
        if (!Requested_) {
            Requested_ = true;
            return ETaskResult::NEED_DATA;
        }
        return ETaskResult::FINISHED;
    }

private:
    bool Requested_ = false;
};

class TNeedFinishedReplayTask : public ITask {
public:
    ETaskResult Execute() override {
        ++Calls_;
        if (Calls_ < 3) {
            return ETaskResult::NEED_DATA;
        }
        return ETaskResult::FINISHED;
    }

    int Calls() const {
        return Calls_;
    }

private:
    int Calls_ = 0;
};

std::unique_ptr<TTaskGraph> MakeRepeatGraph(TRepeatTask*& task, int repeats) {
    auto graph = std::make_unique<TTaskGraph>();
    auto repeat = std::make_unique<TRepeatTask>(repeats);
    task = repeat.get();
    graph->AddOwnedNode(std::move(repeat));
    EXPECT_TRUE(graph->Seal());
    return graph;
}

class TSelfWakeTask : public ITask {
public:
    ETaskResult Execute() override {
        if (++Calls == 1) {
            for (int i = 0; i < 100; ++i) {
                Channel->Notify(Node);
            }
            return ETaskResult::WAITING;
        }
        return ETaskResult::FINISHED;
    }

    TSchedulerChannel* Channel = nullptr;
    TTaskNode* Node = nullptr;
    int Calls = 0;
};

class TExternalTask : public ITask {
public:
    ETaskResult Execute() override {
        ++Calls;
        return Ready.load(std::memory_order_acquire) ? ETaskResult::FINISHED : ETaskResult::WAITING;
    }

    std::atomic<bool> Ready = false;
    int Calls = 0;
};

class THeldExternalTask : public ITask {
public:
    ETaskResult Execute() override {
        if (Running_.exchange(true, std::memory_order_acq_rel)) {
            Violations.fetch_add(1, std::memory_order_relaxed);
        }
        const bool first = ++Calls == 1;
        if (first) {
            Entered.count_down();
            Return.wait();
        }
        Running_.store(false, std::memory_order_release);
        return first ? ETaskResult::WAITING : ETaskResult::FINISHED;
    }

    std::latch Entered{1};
    std::latch Return{1};
    int Calls = 0;
    std::atomic<int> Violations = 0;

private:
    std::atomic<bool> Running_ = false;
};

TEST(SchedulerExternal, WakeDuringExecuteIsRetained) {
    for (size_t workers : {1, 4}) {
        SCOPED_TRACE(workers);
        TSelfWakeTask task;
        TTaskGraph graph;
        auto& node = graph.AddNode(task);
        const auto sealed = graph.Seal();
        ASSERT_TRUE(sealed);
        TThreadedScheduler scheduler(sealed, workers);
        TSchedulerChannel channel(scheduler);
        task.Channel = &channel;
        task.Node = &node;
        std::string error;
        EXPECT_TRUE(scheduler.Run(&error)) << error;
        EXPECT_EQ(task.Calls, 2);
        EXPECT_EQ(scheduler.Stats().Executed, 2);
        EXPECT_EQ(scheduler.Stats().Waiting, 1);
    }
}

void CheckWakeAfterWaiting(TThreadedScheduler& scheduler, TTaskNode& node, TExternalTask& task) {
    std::string error;
    bool success = false;
    std::atomic<bool> returned = false;
    TSchedulerChannel channel(scheduler);
    std::thread runner([&] {
        success = scheduler.Run(&error);
        returned.store(true, std::memory_order_release);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (node.State.load(std::memory_order_acquire) != ETaskState::Waiting
        && !returned.load(std::memory_order_acquire)
        && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::yield();
    }
    EXPECT_EQ(node.State.load(std::memory_order_acquire), ETaskState::Waiting);
    EXPECT_FALSE(returned.load(std::memory_order_acquire));
    task.Ready.store(true, std::memory_order_release);
    channel.Notify(&node);
    runner.join();
    EXPECT_TRUE(success) << error;
    EXPECT_EQ(task.Calls, 2);
    EXPECT_EQ(scheduler.Stats().Waiting, 1);
}

TEST(SchedulerExternal, EmptyReadyQueueWaitsForExternalCompletion) {
    for (size_t workers : {1, 4}) {
        SCOPED_TRACE(workers);
        TTaskGraph graph;
        TExternalTask task;
        auto& node = graph.AddNode(task);
        const auto sealed = graph.Seal();
        ASSERT_TRUE(sealed);
        TThreadedScheduler scheduler(sealed, workers);
        CheckWakeAfterWaiting(scheduler, node, task);
    }
}

void CheckConcurrentWakes(TThreadedScheduler& scheduler, TTaskNode& node, THeldExternalTask& task) {
    std::string error;
    bool success = false;
    TSchedulerChannel channel(scheduler);
    std::thread runner([&] { success = scheduler.Run(&error); });
    task.Entered.wait();
    std::vector<std::thread> producers;
    for (int i = 0; i < 4; ++i) {
        producers.emplace_back([&] {
            for (int j = 0; j < 1000; ++j) {
                channel.Notify(&node);
            }
        });
    }
    for (auto& producer : producers) {
        producer.join();
    }
    task.Return.count_down();
    runner.join();
    EXPECT_TRUE(success) << error;
    EXPECT_EQ(task.Calls, 2);
    EXPECT_EQ(task.Violations.load(std::memory_order_relaxed), 0);
    EXPECT_EQ(scheduler.Stats().Waiting, 1);
}

TEST(SchedulerExternal, ConcurrentDuplicateWakesDoNotExecuteTaskConcurrently) {
    for (size_t workers : {1, 4}) {
        SCOPED_TRACE(workers);
        TTaskGraph graph;
        THeldExternalTask task;
        auto& node = graph.AddNode(task);
        const auto sealed = graph.Seal();
        ASSERT_TRUE(sealed);
        TThreadedScheduler scheduler(sealed, workers);
        CheckConcurrentWakes(scheduler, node, task);
    }
}

TEST(SchedulerExternal, FinishedTaskIgnoresExternalWakes) {
    TFinishTask task;
    TTaskGraph graph;
    auto& node = graph.AddNode(task);
    const auto sealed = graph.Seal();
    ASSERT_TRUE(sealed);
    TThreadedScheduler scheduler(sealed, 2);
    TSchedulerChannel channel(scheduler);
    std::string error;
    ASSERT_TRUE(scheduler.Run(&error)) << error;
    const auto scheduled = scheduler.Stats().Scheduled;
    channel.Notify(&node);
    channel.Notify(nullptr);
    EXPECT_EQ(scheduler.Stats().Scheduled, scheduled);
}

void CheckEarlyCompletion(TThreadedScheduler& scheduler, TTaskNode& source, TTaskNode& root) {
    std::string error;
    bool success = false;
    std::thread runner([&] { success = scheduler.Run(&error); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (source.State.load(std::memory_order_acquire) != ETaskState::Waiting
        && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::yield();
    }
    EXPECT_EQ(source.State.load(std::memory_order_acquire), ETaskState::Waiting);
    TSchedulerChannel channel(scheduler);
    channel.Notify(&root);
    runner.join();
    EXPECT_TRUE(success) << error;
    EXPECT_EQ(root.State.load(std::memory_order_acquire), ETaskState::Finished);
    EXPECT_EQ(source.State.load(std::memory_order_acquire), ETaskState::Waiting);
}

TEST(SchedulerExternal, RootCompletionAbandonsExternalWaits) {
    for (size_t workers : {1, 4}) {
        SCOPED_TRACE(workers);
        TTaskGraph graph;
        TExternalTask sourceTask;
        TManyInputSinkTask sinkTask;
        auto& source = graph.AddNode(sourceTask);
        auto& root = graph.AddNode(sinkTask);
        graph.AddOwnedEdge(source, root, std::make_unique<TOneToOneConnection>());
        const auto sealed = graph.Seal();
        ASSERT_TRUE(sealed);
        TThreadedScheduler scheduler(sealed, workers);
        CheckEarlyCompletion(scheduler, source, root);
    }
}

TEST(SchedulerExternal, SingleThreadedRejectsExternalWaits) {
    TTaskGraph graph;
    TExternalTask task;
    graph.AddNode(task);
    const auto sealed = graph.Seal();
    ASSERT_TRUE(sealed);
    TSingleThreadedScheduler scheduler(sealed);
    std::string error;
    EXPECT_FALSE(scheduler.Run(&error));
    EXPECT_EQ(error, "single-threaded scheduler does not support external waits");
}

// Cancels a run that stalls, so a lost wakeup fails the test instead of hanging it.
bool RunOrCancel(TThreadedScheduler& scheduler, std::string* error) {
    std::mutex mutex;
    std::condition_variable done;
    bool returned = false;
    std::thread watchdog([&] {
        std::unique_lock lock(mutex);
        if (!done.wait_for(lock, std::chrono::seconds(30), [&] { return returned; })) {
            scheduler.Cancel();
        }
    });
    const bool ok = scheduler.Run(error);
    {
        std::lock_guard lock(mutex);
        returned = true;
    }
    done.notify_one();
    watchdog.join();
    return ok;
}

NQdb::TRowSet OneRow() {
    return {.RowCount = 1, .RefCount = 1};
}

// Waits until `ready` holds, for at most five seconds.
template <class TReady>
bool SpinUntil(TReady ready) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!ready()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

// Pushes `rows` one-row batches, one per run, then finishes.
class TPushTask : public ITask {
public:
    explicit TPushTask(int rows)
        : Rows_(rows)
    {}

    ETaskResult Execute() override {
        if (Sent_ == Rows_) {
            Output.Finish();
            return ETaskResult::FINISHED;
        }
        if (!Output.Push(OneRow())) {
            return ETaskResult::BLOCKED_OUTPUT;
        }
        ++Sent_;
        return ETaskResult::OK;
    }

    TOutputPort Output;

private:
    int Rows_;
    int Sent_ = 0;
};

// Pulls its input once, then waits for Ready and an external wake before it reads it all.
class TGateTask : public ITask {
public:
    ETaskResult Execute() override {
        ++Calls;
        if (!Pulled_) {
            Pulled_ = true;
            return ETaskResult::NEED_DATA;
        }
        if (!Ready.load(std::memory_order_acquire)) {
            return ETaskResult::WAITING;
        }
        for (;;) {
            NQdb::TRowSet rowSet{};
            switch (Input.Fetch(rowSet)) {
                case EFetchResult::OK:
                    Rows += rowSet.RowCount;
                    NQdb::Release(&rowSet);
                    break;
                case EFetchResult::NO_DATA:
                    return ETaskResult::NEED_DATA;
                case EFetchResult::FINISHED:
                    return ETaskResult::FINISHED;
            }
        }
    }

    TInputPort Input;
    std::atomic<bool> Ready = false;
    std::atomic<int> Calls = 0;
    int64_t Rows = 0;

private:
    bool Pulled_ = false;
};

TEST(SchedulerExternal, WaitingTaskIsNotRunForEveryPush) {
    constexpr int rows = 1000;
    for (size_t workers : {1, 4}) {
        SCOPED_TRACE(workers);
        TTaskGraph graph;
        auto& producer = graph.AddOwnedNode(std::make_unique<TPushTask>(rows));
        auto gateTask = std::make_unique<TGateTask>();
        auto& gate = *gateTask;
        auto& gateNode = graph.AddOwnedNode(std::move(gateTask));
        auto& edge = graph.AddOwnedEdge(producer, gateNode, std::make_unique<TOneToOneConnection>(rows));
        static_cast<TPushTask*>(producer.Task)->Output = {.Connection = edge.Connection};
        gate.Input = {.Connection = edge.Connection};
        const auto sealed = graph.Seal();
        ASSERT_TRUE(sealed);
        TThreadedScheduler scheduler(sealed, workers);
        TSchedulerChannel channel(scheduler);
        std::string error;
        bool success = false;
        std::thread runner([&] { success = RunOrCancel(scheduler, &error); });
        EXPECT_TRUE(SpinUntil([&] {
            return producer.State.load() == ETaskState::Finished && gateNode.State.load() == ETaskState::Waiting;
        }));
        // The pull, a wait after the first push and one after the producer's finish.
        EXPECT_LE(gate.Calls.load(), 3);
        gate.Ready.store(true, std::memory_order_release);
        channel.Notify(&gateNode);
        runner.join();
        EXPECT_TRUE(success) << error;
        EXPECT_EQ(gate.Rows, rows);
        EXPECT_LE(gate.Calls.load(), 4);
    }
}

// The consumer of TPushDuringRunTask: its second run waits until the producer
// has woken it while it runs, then returns WAITING.
class TWaitDuringPushTask : public ITask {
public:
    ETaskResult Execute() override {
        const int call = ++Calls;
        if (call == 1) {
            return ETaskResult::NEED_DATA;
        }
        if (call == 2) {
            InRun.store(true, std::memory_order_release);
            Woken = SpinUntil([&] {
                return Node->State.load() == ETaskState::Reschedule;
            });
            return ETaskResult::WAITING;
        }
        if (!Ready.load(std::memory_order_acquire)) {
            return ETaskResult::WAITING;
        }
        for (int i = 0; i < 2; ++i) {
            NQdb::TRowSet rowSet{};
            if (Input.Fetch(rowSet) == EFetchResult::OK) {
                Rows += rowSet.RowCount;
                NQdb::Release(&rowSet);
            }
        }
        return ETaskResult::FINISHED;
    }

    TTaskNode* Node = nullptr;
    TInputPort Input;
    std::atomic<bool> InRun = false;
    std::atomic<bool> Ready = false;
    std::atomic<int> Calls = 0;
    bool Woken = false;
    int64_t Rows = 0;
};

// Pushes a row, then another one while its consumer runs, then parks.
class TPushDuringRunTask : public ITask {
public:
    ETaskResult Execute() override {
        const int call = ++Calls_;
        if (call == 2 && !SpinUntil([&] { return Consumer->InRun.load(std::memory_order_acquire); })) {
            return ETaskResult::WAITING;
        }
        if (call <= 2) {
            Output.Push(OneRow());
            return ETaskResult::OK;
        }
        return ETaskResult::WAITING;
    }

    TWaitDuringPushTask* Consumer = nullptr;
    TOutputPort Output;

private:
    int Calls_ = 0;
};

TEST(SchedulerExternal, NeighbourWakeDuringARunDoesNotEndAWait) {
    for (size_t workers : {2, 4}) {
        SCOPED_TRACE(workers);
        TTaskGraph graph;
        auto producerTask = std::make_unique<TPushDuringRunTask>();
        auto consumerTask = std::make_unique<TWaitDuringPushTask>();
        auto& producer = *producerTask;
        auto& consumer = *consumerTask;
        auto& producerNode = graph.AddOwnedNode(std::move(producerTask));
        auto& consumerNode = graph.AddOwnedNode(std::move(consumerTask));
        auto& edge = graph.AddOwnedEdge(producerNode, consumerNode, std::make_unique<TOneToOneConnection>(2));
        producer.Consumer = &consumer;
        producer.Output = {.Connection = edge.Connection};
        consumer.Node = &consumerNode;
        consumer.Input = {.Connection = edge.Connection};
        const auto sealed = graph.Seal();
        ASSERT_TRUE(sealed);
        TThreadedScheduler scheduler(sealed, workers);
        TSchedulerChannel channel(scheduler);
        std::string error;
        bool success = false;
        std::thread runner([&] { success = RunOrCancel(scheduler, &error); });
        EXPECT_TRUE(SpinUntil([&] {
            return consumer.Calls.load() == 2 && consumerNode.State.load() == ETaskState::Waiting;
        }));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        // The producer's wake came during the run that returned WAITING.
        EXPECT_EQ(consumer.Calls.load(), 2);
        consumer.Ready.store(true, std::memory_order_release);
        channel.Notify(&consumerNode);
        runner.join();
        EXPECT_TRUE(success) << error;
        EXPECT_TRUE(consumer.Woken);
        EXPECT_EQ(consumer.Calls.load(), 3);
        EXPECT_EQ(consumer.Rows, 2);
    }
}

// Spends one credit per row, as a remote sink spends its window, and waits
// for an external wake when it has none.
class TCreditSinkTask : public ITask {
public:
    ETaskResult Execute() override {
        if (!HasRow_) {
            NQdb::TRowSet rowSet{};
            const auto fetch = Input.Fetch(rowSet);
            if (fetch == EFetchResult::NO_DATA) {
                return ETaskResult::NEED_DATA;
            }
            if (fetch == EFetchResult::FINISHED) {
                return ETaskResult::FINISHED;
            }
            NQdb::Release(&rowSet);
            HasRow_ = true;
        }
        auto credits = Credits.load(std::memory_order_acquire);
        do {
            if (credits == 0) {
                return ETaskResult::WAITING;
            }
        } while (!Credits.compare_exchange_weak(credits, credits - 1, std::memory_order_acq_rel));
        HasRow_ = false;
        ++Rows;
        return ETaskResult::OK;
    }

    TInputPort Input;
    std::atomic<int> Credits = 0;
    int Rows = 0;

private:
    bool HasRow_ = false;
};

TEST(SchedulerExternal, EveryCreditWakesAWaitingSink) {
    constexpr int rows = 20000;
    for (size_t workers : {1, 2, 4, 8}) {
        for (int round = 0; round < 10; ++round) {
            SCOPED_TRACE(testing::Message() << "workers " << workers << " round " << round);
            TTaskGraph graph;
            auto& producer = graph.AddOwnedNode(std::make_unique<TPushTask>(rows));
            auto sinkTask = std::make_unique<TCreditSinkTask>();
            auto& sink = *sinkTask;
            auto& sinkNode = graph.AddOwnedNode(std::move(sinkTask));
            auto& edge = graph.AddOwnedEdge(producer, sinkNode, std::make_unique<TOneToOneConnection>(2));
            static_cast<TPushTask*>(producer.Task)->Output = {.Connection = edge.Connection};
            sink.Input = {.Connection = edge.Connection};
            const auto sealed = graph.Seal();
            ASSERT_TRUE(sealed);
            TThreadedScheduler scheduler(sealed, workers);
            TSchedulerChannel channel(scheduler);
            std::string error;
            bool success = false;
            std::thread runner([&] { success = RunOrCancel(scheduler, &error); });
            for (int i = 0; i < rows; ++i) {
                sink.Credits.fetch_add(1, std::memory_order_acq_rel);
                channel.Notify(&sinkNode);
                if (i % 16 == 0) {
                    std::this_thread::yield();
                }
            }
            runner.join();
            ASSERT_TRUE(success) << error;
            EXPECT_EQ(sink.Rows, rows);
        }
    }
}

TEST(SchedulerThreaded, RunsSmallGraph) {
    constexpr int total = 100;

    auto sourceTask = std::make_unique<TStressSourceTask>(total);
    auto* sourcePtr = sourceTask.get();
    auto sinkTask = std::make_unique<TStressSinkTask>(*sourcePtr, total);
    auto* sinkPtr = sinkTask.get();

    TTaskGraph graph;
    auto& source = graph.AddOwnedNode(std::move(sourceTask));
    auto& sink = graph.AddOwnedNode(std::move(sinkTask));
    graph.AddOwnedEdge(source, sink, std::make_unique<TOneToOneConnection>());
    const auto sealed = graph.Seal();
    ASSERT_TRUE(sealed);

    TThreadedScheduler scheduler(sealed, 4);
    std::string error;
    ASSERT_TRUE(scheduler.Run(&error)) << error;
    EXPECT_EQ(sourcePtr->Produced.load(std::memory_order_acquire), total);
    EXPECT_TRUE(sourcePtr->Finished.load(std::memory_order_acquire));
    EXPECT_EQ(sinkPtr->Consumed(), total);
}

TEST(SchedulerThreaded, OneWorkerMatchesSingleThreadedScheduler) {
    TRepeatTask* singleTask = nullptr;
    auto singleGraph = MakeRepeatGraph(singleTask, 10);
    TSingleThreadedScheduler singleScheduler(singleGraph->Seal());
    std::string error;
    ASSERT_TRUE(singleScheduler.Run(&error)) << error;

    TRepeatTask* threadedTask = nullptr;
    auto threadedGraph = MakeRepeatGraph(threadedTask, 10);
    TThreadedScheduler threadedScheduler(threadedGraph->Seal(), 1);
    ASSERT_TRUE(threadedScheduler.Run(&error)) << error;

    EXPECT_EQ(threadedTask->Calls(), singleTask->Calls());
}

TEST(SchedulerThreaded, StressProducerConsumerWakeups) {
    constexpr int total = 2000;

    auto sourceTask = std::make_unique<TStressSourceTask>(total);
    auto* sourcePtr = sourceTask.get();
    auto sinkTask = std::make_unique<TStressSinkTask>(*sourcePtr, total);
    auto* sinkPtr = sinkTask.get();

    TTaskGraph graph;
    auto& source = graph.AddOwnedNode(std::move(sourceTask));
    auto& sink = graph.AddOwnedNode(std::move(sinkTask));
    graph.AddOwnedEdge(source, sink, std::make_unique<TOneToOneConnection>());
    const auto sealed = graph.Seal();
    ASSERT_TRUE(sealed);

    TThreadedScheduler scheduler(sealed, 4);
    std::string error;
    ASSERT_TRUE(scheduler.Run(&error)) << error;
    EXPECT_EQ(sinkPtr->Consumed(), total);
}

TEST(SchedulerThreaded, DoesNotExecuteSameTaskConcurrently) {
    auto task = std::make_unique<TGuardedTask>(1000);
    auto* taskPtr = task.get();

    TTaskGraph graph;
    graph.AddOwnedNode(std::move(task));
    const auto sealed = graph.Seal();
    ASSERT_TRUE(sealed);

    TThreadedScheduler scheduler(sealed, 8);
    std::string error;
    ASSERT_TRUE(scheduler.Run(&error)) << error;
    EXPECT_EQ(taskPtr->Violations.load(std::memory_order_relaxed), 0);
}

TEST(SchedulerThreaded, ReadyQueueCapacityIsAtLeastNodeCount) {
    constexpr size_t inputCount = 128;

    TTaskGraph graph;
    std::vector<TTaskNode*> inputs;
    inputs.reserve(inputCount);
    for (size_t i = 0; i < inputCount; ++i) {
        inputs.push_back(&graph.AddOwnedNode(std::make_unique<TFinishTask>()));
    }
    auto& sink = graph.AddOwnedNode(std::make_unique<TManyInputSinkTask>());
    for (auto* input : inputs) {
        graph.AddOwnedEdge(
            *input,
            sink,
            std::make_unique<TOneToOneConnection>());
    }
    const auto sealed = graph.Seal();
    ASSERT_TRUE(sealed);

    TThreadedScheduler scheduler(
        sealed,
        4,
        /*readyQueueCapacity=*/1);
    std::string error;
    ASSERT_TRUE(scheduler.Run(&error)) << error;
}

TEST(SchedulerThreaded, SingleTaskWithReadyQueueCapacityOne) {
    TTaskGraph graph;
    graph.AddOwnedNode(std::make_unique<TFinishTask>());
    const auto sealed = graph.Seal();
    ASSERT_TRUE(sealed);

    TThreadedScheduler scheduler(sealed, 2, /*readyQueueCapacity=*/1);
    std::string error;
    ASSERT_TRUE(scheduler.Run(&error)) << error;
}

TEST(SchedulerThreaded, RechecksConsumerWhenInputsAlreadyFinished) {
    auto sourceTask = std::make_unique<TFinishTask>();
    auto sinkTask = std::make_unique<TNeedFinishedReplayTask>();
    auto* sinkPtr = sinkTask.get();

    TTaskGraph graph;
    auto& source = graph.AddOwnedNode(std::move(sourceTask));
    auto& sink = graph.AddOwnedNode(std::move(sinkTask));
    graph.AddOwnedEdge(
        source,
        sink,
        std::make_unique<TOneToOneConnection>());
    const auto sealed = graph.Seal();
    ASSERT_TRUE(sealed);

    TThreadedScheduler scheduler(sealed, 1);
    std::string error;
    ASSERT_TRUE(scheduler.Run(&error)) << error;
    EXPECT_EQ(sinkPtr->Calls(), 3);
}

TEST(SchedulerThreaded, CannotRunTwice) {
    TRepeatTask* task = nullptr;
    auto graph = MakeRepeatGraph(task, 1);
    TThreadedScheduler scheduler(graph->Seal(), 2);

    std::string error;
    ASSERT_TRUE(scheduler.Run(&error)) << error;
    EXPECT_FALSE(scheduler.Run(&error));
    EXPECT_NE(error.find("cannot run twice"), std::string::npos);
}

TEST(SchedulerThreaded, ReportsTaskExceptions) {
    auto run = [](bool threaded) {
        TTaskGraph graph;
        graph.AddOwnedNode(std::make_unique<TThrowTask>());
        const auto sealed = graph.Seal();
        ASSERT_TRUE(sealed);
        std::string error;
        const bool ok = threaded
            ? TThreadedScheduler(sealed, 2).Run(&error)
            : TSingleThreadedScheduler(sealed).Run(&error);
        EXPECT_FALSE(ok);
        EXPECT_EQ(error, "expected task failure");
    };

    run(false);
    run(true);
}

// Pushes `rows` one-row batches to its outputs, each to a lane among the first
// `lanes` that a seeded generator picks, at most `burst` per run. Like a hash
// shuffle, a run that meets a full lane ends with BLOCKED_OUTPUT, maybe after
// some pushes.
class TFanOutTask : public ITask {
public:
    TFanOutTask(int rows, int burst, uint32_t seed, size_t lanes)
        : Rows_(rows)
        , Burst_(burst)
        , Seed_(seed)
        , Lanes_(lanes)
    {}

    ETaskResult Execute() override {
        for (int pushed = 0; pushed < Burst_; ++pushed) {
            if (Sent_ == Rows_) {
                for (const auto& output : Outputs) {
                    output.Finish();
                }
                return ETaskResult::FINISHED;
            }
            if (!Outputs[(Seed_ >> 16) % Lanes_].Push(NQdb::TRowSet{.RowCount = 1, .RefCount = 1})) {
                return ETaskResult::BLOCKED_OUTPUT;
            }
            ++Sent_;
            Seed_ = Seed_ * 1103515245u + 12345u;
        }
        return ETaskResult::OK;
    }

    std::vector<TOutputPort> Outputs;

private:
    int Rows_;
    int Burst_;
    uint32_t Seed_;
    size_t Lanes_;
    int Sent_ = 0;
};

// Counts the rows of its input, one batch per run.
class TCountTask : public ITask {
public:
    ETaskResult Execute() override {
        ++Calls;
        NQdb::TRowSet rowSet{};
        switch (Input.Fetch(rowSet)) {
            case EFetchResult::OK:
                Rows += rowSet.RowCount;
                NQdb::Release(&rowSet);
                return ETaskResult::OK;
            case EFetchResult::NO_DATA:
                return ETaskResult::NEED_DATA;
            case EFetchResult::FINISHED:
                break;
        }
        return ETaskResult::FINISHED;
    }

    TInputPort Input;
    int64_t Rows = 0;
    int Calls = 0;
};

// Every producer has a lane of capacity one to every consumer, which gathers them.
struct TFanOutGraph {
    TTaskGraph Graph;
    std::vector<TCountTask*> Consumers;
};

std::unique_ptr<TFanOutGraph> MakeFanOut(size_t producers, size_t consumers, int rows, int burst, size_t lanes) {
    auto fanOut = std::make_unique<TFanOutGraph>();
    std::vector<TFanOutTask*> tasks;
    std::vector<TTaskNode*> nodes;
    for (size_t i = 0; i < producers; ++i) {
        auto task = std::make_unique<TFanOutTask>(rows, burst, static_cast<uint32_t>(i + 1), lanes);
        tasks.push_back(task.get());
        nodes.push_back(&fanOut->Graph.AddOwnedNode(std::move(task)));
    }
    for (size_t i = 0; i < consumers; ++i) {
        auto task = std::make_unique<TCountTask>();
        auto& consumer = *task;
        auto& node = fanOut->Graph.AddOwnedNode(std::move(task));
        auto& gather = fanOut->Graph.AddConnection(std::make_unique<TGatherConnection>(1));
        gather.Resize(producers, 1);
        consumer.Input = {.Connection = &gather, .Lane = 0};
        for (size_t j = 0; j < producers; ++j) {
            fanOut->Graph.AddEdge(*nodes[j], node, gather, j, 0);
            tasks[j]->Outputs.push_back({.Connection = &gather, .Lane = j});
        }
        fanOut->Consumers.push_back(&consumer);
    }
    return fanOut;
}

// Zero workers runs the single-threaded scheduler. A stalled threaded run is
// cancelled, so a lost wakeup fails the test instead of hanging it.
bool RunGraph(TTaskGraph& graph, size_t workers, std::string* error) {
    const auto sealed = graph.Seal(error);
    if (!sealed) {
        return false;
    }
    if (workers == 0) {
        return TSingleThreadedScheduler(sealed).Run(error);
    }
    TThreadedScheduler scheduler(sealed, workers);
    std::mutex mutex;
    std::condition_variable done;
    bool returned = false;
    std::thread watchdog([&] {
        std::unique_lock lock(mutex);
        if (!done.wait_for(lock, std::chrono::seconds(30), [&] { return returned; })) {
            scheduler.Cancel();
        }
    });
    const bool ok = scheduler.Run(error);
    {
        std::lock_guard lock(mutex);
        returned = true;
    }
    done.notify_one();
    watchdog.join();
    return ok;
}

TEST(SchedulerWakeups, FanOutDeliversEveryRow) {
    constexpr size_t producers = 3;
    constexpr size_t consumers = 4;
    constexpr int rows = 20000;
    for (size_t workers : {0, 1, 4, 8}) {
        for (int round = 0; round < 10; ++round) {
            SCOPED_TRACE(testing::Message() << "workers " << workers << " round " << round);
            auto fanOut = MakeFanOut(producers, consumers, rows, /*burst=*/3, consumers);
            std::string error;
            ASSERT_TRUE(RunGraph(fanOut->Graph, workers, &error)) << error;
            int64_t received = 0;
            for (const auto* consumer : fanOut->Consumers) {
                received += consumer->Rows;
            }
            EXPECT_EQ(received, static_cast<int64_t>(producers) * rows);
        }
    }
}

TEST(SchedulerWakeups, PushesToOtherLanesDoNotRunAConsumer) {
    for (size_t workers : {0, 4}) {
        SCOPED_TRACE(workers);
        // The producer sends all rows to the first consumer.
        auto fanOut = MakeFanOut(/*producers=*/1, /*consumers=*/2, /*rows=*/1000, /*burst=*/1, /*lanes=*/1);
        std::string error;
        ASSERT_TRUE(RunGraph(fanOut->Graph, workers, &error)) << error;
        EXPECT_EQ(fanOut->Consumers[0]->Rows, 1000);
        EXPECT_EQ(fanOut->Consumers[1]->Rows, 0);
        // Its first run asks for data, the last sees the finish.
        EXPECT_LE(fanOut->Consumers[1]->Calls, 3);
    }
}

// Reads its left input to the end before its right one, as a hash join reads
// its build side first, and asks only for the side it reads. It returns
// NEED_DATA while its right lane holds rows.
class TBuildThenProbeTask : public ITask {
public:
    ETaskResult Execute() override {
        PullBoth_.store(!Started_, std::memory_order_relaxed);
        if (!Started_) {
            Started_ = true;
            return ETaskResult::NEED_DATA;
        }
        NQdb::TRowSet rowSet{};
        switch (Input().Fetch(rowSet)) {
            case EFetchResult::OK:
                Rows += rowSet.RowCount;
                NQdb::Release(&rowSet);
                return ETaskResult::OK;
            case EFetchResult::NO_DATA:
                return ETaskResult::NEED_DATA;
            case EFetchResult::FINISHED:
                break;
        }
        if (LeftDone_.load(std::memory_order_relaxed)) {
            return ETaskResult::FINISHED;
        }
        LeftDone_.store(true, std::memory_order_relaxed);
        return ETaskResult::OK;
    }

    // The first run pulls both sides, so the right producer fills its lane early.
    // The scheduler may ask while the task runs again on another worker.
    bool WantsInput(const TTaskEdge& edge) const noexcept override {
        return PullBoth_.load(std::memory_order_relaxed) || edge.Connection == Input().Connection;
    }

    TInputPort Left;
    TInputPort Right;
    int64_t Rows = 0;

private:
    const TInputPort& Input() const {
        return LeftDone_.load(std::memory_order_relaxed) ? Right : Left;
    }

    bool Started_ = false;
    std::atomic<bool> PullBoth_ = false;
    std::atomic<bool> LeftDone_ = false;
};

TEST(SchedulerWakeups, ConsumerThatSkipsAFullLaneGetsEveryRow) {
    constexpr int rows = 5000;
    for (size_t workers : {0, 1, 4, 8}) {
        for (int round = 0; round < 5; ++round) {
            SCOPED_TRACE(testing::Message() << "workers " << workers << " round " << round);
            TTaskGraph graph;
            auto consumerTask = std::make_unique<TBuildThenProbeTask>();
            auto& consumer = *consumerTask;
            auto& consumerNode = graph.AddOwnedNode(std::move(consumerTask));
            for (auto* input : {&consumer.Left, &consumer.Right}) {
                auto producerTask = std::make_unique<TFanOutTask>(rows, /*burst=*/2, /*seed=*/1, /*lanes=*/1);
                auto& producer = *producerTask;
                auto& producerNode = graph.AddOwnedNode(std::move(producerTask));
                auto& edge = graph.AddOwnedEdge(producerNode, consumerNode, std::make_unique<TOneToOneConnection>(1));
                producer.Outputs.push_back({.Connection = edge.Connection});
                *input = {.Connection = edge.Connection};
            }
            std::string error;
            ASSERT_TRUE(RunGraph(graph, workers, &error)) << error;
            EXPECT_EQ(consumer.Rows, 2 * rows);
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
