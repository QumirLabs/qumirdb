#pragma once

#include <qdb/exec/join_exec.h>
#include <qdb/exec/instrumentation/operator.h>
#include <qdb/io/io.h>
#include <qdb/kernel/compiler.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <vector>

namespace NQdb {

// Equi-join over the row join table (kernel/join/join_rows.oz): the build
// side is appended to an arena of rows until it ends, the pointer table is
// built once, then every probe batch is looked up and materialized while it is
// alive. The build side is fixed by lowering; the processor never switches
// roles, which is what lets it keep one compact table instead of two. An outer
// join builds its null-supplying side, and the probe kernel pads the preserved
// rows without a match, so the output of a probe batch is complete when it is
// released and nothing is left to emit at the end.
//
// A SEMI/ANTI join builds its preserved left side instead. The probe kernel
// marks the build rows the stream matches and emits nothing; once the probe
// side ends, the processor emits the marked rows (SEMI) or the unmarked ones
// (ANTI), each once. An ANTI join also keeps the build rows whose key is NULL,
// which match nothing, in an arena of their own and emits them with the rest.
//
// While the build side has no batch ready, probe batches are pulled and kept
// until the table exists, so the probe pipeline does not stall on a slow build
// input; the symmetric join overlaps its inputs the same way. The hold is
// bounded: once the held batches reach probeHoldBytes the processor asks for
// the build side only, as it would without the hold.
//
// Ownership: the kernel qdb_alloc's every buffer behind the zeroed state
// structs and frees them on Destroy. Build batches are retained only when a
// build row carries a StringView into them (TRowJoinKernels::RetainsBuildBatches).
class TRowJoinProcessor {
public:
    using TFetch = std::function<EJoinFetchResult(TRowSet&)>;

    TRowJoinProcessor(TRowJoinKernels kernels, EJoinBuildSide buildSide, size_t probeHoldBytes);
    TRowJoinProcessor(const TRowJoinProcessor&) = delete;
    TRowJoinProcessor& operator=(const TRowJoinProcessor&) = delete;
    ~TRowJoinProcessor();

    EJoinProcessorResult Process(
        TExecutionContext& context,
        const TFetch& left,
        const TFetch& right,
        TRowSet& output);

    // Either side until the build side ends, then the probe side. The build
    // side only, while the held probe batches are at the budget.
    EJoinBuildSide RequiredInputSide() const;
    void SetInstrumentation(TOperatorInstrumentation* instrumentation);
    void ObserveInstrumentation(TExecutionContext& context);

private:
    // Mirror the RowArena / RowTable / RowScratch / PairBuffer external
    // types (modules/qumirdb.cpp).
    struct TArenaState {
        uint8_t** Blocks = nullptr;
        int64_t BlockCount = 0;
        int64_t BlockCapacity = 0;
        int64_t BlockRows = 0;
        int64_t RowBytes = 0;
        int64_t Rows = 0;
        int64_t CurRows = 0;
    };
    static_assert(sizeof(TArenaState) == TKernelCompiler::RowArenaSize);

    struct TTableState {
        uint64_t* Entries = nullptr;
        int64_t Capacity = 0;
        uint64_t Mask = 0;
        int64_t Shift = 0;
        int64_t Rows = 0;
        int64_t Keys = 0;
    };
    static_assert(sizeof(TTableState) == TKernelCompiler::RowTableSize);

    struct TScratchState {
        uint64_t* Hashes = nullptr;
        uint64_t* Salts = nullptr;
        uint64_t* Offsets = nullptr;
        uint64_t* Entries = nullptr;
        int64_t* Rows = nullptr;
        int64_t* Work = nullptr;
        uint8_t* Keys = nullptr;
        int64_t Capacity = 0;
    };
    static_assert(sizeof(TScratchState) == TKernelCompiler::RowScratchSize);

    struct TPairBufferState {
        int64_t Count = 0;
        int64_t Capacity = 0;
        int64_t* Data = nullptr;
    };
    static_assert(sizeof(TPairBufferState) == TKernelCompiler::PairBufferSize);

    void EnsureInit(TExecutionContext& context);
    // Runs `op` on Arena_, or on `arena`.
    bool Dispatch(TExecutionContext& context, ERowJoinKernelOp op, TRowSet* batch);
    bool Dispatch(
        TExecutionContext& context, ERowJoinKernelOp op, TRowSet* batch, TArenaState& arena);
    bool DrainReadyOutput(TRowSet& rowSet);
    // Pulls one build batch; true when the build side made progress or ended.
    bool PullBuildBatch(TExecutionContext& context, const TFetch& build);
    // Pulls one probe batch: kept in PendingProbe_ while the table is not
    // built, probed otherwise. True when the probe side made progress or ended.
    bool PullProbeBatch(TExecutionContext& context, const TFetch& probe);
    // Looks the batch up, materializes its matches into ReadyOutput_ and
    // releases it.
    void ProbeBatch(TExecutionContext& context, TRowSet& batch);
    // The probe side may be read ahead of the build: the budget is not reached.
    bool CanHoldProbe() const;
    // Payload bytes of the batch, by the instrumentation measure kernel.
    uint64_t ProbeBatchBytes(TExecutionContext& context, const TRowSet& batch) const;
    // After the probe side ended: emits the build rows a SEMI/ANTI join
    // selects by their marks, one output batch per call.
    EJoinProcessorResult Finish(TExecutionContext& context, TRowSet& output);

private:
    TRowJoinKernels Kernels_;
    EJoinBuildSide BuildSide_;
    TOperatorInstrumentation* Instrumentation_ = nullptr;
    TArenaState Arena_;
    // The NULL-key build rows of an ANTI join (TRowJoinKernels::KeepsNullKeyRows).
    TArenaState NullKeyArena_;
    TTableState Table_;
    TScratchState Scratch_;
    TPairBufferState PairBuffer_;
    std::vector<TRowSet> BuildBatches_;
    // A probe batch held for the table, with the bytes measured when it was put here.
    struct THeldProbe {
        TRowSet Batch;
        uint64_t Bytes;
    };
    std::deque<THeldProbe> PendingProbe_;
    size_t ProbeHoldBytes_;
    uint64_t HeldProbeBytes_ = 0;
    std::deque<TRowSet> ReadyOutput_;
    bool Initialized_ = false;
    bool BuildDone_ = false;
    // The probe input ended; batches may still wait in PendingProbe_.
    bool ProbeDone_ = false;
    bool MarksEmitted_ = false;
    // Next pair of the EmitByMark output to materialize.
    int64_t EmitCursor_ = 0;
};

} // namespace NQdb
