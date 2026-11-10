#include <gtest/gtest.h>

#include "runtime_filter_test_util.h"

#include <qdb/plan/types/nullable.h>
#include <qdb/scheduler/channel.h>
#include <qdb/scheduler/connection.h>
#include <qdb/scheduler/connections/local.h>
#include <qdb/scheduler/graph.h>
#include <qdb/scheduler/port.h>
#include <qdb/scheduler/tasks/cte_spool.h>
#include <qdb/scheduler/tasks/runtime_adapter.h>
#include <qdb/scheduler/impl/single_threaded_scheduler.h>
#include <qdb/scheduler/impl/threaded_scheduler.h>

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/parser/type.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace NQdb;
using namespace NQdb::NScheduler;

namespace {

class SchedulerRuntimeFilter : public NTest::TRuntimeFilterTest {};

TEST_F(SchedulerRuntimeFilter, AuxInputWaitsForAllProducers) {
    auto filter = std::make_shared<TRuntimeFilter>(Context_, 16, 1024);
    filter->SetProducerCount(2);
    TTaskNode first;
    TTaskNode second;
    TAuxInput input;
    input.AddFilter(filter);
    input.AddProducer(first);
    input.AddProducer(second);

    EXPECT_EQ(input.Poll(), EAuxInputStatus::NeedProducer);
    first.State.store(ETaskState::Queued);
    EXPECT_EQ(input.Poll(), EAuxInputStatus::NeedProducer);
    second.State.store(ETaskState::Queued);
    EXPECT_EQ(input.Poll(), EAuxInputStatus::Waiting);

    filter->FinishProducer(std::move(filter->MakeBuilder()).TakePartial());
    first.State.store(ETaskState::Finished);
    EXPECT_EQ(input.Poll(), EAuxInputStatus::Waiting);
    filter->FinishProducer(std::move(filter->MakeBuilder()).TakePartial());
    second.State.store(ETaskState::Finished);
    EXPECT_EQ(input.Poll(), EAuxInputStatus::Ready);
}

TEST_F(SchedulerRuntimeFilter, FilterDependencyNotifiesOnlyOnPublication) {
    auto filter = std::make_shared<TRuntimeFilter>(Context_, 16, 1024);
    TTaskNode producer;
    auto code = std::make_shared<TRuntimeFilterCode>();
    code->Probe = filter;
    code->AuxInput = std::make_shared<TAuxInput>();
    code->AuxInput->AddFilter(filter);
    code->AuxInput->AddProducer(producer);
    TRuntimeFilterTask consumer(code, {}, {});
    TTaskEdge dependency{.Kind = ETaskEdgeKind::Dependency};

    EXPECT_FALSE(consumer.TryNotifyDependency());
    EXPECT_TRUE(consumer.WantsInput(dependency));
    producer.State.store(ETaskState::Queued);
    EXPECT_FALSE(consumer.WantsInput(dependency));
    filter->FinishProducer(std::move(filter->MakeBuilder()).TakePartial());
    producer.State.store(ETaskState::Finished);
    EXPECT_TRUE(consumer.TryNotifyDependency());
    EXPECT_FALSE(consumer.TryNotifyDependency());
}

TEST_F(SchedulerRuntimeFilter, MissingFilterPublicationFailsInExecute) {
    auto filter = std::make_shared<TRuntimeFilter>(Context_, 16, 1024);
    TTaskNode producer;
    producer.State.store(ETaskState::Finished);
    auto code = std::make_shared<TRuntimeFilterCode>();
    code->Probe = filter;
    code->AuxInput = std::make_shared<TAuxInput>();
    code->AuxInput->AddFilter(filter);
    code->AuxInput->AddProducer(producer);
    TRuntimeFilterTask consumer(code, {}, {});
    TTaskEdge dependency{.Kind = ETaskEdgeKind::Dependency};

    EXPECT_EQ(code->AuxInput->Poll(), EAuxInputStatus::Failed);
    EXPECT_FALSE(consumer.WantsInput(dependency));
    EXPECT_TRUE(consumer.TryNotifyDependency());
    EXPECT_THROW(consumer.Execute(), std::runtime_error);
}

struct TSourceState {
    int Next = 0;
    int End = 0;
};

struct TUnaryState {
    int Calls = 0;
    int Add = 0;
};

struct TSinkState {
    int Rows = 0;
    int Batches = 0;
};

struct TBinaryState : ITaskState {
    int LeftRows = 0;
    int RightRows = 0;
    bool LeftDone = false;
    bool RightDone = false;
    std::atomic<int>* DestroyCount = nullptr;
};

void CountDestroy(TRowSet* rowSet) {
    auto* counter = static_cast<std::atomic<int>*>(rowSet->Private);
    counter->fetch_add(1, std::memory_order_relaxed);
}

struct TInt64RowSetData {
    std::vector<int64_t> Values;
    std::vector<TColumn> Columns;
    std::atomic<int>* DestroyCount = nullptr;
};

void DestroyInt64RowSet(TRowSet* rowSet) {
    auto* data = static_cast<TInt64RowSetData*>(rowSet->Private);
    data->DestroyCount->fetch_add(1, std::memory_order_relaxed);
    delete data;
}

TRowSet MakeRowSet(int64_t rows, std::atomic<int>* destroyCount) {
    return TRowSet{
        .Columns = nullptr,
        .ColumnCount = 0,
        .RowCount = rows,
        .Selection = nullptr,
        .Destroy = CountDestroy,
        .Private = destroyCount,
        .RefCount = 1,
    };
}

TRowSet MakeInt64RowSet(std::vector<int64_t> values,
    std::atomic<int>* destroyCount)
{
    auto* data = new TInt64RowSetData;
    data->Values = std::move(values);
    data->Columns.push_back(TColumn{
        .Data = reinterpret_cast<char*>(data->Values.data()),
        .Mask = nullptr,
        .Offsets = nullptr,
        .OffsetWidth = 0,
    });
    data->DestroyCount = destroyCount;
    return TRowSet{
        .Columns = data->Columns.data(),
        .ColumnCount = 1,
        .RowCount = static_cast<int64_t>(data->Values.size()),
        .Selection = nullptr,
        .Destroy = DestroyInt64RowSet,
        .Private = data,
        .RefCount = 1,
    };
}

struct TPipeline {
    std::unique_ptr<TTaskGraph> Graph;
    std::shared_ptr<TSourceState> SourceState;
    std::shared_ptr<TUnaryState> FilterState;
    std::shared_ptr<TUnaryState> ProjectState;
    std::shared_ptr<TSinkState> SinkState;
    std::shared_ptr<TSourceCode> SourceCode;
    std::shared_ptr<TUnaryCode> FilterCode;
    std::shared_ptr<TUnaryCode> ProjectCode;
    std::shared_ptr<TSinkCode> SinkCode;
    std::shared_ptr<std::atomic<int>> DestroyCount;
};

TPipeline MakePipeline() {
    TPipeline out;
    out.Graph = std::make_unique<TTaskGraph>();
    out.SourceState = std::make_shared<TSourceState>(TSourceState{
        .Next = 1,
        .End = 4,
    });
    out.FilterState = std::make_shared<TUnaryState>(TUnaryState{
        .Add = 10,
    });
    out.ProjectState = std::make_shared<TUnaryState>(TUnaryState{
        .Add = 100,
    });
    out.SinkState = std::make_shared<TSinkState>();
    out.DestroyCount = std::make_shared<std::atomic<int>>(0);

    out.SourceCode = std::make_shared<TSourceCode>(
        [destroyCount = out.DestroyCount](void* state, TRowSet& rowSet) {
            auto* source = static_cast<TSourceState*>(state);
            if (source->Next > source->End) {
                return ETaskResult::FINISHED;
            }
            rowSet = MakeRowSet(source->Next, destroyCount.get());
            ++source->Next;
            return ETaskResult::OK;
        });
    out.FilterCode = std::make_shared<TUnaryCode>(
        [](void* state, TRowSet& rowSet) {
            auto* unary = static_cast<TUnaryState*>(state);
            ++unary->Calls;
            rowSet.RowCount += unary->Add;
        });
    out.ProjectCode = std::make_shared<TUnaryCode>(
        [](void* state, TRowSet& rowSet) {
            auto* unary = static_cast<TUnaryState*>(state);
            ++unary->Calls;
            rowSet.RowCount += unary->Add;
        });
    out.SinkCode = std::make_shared<TSinkCode>(
        [](void* state, const TRowSet& rowSet) {
            auto* sink = static_cast<TSinkState*>(state);
            sink->Rows += static_cast<int>(rowSet.RowCount);
            ++sink->Batches;
            return ETaskResult::OK;
        });

    auto sourceToFilter = std::make_unique<TOneToOneConnection>();
    auto* sourceToFilterPtr = sourceToFilter.get();
    auto filterToProject = std::make_unique<TOneToOneConnection>();
    auto* filterToProjectPtr = filterToProject.get();
    auto projectToSink = std::make_unique<TOneToOneConnection>();
    auto* projectToSinkPtr = projectToSink.get();

    auto& source = out.Graph->AddOwnedNode(std::make_unique<TSourceTask>(
        out.SourceCode,
        out.SourceState,
        TOutputPort{.Connection = sourceToFilterPtr}));
    auto& filter = out.Graph->AddOwnedNode(std::make_unique<TUnaryTask>(
        out.FilterCode,
        out.FilterState,
        TInputPort{.Connection = sourceToFilterPtr},
        TOutputPort{.Connection = filterToProjectPtr}));
    auto& project = out.Graph->AddOwnedNode(std::make_unique<TUnaryTask>(
        out.ProjectCode,
        out.ProjectState,
        TInputPort{.Connection = filterToProjectPtr},
        TOutputPort{.Connection = projectToSinkPtr}));
    auto& sink = out.Graph->AddOwnedNode(std::make_unique<TSinkTask>(
        out.SinkCode,
        out.SinkState,
        TInputPort{.Connection = projectToSinkPtr}));

    out.Graph->AddOwnedEdge(source, filter, std::move(sourceToFilter));
    out.Graph->AddOwnedEdge(filter, project, std::move(filterToProject));
    out.Graph->AddOwnedEdge(project, sink, std::move(projectToSink));
    EXPECT_TRUE(out.Graph->Seal());
    return out;
}

TEST(SchedulerRuntimeAdapter, RunsSourceFilterProjectSinkSingleThreaded) {
    auto pipeline = MakePipeline();
    TSingleThreadedScheduler scheduler(pipeline.Graph->Seal());

    std::string error;
    ASSERT_TRUE(scheduler.Run(&error)) << error;
    EXPECT_EQ(pipeline.FilterState->Calls, 4);
    EXPECT_EQ(pipeline.ProjectState->Calls, 4);
    EXPECT_EQ(pipeline.SinkState->Batches, 4);
    EXPECT_EQ(pipeline.SinkState->Rows, 450);
    EXPECT_EQ(pipeline.DestroyCount->load(std::memory_order_relaxed), 4);
}

TEST(SchedulerRuntimeAdapter, RunsSourceFilterProjectSinkThreaded) {
    auto pipeline = MakePipeline();
    TThreadedScheduler scheduler(pipeline.Graph->Seal(), 4);

    std::string error;
    ASSERT_TRUE(scheduler.Run(&error)) << error;
    EXPECT_EQ(pipeline.FilterState->Calls, 4);
    EXPECT_EQ(pipeline.ProjectState->Calls, 4);
    EXPECT_EQ(pipeline.SinkState->Batches, 4);
    EXPECT_EQ(pipeline.SinkState->Rows, 450);
    EXPECT_EQ(pipeline.DestroyCount->load(std::memory_order_relaxed), 4);
}

namespace {

struct TWaitingState {
    int Calls = 0;
    int Rows = 0;
    int Batches = 0;
    std::atomic<bool> Waited = false;
};

// Runs source -> sink under the threaded scheduler; `wakeNode` selects which
// task a helper thread schedules once the code reports WAITING.
void RunWaitingGraph(
    std::shared_ptr<TSourceCode> sourceCode,
    std::shared_ptr<TSinkCode> sinkCode,
    std::shared_ptr<TWaitingState> state,
    bool wakeSink,
    TSchedulerRunStats* stats,
    bool wakeByExternalId = false)
{
    TTaskGraph graph;
    auto connection = std::make_unique<TOneToOneConnection>();
    auto* connectionPtr = connection.get();
    auto& source = graph.AddOwnedNode(std::make_unique<TSourceTask>(
        sourceCode, state, TOutputPort{.Connection = connectionPtr}));
    auto& sink = graph.AddOwnedNode(std::make_unique<TSinkTask>(
        sinkCode, state, TInputPort{.Connection = connectionPtr}));
    graph.AddOwnedEdge(source, sink, std::move(connection));
    constexpr uint64_t externalId = 42;
    if (wakeByExternalId) {
        graph.MarkExternal(wakeSink ? sink : source, externalId);
    }
    std::string error;
    const auto sealed = graph.Seal(&error);
    ASSERT_TRUE(sealed) << error;

    TThreadedScheduler scheduler(sealed, 2);
    TSchedulerChannel channel(scheduler);
    TTaskNode* wake = wakeByExternalId ? sealed.ExternalNode(externalId)
        : wakeSink ? &sink : &source;
    ASSERT_NE(wake, nullptr);
    std::thread waker([&] {
        while (!state->Waited.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        channel.Notify(wake);
    });
    const bool ok = scheduler.Run(&error);
    waker.join();
    ASSERT_TRUE(ok) << error;
    *stats = scheduler.Stats();
}

} // namespace

TEST(SchedulerRuntimeAdapter, SourceWaitsForExternalSchedule) {
    auto destroyCount = std::make_shared<std::atomic<int>>(0);
    auto state = std::make_shared<TWaitingState>();
    auto sourceCode = std::make_shared<TSourceCode>(
        [destroyCount](void* raw, TRowSet& rowSet) {
            auto* s = static_cast<TWaitingState*>(raw);
            switch (s->Calls++) {
                case 0:
                    s->Waited.store(true, std::memory_order_release);
                    return ETaskResult::WAITING;
                case 1:
                    rowSet = MakeRowSet(5, destroyCount.get());
                    return ETaskResult::OK;
                default:
                    return ETaskResult::FINISHED;
            }
        });
    auto sinkCode = std::make_shared<TSinkCode>(
        [](void* raw, const TRowSet& rowSet) {
            auto* s = static_cast<TWaitingState*>(raw);
            s->Rows += static_cast<int>(rowSet.RowCount);
            ++s->Batches;
            return ETaskResult::OK;
        });

    TSchedulerRunStats stats;
    RunWaitingGraph(sourceCode, sinkCode, state, false, &stats);
    EXPECT_EQ(state->Calls, 3);
    EXPECT_EQ(state->Batches, 1);
    EXPECT_EQ(state->Rows, 5);
    EXPECT_EQ(stats.Waiting, 1u);
    EXPECT_EQ(destroyCount->load(std::memory_order_relaxed), 1);
}

TEST(SchedulerRuntimeAdapter, SourceWokenByExternalIdThroughChannel) {
    auto destroyCount = std::make_shared<std::atomic<int>>(0);
    auto state = std::make_shared<TWaitingState>();
    auto sourceCode = std::make_shared<TSourceCode>(
        [destroyCount](void* raw, TRowSet& rowSet) {
            auto* s = static_cast<TWaitingState*>(raw);
            switch (s->Calls++) {
                case 0:
                    s->Waited.store(true, std::memory_order_release);
                    return ETaskResult::WAITING;
                case 1:
                    rowSet = MakeRowSet(5, destroyCount.get());
                    return ETaskResult::OK;
                default:
                    return ETaskResult::FINISHED;
            }
        });
    auto sinkCode = std::make_shared<TSinkCode>(
        [](void* raw, const TRowSet& rowSet) {
            auto* s = static_cast<TWaitingState*>(raw);
            s->Rows += static_cast<int>(rowSet.RowCount);
            ++s->Batches;
            return ETaskResult::OK;
        });

    TSchedulerRunStats stats;
    RunWaitingGraph(sourceCode, sinkCode, state, false, &stats, true);
    EXPECT_EQ(state->Calls, 3);
    EXPECT_EQ(state->Rows, 5);
    EXPECT_EQ(stats.Waiting, 1u);
}

TEST(SchedulerRuntimeAdapter, SinkRetriesPendingRowSetAfterWaiting) {
    auto destroyCount = std::make_shared<std::atomic<int>>(0);
    auto state = std::make_shared<TWaitingState>();
    auto sourceCode = std::make_shared<TSourceCode>(
        [destroyCount](void* raw, TRowSet& rowSet) {
            auto* s = static_cast<TWaitingState*>(raw);
            if (s->Calls >= 2) {
                return ETaskResult::FINISHED;
            }
            rowSet = MakeRowSet(++s->Calls, destroyCount.get());
            return ETaskResult::OK;
        });
    // Rejects the first row set once; destroyCount stays 0 until it is written.
    auto attempts = std::make_shared<int>(0);
    auto sinkCode = std::make_shared<TSinkCode>(
        [attempts, destroyCount](void* raw, const TRowSet& rowSet) {
            auto* s = static_cast<TWaitingState*>(raw);
            if ((*attempts)++ == 0) {
                EXPECT_EQ(rowSet.RowCount, 1);
                s->Waited.store(true, std::memory_order_release);
                return ETaskResult::WAITING;
            }
            if (*attempts == 2) {
                EXPECT_EQ(rowSet.RowCount, 1);
                EXPECT_EQ(destroyCount->load(std::memory_order_relaxed), 0);
            }
            s->Rows += static_cast<int>(rowSet.RowCount);
            ++s->Batches;
            return ETaskResult::OK;
        });

    TSchedulerRunStats stats;
    RunWaitingGraph(sourceCode, sinkCode, state, true, &stats);
    EXPECT_EQ(*attempts, 3);
    EXPECT_EQ(state->Batches, 2);
    EXPECT_EQ(state->Rows, 3);
    EXPECT_EQ(stats.Waiting, 1u);
    EXPECT_EQ(destroyCount->load(std::memory_order_relaxed), 2);
}

TEST(SchedulerRuntimeAdapter, SinkRetriesFinishAfterWaiting) {
    auto destroyCount = std::make_shared<std::atomic<int>>(0);
    auto state = std::make_shared<TWaitingState>();
    auto sourceCode = std::make_shared<TSourceCode>(
        [destroyCount](void* raw, TRowSet& rowSet) {
            auto* s = static_cast<TWaitingState*>(raw);
            if (s->Calls >= 2) {
                return ETaskResult::FINISHED;
            }
            rowSet = MakeRowSet(++s->Calls, destroyCount.get());
            return ETaskResult::OK;
        });
    auto finishes = std::make_shared<int>(0);
    auto sinkCode = std::make_shared<TSinkCode>(
        [](void* raw, const TRowSet& rowSet) {
            static_cast<TWaitingState*>(raw)->Rows += static_cast<int>(rowSet.RowCount);
            return ETaskResult::OK;
        },
        [finishes](void* raw) {
            auto* s = static_cast<TWaitingState*>(raw);
            EXPECT_EQ(s->Rows, 3);
            if ((*finishes)++ == 0) {
                s->Waited.store(true, std::memory_order_release);
                return ETaskResult::WAITING;
            }
            return ETaskResult::OK;
        });

    TSchedulerRunStats stats;
    RunWaitingGraph(sourceCode, sinkCode, state, true, &stats);
    EXPECT_EQ(*finishes, 2);
    EXPECT_EQ(stats.Waiting, 1u);
    EXPECT_EQ(destroyCount->load(std::memory_order_relaxed), 2);
}

TEST(SchedulerRuntimeAdapter, SingleThreadedSchedulerRejectsExternalWait) {
    auto state = std::make_shared<TWaitingState>();
    auto sourceCode = std::make_shared<TSourceCode>(
        [](void*, TRowSet&) {
            return ETaskResult::WAITING;
        });
    auto sinkCode = std::make_shared<TSinkCode>(
        [](void*, const TRowSet&) {
            return ETaskResult::OK;
        });
    TTaskGraph graph;
    auto connection = std::make_unique<TOneToOneConnection>();
    auto* connectionPtr = connection.get();
    auto& source = graph.AddOwnedNode(std::make_unique<TSourceTask>(
        sourceCode, state, TOutputPort{.Connection = connectionPtr}));
    auto& sink = graph.AddOwnedNode(std::make_unique<TSinkTask>(
        sinkCode, state, TInputPort{.Connection = connectionPtr}));
    graph.AddOwnedEdge(source, sink, std::move(connection));
    std::string error;
    const auto sealed = graph.Seal(&error);
    ASSERT_TRUE(sealed) << error;

    TSingleThreadedScheduler scheduler(sealed);
    EXPECT_FALSE(scheduler.Run(&error));
    EXPECT_EQ(error, "single-threaded scheduler does not support external waits");
}

TEST(SchedulerRuntimeAdapter, ThreadedRunCompletesWithAbandonedWaitingBranch) {
    auto destroyCount = std::make_shared<std::atomic<int>>(0);
    auto waitState = std::make_shared<TWaitingState>();
    auto leftState = std::make_shared<TSourceState>(TSourceState{.Next = 1, .End = 1});
    auto binaryState = std::make_shared<TBinaryState>();
    binaryState->DestroyCount = destroyCount.get();
    auto sinkState = std::make_shared<TSinkState>();

    // The left source holds its batch until the right one has reported WAITING,
    // so the abandoned branch is really waiting when the root finishes.
    auto leftCode = std::make_shared<TSourceCode>(
        [waitState, destroyCount](void* raw, TRowSet& rowSet) {
            auto* source = static_cast<TSourceState*>(raw);
            if (source->Next > source->End) {
                return ETaskResult::FINISHED;
            }
            while (!waitState->Waited.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            rowSet = MakeRowSet(source->Next++, destroyCount.get());
            return ETaskResult::OK;
        });
    auto rightCode = std::make_shared<TSourceCode>(
        [](void* raw, TRowSet&) {
            static_cast<TWaitingState*>(raw)->Waited.store(true, std::memory_order_release);
            return ETaskResult::WAITING;
        });
    // Emits one row set after the left input and finishes without reading the right.
    auto binaryCode = std::make_shared<TBinaryBlockingCode>(
        [](ITaskState& state, TInputPort& left, TInputPort&, TRowSet& output) {
            auto& binary = static_cast<TBinaryState&>(state);
            if (binary.LeftDone) {
                return ETaskResult::FINISHED;
            }
            TRowSet rowSet{};
            const auto fetch = left.Fetch(rowSet);
            if (fetch == EFetchResult::NO_DATA) {
                return ETaskResult::NEED_DATA;
            }
            binary.LeftDone = true;
            if (fetch == EFetchResult::FINISHED) {
                return ETaskResult::FINISHED;
            }
            output = rowSet;
            return ETaskResult::OK;
        });
    auto sinkCode = std::make_shared<TSinkCode>(
        [](void* raw, const TRowSet& rowSet) {
            auto* sink = static_cast<TSinkState*>(raw);
            sink->Rows += static_cast<int>(rowSet.RowCount);
            ++sink->Batches;
            return ETaskResult::OK;
        });

    TTaskGraph graph;
    auto leftToBinary = std::make_unique<TOneToOneConnection>();
    auto* leftToBinaryPtr = leftToBinary.get();
    auto rightToBinary = std::make_unique<TOneToOneConnection>();
    auto* rightToBinaryPtr = rightToBinary.get();
    auto binaryToSink = std::make_unique<TOneToOneConnection>();
    auto* binaryToSinkPtr = binaryToSink.get();
    auto& left = graph.AddOwnedNode(std::make_unique<TSourceTask>(
        leftCode, leftState, TOutputPort{.Connection = leftToBinaryPtr}));
    auto& right = graph.AddOwnedNode(std::make_unique<TSourceTask>(
        rightCode, waitState, TOutputPort{.Connection = rightToBinaryPtr}));
    auto& binary = graph.AddOwnedNode(std::make_unique<TBinaryBlockingTask>(
        binaryCode,
        binaryState,
        TInputPort{.Connection = leftToBinaryPtr},
        TInputPort{.Connection = rightToBinaryPtr},
        TOutputPort{.Connection = binaryToSinkPtr}));
    auto& sink = graph.AddOwnedNode(std::make_unique<TSinkTask>(
        sinkCode, sinkState, TInputPort{.Connection = binaryToSinkPtr}));
    graph.AddOwnedEdge(left, binary, std::move(leftToBinary));
    graph.AddOwnedEdge(right, binary, std::move(rightToBinary));
    graph.AddOwnedEdge(binary, sink, std::move(binaryToSink));
    std::string error;
    const auto sealed = graph.Seal(&error);
    ASSERT_TRUE(sealed) << error;

    // A regression would hang Run(); the watchdog turns the hang into a failure.
    std::atomic<bool> finished = false;
    std::thread watchdog([&] {
        for (int i = 0; i < 300 && !finished.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!finished.load()) {
            ADD_FAILURE() << "Run() hung with an abandoned waiting branch";
            std::abort();
        }
    });
    TThreadedScheduler scheduler(sealed, 2);
    const bool ok = scheduler.Run(&error);
    finished.store(true);
    watchdog.join();
    ASSERT_TRUE(ok) << error;
    EXPECT_TRUE(waitState->Waited.load());
    EXPECT_EQ(sinkState->Batches, 1);
    EXPECT_EQ(scheduler.Stats().Waiting, 1u);
}

namespace {

// One graph with several roots built from counting sources and sinks.
struct TMultiRootGraph {
    // Outlives Graph: its connections release queued row sets at teardown.
    std::shared_ptr<std::atomic<int>> DestroyCount = std::make_shared<std::atomic<int>>(0);
    std::unique_ptr<TTaskGraph> Graph = std::make_unique<TTaskGraph>();

    IConnection& Connect(std::unique_ptr<IConnection> connection) {
        return Graph->AddConnection(std::move(connection));
    }

    // Batch i has i rows; a negative count never finishes.
    TTaskNode& AddSource(IConnection& output, int batches, bool throws = false) {
        auto code = std::make_shared<TSourceCode>(
            [destroyCount = DestroyCount, throws](void* raw, TRowSet& rowSet) {
                if (throws) {
                    throw std::runtime_error("expected task failure");
                }
                auto* source = static_cast<TSourceState*>(raw);
                if (source->End >= 0 && source->Next > source->End) {
                    return ETaskResult::FINISHED;
                }
                rowSet = MakeRowSet(source->Next++, destroyCount.get());
                return ETaskResult::OK;
            });
        return Graph->AddOwnedNode(std::make_unique<TSourceTask>(
            code,
            std::make_shared<TSourceState>(TSourceState{.Next = 1, .End = batches}),
            TOutputPort{.Connection = &output}));
    }

    // Passes the first `limit` batches through, then finishes.
    TTaskNode& AddLimit(TTaskNode& producer, IConnection& input, size_t lane, IConnection& output, int limit) {
        struct TLimitState : ITaskState {
            int Batches = 0;
        };
        auto code = std::make_shared<TBlockingCode>(
            [limit](ITaskState& raw, TInputPort& in, TRowSet& out) {
                auto& state = static_cast<TLimitState&>(raw);
                if (state.Batches == limit) {
                    return ETaskResult::FINISHED;
                }
                const auto fetch = in.Fetch(out);
                if (fetch == EFetchResult::NO_DATA) {
                    return ETaskResult::NEED_DATA;
                }
                if (fetch == EFetchResult::FINISHED) {
                    return ETaskResult::FINISHED;
                }
                ++state.Batches;
                return ETaskResult::OK;
            });
        auto& node = Graph->AddOwnedNode(std::make_unique<TBlockingTask>(
            code,
            std::make_shared<TLimitState>(),
            TInputPort{.Connection = &input, .Lane = lane},
            TOutputPort{.Connection = &output}));
        Graph->AddEdge(producer, node, input, 0, lane);
        return node;
    }

    std::shared_ptr<TSinkState> AddSink(TTaskNode& producer, IConnection& input, size_t lane = 0) {
        auto state = std::make_shared<TSinkState>();
        auto code = std::make_shared<TSinkCode>(
            [](void* raw, const TRowSet& rowSet) {
                auto* sink = static_cast<TSinkState*>(raw);
                sink->Rows += static_cast<int>(rowSet.RowCount);
                ++sink->Batches;
                return ETaskResult::OK;
            });
        auto& node = Graph->AddOwnedNode(std::make_unique<TSinkTask>(
            code, state, TInputPort{.Connection = &input, .Lane = lane}));
        Graph->AddEdge(producer, node, input, 0, lane);
        return state;
    }

    // No workers selects the single-threaded scheduler.
    bool Run(size_t workers, std::string& error) {
        const auto sealed = Graph->Seal(&error);
        if (!sealed) {
            return false;
        }
        if (workers == 0) {
            return TSingleThreadedScheduler(sealed).Run(&error);
        }
        return TThreadedScheduler(sealed, workers).Run(&error);
    }
};

constexpr size_t SchedulerWorkers[] = {0, 1, 4};

} // namespace

TEST(SchedulerMultiRoot, RunsTwoIndependentChains) {
    for (size_t workers : SchedulerWorkers) {
        SCOPED_TRACE(workers);
        TMultiRootGraph graph;
        auto& first = graph.Connect(std::make_unique<TOneToOneConnection>());
        auto& second = graph.Connect(std::make_unique<TOneToOneConnection>());
        const auto firstSink = graph.AddSink(graph.AddSource(first, 3), first);
        const auto secondSink = graph.AddSink(graph.AddSource(second, 5), second);
        std::string error;
        ASSERT_TRUE(graph.Run(workers, error)) << error;
        EXPECT_EQ(firstSink->Rows, 6);
        EXPECT_EQ(secondSink->Rows, 15);
        EXPECT_EQ(graph.DestroyCount->load(), 8);
    }
}

TEST(SchedulerMultiRoot, RootsSharingAnUpstreamGetEveryRow) {
    for (size_t workers : SchedulerWorkers) {
        SCOPED_TRACE(workers);
        TMultiRootGraph graph;
        auto& broadcast = graph.Connect(std::make_unique<TBroadcastConnection>());
        broadcast.Resize(1, 2);
        auto& source = graph.AddSource(broadcast, 5);
        const auto left = graph.AddSink(source, broadcast, 0);
        const auto right = graph.AddSink(source, broadcast, 1);
        std::string error;
        ASSERT_TRUE(graph.Run(workers, error)) << error;
        EXPECT_EQ(left->Rows, 15);
        EXPECT_EQ(right->Rows, 15);
        EXPECT_EQ(graph.DestroyCount->load(), 5);
    }
}

// Two LIMIT 2 roots finish first. One shares its source with a root that still
// reads every batch; the other leaves its endless source blocked for good.
TEST(SchedulerMultiRoot, LimitRootsFinishWhileAnotherRootKeepsReading) {
    constexpr int batches = 20;
    for (size_t workers : SchedulerWorkers) {
        SCOPED_TRACE(workers);
        TMultiRootGraph graph;
        // Holds every batch: a finished consumer cannot block the shared source.
        auto& broadcast = graph.Connect(std::make_unique<TBroadcastConnection>(batches));
        broadcast.Resize(1, 2);
        auto& sharedLimited = graph.Connect(std::make_unique<TOneToOneConnection>());
        auto& endless = graph.Connect(std::make_unique<TOneToOneConnection>());
        auto& endlessLimited = graph.Connect(std::make_unique<TOneToOneConnection>());

        auto& shared = graph.AddSource(broadcast, batches);
        const auto sharedLimit = graph.AddSink(
            graph.AddLimit(shared, broadcast, 0, sharedLimited, 2), sharedLimited);
        const auto full = graph.AddSink(shared, broadcast, 1);
        auto& endlessSource = graph.AddSource(endless, -1);
        const auto endlessLimit = graph.AddSink(
            graph.AddLimit(endlessSource, endless, 0, endlessLimited, 2), endlessLimited);

        std::string error;
        ASSERT_TRUE(graph.Run(workers, error)) << error;
        EXPECT_EQ(sharedLimit->Rows, 3);
        EXPECT_EQ(full->Rows, batches * (batches + 1) / 2);
        EXPECT_EQ(endlessLimit->Rows, 3);
        EXPECT_NE(endlessSource.State.load(), ETaskState::Finished);
    }
}

TEST(SchedulerMultiRoot, ErrorInOneBranchFailsTheRun) {
    for (size_t workers : SchedulerWorkers) {
        SCOPED_TRACE(workers);
        TMultiRootGraph graph;
        auto& endless = graph.Connect(std::make_unique<TOneToOneConnection>());
        auto& failing = graph.Connect(std::make_unique<TOneToOneConnection>());
        graph.AddSink(graph.AddSource(endless, -1), endless);
        graph.AddSink(graph.AddSource(failing, 1, true), failing);
        std::string error;
        EXPECT_FALSE(graph.Run(workers, error));
        EXPECT_EQ(error, "expected task failure");
    }
}

TEST(SchedulerRuntimeAdapter, PartitionTasksShareCodeButOwnState) {
    auto code = std::make_shared<TUnaryCode>(
        [](void* state, TRowSet& rowSet) {
            auto* unary = static_cast<TUnaryState*>(state);
            ++unary->Calls;
            rowSet.RowCount += unary->Add;
        });
    auto leftState = std::make_shared<TUnaryState>(TUnaryState{
        .Add = 1,
    });
    auto rightState = std::make_shared<TUnaryState>(TUnaryState{
        .Add = 2,
    });

    TUnaryTask left(
        code,
        leftState,
        TInputPort{},
        TOutputPort{});
    TUnaryTask right(
        code,
        rightState,
        TInputPort{},
        TOutputPort{});

    EXPECT_EQ(left.Code().get(), right.Code().get());
    EXPECT_NE(left.State().get(), right.State().get());
    EXPECT_EQ(code.use_count(), 3);
}

TEST(SchedulerRuntimeAdapter, RunsBinaryBlockingTask) {
    auto graph = std::make_unique<TTaskGraph>();
    auto destroyCount = std::make_shared<std::atomic<int>>(0);
    auto leftState = std::make_shared<TSourceState>(TSourceState{
        .Next = 1,
        .End = 2,
    });
    auto rightState = std::make_shared<TSourceState>(TSourceState{
        .Next = 10,
        .End = 11,
    });
    auto binaryState = std::make_shared<TBinaryState>();
    binaryState->DestroyCount = destroyCount.get();
    auto sinkState = std::make_shared<TSinkState>();

    auto sourceCode = std::make_shared<TSourceCode>(
        [destroyCount](void* state, TRowSet& rowSet) {
            auto* source = static_cast<TSourceState*>(state);
            if (source->Next > source->End) {
                return ETaskResult::FINISHED;
            }
            rowSet = MakeRowSet(source->Next, destroyCount.get());
            ++source->Next;
            return ETaskResult::OK;
        });
    auto binaryCode = std::make_shared<TBinaryBlockingCode>(
        [](ITaskState& state, TInputPort& left, TInputPort& right, TRowSet& output) {
            auto* binary = &static_cast<TBinaryState&>(state);
            TRowSet rowSet{};
            while (!binary->LeftDone) {
                auto fetch = left.Fetch(rowSet);
                if (fetch == EFetchResult::NO_DATA) {
                    break;
                }
                if (fetch == EFetchResult::FINISHED) {
                    binary->LeftDone = true;
                    break;
                }
                binary->LeftRows += static_cast<int>(rowSet.RowCount);
                Release(&rowSet);
                rowSet = {};
            }
            while (!binary->RightDone) {
                auto fetch = right.Fetch(rowSet);
                if (fetch == EFetchResult::NO_DATA) {
                    break;
                }
                if (fetch == EFetchResult::FINISHED) {
                    binary->RightDone = true;
                    break;
                }
                binary->RightRows += static_cast<int>(rowSet.RowCount);
                Release(&rowSet);
                rowSet = {};
            }
            if (!binary->LeftDone || !binary->RightDone) {
                return ETaskResult::NEED_DATA;
            }
            if (binary->DestroyCount == nullptr) {
                return ETaskResult::FINISHED;
            }
            output = MakeRowSet(
                binary->LeftRows * 100 + binary->RightRows,
                binary->DestroyCount);
            binary->DestroyCount = nullptr;
            return ETaskResult::OK;
        });
    auto sinkCode = std::make_shared<TSinkCode>(
        [](void* state, const TRowSet& rowSet) {
            auto* sink = static_cast<TSinkState*>(state);
            sink->Rows += static_cast<int>(rowSet.RowCount);
            ++sink->Batches;
            return ETaskResult::OK;
        });

    auto leftToBinary = std::make_unique<TOneToOneConnection>();
    auto* leftToBinaryPtr = leftToBinary.get();
    auto rightToBinary = std::make_unique<TOneToOneConnection>();
    auto* rightToBinaryPtr = rightToBinary.get();
    auto binaryToSink = std::make_unique<TOneToOneConnection>();
    auto* binaryToSinkPtr = binaryToSink.get();

    auto& left = graph->AddOwnedNode(std::make_unique<TSourceTask>(
        sourceCode,
        leftState,
        TOutputPort{.Connection = leftToBinaryPtr}));
    auto& right = graph->AddOwnedNode(std::make_unique<TSourceTask>(
        sourceCode,
        rightState,
        TOutputPort{.Connection = rightToBinaryPtr}));
    auto& binary = graph->AddOwnedNode(std::make_unique<TBinaryBlockingTask>(
        binaryCode,
        binaryState,
        TInputPort{.Connection = leftToBinaryPtr},
        TInputPort{.Connection = rightToBinaryPtr},
        TOutputPort{.Connection = binaryToSinkPtr}));
    auto& sink = graph->AddOwnedNode(std::make_unique<TSinkTask>(
        sinkCode,
        sinkState,
        TInputPort{.Connection = binaryToSinkPtr}));

    graph->AddOwnedEdge(left, binary, std::move(leftToBinary));
    graph->AddOwnedEdge(right, binary, std::move(rightToBinary));
    graph->AddOwnedEdge(binary, sink, std::move(binaryToSink));
    std::string error;
    const auto sealed = graph->Seal(&error);
    ASSERT_TRUE(sealed) << error;
    TThreadedScheduler scheduler(sealed, 3);
    ASSERT_TRUE(scheduler.Run(&error)) << error;
    EXPECT_EQ(binaryState->LeftRows, 3);
    EXPECT_EQ(binaryState->RightRows, 21);
    EXPECT_EQ(sinkState->Batches, 1);
    EXPECT_EQ(sinkState->Rows, 321);
    EXPECT_EQ(destroyCount->load(std::memory_order_relaxed), 5);
}

namespace {

struct TGateResult {
    bool ProbePulledEarly = false;
    int RightRows = 0;
    int LeftRows = 0;
    int SinkRows = 0;
    int SinkBatches = 0;
};

// Runs a graph: build (right) source -> binary <- probe (left) source -> sink,
// where the binary consumes the build side fully before the probe. With `gate`,
// the binary reports RequiredSide so the scheduler defers the probe producer
// until build EOF; without it the probe is prefetched. The probe source records
// whether it was ever pulled before the build finished.
TGateResult RunAsymmetricGateGraph(bool gate) {
    auto graph = std::make_unique<TTaskGraph>();
    auto destroy = std::make_shared<std::atomic<int>>(0);
    auto buildDone = std::make_shared<std::atomic<bool>>(false);
    auto probeEarly = std::make_shared<std::atomic<bool>>(false);

    auto buildState = std::make_shared<TSourceState>(TSourceState{.Next = 1, .End = 3});
    auto buildCode = std::make_shared<TSourceCode>(
        [buildDone, destroy](void* state, TRowSet& rowSet) {
            auto* s = static_cast<TSourceState*>(state);
            if (s->Next > s->End) {
                buildDone->store(true, std::memory_order_relaxed);
                return ETaskResult::FINISHED;
            }
            rowSet = MakeRowSet(1, destroy.get());
            ++s->Next;
            return ETaskResult::OK;
        });

    auto probeState = std::make_shared<TSourceState>(TSourceState{.Next = 1, .End = 2});
    auto probeCode = std::make_shared<TSourceCode>(
        [buildDone, probeEarly, destroy](void* state, TRowSet& rowSet) {
            if (!buildDone->load(std::memory_order_relaxed)) {
                probeEarly->store(true, std::memory_order_relaxed);
            }
            auto* s = static_cast<TSourceState*>(state);
            if (s->Next > s->End) {
                return ETaskResult::FINISHED;
            }
            rowSet = MakeRowSet(1, destroy.get());
            ++s->Next;
            return ETaskResult::OK;
        });

    auto binaryState = std::make_shared<TBinaryState>();
    binaryState->DestroyCount = destroy.get();
    auto binaryCode = std::make_shared<TBinaryBlockingCode>(
        [](ITaskState& state, TInputPort& left, TInputPort& right, TRowSet& output) {
            auto* b = &static_cast<TBinaryState&>(state);
            TRowSet rowSet{};
            while (!b->RightDone) { // build side first
                auto fetch = right.Fetch(rowSet);
                if (fetch == EFetchResult::NO_DATA) return ETaskResult::NEED_DATA;
                if (fetch == EFetchResult::FINISHED) { b->RightDone = true; break; }
                ++b->RightRows;
                Release(&rowSet);
                rowSet = {};
            }
            while (!b->LeftDone) { // probe side after build EOF
                auto fetch = left.Fetch(rowSet);
                if (fetch == EFetchResult::NO_DATA) return ETaskResult::NEED_DATA;
                if (fetch == EFetchResult::FINISHED) { b->LeftDone = true; break; }
                ++b->LeftRows;
                Release(&rowSet);
                rowSet = {};
            }
            if (b->DestroyCount == nullptr) {
                return ETaskResult::FINISHED;
            }
            output = MakeRowSet(b->LeftRows * 100 + b->RightRows, b->DestroyCount);
            b->DestroyCount = nullptr;
            return ETaskResult::OK;
        });
    if (gate) {
        binaryCode->RequiredSide = [](ITaskState& state) {
            auto* b = &static_cast<TBinaryState&>(state);
            return b->RightDone ? ERequiredSide::Left : ERequiredSide::Right;
        };
    }

    auto sinkState = std::make_shared<TSinkState>();
    auto sinkCode = std::make_shared<TSinkCode>(
        [](void* state, const TRowSet& rowSet) {
            auto* sink = static_cast<TSinkState*>(state);
            sink->Rows += static_cast<int>(rowSet.RowCount);
            ++sink->Batches;
            return ETaskResult::OK;
        });

    auto leftToBinary = std::make_unique<TOneToOneConnection>();
    auto* leftPtr = leftToBinary.get();
    auto rightToBinary = std::make_unique<TOneToOneConnection>();
    auto* rightPtr = rightToBinary.get();
    auto binaryToSink = std::make_unique<TOneToOneConnection>();
    auto* sinkPtr = binaryToSink.get();

    auto& probe = graph->AddOwnedNode(std::make_unique<TSourceTask>(
        probeCode, probeState, TOutputPort{.Connection = leftPtr}));
    auto& build = graph->AddOwnedNode(std::make_unique<TSourceTask>(
        buildCode, buildState, TOutputPort{.Connection = rightPtr}));
    auto& binary = graph->AddOwnedNode(std::make_unique<TBinaryBlockingTask>(
        binaryCode, binaryState,
        TInputPort{.Connection = leftPtr},
        TInputPort{.Connection = rightPtr},
        TOutputPort{.Connection = sinkPtr}));
    auto& sink = graph->AddOwnedNode(std::make_unique<TSinkTask>(
        sinkCode, sinkState, TInputPort{.Connection = sinkPtr}));

    graph->AddOwnedEdge(probe, binary, std::move(leftToBinary));
    graph->AddOwnedEdge(build, binary, std::move(rightToBinary));
    graph->AddOwnedEdge(binary, sink, std::move(binaryToSink));
    std::string error;
    const auto sealed = graph->Seal(&error);
    EXPECT_TRUE(sealed) << error;
    TSingleThreadedScheduler scheduler(sealed);
    EXPECT_TRUE(scheduler.Run(&error)) << error;

    return {
        .ProbePulledEarly = probeEarly->load(std::memory_order_relaxed),
        .RightRows = binaryState->RightRows,
        .LeftRows = binaryState->LeftRows,
        .SinkRows = sinkState->Rows,
        .SinkBatches = sinkState->Batches,
    };
}

} // namespace

TEST(SchedulerRuntimeAdapter, RequiredSideDefersProbeUntilBuildDone) {
    auto r = RunAsymmetricGateGraph(/*gate=*/true);
    EXPECT_FALSE(r.ProbePulledEarly);
    EXPECT_EQ(r.RightRows, 3);
    EXPECT_EQ(r.LeftRows, 2);
    EXPECT_EQ(r.SinkBatches, 1);
    EXPECT_EQ(r.SinkRows, 203); // 2*100 + 3
}

TEST(SchedulerRuntimeAdapter, WithoutRequiredSideProbeIsPrefetched) {
    // Same graph without the gate: the probe producer is scheduled alongside the
    // build side and runs before build EOF. Guards the gate test from being vacuous.
    auto r = RunAsymmetricGateGraph(/*gate=*/false);
    EXPECT_TRUE(r.ProbePulledEarly);
    EXPECT_EQ(r.SinkRows, 203); // result identical either way
}

// The outputs of one shuffle source lane: a single-source gather per
// destination partition.
struct TShuffleOutputs {
    TShuffleOutputs(size_t capacity, size_t partitions)
    {
        for (size_t i = 0; i < partitions; ++i) {
            Partitions.push_back(std::make_unique<TGatherConnection>(capacity));
        }
    }

    std::vector<TOutputPort> Ports() const
    {
        std::vector<TOutputPort> ports;
        for (const auto& partition : Partitions) {
            ports.push_back(TOutputPort{.Connection = partition.get()});
        }
        return ports;
    }

    TGatherConnection& operator[](size_t partition)
    {
        return *Partitions[partition];
    }

    std::vector<std::unique_ptr<TGatherConnection>> Partitions;
};

TEST(SchedulerRuntimeAdapter, HashShuffleTaskScattersSelectedRows) {
    std::atomic<int> destroyCount = 0;
    TOneToOneConnection input(1);
    TShuffleOutputs output(2, 2);

    std::vector<uint8_t> selection = {0xff, 0, 0xff, 0xff};
    std::vector<uint64_t> precomputed = {0, 1, 1, 0};
    auto rowSet = MakeRowSet(4, &destroyCount);
    rowSet.Selection = selection.data();
    rowSet.Hash = precomputed.data();
    ASSERT_TRUE(input.Push(0, std::move(rowSet)));
    input.Finish(0);

    auto code = std::make_shared<THashShuffleCode>(
        [](TRowSet*, uint64_t*) {
            ADD_FAILURE() << "shuffle recomputed input hashes";
            return false;
        });
    code->UseInputHash = true;
    THashShuffleTask task(
        code,
        TInputPort{.Connection = &input},
        output.Ports());

    EXPECT_EQ(task.Execute(), ETaskResult::OK);

    TRowSet left{};
    ASSERT_EQ(output[0].Fetch(0, left), EFetchResult::OK);
    EXPECT_EQ(left.Hash, precomputed.data());
    ASSERT_NE(left.Selection, nullptr);
    EXPECT_EQ(
        std::vector<uint8_t>(left.Selection, left.Selection + left.RowCount),
        (std::vector<uint8_t>{0xff, 0, 0, 0xff}));
    Release(&left);

    TRowSet right{};
    ASSERT_EQ(output[1].Fetch(0, right), EFetchResult::OK);
    EXPECT_EQ(right.Hash, precomputed.data());
    ASSERT_NE(right.Selection, nullptr);
    EXPECT_EQ(
        std::vector<uint8_t>(right.Selection, right.Selection + right.RowCount),
        (std::vector<uint8_t>{0, 0, 0xff, 0}));
    Release(&right);

    EXPECT_EQ(task.Execute(), ETaskResult::FINISHED);
    EXPECT_EQ(output[0].Fetch(0, left), EFetchResult::FINISHED);
    EXPECT_EQ(output[1].Fetch(0, right), EFetchResult::FINISHED);
    EXPECT_EQ(destroyCount.load(std::memory_order_relaxed), 1);
}

TEST(SchedulerRuntimeAdapter, HashShuffleTaskBatchesRowsUntilFinish) {
    using namespace NQumir::NAst;

    std::atomic<int> destroyCount = 0;
    TOneToOneConnection input(2);
    TShuffleOutputs output(4, 2);

    ASSERT_TRUE(input.Push(0, MakeInt64RowSet({0, 1}, &destroyCount)));
    ASSERT_TRUE(input.Push(0, MakeInt64RowSet({2, 3}, &destroyCount)));
    input.Finish(0);

    auto schema = std::make_shared<TStructType>(
        std::vector<std::pair<std::string, TTypePtr>>{
            {"value", std::make_shared<TIntegerType>()},
        });
    auto code = std::make_shared<THashShuffleCode>(
        [](TRowSet* batch, uint64_t* hashes) {
            auto* values = reinterpret_cast<int64_t*>(batch->Columns[0].Data);
            for (int64_t row = 0; row < batch->RowCount; ++row) {
                hashes[row] = static_cast<uint64_t>(values[row]);
            }
            return true;
        },
        schema,
        16,
        64,
        1024 * 1024);
    THashShuffleTask task(
        code,
        TInputPort{.Connection = &input},
        output.Ports());

    EXPECT_EQ(task.Execute(), ETaskResult::OK);
    TRowSet left{};
    EXPECT_EQ(output[0].Fetch(0, left), EFetchResult::NO_DATA);

    EXPECT_EQ(task.Execute(), ETaskResult::OK);
    EXPECT_EQ(output[0].Fetch(0, left), EFetchResult::NO_DATA);

    EXPECT_EQ(task.Execute(), ETaskResult::FINISHED);

    ASSERT_EQ(output[0].Fetch(0, left), EFetchResult::OK);
    ASSERT_EQ(left.RowCount, 2);
    ASSERT_EQ(left.Selection, nullptr);
    auto* leftValues = reinterpret_cast<int64_t*>(left.Columns[0].Data);
    EXPECT_EQ(std::vector<int64_t>(leftValues, leftValues + left.RowCount),
        (std::vector<int64_t>{0, 2}));
    ASSERT_NE(left.Hash, nullptr);
    EXPECT_EQ(std::vector<uint64_t>(left.Hash, left.Hash + left.RowCount),
        (std::vector<uint64_t>{0, 2}));
    Release(&left);

    TRowSet right{};
    ASSERT_EQ(output[1].Fetch(0, right), EFetchResult::OK);
    ASSERT_EQ(right.RowCount, 2);
    ASSERT_EQ(right.Selection, nullptr);
    auto* rightValues = reinterpret_cast<int64_t*>(right.Columns[0].Data);
    EXPECT_EQ(std::vector<int64_t>(rightValues, rightValues + right.RowCount),
        (std::vector<int64_t>{1, 3}));
    ASSERT_NE(right.Hash, nullptr);
    EXPECT_EQ(std::vector<uint64_t>(right.Hash, right.Hash + right.RowCount),
        (std::vector<uint64_t>{1, 3}));
    Release(&right);

    EXPECT_EQ(output[0].Fetch(0, left), EFetchResult::FINISHED);
    EXPECT_EQ(output[1].Fetch(0, right), EFetchResult::FINISHED);
    EXPECT_EQ(destroyCount.load(std::memory_order_relaxed), 2);
}

TEST_F(SchedulerRuntimeFilter, PublishesRuntimeFilterBeforeBufferedOutputDrains) {
    using namespace NQumir::NAst;
    std::atomic<int> destroyCount = 0;
    TOneToOneConnection input(1);
    TShuffleOutputs output(1, 1);
    ASSERT_TRUE(input.Push(0, MakeInt64RowSet({11, 22}, &destroyCount)));
    input.Finish(0);
    ASSERT_TRUE(output[0].Push(0, MakeRowSet(1, &destroyCount)));

    auto schema = std::make_shared<TStructType>(
        std::vector<std::pair<std::string, TTypePtr>>{
            {"value", std::make_shared<TIntegerType>()},
        });
    auto code = std::make_shared<THashShuffleCode>(
        [](TRowSet* batch, uint64_t* hashes) {
            auto* values = reinterpret_cast<int64_t*>(batch->Columns[0].Data);
            for (int64_t row = 0; row < batch->RowCount; ++row) {
                hashes[row] = static_cast<uint64_t>(values[row]);
            }
            return true;
        }, schema, 16, 64, 1024 * 1024);
    auto filter = std::make_shared<TRuntimeFilter>(Context_);
    code->ProducedFilter = filter;
    THashShuffleTask task(code, TInputPort{.Connection = &input}, output.Ports());

    EXPECT_EQ(task.Execute(), ETaskResult::OK);
    EXPECT_FALSE(filter->Ready());
    EXPECT_EQ(task.Execute(), ETaskResult::BLOCKED_OUTPUT);
    ASSERT_TRUE(filter->Ready());
    EXPECT_TRUE(NTest::RuntimeFilterMayContain(*filter, 11));
    EXPECT_TRUE(NTest::RuntimeFilterMayContain(*filter, 22));
    EXPECT_EQ(filter->StatsSnapshot().BuildRows, 2u);

    TRowSet out{};
    ASSERT_EQ(output[0].Fetch(0, out), EFetchResult::OK);
    Release(&out);
    EXPECT_EQ(task.Execute(), ETaskResult::OK);
    EXPECT_EQ(task.Execute(), ETaskResult::FINISHED);
    EXPECT_EQ(filter->StatsSnapshot().BuildRows, 2u);
    ASSERT_EQ(output[0].Fetch(0, out), EFetchResult::OK);
    Release(&out);
    EXPECT_EQ(output[0].Fetch(0, out), EFetchResult::FINISHED);
    EXPECT_EQ(destroyCount.load(), 2);
}

// Stands in for the hash kernel over one i64 column: the hash is the value,
// and an unselected row gets none.
bool HashSelectedValues(TRowSet* batch, uint64_t* hashes) {
    auto* values = reinterpret_cast<int64_t*>(batch->Columns[0].Data);
    for (int64_t row = 0; row < batch->RowCount; ++row) {
        if (!batch->Selection || batch->Selection[row] != 0) {
            hashes[row] = static_cast<uint64_t>(values[row]);
        }
    }
    return true;
}

struct TSelectedRows {
    std::vector<int64_t> Values;
    std::vector<uint64_t> Hashes;
};

// Drains one lane into the selected rows of its rowsets and their hashes.
TSelectedRows FetchSelectedRows(IConnection& connection, size_t lane) {
    TSelectedRows out;
    TRowSet rowSet{};
    while (connection.Fetch(lane, rowSet) == EFetchResult::OK) {
        auto* values = reinterpret_cast<int64_t*>(rowSet.Columns[0].Data);
        for (int64_t row = 0; row < rowSet.RowCount; ++row) {
            if (rowSet.Selection && rowSet.Selection[row] == 0) {
                continue;
            }
            out.Values.push_back(values[row]);
            if (rowSet.Hash) {
                out.Hashes.push_back(rowSet.Hash[row]);
            }
        }
        Release(&rowSet);
        rowSet = {};
    }
    return out;
}

// Three source lanes route into four partitions; no value picks partition 3.
TEST(SchedulerRuntimeAdapter, HashShuffleTasksRouteEveryRowToItsPartition)
{
    constexpr size_t sources = 3;
    constexpr size_t partitions = 4;
    std::atomic<int> destroyed = 0;
    TOneToOneConnection input(1);
    input.Resize(sources, sources);
    std::vector<std::unique_ptr<TGatherConnection>> gathers;
    for (size_t d = 0; d < partitions; ++d) {
        gathers.push_back(std::make_unique<TGatherConnection>(4));
        gathers.back()->Resize(sources, 1);
    }
    const std::vector<std::vector<int64_t>> values = {{0, 1, 2, 4}, {5, 6, 8}, {9, 10}};
    auto code = std::make_shared<THashShuffleCode>(HashSelectedValues);
    std::vector<std::unique_ptr<THashShuffleTask>> tasks;
    for (size_t s = 0; s < sources; ++s) {
        ASSERT_TRUE(input.Push(s, MakeInt64RowSet(values[s], &destroyed)));
        input.Finish(s);
        std::vector<TOutputPort> ports;
        for (const auto& gather : gathers) {
            ports.push_back(TOutputPort{.Connection = gather.get(), .Lane = s});
        }
        tasks.push_back(std::make_unique<THashShuffleTask>(
            code, TInputPort{.Connection = &input, .Lane = s}, std::move(ports)));
    }

    std::vector<std::vector<int64_t>> routed(partitions);
    auto drain = [&](EFetchResult last) {
        for (size_t d = 0; d < partitions; ++d) {
            const auto rows = FetchSelectedRows(*gathers[d], 0).Values;
            routed[d].insert(routed[d].end(), rows.begin(), rows.end());
            TRowSet rowSet{};
            EXPECT_EQ(gathers[d]->Fetch(0, rowSet), last) << d;
        }
    };
    for (size_t s = 0; s + 1 < sources; ++s) {
        EXPECT_EQ(tasks[s]->Execute(), ETaskResult::OK);
        EXPECT_EQ(tasks[s]->Execute(), ETaskResult::FINISHED);
    }
    // A partition finishes only once every source lane has finished.
    drain(EFetchResult::NO_DATA);
    EXPECT_EQ(tasks.back()->Execute(), ETaskResult::OK);
    EXPECT_EQ(tasks.back()->Execute(), ETaskResult::FINISHED);
    drain(EFetchResult::FINISHED);

    for (auto& rows : routed) {
        std::ranges::sort(rows);
    }
    EXPECT_EQ(routed, (std::vector<std::vector<int64_t>>{{0, 4, 8}, {1, 5, 9}, {2, 6, 10}, {}}));
    EXPECT_EQ(destroyed.load(), 3);
}

TEST(SchedulerRuntimeAdapter, HashShuffleBoundsSparseInputOwnersBeforeEof) {
    using namespace NQumir::NAst;
    std::atomic<int> destroyed = 0;
    TOneToOneConnection input(1);
    TShuffleOutputs output(1, 1);
    auto schema = std::make_shared<TStructType>(
        std::vector<std::pair<std::string, TTypePtr>>{{"value", std::make_shared<TIntegerType>()}});
    auto code = std::make_shared<THashShuffleCode>(HashSelectedValues, schema, 16, 64, 800);
    THashShuffleTask task(code, TInputPort{.Connection = &input}, output.Ports());
    std::vector<uint8_t> selection(64, 0);

    for (int cycle = 0; cycle < 4; ++cycle) {
        selection[7] = 1;
        for (int batch = 0; batch < 2; ++batch) {
            std::vector<int64_t> values(64, -1);
            values[7] = cycle * 2 + batch;
            auto rowSet = MakeInt64RowSet(std::move(values), &destroyed);
            rowSet.Selection = selection.data();
            ASSERT_TRUE(input.Push(0, std::move(rowSet)));
            EXPECT_EQ(task.Execute(), ETaskResult::OK);
            if (batch == 0) {
                EXPECT_EQ(destroyed.load(), cycle * 2);
                TRowSet absent{};
                EXPECT_EQ(output[0].Fetch(0, absent), EFetchResult::NO_DATA);
            }
        }
        // Only two selected rows, below the output target. Both source owners
        // must already be released even though upstream has not reached EOF.
        EXPECT_EQ(destroyed.load(), (cycle + 1) * 2);
        selection[7] = 0;
        const auto rows = FetchSelectedRows(output[0], 0);
        EXPECT_EQ(rows.Values, (std::vector<int64_t>{cycle * 2, cycle * 2 + 1}));
    }
    input.Finish(0);
    EXPECT_EQ(task.Execute(), ETaskResult::FINISHED);
}

TEST(SchedulerRuntimeAdapter, HashShuffleBudgetSharesOwnersAndHonorsBackpressure) {
    using namespace NQumir::NAst;
    std::atomic<int> destroyed = 0;
    std::atomic<int> dummyDestroyed = 0;
    TOneToOneConnection input(1);
    TShuffleOutputs output(1, 2);
    auto schema = std::make_shared<TStructType>(
        std::vector<std::pair<std::string, TTypePtr>>{
            {"value", std::make_shared<TIntegerType>()}});
    auto code = std::make_shared<THashShuffleCode>(
        HashSelectedValues, schema, 16, 64, 1800);
    THashShuffleTask task(code, TInputPort{.Connection = &input}, output.Ports());
    std::vector<uint8_t> both(64, 0);
    std::vector<uint8_t> evenOnly(64, 0);
    std::vector<uint8_t> oddOnly(64, 0);
    both[2] = 1;
    both[3] = 1;
    evenOnly[2] = 1;
    oddOnly[3] = 1;
    auto push = [&](int64_t first, std::vector<uint8_t>& selection) {
        std::vector<int64_t> values(64, -1);
        values[2] = first;
        values[3] = first + 1;
        auto rowSet = MakeInt64RowSet(std::move(values), &destroyed);
        rowSet.Selection = selection.data();
        ASSERT_TRUE(input.Push(0, std::move(rowSet)));
    };
    push(2, both);
    EXPECT_EQ(task.Execute(), ETaskResult::OK);
    ASSERT_TRUE(output[0].Push(0, MakeRowSet(1, &dummyDestroyed)));
    push(6, evenOnly);
    EXPECT_EQ(task.Execute(), ETaskResult::BLOCKED_OUTPUT);
    EXPECT_EQ(destroyed.load(), 1) << "the first owner still belongs to partition1";
    push(8, oddOnly);
    EXPECT_EQ(task.Execute(), ETaskResult::BLOCKED_OUTPUT);
    EXPECT_FALSE(input.CanPush(0)) << "blocked output must not consume the next input";
    TRowSet dummy{};
    ASSERT_EQ(output[0].Fetch(0, dummy), EFetchResult::OK);
    Release(&dummy);
    EXPECT_EQ(task.Execute(), ETaskResult::OK);
    evenOnly[2] = 0;
    const auto even = FetchSelectedRows(output[0], 0);
    EXPECT_EQ(even.Values, (std::vector<int64_t>{2, 6}));
    EXPECT_EQ(even.Hashes, (std::vector<uint64_t>{2, 6}));
    EXPECT_EQ(destroyed.load(), 1);
    EXPECT_EQ(task.Execute(), ETaskResult::OK);
    EXPECT_EQ(destroyed.load(), 3);
    both.assign(64, 0);
    oddOnly.assign(64, 0);
    const auto odd = FetchSelectedRows(output[1], 0);
    EXPECT_EQ(odd.Values, (std::vector<int64_t>{3, 9}));
    EXPECT_EQ(odd.Hashes, (std::vector<uint64_t>{3, 9}));
    input.Finish(0);
    EXPECT_EQ(task.Execute(), ETaskResult::FINISHED);
}

struct TShufflePayload {
    std::string Data;
    std::vector<int32_t> Offsets32;
    std::vector<int64_t> Offsets64;
    std::vector<uint8_t> Mask;
    TColumn Column{};
    std::atomic<int>* Destroyed;
};

TRowSet PayloadRowSet(std::unique_ptr<TShufflePayload> data, int64_t rows) {
    auto* payload = data.release();
    return TRowSet{
        .Columns = &payload->Column,
        .ColumnCount = 1,
        .RowCount = rows,
        .Destroy = [](TRowSet* rowSet) {
            auto* payload = static_cast<TShufflePayload*>(rowSet->Private);
            ++*payload->Destroyed;
            delete payload;
        },
        .Private = payload,
        .RefCount = 1,
    };
}

TEST(SchedulerRuntimeAdapter, HashShuffleInputBudgetPreservesNullableBitmapOffsets) {
    using namespace NQumir::NAst;
    std::atomic<int> destroyed = 0;
    TOneToOneConnection input(1);
    TShuffleOutputs output(1, 1);
    auto nullableBool = std::make_shared<TNullable>(std::make_shared<TBoolType>());
    auto schema = std::make_shared<TStructType>(
        std::vector<std::pair<std::string, TTypePtr>>{{"flag", std::move(nullableBool)}});
    auto code = std::make_shared<THashShuffleCode>(
        [](TRowSet*, uint64_t*) {
            ADD_FAILURE() << "a one-partition shuffle does not need hashes";
            return false;
        }, schema, 16, 64, 70);
    THashShuffleTask task(code, TInputPort{.Connection = &input}, output.Ports());
    std::vector<uint8_t> selection(32, 0);
    selection[16] = 1;
    for (bool valid : {false, true}) {
        auto data = std::make_unique<TShufflePayload>();
        data->Destroyed = &destroyed;
        data->Data.assign(5, char{0});
        data->Data[2] = char{8};
        data->Mask.assign(5, 0);
        data->Mask[2] = valid ? 8 : 0;
        data->Column = {.Data = data->Data.data(), .DataBitOffset = 3,
            .Mask = data->Mask.data(), .MaskBitOffset = 3};
        auto rowSet = PayloadRowSet(std::move(data), 32);
        rowSet.Selection = selection.data();
        ASSERT_TRUE(input.Push(0, std::move(rowSet)));
        EXPECT_EQ(task.Execute(), ETaskResult::OK);
    }
    EXPECT_EQ(destroyed.load(), 2);
    TRowSet result{};
    ASSERT_EQ(output[0].Fetch(0, result), EFetchResult::OK);
    EXPECT_EQ(result.RowCount, 2);
    ASSERT_NE(result.Columns[0].Mask, nullptr);
    EXPECT_EQ(result.Columns[0].Mask[0] & 3, 2);
    EXPECT_EQ(static_cast<uint8_t>(result.Columns[0].Data[0]) & 3, 2);
    Release(&result);
    input.Finish(0);
    EXPECT_EQ(task.Execute(), ETaskResult::FINISHED);
}

TEST(SchedulerRuntimeAdapter, HashShuffleInputBudgetCountsUnselectedStringPayload) {
    using namespace NQumir::NAst;
    for (uint8_t width : {4, 8}) {
        for (bool dense : {false, true}) {
            std::atomic<int> destroyed = 0;
            TOneToOneConnection input(1);
            TShuffleOutputs output(1, 1);
            auto schema = std::make_shared<TStructType>(
                std::vector<std::pair<std::string, TTypePtr>>{
                    {"text", std::make_shared<TStringType>()}});
            auto code = std::make_shared<THashShuffleCode>(
                [](TRowSet*, uint64_t*) {
                    ADD_FAILURE() << "a one-partition shuffle does not need hashes";
                    return false;
                }, schema, 16, 64, 128);
            THashShuffleTask task(code, TInputPort{.Connection = &input}, output.Ports());
            auto data = std::make_unique<TShufflePayload>();
            data->Destroyed = &destroyed;
            data->Data = std::string(2048, 'x') + "keep";
            data->Offsets32 = {0, 2048, 2052, 2052};
            data->Offsets64 = {0, 2048, 2052, 2052};
            data->Mask = {3};
            data->Column = {
                .Data = data->Data.data(),
                .Mask = data->Mask.data(),
                .Offsets = width == 4
                    ? static_cast<void*>(data->Offsets32.data())
                    : static_cast<void*>(data->Offsets64.data()),
                .OffsetWidth = width,
            };
            const auto* sourceData = data->Data.data();
            auto rowSet = PayloadRowSet(std::move(data), 3);
            std::vector<uint8_t> selection = {0, 1, 0};
            rowSet.Selection = dense ? nullptr : selection.data();
            ASSERT_TRUE(input.Push(0, std::move(rowSet)));
            EXPECT_EQ(task.Execute(), ETaskResult::OK);
            TRowSet result{};
            ASSERT_EQ(output[0].Fetch(0, result), EFetchResult::OK);
            if (dense) {
                EXPECT_EQ(result.Columns[0].Data, sourceData);
                EXPECT_EQ(result.RowCount, 3);
                EXPECT_EQ(destroyed.load(), 0);
            } else {
                EXPECT_EQ(result.RowCount, 1);
                EXPECT_EQ(result.Selection, nullptr);
                EXPECT_EQ(std::string(result.Columns[0].Data, 4), "keep");
                EXPECT_EQ(destroyed.load(), 1) << "do not transfer the sparse owner downstream";
            }
            Release(&result);
            EXPECT_EQ(destroyed.load(), 1);
            input.Finish(0);
            EXPECT_EQ(task.Execute(), ETaskResult::FINISHED);
        }
    }
}

// Every partition of a dense input references the whole input owner. Above the
// byte target each partition is compacted and the owner is released at once;
// below it the partitions stay views of the owner until EOF.
TEST(SchedulerRuntimeAdapter, HashShuffleBoundsDenseInputSplitAcrossPartitions) {
    using namespace NQumir::NAst;
    for (bool overBudget : {true, false}) {
        SCOPED_TRACE(overBudget);
        std::atomic<int> destroyed = 0;
        TOneToOneConnection input(1);
        TShuffleOutputs output(1, 4);
        auto schema = std::make_shared<TStructType>(
            std::vector<std::pair<std::string, TTypePtr>>{
                {"value", std::make_shared<TIntegerType>()}});
        auto code = std::make_shared<THashShuffleCode>(
            HashSelectedValues, schema, 64, 64, overBudget ? 200 : 1 << 20);
        THashShuffleTask task(code, TInputPort{.Connection = &input}, output.Ports());
        std::vector<int64_t> values(64);
        for (int64_t i = 0; i < 64; ++i) {
            values[i] = i;
        }
        auto rowSet = MakeInt64RowSet(std::move(values), &destroyed);
        const char* sourceData = rowSet.Columns[0].Data;
        ASSERT_TRUE(input.Push(0, std::move(rowSet)));
        EXPECT_EQ(task.Execute(), ETaskResult::OK);
        EXPECT_EQ(destroyed.load(), overBudget ? 1 : 0);
        if (!overBudget) {
            TRowSet absent{};
            EXPECT_EQ(output[0].Fetch(0, absent), EFetchResult::NO_DATA);
            input.Finish(0);
            EXPECT_EQ(task.Execute(), ETaskResult::FINISHED);
        }

        std::vector<TRowSet> results(4);
        for (size_t partition = 0; partition < 4; ++partition) {
            auto& result = results[partition];
            ASSERT_EQ(output[partition].Fetch(0, result), EFetchResult::OK);
            EXPECT_EQ(result.Columns[0].Data == sourceData, !overBudget);
            EXPECT_EQ(result.Selection == nullptr, overBudget);
            EXPECT_EQ(result.RowCount, overBudget ? 16 : 64);
            std::vector<int64_t> selected;
            const auto* data = reinterpret_cast<const int64_t*>(result.Columns[0].Data);
            for (int64_t row = 0; row < result.RowCount; ++row) {
                if (!result.Selection || result.Selection[row] != 0) {
                    selected.push_back(data[row]);
                    EXPECT_EQ(result.Hash[row], static_cast<uint64_t>(data[row]));
                }
            }
            std::vector<int64_t> expected;
            for (int64_t value = static_cast<int64_t>(partition); value < 64; value += 4) {
                expected.push_back(value);
            }
            EXPECT_EQ(selected, expected);
        }
        for (auto& result : results) {
            EXPECT_EQ(destroyed.load(), overBudget ? 1 : 0);
            Release(&result);
        }
        EXPECT_EQ(destroyed.load(), 1);
        if (overBudget) {
            input.Finish(0);
            EXPECT_EQ(task.Execute(), ETaskResult::FINISHED);
        }
    }
}

// A zero byte target disables both the byte bound and the retention budget:
// sparse inputs accumulate up to the row target, and at EOF a single sparse
// input is forwarded as a view of its owner.
TEST(SchedulerRuntimeAdapter, HashShuffleZeroByteTargetKeepsRowBatching) {
    using namespace NQumir::NAst;
    std::atomic<int> destroyed = 0;
    TOneToOneConnection input(1);
    TShuffleOutputs output(1, 1);
    auto schema = std::make_shared<TStructType>(
        std::vector<std::pair<std::string, TTypePtr>>{
            {"value", std::make_shared<TIntegerType>()}});
    auto code = std::make_shared<THashShuffleCode>(HashSelectedValues, schema, 4, 64, 0);
    THashShuffleTask task(code, TInputPort{.Connection = &input}, output.Ports());
    std::vector<uint8_t> selection(64, 0);
    selection[7] = 1;
    const char* lastData = nullptr;
    auto push = [&](int64_t value) {
        std::vector<int64_t> values(64, -1);
        values[7] = value;
        auto rowSet = MakeInt64RowSet(std::move(values), &destroyed);
        rowSet.Selection = selection.data();
        lastData = rowSet.Columns[0].Data;
        ASSERT_TRUE(input.Push(0, std::move(rowSet)));
        EXPECT_EQ(task.Execute(), ETaskResult::OK);
    };
    for (int64_t value = 0; value < 3; ++value) {
        push(value);
        TRowSet absent{};
        EXPECT_EQ(output[0].Fetch(0, absent), EFetchResult::NO_DATA);
        EXPECT_EQ(destroyed.load(), 0);
    }
    push(3);
    EXPECT_EQ(destroyed.load(), 4);
    TRowSet result{};
    ASSERT_EQ(output[0].Fetch(0, result), EFetchResult::OK);
    EXPECT_EQ(result.RowCount, 4);
    EXPECT_EQ(result.Selection, nullptr);
    EXPECT_EQ(std::vector<int64_t>(
        reinterpret_cast<const int64_t*>(result.Columns[0].Data),
        reinterpret_cast<const int64_t*>(result.Columns[0].Data) + 4),
        (std::vector<int64_t>{0, 1, 2, 3}));
    Release(&result);

    push(4);
    input.Finish(0);
    EXPECT_EQ(task.Execute(), ETaskResult::FINISHED);
    result = {};
    ASSERT_EQ(output[0].Fetch(0, result), EFetchResult::OK);
    EXPECT_EQ(result.Columns[0].Data, lastData);
    EXPECT_EQ(result.RowCount, 64);
    ASSERT_NE(result.Selection, nullptr);
    EXPECT_NE(result.Selection[7], 0);
    EXPECT_EQ(destroyed.load(), 4);
    Release(&result);
    EXPECT_EQ(destroyed.load(), 5);
}

TEST(SchedulerRuntimeAdapter, HashShuffleCancellationReleasesInputOwners) {
    using namespace NQumir::NAst;
    for (bool pending : {false, true}) {
        std::atomic<int> destroyed = 0;
        std::atomic<int> dummyDestroyed = 0;
        TOneToOneConnection input(1);
        TShuffleOutputs output(1, 1);
        auto schema = std::make_shared<TStructType>(
            std::vector<std::pair<std::string, TTypePtr>>{
                {"value", std::make_shared<TIntegerType>()}});
        auto code = std::make_shared<THashShuffleCode>(
            HashSelectedValues, schema, 16, 64, pending ? 32 : 1024);
        if (pending) {
            ASSERT_TRUE(output[0].Push(0, MakeRowSet(1, &dummyDestroyed)));
        }
        ASSERT_TRUE(input.Push(0, MakeInt64RowSet(std::vector<int64_t>(8, 1), &destroyed)));
        {
            THashShuffleTask task(code, TInputPort{.Connection = &input}, output.Ports());
            EXPECT_EQ(task.Execute(), pending ? ETaskResult::BLOCKED_OUTPUT : ETaskResult::OK);
            EXPECT_EQ(destroyed.load(), 0);
        }
        EXPECT_EQ(destroyed.load(), 1);
        if (pending) {
            TRowSet dummy{};
            ASSERT_EQ(output[0].Fetch(0, dummy), EFetchResult::OK);
            Release(&dummy);
        }
    }
}

// Two batches with sparse selections, repartitioned by value parity. The slot
// of an unselected row stays 0: a shuffle that read it would send the row to
// partition 0 and add 0 to the filter.
TEST_F(SchedulerRuntimeFilter, HashShuffleReadsHashesOfSelectedRowsOnly) {
    using namespace NQumir::NAst;
    std::atomic<int> destroyCount = 0;
    TOneToOneConnection input(2);
    TShuffleOutputs output(4, 2);

    std::vector<uint8_t> firstSelection = {0xff, 0, 1, 0, 2};
    std::vector<uint8_t> secondSelection = {0, 0, 1, 0xff};
    auto first = MakeInt64RowSet({12, 13, 2, 5, 21}, &destroyCount);
    first.Selection = firstSelection.data();
    auto second = MakeInt64RowSet({7, 9, 4, 3}, &destroyCount);
    second.Selection = secondSelection.data();
    ASSERT_TRUE(input.Push(0, std::move(first)));
    ASSERT_TRUE(input.Push(0, std::move(second)));
    input.Finish(0);

    auto schema = std::make_shared<TStructType>(
        std::vector<std::pair<std::string, TTypePtr>>{
            {"value", std::make_shared<TIntegerType>()},
        });
    auto code = std::make_shared<THashShuffleCode>(
        HashSelectedValues, schema, 16, 64, 1024 * 1024);
    auto filter = std::make_shared<TRuntimeFilter>(Context_);
    code->ProducedFilter = filter;
    THashShuffleTask task(code, TInputPort{.Connection = &input}, output.Ports());

    EXPECT_EQ(task.Execute(), ETaskResult::OK);
    EXPECT_EQ(task.Execute(), ETaskResult::OK);
    EXPECT_EQ(task.Execute(), ETaskResult::FINISHED);

    const auto even = FetchSelectedRows(output[0], 0);
    EXPECT_EQ(even.Values, (std::vector<int64_t>{12, 2, 4}));
    EXPECT_EQ(even.Hashes, (std::vector<uint64_t>{12, 2, 4}));
    const auto odd = FetchSelectedRows(output[1], 0);
    EXPECT_EQ(odd.Values, (std::vector<int64_t>{21, 3}));
    EXPECT_EQ(odd.Hashes, (std::vector<uint64_t>{21, 3}));

    ASSERT_TRUE(filter->Ready());
    const auto stats = filter->StatsSnapshot();
    EXPECT_EQ(stats.BuildRows, 5u);
    EXPECT_EQ(stats.ExactKeys, 5u);
    for (uint64_t key : {12, 2, 21, 4, 3}) {
        EXPECT_TRUE(NTest::RuntimeFilterMayContain(*filter, key)) << key;
    }
    for (uint64_t key : {0, 13, 5, 7, 9}) {
        EXPECT_FALSE(NTest::RuntimeFilterMayContain(*filter, key)) << key;
    }
    EXPECT_EQ(destroyCount.load(), 2);
}

TEST_F(SchedulerRuntimeFilter, HashShuffleDropsBatchWithEmptySelection) {
    std::atomic<int> destroyCount = 0;
    TOneToOneConnection input(1);
    TShuffleOutputs output(2, 2);

    std::vector<uint8_t> selection = {0, 0, 0};
    auto rowSet = MakeInt64RowSet({2, 3, 4}, &destroyCount);
    rowSet.Selection = selection.data();
    ASSERT_TRUE(input.Push(0, std::move(rowSet)));
    input.Finish(0);

    auto code = std::make_shared<THashShuffleCode>(HashSelectedValues);
    auto filter = std::make_shared<TRuntimeFilter>(Context_);
    code->ProducedFilter = filter;
    THashShuffleTask task(code, TInputPort{.Connection = &input}, output.Ports());

    EXPECT_EQ(task.Execute(), ETaskResult::OK);
    EXPECT_EQ(task.Execute(), ETaskResult::FINISHED);

    EXPECT_TRUE(FetchSelectedRows(output[0], 0).Values.empty());
    EXPECT_TRUE(FetchSelectedRows(output[1], 0).Values.empty());
    ASSERT_TRUE(filter->Ready());
    EXPECT_EQ(filter->StatsSnapshot().BuildRows, 0u);
    EXPECT_EQ(destroyCount.load(), 1);
}

TEST(SchedulerRuntimeAdapter, HashShuffleTreatsFullSelectionAsNoSelection) {
    for (bool withSelection : {false, true}) {
        std::atomic<int> destroyCount = 0;
        TOneToOneConnection input(1);
        TShuffleOutputs output(2, 2);

        std::vector<uint8_t> selection = {1, 0xff, 2, 1};
        auto rowSet = MakeInt64RowSet({2, 3, 4, 5}, &destroyCount);
        rowSet.Selection = withSelection ? selection.data() : nullptr;
        ASSERT_TRUE(input.Push(0, std::move(rowSet)));
        input.Finish(0);

        auto code = std::make_shared<THashShuffleCode>(HashSelectedValues);
        THashShuffleTask task(
            code, TInputPort{.Connection = &input}, output.Ports());
        EXPECT_EQ(task.Execute(), ETaskResult::OK);

        const auto even = FetchSelectedRows(output[0], 0);
        EXPECT_EQ(even.Values, (std::vector<int64_t>{2, 4})) << withSelection;
        EXPECT_EQ(even.Hashes, (std::vector<uint64_t>{2, 4})) << withSelection;
        const auto odd = FetchSelectedRows(output[1], 0);
        EXPECT_EQ(odd.Values, (std::vector<int64_t>{3, 5})) << withSelection;
        EXPECT_EQ(odd.Hashes, (std::vector<uint64_t>{3, 5})) << withSelection;
    }
}

// The port of `kind` on {from, lane} moves away once and then moves back.
void ExpectRebinds(ITask& task, EPortKind kind, IConnection& from, size_t lane) {
    TOneToOneConnection other;
    const TPortRebind away{.Kind = kind, .From = &from, .FromLane = lane, .To = &other};
    EXPECT_TRUE(task.RebindPort(away));
    EXPECT_FALSE(task.RebindPort(away));
    EXPECT_TRUE(task.RebindPort({.Kind = kind, .From = &other, .To = &from, .ToLane = lane}));
}

TEST(SchedulerRuntimeAdapter, TasksRebindOnlyTheMatchingPort) {
    TOneToOneConnection in;
    in.Resize(2, 2);
    TOneToOneConnection out;
    out.Resize(2, 2);
    const TInputPort in0{.Connection = &in, .Lane = 0};
    const TInputPort in1{.Connection = &in, .Lane = 1};
    const TOutputPort out0{.Connection = &out, .Lane = 0};
    const TOutputPort out1{.Connection = &out, .Lane = 1};

    TSourceTask source(nullptr, nullptr, out0);
    ExpectRebinds(source, EPortKind::Output, out, 0);
    EXPECT_FALSE(source.RebindPort({.Kind = EPortKind::Input, .From = &out, .To = &in}));
    TSinkTask sink(nullptr, nullptr, in0);
    ExpectRebinds(sink, EPortKind::Input, in, 0);
    EXPECT_FALSE(sink.RebindPort({.Kind = EPortKind::Input, .From = &in, .FromLane = 1, .To = &out}));

    TUnaryTask unary(nullptr, nullptr, in0, out0);
    TRuntimeFilterTask filter(nullptr, in0, out0);
    TBlockingTask blocking(nullptr, nullptr, in0, out0);
    TCteProducerTask producer(nullptr, 0, in0, out0);
    TCtePublishTask publish(nullptr, in0, out0);
    TCteConsumerTask consumer(nullptr, in0, out0);
    const std::vector<ITask*> singlePortTasks = {&unary, &filter, &blocking, &producer, &publish, &consumer};
    for (auto* task : singlePortTasks) {
        ExpectRebinds(*task, EPortKind::Input, in, 0);
        ExpectRebinds(*task, EPortKind::Output, out, 0);
    }

    TBinaryBlockingTask binary(nullptr, nullptr, in0, in1, out0);
    ExpectRebinds(binary, EPortKind::Input, in, 0);
    ExpectRebinds(binary, EPortKind::Input, in, 1);
    ExpectRebinds(binary, EPortKind::Output, out, 0);
    TMergeTask merge(nullptr, nullptr, {in0, in1}, out0);
    ExpectRebinds(merge, EPortKind::Input, in, 1);
    ExpectRebinds(merge, EPortKind::Output, out, 0);
    THashShuffleTask shuffle(nullptr, in0, {out0, out1});
    ExpectRebinds(shuffle, EPortKind::Input, in, 0);
    ExpectRebinds(shuffle, EPortKind::Output, out, 1);
}

// One runtime filter task between two connections. The filter holds 0, the
// hash an unselected row is left with, so a probe that read it would keep the
// row.
class RuntimeFilterTask : public NTest::TRuntimeFilterTest {
protected:
    void SetUp() override {
        NTest::TRuntimeFilterTest::SetUp();
        Filter_ = std::make_shared<TRuntimeFilter>(Context_);
        const std::vector<uint64_t> keys = {0, 11, 22};
        auto builder = Filter_->MakeBuilder();
        builder.Add(keys);
        Filter_->FinishProducer(std::move(builder).TakePartial());

        auto code = std::make_shared<TRuntimeFilterCode>();
        code->Hash = HashSelectedValues;
        code->Probe = Filter_;
        code->AuxInput = std::make_shared<TAuxInput>();
        code->AuxInput->AddFilter(Filter_);
        Task_ = std::make_unique<TRuntimeFilterTask>(
            code,
            TInputPort{.Connection = &Input_},
            TOutputPort{.Connection = &Output_});
    }

    // An empty selection vector means the rowset has no selection.
    void Push(std::vector<int64_t> values, std::vector<uint8_t> selection) {
        Selection_ = std::move(selection);
        auto rowSet = MakeInt64RowSet(std::move(values), &DestroyCount_);
        rowSet.Selection = Selection_.empty() ? nullptr : Selection_.data();
        ASSERT_TRUE(Input_.Push(0, std::move(rowSet)));
    }

    std::atomic<int> DestroyCount_ = 0;
    std::vector<uint8_t> Selection_;
    TOneToOneConnection Input_{1};
    TOneToOneConnection Output_{1};
    std::shared_ptr<TRuntimeFilter> Filter_;
    std::unique_ptr<TRuntimeFilterTask> Task_;
};

TEST_F(RuntimeFilterTask, SparseSelectionProbesOnlySelectedRows) {
    Push({11, 33, 0, 22, 11, 44}, {1, 1, 0, 0xff, 0, 2});
    EXPECT_EQ(Task_->Execute(), ETaskResult::OK);

    const auto kept = FetchSelectedRows(Output_, 0);
    EXPECT_EQ(kept.Values, (std::vector<int64_t>{11, 22}));
    EXPECT_EQ(kept.Hashes, (std::vector<uint64_t>{11, 22}));
    const auto stats = Filter_->StatsSnapshot();
    EXPECT_EQ(stats.ProbeRows, 4u);
    EXPECT_EQ(stats.RejectedRows, 2u);
}

TEST_F(RuntimeFilterTask, EmptySelectionKeepsNothing) {
    Push({11, 0, 22}, {0, 0, 0});
    EXPECT_EQ(Task_->Execute(), ETaskResult::OK);

    EXPECT_TRUE(FetchSelectedRows(Output_, 0).Values.empty());
    const auto stats = Filter_->StatsSnapshot();
    EXPECT_EQ(stats.ProbeRows, 0u);
    EXPECT_EQ(stats.RejectedRows, 0u);
}

TEST_F(RuntimeFilterTask, FullSelectionMatchesNoSelection) {
    for (const auto& selection :
        {std::vector<uint8_t>{}, std::vector<uint8_t>{1, 0xff, 2, 1}}) {
        Push({11, 33, 0, 22}, selection);
        EXPECT_EQ(Task_->Execute(), ETaskResult::OK);
        const auto kept = FetchSelectedRows(Output_, 0);
        EXPECT_EQ(kept.Values, (std::vector<int64_t>{11, 0, 22}));
        EXPECT_EQ(kept.Hashes, (std::vector<uint64_t>{11, 0, 22}));
    }
    const auto stats = Filter_->StatsSnapshot();
    EXPECT_EQ(stats.ProbeRows, 8u);
    EXPECT_EQ(stats.RejectedRows, 2u);
}

TEST_F(RuntimeFilterTask, BlockedOutputProbesTheBatchOnce) {
    ASSERT_TRUE(Output_.Push(0, MakeInt64RowSet({7}, &DestroyCount_)));
    Push({11, 33, 0, 22}, {1, 1, 0, 1});
    EXPECT_EQ(Task_->Execute(), ETaskResult::BLOCKED_OUTPUT);
    EXPECT_EQ(Task_->Execute(), ETaskResult::BLOCKED_OUTPUT);

    TRowSet blocker{};
    ASSERT_EQ(Output_.Fetch(0, blocker), EFetchResult::OK);
    Release(&blocker);
    EXPECT_EQ(Task_->Execute(), ETaskResult::OK);

    const auto kept = FetchSelectedRows(Output_, 0);
    EXPECT_EQ(kept.Values, (std::vector<int64_t>{11, 22}));
    const auto stats = Filter_->StatsSnapshot();
    EXPECT_EQ(stats.ProbeRows, 3u);
    EXPECT_EQ(stats.RejectedRows, 1u);
}

} // namespace

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    NQumir::NCodeGen::TLLVMInitializer initializer;
    return RUN_ALL_TESTS();
}
