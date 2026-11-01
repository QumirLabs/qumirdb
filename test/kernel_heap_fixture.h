#pragma once

#include <qdb/kernel/execution_context.h>

#include <gtest/gtest.h>

namespace NQdb {
namespace NTest {

// Raw kernel tests use the same allocation ownership as scheduled tasks.
// The scope ends before the heap is reclaimed, without executing kernel code.
class TKernelHeapTest : public testing::Test {
protected:
    TKernelHeap Heap_;
    TKernelHeapScope Scope_{Heap_};
};

} // namespace NTest
} // namespace NQdb
