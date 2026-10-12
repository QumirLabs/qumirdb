#pragma once

#include <qdb/io/io.h>
#include <qdb/kernel/compiler.h>

#include <qumir/parser/type.h>

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <vector>

namespace NQdb {

// Packed row identifier: (batchIdx << 32) | rowIdx. A packed id of -1 marks an
// absent row (NULL padding on the opposite side of an OUTER join); the join
// kernel emits and decodes that sentinel itself during materialization.
using TRowId = int64_t;

inline TRowId MakeRowId(int32_t batchIdx, int32_t rowIdx) {
    return (static_cast<int64_t>(batchIdx) << 32)
        | static_cast<int64_t>(static_cast<uint32_t>(rowIdx));
}

inline int32_t BatchIndex(TRowId id) {
    return static_cast<int32_t>(id >> 32);
}

inline int32_t RowIndex(TRowId id) {
    return static_cast<int32_t>(static_cast<uint32_t>(id & 0xffffffffu));
}

// Holds all input batches of one join side so their rows can be materialized
// later (on match) by RowId. Reuses the TRowSet Retain/Release refcount from
// io.h.
//
// Ownership: PushBatch TAKES ownership of one reference (move semantics, like
// TProjectedRowSetData in project_exec.cpp). The caller transfers the batch and
// must not use or Release it afterwards; the store Releases every batch on
// destruction. Note RefCount lives inside the TRowSet struct, so there is
// exactly one canonical copy - the one held here.
class TRowStore {
public:
    TRowStore() = default;
    TRowStore(const TRowStore&) = delete;
    TRowStore& operator=(const TRowStore&) = delete;

    ~TRowStore() {
        for (auto& batch : Batches_) {
            Release(&batch);
        }
    }

    // Returns the batch index used to build RowIds for rows of this batch.
    int32_t PushBatch(TRowSet batch) {
        Batches_.push_back(batch);
        return static_cast<int32_t>(Batches_.size() - 1);
    }

    int32_t BatchCount() const { return static_cast<int32_t>(Batches_.size()); }

    // Contiguous base of the batch array, indexable by batch index. Passed to
    // the join kernels so the injected jt_residual_filter can read columns of
    // already-stored rows by decoding packed row IDs. The pointer may be
    // invalidated by a later PushBatch (vector growth) - re-read it each call.
    const TRowSet* Data() const { return Batches_.data(); }

    const TRowSet& Batch(int32_t batchIdx) const { return Batches_[batchIdx]; }

    const TColumn& Column(int32_t batchIdx, int32_t colIdx) const {
        return Batches_[batchIdx].Columns[colIdx];
    }

    const TColumn& Column(TRowId id, int32_t colIdx) const {
        return Batches_[BatchIndex(id)].Columns[colIdx];
    }

private:
    std::vector<TRowSet> Batches_;
};

// Default target rows per output batch (drain granularity).
inline constexpr int64_t JoinOutputBatchRows = 1024;

enum class EJoinSide { Left, Right };

// Build the hash table on the side with fewer estimated rows and stream the
// other when it is at least this many times larger. Hash-table cost is roughly
// per build row, so rows (not bytes) is the right metric. Auto keeps the
// adaptive symmetric build-up.
inline constexpr double JoinAsymmetryRatio = 10.0;
enum class EJoinBuildSide { Auto, Left, Right };

enum class EJoinStreamMode {
    Symmetric,
    StreamLeftAgainstRight,
    StreamRightAgainstLeft,
};

enum class EJoinFetchResult {
    OK = 0,
    NO_DATA = 1,
    FINISHED = 2,
};

enum class EJoinProcessorResult {
    OK = 0,
    NEED_DATA = 1,
    FINISHED = 2,
};

class TInnerJoinProcessor {
public:
    using TFetch = std::function<EJoinFetchResult(TRowSet&)>;

    TInnerJoinProcessor(
        TJoinKernels kernels,
        EJoinType joinType = EJoinType::Inner,
        EJoinBuildSide buildSide = EJoinBuildSide::Auto);
    TInnerJoinProcessor(const TInnerJoinProcessor&) = delete;
    TInnerJoinProcessor& operator=(const TInnerJoinProcessor&) = delete;
    ~TInnerJoinProcessor();

