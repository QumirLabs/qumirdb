#include <qdb/exec/kernel_rowset.h>
#include <qdb/kernel/execution_context.h>
#include <qdb/modules/qumirdb_runtime.h>
#include <qdb/scheduler/graph.h>
#include <qdb/scheduler/threaded_scheduler.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace NQdb;
using namespace NQdb::NScheduler;

TEST(ExecutionContext, NestedPinsRestoreOnException) {
    TExecutionState state(2);
    TExecutionContext context{.State = &state};
    EXPECT_THROW(ExecutionContext(), std::logic_error);
    {
        TTaskExecutionGuard outer(context, 0);
        EXPECT_EQ(&ExecutionContext(), &context);
        EXPECT_EQ(context.Heap, &state.Heap(0));
        try {
            TTaskExecutionGuard inner(context, 1);
            EXPECT_EQ(context.Heap, &state.Heap(1));
            throw std::runtime_error("task failed");
        } catch (const std::runtime_error&) {
        }
        EXPECT_EQ(context.Heap, &state.Heap(0));
    }
    EXPECT_EQ(TryExecutionContext(), nullptr);
    EXPECT_EQ(context.Heap, nullptr);
}

TEST(ExecutionContext, InvalidPinDoesNotChangeCurrentContext) {
    TExecutionState state(1);
    TExecutionContext context{.State = &state};
    TTaskExecutionGuard pin(context, 0);
    EXPECT_THROW(TTaskExecutionGuard(context, 1), std::out_of_range);
    EXPECT_EQ(context.Heap, &state.Heap(0));
}

TEST(ExecutionContext, TaskMigratesAndAcceptsConcurrentReturns) {
    TExecutionState state(2);
    std::vector<void*> allocations;
    std::thread([&] {
        TExecutionContext worker{.State = &state};
        TTaskExecutionGuard pin(worker, 0);
        for (int i = 0; i < 4096; ++i) {
            auto* block = qdb_alloc(32);
            ASSERT_NE(block, nullptr);
            allocations.push_back(block);
        }
    }).join();
    EXPECT_EQ(state.Heap(0).Bytes(), 4096 * 32);
    std::vector<std::thread> releasers;
    for (size_t t = 0; t < 4; ++t) {
        releasers.emplace_back([&, t] {
            for (size_t i = t; i < allocations.size(); i += 4) {
                qdb_free(allocations[i]);
            }
        });
    }
    // The owner migrates to this thread while remote frees are arriving.
    {
        TExecutionContext worker{.State = &state};
        TTaskExecutionGuard pin(worker, 0);
        for (int i = 0; i < 4096; ++i) {
            qdb_free(qdb_alloc(48));
        }
    }
    for (auto& thread : releasers) {
        thread.join();
    }
    {
        TExecutionContext worker{.State = &state};
        TTaskExecutionGuard pin(worker, 0);
        EXPECT_EQ(worker.Heap->Bytes(), 0);
    }
}

TEST(ExecutionContext, ReturnedBlockBelongsToOriginalTask) {
    TExecutionState state(2);
    TExecutionContext worker{.State = &state};
    void* block;
    {
        TTaskExecutionGuard pin(worker, 0);
        block = qdb_alloc(128);
    }
    {
        TTaskExecutionGuard pin(worker, 1);
        qdb_free(block);
        EXPECT_EQ(worker.Heap->Bytes(), 0);
    }
    {
        TTaskExecutionGuard pin(worker, 0);
        EXPECT_EQ(worker.Heap->Bytes(), 0);
    }
}

TEST(ExecutionContext, TransferredAllocationOutlivesQueryAndWorker) {
    void* block;
    {
        TExecutionState state(1);
        std::thread([&] {
            TExecutionContext worker{.State = &state};
            TTaskExecutionGuard pin(worker, 0);
            block = qdb_alloc(64);
            *static_cast<uint64_t*>(block) = 123;
            TKernelHeap::Detach(block);
            EXPECT_EQ(worker.Heap->Bytes(), 0);
        }).join();
    }
    EXPECT_EQ(*static_cast<uint64_t*>(block), 123);
    std::thread([&] { qdb_free(block); }).join();
}

