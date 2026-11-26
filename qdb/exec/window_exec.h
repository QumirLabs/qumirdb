#pragma once

#include <qdb/exec/join_exec.h>
#include <qdb/exec/sort_exec.h>
#include <qdb/io/rowset_ops.h>

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace NQdb {

// Binds the window kernel from a dispatch over the kernel's slot.
TSortRadixKernel MakeWindowKernel(TKernelCompiler::TSortRadixCompositeDispatch dispatch);

// Blocking window processor. Buffers all input, then in a single Next() sorts
// the row-id permutation by (partition ++ order) and materializes the input
// columns followed by one computed column per window function - all inside the
// JIT window kernel (qdb_window_run). The whole result is emitted at once
// because the running/prefix compute spans the full partition.
//
// With a range kernel it is the task of one key range of the range scheme
// (wiki Window-parallelism): Next folds the partial carries of AddCarry.
class TWindowProcessor {
public:
    TWindowProcessor(
        std::vector<TSortKey> keys,
        std::vector<TSortColumnRef> keyColumns,
        TSortRadixKernel kernel,
        TKernelCompiler::TWindowRangeDispatch rangeKernel = {});

    void Add(TRowSet& rowSet);
    void AddCarry(TRowSet& carry);
    void SetInstrumentation(TOperatorInstrumentation* instrumentation) {
        Store_.SetInstrumentation(instrumentation, 0);
    }

    void Finish();
    bool Next(TRowSet& rowSet);

private:
    std::vector<TSortKey> Keys_;
    std::vector<TSortColumnRef> KeyColumns_;
    TSortRadixKernel Kernel_;
    TKernelCompiler::TWindowRangeDispatch RangeKernel_;

    TRowStore Store_;
    TRowStore Carries_;
    std::vector<TRowId> Rows_;
    bool Emitted_ = false;
};

// One input lane of the range scheme: keeps the lane's rows, materializes every
// k-th selected row as its part of the splitter sample, then hands the rows on.
class TWindowSampleProcessor {
public:
    TWindowSampleProcessor(TKernelCompiler::TWindowRangeDispatch kernel, uint64_t sampleRows);
    ~TWindowSampleProcessor();

    void Add(TRowSet& rowSet);
    // The sample of all added rows; false when there are none.
    bool Sample(TRowSet& sample);
    // The added rowsets, in order.
    bool Next(TRowSet& rowSet);

private:
    TKernelCompiler::TWindowRangeDispatch Kernel_;
    uint64_t SampleRows_ = 0;
    std::vector<TRowSet> Batches_;
    uint64_t SelectedRows_ = 0;
    size_t Cursor_ = 0;
};

// One lane of the range scheme's routing: picks the splitters from the sorted
// sample, sends each row to the task of its key range and computes the lane's
// partial carries.
class TWindowRouteProcessor {
public:
    // carryLayouts: the columns of a carry (TWindowPlanData::CarryLayouts).
    TWindowRouteProcessor(
        TKernelCompiler::TWindowRangeDispatch kernel,
        size_t ranges,
        std::vector<TValueLayout> carryLayouts);
    ~TWindowRouteProcessor();

    // A batch of the sorted sample; the whole sample comes before Route.
    void AddSample(TRowSet& rowSet);
    // Takes rowSet; `out` gets the rows of every range that has some.
    void Route(TRowSet& rowSet, std::vector<std::pair<size_t, TRowSet>>& out);
    // The lane's partial carry of `range`, after the last Route: one row, or
    // false when no row of the range's leading partition comes before it.
    bool Carry(size_t range, TRowSet& carry);

    struct TCarryTable;

private:
    void PickSplitters();

    TKernelCompiler::TWindowRangeDispatch Kernel_;
    size_t Ranges_ = 0;
    std::vector<TValueLayout> CarryLayouts_;
    // The sample batches, then a slot for the batch being routed.
    std::vector<TRowSet> Store_;
    size_t SampleBatches_ = 0;
    bool Picked_ = false;
    std::vector<int64_t> Splitters_;
    std::shared_ptr<TCarryTable> Tails_;
    std::shared_ptr<TCarryTable> Carries_;
};

} // namespace NQdb
