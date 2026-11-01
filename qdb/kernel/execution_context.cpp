#include <qdb/kernel/execution_context.h>

#include <cassert>
#include <cstdlib>
#include <limits>
#include <new>
#include <stdexcept>

namespace NQdb {
namespace {

thread_local TExecutionContext* CurrentContext = nullptr;

TKernelHeap* CurrentHeap() noexcept {
    return CurrentContext
        ? CurrentContext->Heap
        : nullptr;
}

} // namespace

struct alignas(std::max_align_t) TKernelHeap::TBlock {
    TKernelHeap* Owner = nullptr;
    TBlock* Previous = nullptr;
    TBlock* Next = nullptr;
    TBlock* ReturnedNext = nullptr;
    size_t Size = 0;
};

static_assert(std::atomic<void*>::is_always_lock_free);

TKernelHeap::~TKernelHeap() {
    Reset();
}

TKernelHeap::TBlock* TKernelHeap::NewBlock(size_t size) noexcept {
    if (size > std::numeric_limits<size_t>::max() - sizeof(TBlock)) {
        return nullptr;
    }
    auto* memory = std::malloc(sizeof(TBlock) + size);
    if (!memory) {
        return nullptr;
    }
    return new (memory) TBlock{.Size = size};
}

void* TKernelHeap::AllocateUnowned(size_t size) noexcept {
    auto* block = NewBlock(size);
    return block
        ? block + 1
        : nullptr;
}

void* TKernelHeap::Allocate(size_t size) noexcept {
    assert(CurrentHeap() == this);
    Collect();
    auto* block = NewBlock(size);
    if (!block) {
        return nullptr;
    }
    block->Owner = this;
    block->Next = Head_;
    if (Head_) {
        Head_->Previous = block;
    }
    Head_ = block;
    Bytes_ += size;
    return block + 1;
}

void TKernelHeap::Unlink(TBlock* block) noexcept {
    if (block->Previous) {
        block->Previous->Next = block->Next;
    } else {
        Head_ = block->Next;
    }
    if (block->Next) {
        block->Next->Previous = block->Previous;
    }
    Bytes_ -= block->Size;
}

void TKernelHeap::Free(void* ptr) noexcept {
    if (!ptr) {
        return;
    }
    auto* block = static_cast<TBlock*>(ptr) - 1;
    auto* owner = block->Owner;
    if (!owner) {
        std::free(block);
    } else if (owner == CurrentHeap()) {
        owner->Unlink(block);
        std::free(block);
    } else {
        auto* head = owner->Returned_.load(std::memory_order_relaxed);
        do {
            block->ReturnedNext = head;
        } while (!owner->Returned_.compare_exchange_weak(
            head, block, std::memory_order_release, std::memory_order_relaxed));
    }
}

void TKernelHeap::Detach(void* ptr) noexcept {
    if (!ptr) {
        return;
    }
    auto* block = static_cast<TBlock*>(ptr) - 1;
    if (block->Owner) {
        // Ownership must move before publishing a batch to another task.
        assert(block->Owner == CurrentHeap());
        block->Owner->Unlink(block);
        block->Owner = nullptr;
    }
}

void TKernelHeap::Collect() noexcept {
    if (!Returned_.load(std::memory_order_relaxed)) {
        return;
    }
    auto* block = Returned_.exchange(nullptr, std::memory_order_acquire);
    while (block) {
        auto* next = block->ReturnedNext;
        Unlink(block);
        std::free(block);
        block = next;
    }
}

void TKernelHeap::Reset() noexcept {
    // Teardown follows worker join and batch release; no producer may still
    // return a block while the allocation domain is being destroyed.
    Collect();
    while (Head_) {
        auto* block = Head_;
        Head_ = block->Next;
        std::free(block);
    }
    Bytes_ = 0;
}

TExecutionState::TExecutionState(size_t taskCount)
    : TaskCount_(taskCount)
    , Heaps_(std::make_unique<TKernelHeap[]>(taskCount))
{}

TKernelHeap& TExecutionState::Heap(size_t task) {
    if (task >= TaskCount_) {
        throw std::out_of_range("execution task slot is out of range");
    }
    return Heaps_[task];
}

TExecutionContext* TryExecutionContext() noexcept {
    return CurrentContext;
}

TExecutionContext& ExecutionContext() {
    if (!CurrentContext) {
        throw std::logic_error("kernel execution requires an execution guard");
    }
    return *CurrentContext;
}

TTaskExecutionGuard::TTaskExecutionGuard(TExecutionContext& context, size_t task)
    : Context_(context)
    , Previous_(CurrentContext)
    , PreviousHeap_(context.Heap)
{
    if (!context.State) {
        throw std::logic_error("task execution requires a task table");
    }
    auto& heap = context.State->Heap(task);
    Context_.Heap = &heap;
    CurrentContext = &Context_;
    heap.Collect();
}

TTaskExecutionGuard::~TTaskExecutionGuard() {
    Context_.Heap->Collect();
    Context_.Heap = PreviousHeap_;
    CurrentContext = Previous_;
}

TKernelHeapScope::TKernelHeapScope(TKernelHeap& heap) noexcept
    : Previous_(CurrentContext)
{
    if (!CurrentContext || (!CurrentContext->State && CurrentContext->Heap != &heap)) {
        Context_.Heap = &heap;
        CurrentContext = &Context_;
        heap.Collect();
    }
}

TKernelHeapScope::~TKernelHeapScope() {
    if (CurrentContext == &Context_) {
        Context_.Heap->Collect();
        CurrentContext = Previous_;
    }
}

} // namespace NQdb
