#include <gtest/gtest.h>

#include <qdb/scheduler/connection.h>
#include <qdb/scheduler/connections/local.h>
#include <qdb/scheduler/graph.h>
#include <qdb/scheduler/impl/single_threaded_scheduler.h>
#include <qdb/scheduler/impl/threaded_scheduler.h>

#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

using namespace NQdb::NScheduler;

namespace {

class TNoopTask : public ITask {
public:
    ETaskResult Execute() override {
        ++Calls_;
        return ETaskResult::FINISHED;
    }

    int Calls() const {
        return Calls_;
    }

private:
    int Calls_ = 0;
};

class TSourceTask : public ITask {
public:
    ETaskResult Execute() override {
        if (!Produced) {
            Produced = true;
            return ETaskResult::OK;
        }
        Finished = true;
        return ETaskResult::FINISHED;
    }

    bool Produced = false;
    bool Finished = false;
};

class TSinkTask : public ITask {
public:
    explicit TSinkTask(TSourceTask& source)
        : Source_(source)
    {}

    ETaskResult Execute() override {
        if (!Source_.Produced) {
            return ETaskResult::NEED_DATA;
        }
        if (!Consumed_) {
            Consumed_ = true;
            return ETaskResult::OK;
        }
        if (Source_.Finished) {
            Finished = true;
            return ETaskResult::FINISHED;
        }
        return ETaskResult::NEED_DATA;
    }

    bool Consumed() const {
        return Consumed_;
    }

    bool Finished = false;

private:
    TSourceTask& Source_;
    bool Consumed_ = false;
};

class TOrderedProbeTask : public ITask {
public:
    TTaskNode* Prerequisite = nullptr;
    bool RanAfterPrerequisite = false;

    ETaskResult Execute() override {
        if (Prerequisite->State.load() != ETaskState::Finished) {
            return ETaskResult::NEED_DATA;
        }
        RanAfterPrerequisite = true;
        return ETaskResult::FINISHED;
    }
};

class TWaitForBothTask : public ITask {
public:
    TTaskNode* Left = nullptr;
    TTaskNode* Right = nullptr;

