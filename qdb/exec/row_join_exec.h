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

// Inner equi-join over the row join table (kernel/join/join_rows.oz): the
// build side is appended to an arena of rows until it ends, the pointer table
// is built once, then every probe batch is looked up and materialized while it
// is alive. The build side is fixed by lowering; the processor never switches
// roles, which is what lets it keep one compact table instead of two.
//
// Ownership: the kernel qdb_alloc's every buffer behind the zeroed state
// structs and frees them on Destroy. Build batches are retained only when a
// build row carries a StringView into them (TRowJoinKernels::RetainsBuildBatches).
class TRowJoinProcessor {
public:
    using TFetch = std::function<EJoinFetchResult(TRowSet&)>;

    TRowJoinProcessor(TRowJoinKernels kernels, EJoinBuildSide buildSide);
    TRowJoinProcessor(const TRowJoinProcessor&) = delete;
    TRowJoinProcessor& operator=(const TRowJoinProcessor&) = delete;
    ~TRowJoinProcessor();

    EJoinProcessorResult Process(
        const TFetch& left,
        const TFetch& right,
        TRowSet& output);

    // The build side until it ends, then the probe side.
    EJoinBuildSide RequiredInputSide() const;
    void SetInstrumentation(TOperatorInstrumentation* instrumentation);
    void ObserveInstrumentation();

private:
    // Mirror the JoinArena / JoinTable / JoinScratch / PairBuffer external
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
    static_assert(sizeof(TArenaState) == TKernelCompiler::JoinArenaSize);

    struct TTableState {
        uint64_t* Entries = nullptr;
        int64_t Capacity = 0;
        uint64_t Mask = 0;
        int64_t Shift = 0;
        int64_t Rows = 0;
        int64_t Keys = 0;
    };
    static_assert(sizeof(TTableState) == TKernelCompiler::JoinTableSize);

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
    static_assert(sizeof(TScratchState) == TKernelCompiler::JoinScratchSize);

    struct TPairBufferState {
        int64_t Count = 0;
        int64_t Capacity = 0;
        int64_t* Data = nullptr;
    };
    static_assert(sizeof(TPairBufferState) == TKernelCompiler::PairBufferSize);

    void EnsureInit();
    bool Dispatch(ERowJoinKernelOp op, TRowSet* batch);
    bool DrainReadyOutput(TRowSet& rowSet);
    // Pulls one build batch; true when the build side made progress or ended.
    bool PullBuildBatch(const TFetch& build);
    // Pulls one probe batch and materializes its matches into ReadyOutput_.
    bool PullProbeBatch(const TFetch& probe);

private:
    TRowJoinKernels Kernels_;
    EJoinBuildSide BuildSide_;
    TOperatorInstrumentation* Instrumentation_ = nullptr;
    TArenaState Arena_;
    TTableState Table_;
    TScratchState Scratch_;
    TPairBufferState PairBuffer_;
    std::vector<TRowSet> BuildBatches_;
    std::deque<TRowSet> ReadyOutput_;
    bool Initialized_ = false;
    bool BuildDone_ = false;
    bool ProbeDone_ = false;
};

} // namespace NQdb