TEST(ExecutionContext, MaterializedBatchOutlivesItsTaskTable) {
    TRowSet batch{};
    {
        TExecutionState state(1);
        std::thread([&] {
            TExecutionContext worker{.State = &state};
            TTaskExecutionGuard pin(worker, 0);
            auto* value = static_cast<int64_t*>(qdb_alloc(sizeof(int64_t)));
            *value = 456;
            auto* columns = static_cast<TColumn*>(qdb_alloc(sizeof(TColumn)));
            *columns = {.Data = reinterpret_cast<char*>(value)};
            auto* owners = static_cast<int64_t*>(qdb_alloc(3 * sizeof(int64_t)));
            owners[0] = 2;
            owners[1] = reinterpret_cast<int64_t>(value);
            owners[2] = reinterpret_cast<int64_t>(columns);
            batch.RefCount = 1;
            batch.RowCount = 1;
            batch.Columns = columns;
            batch.Private = owners;
            AdoptKernelOwnedRowSet(batch);
            EXPECT_EQ(worker.Heap->Bytes(), 0);
        }).join();
    }
    std::thread([&] {
        EXPECT_EQ(*reinterpret_cast<int64_t*>(batch.Columns[0].Data), 456);
        Release(&batch);
    }).join();
}

TEST(ExecutionContext, StandaloneScopesRestoreTheirAllocationOwner) {
    TKernelHeap outerHeap;
    TKernelHeap innerHeap;
    TKernelHeapScope outer(outerHeap);
    auto* a = qdb_alloc(11);
    {
        TKernelHeapScope inner(innerHeap);
        auto* b = qdb_alloc(13);
        EXPECT_EQ(ExecutionContext().Heap, &innerHeap);
        EXPECT_EQ(outerHeap.Bytes(), 11);
        EXPECT_EQ(innerHeap.Bytes(), 13);
        qdb_free(b);
    }
    EXPECT_EQ(ExecutionContext().Heap, &outerHeap);
    qdb_free(a);
    EXPECT_EQ(outerHeap.Bytes(), 0);
}

TEST(ExecutionContext, NestedQueriesRestoreWorkerContext) {
    TExecutionState a(1);
    TExecutionState b(1);
    TExecutionContext first{.State = &a};
    TExecutionContext second{.State = &b};
    TTaskExecutionGuard outer(first, 0);
    {
        TTaskExecutionGuard inner(second, 0);
        EXPECT_EQ(&ExecutionContext(), &second);
    }
    EXPECT_EQ(&ExecutionContext(), &first);
    EXPECT_EQ(second.Heap, nullptr);
}

TEST(ExecutionContext, AllocationAlignmentAndFailure) {
    TKernelHeap heap;
    TKernelHeapScope scope(heap);
    for (int64_t size : {0, 1, 16, 67, 1024}) {
        auto* block = qdb_alloc(size);
        ASSERT_NE(block, nullptr);
        EXPECT_EQ(reinterpret_cast<uintptr_t>(block) % alignof(std::max_align_t), 0);
        qdb_free(block);
    }
    EXPECT_EQ(qdb_alloc(-1), nullptr);
    EXPECT_EQ(TKernelHeap::AllocateUnowned(SIZE_MAX), nullptr);
    qdb_free(nullptr);
    EXPECT_EQ(heap.Bytes(), 0);
}

namespace {

class TAllocatingTask final : public ITaskNode {
public:
    ETaskResult Execute() override {
        EXPECT_NE(TryExecutionContext(), nullptr);
        EXPECT_NE(ExecutionContext().Heap, nullptr);
        qdb_alloc(96);
        return ETaskResult::FINISHED;
    }
};

} // namespace

TEST(ExecutionContext, SchedulerPinsAllocationDomain) {
    TTaskGraph graph;
    auto& node = graph.AddOwnedNode(std::make_unique<TAllocatingTask>());
    TThreadedScheduler scheduler(graph, 2);
    std::string error;
    ASSERT_TRUE(scheduler.Run(&error)) << error;
    EXPECT_EQ(graph.PrepareExecution().Heap(node.ExecutionSlot).Bytes(), 96);
    EXPECT_EQ(TryExecutionContext(), nullptr);
}

TEST(ExecutionContext, TaskTableIsFrozenBeforeWorkersStart) {
    TAllocatingTask task;
    TTaskGraph graph;
    graph.AddNode(task);
    graph.PrepareExecution();
    EXPECT_THROW(graph.AddNode(task), std::logic_error);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
