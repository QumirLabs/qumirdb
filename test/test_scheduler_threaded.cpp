#include <gtest/gtest.h>

#include <qdb/scheduler/connection.h>
#include <qdb/scheduler/graph.h>
#include <qdb/scheduler/single_threaded_scheduler.h>
#include <qdb/scheduler/threaded_scheduler.h>

#include <atomic>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace NQdb::NScheduler;

namespace {

class TRepeatTask : public ITaskNode {
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

class TStressSourceTask : public ITaskNode {
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

class TStressSinkTask : public ITaskNode {
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

class TGuardedTask : public ITaskNode {
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

class TFinishTask : public ITaskNode {
public:
    ETaskResult Execute() override {
        return ETaskResult::FINISHED;
    }
};

class TThrowTask : public ITaskNode {
public:
    ETaskResult Execute() override {
        throw std::runtime_error("expected task failure");
    }
};

class TManyInputSinkTask : public ITaskNode {
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

class TNeedFinishedReplayTask : public ITaskNode {
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
    graph->Build();
    return graph;
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
    graph.Build();

    TThreadedScheduler scheduler(graph, 4);
    std::string error;
    ASSERT_TRUE(scheduler.Run(&error)) << error;
    EXPECT_EQ(sourcePtr->Produced.load(std::memory_order_acquire), total);
    EXPECT_TRUE(sourcePtr->Finished.load(std::memory_order_acquire));
    EXPECT_EQ(sinkPtr->Consumed(), total);
}

TEST(SchedulerThreaded, OneWorkerMatchesSingleThreadedScheduler) {
    TRepeatTask* singleTask = nullptr;
    auto singleGraph = MakeRepeatGraph(singleTask, 10);
    TSingleThreadedScheduler singleScheduler(*singleGraph);
    std::string error;
    ASSERT_TRUE(singleScheduler.Run(&error)) << error;

    TRepeatTask* threadedTask = nullptr;
    auto threadedGraph = MakeRepeatGraph(threadedTask, 10);
    TThreadedScheduler threadedScheduler(*threadedGraph, 1);
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
    graph.Build();

    TThreadedScheduler scheduler(graph, 4);
    std::string error;
    ASSERT_TRUE(scheduler.Run(&error)) << error;
    EXPECT_EQ(sinkPtr->Consumed(), total);
}

TEST(SchedulerThreaded, DoesNotExecuteSameTaskConcurrently) {
    auto task = std::make_unique<TGuardedTask>(1000);
    auto* taskPtr = task.get();

    TTaskGraph graph;
    graph.AddOwnedNode(std::move(task));
    graph.Build();

    TThreadedScheduler scheduler(graph, 8);
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
    graph.Build();

    TThreadedScheduler scheduler(
        graph,
        4,
        /*readyQueueCapacity=*/1);
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
    graph.Build();

    TThreadedScheduler scheduler(graph, 1);
    std::string error;
    ASSERT_TRUE(scheduler.Run(&error)) << error;
    EXPECT_EQ(sinkPtr->Calls(), 3);
}

TEST(SchedulerThreaded, CannotRunTwice) {
    TRepeatTask* task = nullptr;
    auto graph = MakeRepeatGraph(task, 1);
    TThreadedScheduler scheduler(*graph, 2);

    std::string error;
    ASSERT_TRUE(scheduler.Run(&error)) << error;
    EXPECT_FALSE(scheduler.Run(&error));
    EXPECT_NE(error.find("cannot run twice"), std::string::npos);
}

TEST(SchedulerThreaded, ReportsTaskExceptions) {
    auto run = [](bool threaded) {
        TTaskGraph graph;
        graph.AddOwnedNode(std::make_unique<TThrowTask>());
        graph.Build();
        std::string error;
        const bool ok = threaded
            ? TThreadedScheduler(graph, 2).Run(&error)
            : TSingleThreadedScheduler(graph).Run(&error);
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
