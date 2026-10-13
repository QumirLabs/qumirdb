#include <gtest/gtest.h>

#include "runtime_filter_test_util.h"

#include <qdb/scheduler/connection.h>
#include <qdb/scheduler/graph.h>
#include <qdb/scheduler/runtime_adapter.h>
#include <qdb/scheduler/single_threaded_scheduler.h>
#include <qdb/scheduler/threaded_scheduler.h>

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/parser/type.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
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

struct TBinaryState {
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
                return false;
            }
            rowSet = MakeRowSet(source->Next, destroyCount.get());
            ++source->Next;
            return true;
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
    out.Graph->Build();
    return out;
}

TEST(SchedulerRuntimeAdapter, RunsSourceFilterProjectSinkSingleThreaded) {
    auto pipeline = MakePipeline();
    TSingleThreadedScheduler scheduler(*pipeline.Graph);

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
    TThreadedScheduler scheduler(*pipeline.Graph, 4);

    std::string error;
    ASSERT_TRUE(scheduler.Run(&error)) << error;
    EXPECT_EQ(pipeline.FilterState->Calls, 4);
    EXPECT_EQ(pipeline.ProjectState->Calls, 4);
    EXPECT_EQ(pipeline.SinkState->Batches, 4);
    EXPECT_EQ(pipeline.SinkState->Rows, 450);
    EXPECT_EQ(pipeline.DestroyCount->load(std::memory_order_relaxed), 4);
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
    auto binaryState = std::make_shared<TBinaryState>(TBinaryState{
        .DestroyCount = destroyCount.get(),
    });
    auto sinkState = std::make_shared<TSinkState>();

    auto sourceCode = std::make_shared<TSourceCode>(
        [destroyCount](void* state, TRowSet& rowSet) {
            auto* source = static_cast<TSourceState*>(state);
            if (source->Next > source->End) {
                return false;
            }
            rowSet = MakeRowSet(source->Next, destroyCount.get());
            ++source->Next;
            return true;
        });
    auto binaryCode = std::make_shared<TBinaryBlockingCode>(
        [](void* state, TInputPort& left, TInputPort& right, TRowSet& output) {
            auto* binary = static_cast<TBinaryState*>(state);
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
    graph->Build();

    std::string error;
    ASSERT_TRUE(graph->Validate(&error)) << error;
    TThreadedScheduler scheduler(*graph, 3);
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
                return false;
            }
            rowSet = MakeRowSet(1, destroy.get());
            ++s->Next;
            return true;
        });

    auto probeState = std::make_shared<TSourceState>(TSourceState{.Next = 1, .End = 2});
    auto probeCode = std::make_shared<TSourceCode>(
        [buildDone, probeEarly, destroy](void* state, TRowSet& rowSet) {
            if (!buildDone->load(std::memory_order_relaxed)) {
                probeEarly->store(true, std::memory_order_relaxed);
            }
            auto* s = static_cast<TSourceState*>(state);
            if (s->Next > s->End) {
                return false;
            }
            rowSet = MakeRowSet(1, destroy.get());
            ++s->Next;
            return true;
        });

    auto binaryState = std::make_shared<TBinaryState>(TBinaryState{.DestroyCount = destroy.get()});
    auto binaryCode = std::make_shared<TBinaryBlockingCode>(
        [](void* state, TInputPort& left, TInputPort& right, TRowSet& output) {
            auto* b = static_cast<TBinaryState*>(state);
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
        binaryCode->RequiredSide = [](void* state) {
            auto* b = static_cast<TBinaryState*>(state);
            return b->RightDone ? ERequiredSide::Left : ERequiredSide::Right;
        };
    }

    auto sinkState = std::make_shared<TSinkState>();
    auto sinkCode = std::make_shared<TSinkCode>(
        [](void* state, const TRowSet& rowSet) {
            auto* sink = static_cast<TSinkState*>(state);
            sink->Rows += static_cast<int>(rowSet.RowCount);
            ++sink->Batches;
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
    graph->Build();

    std::string error;
    EXPECT_TRUE(graph->Validate(&error)) << error;
    TSingleThreadedScheduler scheduler(*graph);
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

TEST(SchedulerRuntimeAdapter, HashShuffleTaskScattersSelectedRows) {
    std::atomic<int> destroyCount = 0;
    TOneToOneConnection input(1);
    THashShuffleConnection output(2);
    output.Resize(1, 2);

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
    auto state = std::make_shared<int>(0);
    THashShuffleTask task(
        code,
        state,
        TInputPort{.Connection = &input},
        output,
        0);

    EXPECT_EQ(task.Execute(), ETaskResult::OK);

    TRowSet left{};
    ASSERT_EQ(output.Fetch(0, left), EFetchResult::OK);
    EXPECT_EQ(left.Hash, precomputed.data());
    ASSERT_NE(left.Selection, nullptr);
    EXPECT_EQ(
        std::vector<uint8_t>(left.Selection, left.Selection + left.RowCount),
        (std::vector<uint8_t>{0xff, 0, 0, 0xff}));
    Release(&left);

    TRowSet right{};
    ASSERT_EQ(output.Fetch(1, right), EFetchResult::OK);
    EXPECT_EQ(right.Hash, precomputed.data());
    ASSERT_NE(right.Selection, nullptr);
    EXPECT_EQ(
        std::vector<uint8_t>(right.Selection, right.Selection + right.RowCount),
        (std::vector<uint8_t>{0, 0, 0xff, 0}));
    Release(&right);

    EXPECT_EQ(task.Execute(), ETaskResult::FINISHED);
    EXPECT_EQ(output.Fetch(0, left), EFetchResult::FINISHED);
    EXPECT_EQ(output.Fetch(1, right), EFetchResult::FINISHED);
    EXPECT_EQ(destroyCount.load(std::memory_order_relaxed), 1);
}

TEST(SchedulerRuntimeAdapter, HashShuffleTaskBatchesRowsUntilFinish) {
    using namespace NQumir::NAst;

    std::atomic<int> destroyCount = 0;
    TOneToOneConnection input(2);
    THashShuffleConnection output(4);
    output.Resize(1, 2);

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
    auto state = std::make_shared<int>(0);
    THashShuffleTask task(
        code,
        state,
        TInputPort{.Connection = &input},
        output,
        0);

    EXPECT_EQ(task.Execute(), ETaskResult::OK);
    TRowSet left{};
    EXPECT_EQ(output.Fetch(0, left), EFetchResult::NO_DATA);

    EXPECT_EQ(task.Execute(), ETaskResult::OK);
    EXPECT_EQ(output.Fetch(0, left), EFetchResult::NO_DATA);

    EXPECT_EQ(task.Execute(), ETaskResult::FINISHED);

    ASSERT_EQ(output.Fetch(0, left), EFetchResult::OK);
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
    ASSERT_EQ(output.Fetch(1, right), EFetchResult::OK);
    ASSERT_EQ(right.RowCount, 2);
    ASSERT_EQ(right.Selection, nullptr);
    auto* rightValues = reinterpret_cast<int64_t*>(right.Columns[0].Data);
    EXPECT_EQ(std::vector<int64_t>(rightValues, rightValues + right.RowCount),
        (std::vector<int64_t>{1, 3}));
    ASSERT_NE(right.Hash, nullptr);
    EXPECT_EQ(std::vector<uint64_t>(right.Hash, right.Hash + right.RowCount),
        (std::vector<uint64_t>{1, 3}));
    Release(&right);

    EXPECT_EQ(output.Fetch(0, left), EFetchResult::FINISHED);
    EXPECT_EQ(output.Fetch(1, right), EFetchResult::FINISHED);
    EXPECT_EQ(destroyCount.load(std::memory_order_relaxed), 2);
}

TEST_F(SchedulerRuntimeFilter, PublishesRuntimeFilterBeforeBufferedOutputDrains) {
    using namespace NQumir::NAst;
    std::atomic<int> destroyCount = 0;
    TOneToOneConnection input(1);
    THashShuffleConnection output(1);
    output.Resize(1, 1);
    ASSERT_TRUE(input.Push(0, MakeInt64RowSet({11, 22}, &destroyCount)));
    input.Finish(0);
    ASSERT_TRUE(output.PushTo(0, 0, MakeRowSet(1, &destroyCount)));

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
    THashShuffleTask task(code, nullptr, TInputPort{.Connection = &input}, output, 0);

    EXPECT_EQ(task.Execute(), ETaskResult::OK);
    EXPECT_FALSE(filter->Ready());
    EXPECT_EQ(task.Execute(), ETaskResult::BLOCKED_OUTPUT);
    ASSERT_TRUE(filter->Ready());
    EXPECT_TRUE(NTest::RuntimeFilterMayContain(*filter, 11));
    EXPECT_TRUE(NTest::RuntimeFilterMayContain(*filter, 22));
    EXPECT_EQ(filter->StatsSnapshot().BuildRows, 2u);

    TRowSet out{};
    ASSERT_EQ(output.Fetch(0, out), EFetchResult::OK);
    Release(&out);
    EXPECT_EQ(task.Execute(), ETaskResult::OK);
    EXPECT_EQ(task.Execute(), ETaskResult::FINISHED);
    EXPECT_EQ(filter->StatsSnapshot().BuildRows, 2u);
    ASSERT_EQ(output.Fetch(0, out), EFetchResult::OK);
    Release(&out);
    EXPECT_EQ(output.Fetch(0, out), EFetchResult::FINISHED);
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

// Two batches with sparse selections, repartitioned by value parity. The slot
// of an unselected row stays 0: a shuffle that read it would send the row to
// partition 0 and add 0 to the filter.
TEST_F(SchedulerRuntimeFilter, HashShuffleReadsHashesOfSelectedRowsOnly) {
    using namespace NQumir::NAst;
    std::atomic<int> destroyCount = 0;
    TOneToOneConnection input(2);
    THashShuffleConnection output(4);
    output.Resize(1, 2);

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
    THashShuffleTask task(code, nullptr, TInputPort{.Connection = &input}, output, 0);

    EXPECT_EQ(task.Execute(), ETaskResult::OK);
    EXPECT_EQ(task.Execute(), ETaskResult::OK);
    EXPECT_EQ(task.Execute(), ETaskResult::FINISHED);

    const auto even = FetchSelectedRows(output, 0);
    EXPECT_EQ(even.Values, (std::vector<int64_t>{12, 2, 4}));
    EXPECT_EQ(even.Hashes, (std::vector<uint64_t>{12, 2, 4}));
    const auto odd = FetchSelectedRows(output, 1);
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
    THashShuffleConnection output(2);
    output.Resize(1, 2);

    std::vector<uint8_t> selection = {0, 0, 0};
    auto rowSet = MakeInt64RowSet({2, 3, 4}, &destroyCount);
    rowSet.Selection = selection.data();
    ASSERT_TRUE(input.Push(0, std::move(rowSet)));
    input.Finish(0);

    auto code = std::make_shared<THashShuffleCode>(HashSelectedValues);
    auto filter = std::make_shared<TRuntimeFilter>(Context_);
    code->ProducedFilter = filter;
    THashShuffleTask task(code, nullptr, TInputPort{.Connection = &input}, output, 0);

    EXPECT_EQ(task.Execute(), ETaskResult::OK);
    EXPECT_EQ(task.Execute(), ETaskResult::FINISHED);

    EXPECT_TRUE(FetchSelectedRows(output, 0).Values.empty());
    EXPECT_TRUE(FetchSelectedRows(output, 1).Values.empty());
    ASSERT_TRUE(filter->Ready());
    EXPECT_EQ(filter->StatsSnapshot().BuildRows, 0u);
    EXPECT_EQ(destroyCount.load(), 1);
}

TEST(SchedulerRuntimeAdapter, HashShuffleTreatsFullSelectionAsNoSelection) {
    for (bool withSelection : {false, true}) {
        std::atomic<int> destroyCount = 0;
        TOneToOneConnection input(1);
        THashShuffleConnection output(2);
        output.Resize(1, 2);

        std::vector<uint8_t> selection = {1, 0xff, 2, 1};
        auto rowSet = MakeInt64RowSet({2, 3, 4, 5}, &destroyCount);
        rowSet.Selection = withSelection ? selection.data() : nullptr;
        ASSERT_TRUE(input.Push(0, std::move(rowSet)));
        input.Finish(0);

        auto code = std::make_shared<THashShuffleCode>(HashSelectedValues);
        THashShuffleTask task(
            code, nullptr, TInputPort{.Connection = &input}, output, 0);
        EXPECT_EQ(task.Execute(), ETaskResult::OK);

        const auto even = FetchSelectedRows(output, 0);
        EXPECT_EQ(even.Values, (std::vector<int64_t>{2, 4})) << withSelection;
        EXPECT_EQ(even.Hashes, (std::vector<uint64_t>{2, 4})) << withSelection;
        const auto odd = FetchSelectedRows(output, 1);
        EXPECT_EQ(odd.Values, (std::vector<int64_t>{3, 5})) << withSelection;
        EXPECT_EQ(odd.Hashes, (std::vector<uint64_t>{3, 5})) << withSelection;
    }
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