    EJoinProcessorResult Process(
        const TFetch& left,
        const TFetch& right,
        TRowSet& output);

    // Input side Process reads next: build side while building, probe side while
    // streaming, Auto (both) for the symmetric/adaptive path. Lets the scheduler
    // skip waking the side not read yet.
    EJoinBuildSide RequiredInputSide() const;

private:
    // Mirrors the PairBuffer external type (modules/qumirdb.cpp).
    struct TPairBufferState {
        int64_t Count = 0;
        int64_t Capacity = 0;
        int64_t* Data = nullptr;
    };
    static_assert(sizeof(TPairBufferState) == TKernelCompiler::PairBufferSize);

    bool IsOuter() const;
    bool IsSemiAnti() const;
    void EnsureInit();
    bool DrainReadyOutput(TRowSet& rowSet);
    bool DrainMaterialized(TRowSet& rowSet);
    void DrainStreamingPairs(const TRowSet& streamBatch, EJoinSide streamSide);
    bool PullOneInputBatch(const TFetch& left, const TFetch& right);
    bool PullSemiAntiBatch(const TFetch& left, const TFetch& right);
    bool StreamsSemiAntiLeft() const;
    void FinalizeOuterJoin();
    void FinalizeSemiAntiJoin();
    EJoinProcessorResult ProcessSemiAnti(
        const TFetch& left, const TFetch& right, TRowSet& output);
    EJoinSide ChooseSymmetricPullSide() const;

private:
    TJoinKernels Kernels_;
    EJoinType JoinType_ = EJoinType::Inner;
    EJoinBuildSide BuildSide_ = EJoinBuildSide::Auto;
    bool OuterFinalized_ = false;
    bool SemiAntiFinalized_ = false;
    TRowStore LeftRows_;
    TRowStore RightRows_;
    std::array<uint8_t, TKernelCompiler::HashTableSize> LeftTable_{};
    std::array<uint8_t, TKernelCompiler::HashTableSize> RightTable_{};
    TPairBufferState PairBuffer_;
    // Next undrained pair in PairBuffer_ (kernel jt_materialize cursor). The
    // buffer is reset once the cursor catches up, so Finalize ops (whose RIGHT
    // variant swaps every pair in the buffer) always start from an empty one.
    int64_t MaterializeCursor_ = 0;
    std::deque<TRowSet> ReadyOutput_;
    bool Initialized_ = false;
    bool LeftDone_ = false;
    bool RightDone_ = false;
    bool BothDone_ = false;
    EJoinStreamMode StreamMode_ = EJoinStreamMode::Symmetric;
    int64_t StoredLeftRows_ = 0;
    int64_t StoredRightRows_ = 0;
    int64_t LastLeftBatchRows_ = 0;
    int64_t LastRightBatchRows_ = 0;
};

// Fetch-driven Cartesian product join for the scheduler cross-scalar path.
// Buffers the broadcast right side, emits row-id pairs in xj_dispatch, then
// materializes through the shared jt_materialize kernel. Residual filtering is
// applied downstream (cross -> filter).
class TCrossJoinProcessor {
public:
    using TFetch = std::function<EJoinFetchResult(TRowSet&)>;

    explicit TCrossJoinProcessor(TCrossJoinKernels kernels);
    TCrossJoinProcessor(const TCrossJoinProcessor&) = delete;
    TCrossJoinProcessor& operator=(const TCrossJoinProcessor&) = delete;
    ~TCrossJoinProcessor();

    EJoinProcessorResult Process(
        const TFetch& left,
        const TFetch& right,
        TRowSet& output);

private:
    struct TPairBufferState {
        int64_t Count = 0;
        int64_t Capacity = 0;
        int64_t* Data = nullptr;
    };
    static_assert(sizeof(TPairBufferState) == TKernelCompiler::PairBufferSize);

    bool DrainMaterialized(TRowSet& rowSet);

private:
    TCrossJoinKernels Kernels_;
    TRowStore LeftRows_;
    TRowStore RightRows_;
    TPairBufferState PairBuffer_;
    int64_t MaterializeCursor_ = 0;
    bool RightDrained_ = false;
    bool LeftDone_ = false;
    int64_t RightTotalRows_ = 0;
};

} // namespace NQdb
