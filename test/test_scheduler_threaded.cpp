#include <gtest/gtest.h>

#include <qdb/scheduler/channel.h>
#include <qdb/scheduler/connection.h>
#include <qdb/scheduler/connections/local.h>
#include <qdb/scheduler/graph.h>
#include <qdb/scheduler/impl/single_threaded_scheduler.h>
#include <qdb/scheduler/impl/threaded_scheduler.h>
#include <qdb/scheduler/port.h>

#include <array>
#include <atomic>
#include <chrono>
#include <latch>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace NQdb::NScheduler;
using NQdb::Release;
using NQdb::TRowSet;

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

// Pushes `total` row sets in order, alternating between its outputs, like a hash
// shuffle with pending batches: a full lane holds back everything after it.
class TAlternatingProducer : public ITask {
public:
    explicit TAlternatingProducer(int total)
        : Total_(total)
    {}

    ETaskResult Execute() override {
        Runs.fetch_add(1, std::memory_order_relaxed);
        if (Next_ == Total_) {
            for (const auto& output : Outputs) {
                output.Finish();
            }
            return ETaskResult::FINISHED;
        }
        const auto& output = Outputs[Next_ % Outputs.size()];
        if (!output.CanPush() || !output.Push(TRowSet{.RowCount = 1, .RefCount = 1})) {
            return ETaskResult::BLOCKED_OUTPUT;
        }
        ++Next_;
        return ETaskResult::OK;
    }

    std::vector<TOutputPort> Outputs;
    std::atomic<int> Runs = 0;

private:
    int Total_;
    int Next_ = 0;
};

// Reads its lane; with `Hold` set it waits for an outside wake before reading, and
// with `FinishOnRelease` it then finishes without reading.
class TLaneConsumer : public ITask {
public:
    ETaskResult Execute() override {
        Runs.fetch_add(1, std::memory_order_relaxed);
        if (Hold && !Released.load(std::memory_order_acquire)) {
            return ETaskResult::WAITING;
        }
        if (FinishOnRelease) {
            return ETaskResult::FINISHED;
        }
        TRowSet rowSet{};
        switch (Input.Fetch(rowSet)) {
            case EFetchResult::OK:
                Release(&rowSet);
                ++Consumed;
                return ETaskResult::OK;
            case EFetchResult::NO_DATA:
                return ETaskResult::NEED_DATA;
            case EFetchResult::FINISHED:
                return ETaskResult::FINISHED;
        }
        return ETaskResult::FINISHED;
    }

    TInputPort Input;
    bool Hold = false;
    bool FinishOnRelease = false;
    std::atomic<bool> Released = false;
    std::atomic<int> Runs = 0;
    int Consumed = 0;
};

struct TFanOut {
    TAlternatingProducer* Producer = nullptr;
    std::vector<TLaneConsumer*> Consumers;
    std::vector<TTaskNode*> ConsumerNodes;
    TSealedTaskGraph Sealed;
};

// One producer feeding `consumers` one-to-one lanes of capacity 1.
TFanOut MakeFanOut(TTaskGraph& graph, int total, size_t consumers) {
    TFanOut fanOut;
    auto producer = std::make_unique<TAlternatingProducer>(total);
    fanOut.Producer = producer.get();
    auto& producerNode = graph.AddOwnedNode(std::move(producer));
    for (size_t i = 0; i < consumers; ++i) {
        auto consumer = std::make_unique<TLaneConsumer>();
        auto* consumerPtr = consumer.get();
        auto& consumerNode = graph.AddOwnedNode(std::move(consumer));
        auto& edge = graph.AddOwnedEdge(producerNode, consumerNode, std::make_unique<TOneToOneConnection>(1));
        fanOut.Producer->Outputs.push_back(TOutputPort{.Connection = edge.Connection});
        consumerPtr->Input = TInputPort{.Connection = edge.Connection};
        fanOut.Consumers.push_back(consumerPtr);
        fanOut.ConsumerNodes.push_back(&consumerNode);
    }
    return fanOut;
}

