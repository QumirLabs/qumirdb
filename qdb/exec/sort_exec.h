#pragma once

#include <qdb/exec/join_exec.h>
#include <qdb/kernel/compiler.h>
#include <qdb/plan/ops/sort.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace NQdb {

struct TTopSortScratch;

struct TSortColumnRef {
    int32_t Index = 0;
    NQumir::NAst::TTypePtr Type;
};

struct TSortRadixKernel {
    bool Enabled = false;
    TKernelCompiler::TSortRadixCompositeDispatch Dispatch;
    TKernelCompiler::TSortRadixCompositeNullableDispatch NullableDispatch;
    TKernelCompiler::TTopSortDispatch TopSortDispatch;
    TKernelCompiler::THeapTopKDispatch HeapTopKDispatch;
};

// What a sort or top-sort needs besides its kernels, so it can be rebuilt later.
struct TSortPlanData {
    // Kernel fields are indexes in the sink (0 without a sink).
    struct TTop {
        uint32_t TopSortKernel = 0;
        uint32_t HeapKernel = 0;
        int64_t Limit = 0;
    };

    NQumir::NAst::TTypePtr OutputType;
    std::vector<TSortKey> Keys;
    std::vector<TSortColumnRef> KeyColumns;
    uint32_t RadixKernel = 0;
    // The radix kernel is the nullable variant.
    bool RadixNullable = false;
    std::optional<TTop> Top;
};

// Binds the sort kernels from the plan data and dispatches over the kernels'
// slots; the top-sort and heap dispatches are used only when the plan has Top.
TSortRadixKernel MakeSortRadixKernel(
    const TSortPlanData& plan,
    TKernelCompiler::TSortRadixCompositeDispatch radix,
    TKernelCompiler::TTopSortDispatch topSort,
    TKernelCompiler::THeapTopKDispatch heapTopK);

class TSortProcessor {
public:
    TSortProcessor(
        NQumir::NAst::TTypePtr outputType,
        std::vector<TSortKey> keys,
        std::vector<TSortColumnRef> keyColumns,
        TSortRadixKernel radixKernel,
        int64_t batchRows = JoinOutputBatchRows);

    void Add(TRowSet& rowSet);
    void SetInstrumentation(TOperatorInstrumentation* instrumentation) {
        Store_.SetInstrumentation(instrumentation, 0);
    }

    void Finish();
    bool Next(TRowSet& rowSet);

private:
    bool TryRadixSort();

    NQumir::NAst::TTypePtr OutputType_;
    std::vector<TSortKey> Keys_;
    std::vector<TSortColumnRef> KeyColumns_;
    TSortRadixKernel RadixKernel_;
    int64_t BatchRows_ = JoinOutputBatchRows;

    bool Materialized_ = false;
    TRowStore Store_;
    std::vector<TRowId> Rows_;
    size_t Cursor_ = 0;
};

// K-way merge of already-sorted runs (one per input lane). Reuses the sort key
// comparator and gather path; runs arrive as sorted batches and are ordered
// among themselves. Buffering merge: batches are held in a row store and merged
// into one sorted row-id sequence, then gathered out in batches.
class TMergeProcessor {
public:
    TMergeProcessor(
        NQumir::NAst::TTypePtr outputType,
        std::vector<TSortKey> keys,
        std::vector<TSortColumnRef> keyColumns,
        size_t runCount,
        int64_t batchRows = JoinOutputBatchRows);

    void Add(TRowSet& rowSet, size_t run);
    void SetInstrumentation(TOperatorInstrumentation* instrumentation) {
        Store_.SetInstrumentation(instrumentation, 0);
    }

    void Finish();
    bool Next(TRowSet& rowSet);

private:
    NQumir::NAst::TTypePtr OutputType_;
    std::vector<TSortKey> Keys_;
    std::vector<TSortColumnRef> KeyColumns_;
    int64_t BatchRows_ = JoinOutputBatchRows;

    bool Materialized_ = false;
    TRowStore Store_;
    std::vector<std::vector<TRowId>> Runs_;
    std::vector<TRowId> Merged_;
    size_t Cursor_ = 0;
};

class TTopSortProcessor {
public:
    TTopSortProcessor(
        NQumir::NAst::TTypePtr outputType,
        std::vector<TSortKey> keys,
        std::vector<TSortColumnRef> keyColumns,
        TSortRadixKernel radixKernel,
        int64_t limit,
        int64_t batchRows = JoinOutputBatchRows);
    ~TTopSortProcessor();

    void Add(TRowSet& rowSet);
    void SetInstrumentation(TOperatorInstrumentation* instrumentation);
    void Finish();
    bool Next(TRowSet& rowSet);

private:
    NQumir::NAst::TTypePtr OutputType_;
    std::vector<TSortKey> Keys_;
    std::vector<TSortColumnRef> KeyColumns_;
    TSortRadixKernel RadixKernel_;
    int64_t Limit_ = 0;
    TOperatorInstrumentation* Instrumentation_ = nullptr;
    int64_t BatchRows_ = JoinOutputBatchRows;

    bool Materialized_ = false;
    std::unique_ptr<TTopSortScratch> Scratch_;
    size_t Cursor_ = 0;
};

class TLimitProcessor {
public:
    TLimitProcessor(int64_t limit, int64_t offset);

    bool Finished() const;
    bool Process(TRowSet& input, TRowSet& rowSet);

private:
    int64_t Limit_ = 0;
    int64_t Offset_ = 0;
    int64_t Skipped_ = 0;
    int64_t Emitted_ = 0;
};

} // namespace NQdb