    ETaskResult Execute() override {
        return Left->State.load() == ETaskState::Finished
                && Right->State.load() == ETaskState::Finished
            ? ETaskResult::FINISHED : ETaskResult::NEED_DATA;
    }
};

void CheckControlDependency(bool threaded) {
    TTaskGraph graph;
    auto& build = graph.AddOwnedNode(std::make_unique<TNoopTask>());
    auto probeTask = std::make_unique<TOrderedProbeTask>();
    auto* probePtr = probeTask.get();
    auto& probe = graph.AddOwnedNode(std::move(probeTask));
    auto rootTask = std::make_unique<TWaitForBothTask>();
    auto& root = graph.AddOwnedNode(std::move(rootTask));
    probePtr->Prerequisite = &build;
    auto* rootPtr = static_cast<TWaitForBothTask*>(root.Task);
    rootPtr->Left = &build;
    rootPtr->Right = &probe;
    graph.AddOwnedEdge(build, root, std::make_unique<TOneToOneConnection>());
    graph.AddOwnedEdge(probe, root, std::make_unique<TOneToOneConnection>());
    graph.AddDependencyEdge(build, probe);
    const auto sealed = graph.Seal();
    ASSERT_TRUE(sealed);

    std::string error;
    if (threaded) {
        TThreadedScheduler scheduler(sealed, 2);
        ASSERT_TRUE(scheduler.Run(&error)) << error;
    } else {
        TSingleThreadedScheduler scheduler(sealed);
        ASSERT_TRUE(scheduler.Run(&error)) << error;
    }
    EXPECT_TRUE(probePtr->RanAfterPrerequisite);
    EXPECT_EQ(root.State.load(), ETaskState::Finished);
}

TEST(SchedulerGraph, BuildsAdjacencyAndValidates) {
    TTaskGraph graph;
    auto& source = graph.AddOwnedNode(std::make_unique<TNoopTask>());
    auto& sink = graph.AddOwnedNode(std::make_unique<TNoopTask>());
    graph.AddOwnedEdge(source, sink, std::make_unique<TOneToOneConnection>());
    std::string error;
    EXPECT_EQ(graph.Seal(&error).TaskCount(), 2u) << error;
    ASSERT_EQ(graph.Leaves().size(), 1u);
    EXPECT_EQ(graph.Leaves()[0], &source);
    EXPECT_EQ(graph.Root(), &sink);
    EXPECT_EQ(source.Outbound.size(), 1u);
    EXPECT_EQ(sink.Inbound.size(), 1u);
}

TEST(SchedulerGraph, RejectsEmptyGraph) {
    TTaskGraph graph;
    std::string error;
    EXPECT_FALSE(graph.Seal(&error));
    EXPECT_EQ(error, "task graph is empty");
}

TEST(SchedulerGraph, SealFreezesStructure) {
    TNoopTask task;
    TTaskGraph graph;
    auto& source = graph.AddNode(task);
    auto& sink = graph.AddNode(task);
    auto& connection = graph.AddConnection(std::make_unique<TOneToOneConnection>());
    graph.AddEdge(source, sink, connection);
    ASSERT_EQ(graph.Seal().TaskCount(), 2u);

    EXPECT_THROW(graph.AddNode(task), std::logic_error);
    EXPECT_THROW(graph.AddOwnedNode(std::make_unique<TNoopTask>()), std::logic_error);
    EXPECT_THROW(graph.AddConnection(std::make_unique<TOneToOneConnection>()), std::logic_error);
    EXPECT_THROW(graph.AddEdge(source, sink, connection), std::logic_error);
    EXPECT_THROW(graph.AddOwnedEdge(source, sink, {}), std::logic_error);
    EXPECT_THROW(graph.AddDependencyEdge(source, sink), std::logic_error);
    EXPECT_EQ(graph.Nodes().size(), 2u);
    EXPECT_EQ(graph.Edges().size(), 1u);
}

TEST(SchedulerGraph, RepeatedSealPreservesTaskState) {
    TNoopTask task;
    TTaskGraph graph;
    auto& root = graph.AddNode(task);
    ASSERT_TRUE(graph.Seal());
    root.State.store(ETaskState::Finished);

    const auto sealed = graph.Seal();
    ASSERT_TRUE(sealed);
    EXPECT_EQ(&sealed.Root(), &root);
    EXPECT_EQ(sealed.TaskCount(), 1u);
    EXPECT_EQ(root.State.load(), ETaskState::Finished);
}

TEST(SchedulerGraph, FailedSealDoesNotFreezeStructure) {
    TNoopTask task;
    TTaskGraph graph;
    auto& source = graph.AddNode(task);
    auto& sink = graph.AddNode(task);
    EXPECT_FALSE(graph.Seal());

    graph.AddOwnedEdge(source, sink, std::make_unique<TOneToOneConnection>());
    const auto sealed = graph.Seal();
    ASSERT_TRUE(sealed);
    EXPECT_EQ(&sealed.Root(), &sink);
    EXPECT_EQ(sealed.TaskCount(), 2u);
}

TEST(SchedulerGraph, SchedulersRejectFailedSeal) {
    TTaskGraph graph;
    const auto sealed = graph.Seal();
    ASSERT_FALSE(sealed);
    std::string error;
    EXPECT_FALSE(TSingleThreadedScheduler(sealed).Run(&error));
    EXPECT_EQ(error, "task graph is not sealed");
    error.clear();
    EXPECT_FALSE(TThreadedScheduler(sealed, 2).Run(&error));
    EXPECT_EQ(error, "task graph is not sealed");
}

TEST(SchedulerGraph, RejectsMissingConnection) {
    TTaskGraph graph;
    auto& source = graph.AddOwnedNode(std::make_unique<TNoopTask>());
    auto& sink = graph.AddOwnedNode(std::make_unique<TNoopTask>());
    graph.AddOwnedEdge(source, sink, {});
    std::string error;
    EXPECT_FALSE(graph.Seal(&error));
    EXPECT_NE(error.find("without a connection"), std::string::npos);
}

TEST(SchedulerGraph, RejectsMultipleRoots) {
    TTaskGraph graph;
    graph.AddOwnedNode(std::make_unique<TNoopTask>());
    graph.AddOwnedNode(std::make_unique<TNoopTask>());
    std::string error;
    EXPECT_FALSE(graph.Seal(&error));
    EXPECT_NE(error.find("exactly one root"), std::string::npos);
}

TEST(SchedulerGraph, RejectsInvalidLaneIds) {
    TTaskGraph graph;
    auto& source = graph.AddOwnedNode(std::make_unique<TNoopTask>());
    auto& sink = graph.AddOwnedNode(std::make_unique<TNoopTask>());
    graph.AddOwnedEdge(source, sink, std::make_unique<TOneToOneConnection>(), 1, 0);
    std::string error;
    EXPECT_FALSE(graph.Seal(&error));
    EXPECT_NE(error.find("source lane"), std::string::npos);
}

TEST(SchedulerGraph, PrintsConnectionsAndLaneMapping) {
    auto connection = std::make_unique<TGatherConnection>();
    connection->Resize(2, 1);

    TTaskGraph graph;
    auto& left = graph.AddOwnedNode(std::make_unique<TNoopTask>());
    auto& right = graph.AddOwnedNode(std::make_unique<TNoopTask>());
    auto& sink = graph.AddOwnedNode(std::make_unique<TNoopTask>());
    auto& conn = graph.AddConnection(std::move(connection));
    graph.AddEdge(left, sink, conn, 0, 0);
    graph.AddEdge(right, sink, conn, 1, 0);
    ASSERT_TRUE(graph.Seal());

    std::stringstream out;
    graph.Print(out);
    const auto text = out.str();
    EXPECT_NE(text.find("conn c0 - gather src=2 dst=1"), std::string::npos);
    EXPECT_NE(text.find("edge e0 n0[0] -> n2[0] via c0"), std::string::npos);
    EXPECT_NE(text.find("edge e1 n1[1] -> n2[0] via c0"), std::string::npos);
}

TEST(SchedulerGraph, RejectsDependencyCycle) {
    TTaskGraph graph;
    auto& source = graph.AddOwnedNode(std::make_unique<TNoopTask>());
    auto& sink = graph.AddOwnedNode(std::make_unique<TNoopTask>());
    graph.AddOwnedEdge(source, sink, std::make_unique<TOneToOneConnection>());
    graph.AddDependencyEdge(sink, source);
    std::string error;
    EXPECT_FALSE(graph.Seal(&error));
    EXPECT_NE(error.find("scheduling cycle"), std::string::npos);
}

TEST(SchedulerGraph, DependencyEdgesDoNotCarryRowsets) {
    TTaskGraph graph;
    auto& build = graph.AddOwnedNode(std::make_unique<TNoopTask>());
    auto& probe = graph.AddOwnedNode(std::make_unique<TNoopTask>());
    auto& sink = graph.AddOwnedNode(std::make_unique<TNoopTask>());
    graph.AddOwnedEdge(build, sink, std::make_unique<TOneToOneConnection>());
    graph.AddOwnedEdge(probe, sink, std::make_unique<TOneToOneConnection>());
    graph.AddDependencyEdge(build, probe);
    ASSERT_TRUE(graph.Seal());

    ASSERT_EQ(probe.Inbound.size(), 1u);
    EXPECT_EQ(probe.Inbound.front()->Kind, ETaskEdgeKind::Dependency);
    EXPECT_EQ(probe.Inbound.front()->Src, &build);
    EXPECT_EQ(probe.Inbound.front()->Connection, nullptr);
    std::stringstream out;
    graph.Print(out);
    EXPECT_NE(out.str().find("dependency n0 -> n1"), std::string::npos);
}

TEST(SchedulerGraph, SingleThreadedDependencyInput) {
    CheckControlDependency(false);
}

TEST(SchedulerGraph, ThreadedDependencyInput) {
    CheckControlDependency(true);
}

TEST(SchedulerGraph, SingleThreadedSchedulerRunsSmallGraph) {
    auto sourceTask = std::make_unique<TSourceTask>();
    auto* sourcePtr = sourceTask.get();
    auto sinkTask = std::make_unique<TSinkTask>(*sourcePtr);
    auto* sinkPtr = sinkTask.get();

    TTaskGraph graph;
    auto& source = graph.AddOwnedNode(std::move(sourceTask));
    auto& sink = graph.AddOwnedNode(std::move(sinkTask));
    graph.AddOwnedEdge(source, sink, std::make_unique<TOneToOneConnection>());
    const auto sealed = graph.Seal();
    ASSERT_TRUE(sealed);

    TSingleThreadedScheduler scheduler(sealed);
    std::string error;
    ASSERT_TRUE(scheduler.Run(&error)) << error;
    EXPECT_TRUE(sourcePtr->Finished);
    EXPECT_TRUE(sinkPtr->Consumed());
    EXPECT_TRUE(sinkPtr->Finished);
}

TEST(SchedulerGraph, SingleThreadedSchedulerCannotRunTwice) {
    TTaskGraph graph;
    graph.AddOwnedNode(std::make_unique<TNoopTask>());
    const auto sealed = graph.Seal();
    ASSERT_TRUE(sealed);

    TSingleThreadedScheduler scheduler(sealed);
    std::string error;
    ASSERT_TRUE(scheduler.Run(&error)) << error;
    EXPECT_FALSE(scheduler.Run(&error));
    EXPECT_NE(error.find("cannot run twice"), std::string::npos);
}

} // namespace

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