// The demand of a consumer with an empty lane must not keep running a producer that
// waits for room on another lane (the busy loop of #320).
TEST(SchedulerThreaded, ProducerBlockedOnOneLaneIsNotRunByOtherDemand) {
    constexpr int total = 40;
    TTaskGraph graph;
    auto fanOut = MakeFanOut(graph, total, 2);
    auto* held = fanOut.Consumers[1];
    held->Hold = true;
    fanOut.Sealed = graph.Seal();
    ASSERT_TRUE(fanOut.Sealed);

    TThreadedScheduler scheduler(fanOut.Sealed, 4);
    TSchedulerChannel channel(scheduler);
    std::string error;
    bool ok = false;
    std::thread runner([&] {
        ok = scheduler.Run(&error);
    });
    // The producer fills the held lane and parks while the other consumer idles.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const int producerRuns = fanOut.Producer->Runs.load(std::memory_order_relaxed);
    const int otherRuns = fanOut.Consumers[0]->Runs.load(std::memory_order_relaxed);
    held->Released.store(true, std::memory_order_release);
    channel.Notify(fanOut.ConsumerNodes[1]);
    runner.join();

    ASSERT_TRUE(ok) << error;
    EXPECT_LT(producerRuns, 20);
    EXPECT_LT(otherRuns, 20);
    EXPECT_EQ(fanOut.Consumers[0]->Consumed, total / 2);
    EXPECT_EQ(held->Consumed, total / 2);
}

TEST(SchedulerThreaded, FanOutCompletesInBothSchedulers) {
    constexpr int total = 1000;
    for (bool threaded : {false, true}) {
        SCOPED_TRACE(threaded);
        TTaskGraph graph;
        auto fanOut = MakeFanOut(graph, total, 3);
        fanOut.Sealed = graph.Seal();
        ASSERT_TRUE(fanOut.Sealed);
        std::string error;
        const bool ok = threaded
            ? TThreadedScheduler(fanOut.Sealed, 4).Run(&error)
            : TSingleThreadedScheduler(fanOut.Sealed).Run(&error);
        ASSERT_TRUE(ok) << error;
        int consumed = 0;
        for (const auto* consumer : fanOut.Consumers) {
            consumed += consumer->Consumed;
        }
        EXPECT_EQ(consumed, total);
    }
}

// A broadcast producer parked on the full lane of a consumer that then finishes
// without reading goes on for the other one: the closed lane no longer blocks it.
TEST(SchedulerThreaded, ProducerParkedOnFinishedConsumerIsWoken) {
    constexpr int total = 50;
    TTaskGraph graph;
    auto producer = std::make_unique<TAlternatingProducer>(total);
    auto* producerPtr = producer.get();
    auto& producerNode = graph.AddOwnedNode(std::move(producer));
    auto& broadcast = graph.AddConnection(std::make_unique<TBroadcastConnection>(1));
    broadcast.Resize(1, 2);
    std::array<TLaneConsumer*, 2> consumers{};
    std::array<TTaskNode*, 2> consumerNodes{};
    for (size_t i = 0; i < consumers.size(); ++i) {
        auto consumer = std::make_unique<TLaneConsumer>();
        consumers[i] = consumer.get();
        consumers[i]->Input = TInputPort{.Connection = &broadcast, .Lane = i};
        consumerNodes[i] = &graph.AddOwnedNode(std::move(consumer));
        graph.AddEdge(producerNode, *consumerNodes[i], broadcast, 0, i);
    }
    producerPtr->Outputs.push_back(TOutputPort{.Connection = &broadcast});
    consumers[0]->Hold = true;
    consumers[0]->FinishOnRelease = true;
    const auto sealed = graph.Seal();
    ASSERT_TRUE(sealed);

    TThreadedScheduler scheduler(sealed, 4);
    TSchedulerChannel channel(scheduler);
    std::string error;
    bool ok = false;
    std::atomic<bool> done = false;
    std::thread runner([&] {
        ok = scheduler.Run(&error);
        done.store(true, std::memory_order_release);
    });
    // The producer parks on the held lane while the other consumer drains its own.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    consumers[0]->Released.store(true, std::memory_order_release);
    channel.Notify(consumerNodes[0]);
    // A lost wakeup leaves every worker idle: cancel instead of hanging.
    for (int i = 0; i < 500 && !done.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!done.load(std::memory_order_acquire)) {
        scheduler.Cancel();
    }
    runner.join();

    ASSERT_TRUE(ok) << error;
    EXPECT_EQ(consumers[0]->Consumed, 0);
    EXPECT_EQ(consumers[1]->Consumed, total);
}

