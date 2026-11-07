#include <gtest/gtest.h>

#include <qdb/scheduler/channel.h>
#include <qdb/scheduler/connection.h>
#include <qdb/scheduler/connections/local.h>
#include <qdb/scheduler/graph.h>
#include <qdb/scheduler/impl/single_threaded_scheduler.h>
#include <qdb/scheduler/impl/threaded_scheduler.h>

#include <atomic>
#include <chrono>
#include <latch>
#include <memory>
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

} // namespace

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
