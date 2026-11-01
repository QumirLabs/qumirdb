#pragma once

#include <atomic>
#include <cstddef>
#include <memory>

namespace NQdb {

// One serialized task owns the allocation list; other tasks return blocks
// through Returned_. The heap must outlive those returns.
class alignas(64) TKernelHeap {
public:
    TKernelHeap() = default;
    ~TKernelHeap();
    TKernelHeap(const TKernelHeap&) = delete;
    TKernelHeap& operator=(const TKernelHeap&) = delete;

    void* Allocate(size_t size) noexcept;
    void Collect() noexcept;
    void Reset() noexcept;
    // Payload bytes, excluding headers. Read by the pinned owner, or after
    // all users have stopped.
    size_t Bytes() const noexcept {
        return Bytes_;
    }
    static void* AllocateUnowned(size_t size) noexcept;
    static void Free(void* ptr) noexcept;
    static void Detach(void* ptr) noexcept;

private:
    struct TBlock;
    static TBlock* NewBlock(size_t size) noexcept;
    void Unlink(TBlock* block) noexcept;

    TBlock* Head_ = nullptr;
    std::atomic<TBlock*> Returned_ = nullptr;
    size_t Bytes_ = 0;
};

// Sized once before scheduling. A task keeps its slot when it changes workers.
class TExecutionState {
public:
    explicit TExecutionState(size_t taskCount);
    TKernelHeap& Heap(size_t task);

private:
    size_t TaskCount_;
    std::unique_ptr<TKernelHeap[]> Heaps_;
};

// Mutable worker state. TLS only borrows this object during a task invocation.
struct TExecutionContext {
    TExecutionState* State = nullptr;
    TKernelHeap* Heap = nullptr;
};

TExecutionContext* TryExecutionContext() noexcept;
TExecutionContext& ExecutionContext();

class TTaskExecutionGuard {
public:
    TTaskExecutionGuard(TExecutionContext& context, size_t task);
    ~TTaskExecutionGuard();
    TTaskExecutionGuard(const TTaskExecutionGuard&) = delete;
    TTaskExecutionGuard& operator=(const TTaskExecutionGuard&) = delete;

private:
    TExecutionContext& Context_;
    TExecutionContext* Previous_;
    TKernelHeap* PreviousHeap_;
};

// Standalone processor calls have no scheduler. They use their own heap,
// while scheduled calls retain the task's allocation domain.
class TKernelHeapScope {
public:
    explicit TKernelHeapScope(TKernelHeap& heap) noexcept;
    ~TKernelHeapScope();
    TKernelHeapScope(const TKernelHeapScope&) = delete;
    TKernelHeapScope& operator=(const TKernelHeapScope&) = delete;

private:
    TExecutionContext Context_;
    TExecutionContext* Previous_;
};

} // namespace NQdb