// Pushes `Total` row sets, then waits for an outside wake before it finishes.
class TPausingProducer : public ITask {
public:
    ETaskResult Execute() override {
        if (Pushed_ == Total) {
            if (!Released.load(std::memory_order_acquire)) {
                return ETaskResult::WAITING;
            }
            Output.Finish();
            return ETaskResult::FINISHED;
        }
        if (!Output.CanPush() || !Output.Push(TRowSet{.RowCount = 1, .RefCount = 1})) {
            return ETaskResult::BLOCKED_OUTPUT;
        }
        ++Pushed_;
        return ETaskResult::OK;
    }

    TOutputPort Output;
    int Total = 0;
    std::atomic<bool> Released = false;

private:
    int Pushed_ = 0;
};

// Takes at most one row set per run, the first input first, and asks for more
// after it, as a join does while it builds.
class TOneBatchConsumer : public ITask {
public:
    ETaskResult Execute() override {
        bool finished = true;
        for (size_t i = 0; i < Inputs.size(); ++i) {
            TRowSet rowSet{};
            const auto fetch = Inputs[i].Fetch(rowSet);
            if (fetch == EFetchResult::OK) {
                Release(&rowSet);
                Consumed[i].fetch_add(1, std::memory_order_relaxed);
                return ETaskResult::NEED_DATA;
            }
            finished = finished && fetch == EFetchResult::FINISHED;
        }
        return finished ? ETaskResult::FINISHED : ETaskResult::NEED_DATA;
    }

    std::array<TInputPort, 2> Inputs;
    std::array<std::atomic<int>, 2> Consumed{};
};

// The consumer takes the last row of an idle producer and asks for more while the
// other lane is full behind a parked producer: it must run again to read that lane.
TEST(SchedulerThreaded, ConsumerIsRerunWhileItsLanesHoldRows) {
    constexpr int total = 50;
    for (size_t workers : {1, 4}) {
        SCOPED_TRACE(workers);
        TTaskGraph graph;
        auto consumer = std::make_unique<TOneBatchConsumer>();
        auto* consumerPtr = consumer.get();
        auto& consumerNode = graph.AddOwnedNode(std::move(consumer));
        std::array<TPausingProducer*, 2> producers{};
        std::array<TTaskNode*, 2> producerNodes{};
        for (size_t i = 0; i < producers.size(); ++i) {
            auto producer = std::make_unique<TPausingProducer>();
            producers[i] = producer.get();
            producers[i]->Total = i == 0 ? 5 : total;
            producers[i]->Released.store(i != 0, std::memory_order_relaxed);
            producerNodes[i] = &graph.AddOwnedNode(std::move(producer));
            auto& edge = graph.AddOwnedEdge(*producerNodes[i], consumerNode, std::make_unique<TOneToOneConnection>(1));
            producers[i]->Output = TOutputPort{.Connection = edge.Connection};
            consumerPtr->Inputs[i] = TInputPort{.Connection = edge.Connection};
        }
        const auto sealed = graph.Seal();
        ASSERT_TRUE(sealed);

        TThreadedScheduler scheduler(sealed, workers);
        TSchedulerChannel channel(scheduler);
        std::string error;
        bool ok = false;
        std::thread runner([&] {
            ok = scheduler.Run(&error);
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (consumerPtr->Consumed[1].load(std::memory_order_relaxed) < total
            && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const int consumedBeforeRelease = consumerPtr->Consumed[1].load(std::memory_order_relaxed);
        producers[0]->Released.store(true, std::memory_order_release);
        channel.Notify(producerNodes[0]);
        runner.join();

        ASSERT_TRUE(ok) << error;
        EXPECT_EQ(consumedBeforeRelease, total);
        EXPECT_EQ(consumerPtr->Consumed[0].load(), 5);
    }
}

} // namespace

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
