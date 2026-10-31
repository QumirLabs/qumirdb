#pragma once

#include <qdb/exec/join_exec.h>
#include <qdb/exec/instrumentation/operator.h>
#include <qdb/io/io.h>
#include <qdb/kernel/compiler.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <vector>

namespace NQdb {

// Mirror the RowArena / RowTable / RowScratch external types
// (modules/qumirdb.cpp); the pair buffer is TJoinPairBuffer. The kernel
// qdb_alloc's every buffer behind a zeroed struct and frees it on Destroy; a
// zeroed struct owns nothing.
struct TRowArenaState {
    uint8_t** Blocks = nullptr;
    int64_t BlockCount = 0;
    int64_t BlockCapacity = 0;
    int64_t BlockRows = 0;
    int64_t RowBytes = 0;
    int64_t Rows = 0;
    int64_t CurRows = 0;
};
static_assert(sizeof(TRowArenaState) == TKernelCompiler::RowArenaSize);

struct TRowTableState {
    uint64_t* Entries = nullptr;
    int64_t Capacity = 0;
    uint64_t Mask = 0;
    int64_t Shift = 0;
    int64_t Rows = 0;
    int64_t Keys = 0;
};
static_assert(sizeof(TRowTableState) == TKernelCompiler::RowTableSize);

struct TRowScratchState {
    uint64_t* Hashes = nullptr;
    uint64_t* Salts = nullptr;
    uint64_t* Offsets = nullptr;
    uint64_t* Entries = nullptr;
    int64_t* Rows = nullptr;
    int64_t* Work = nullptr;
    uint8_t* Keys = nullptr;
    int64_t Capacity = 0;
};
static_assert(sizeof(TRowScratchState) == TKernelCompiler::RowScratchSize);

// The build half of the row join table (kernel/join/join_rows.oz): the arena
// of build rows and the pointer table over it, plus the arena of NULL-key
// build rows of an ANTI join (TRowJoinKernels::KeepsNullKeyRows). One owner
// adds every build batch and seals the table. An inner or outer join's probe
// only reads a sealed table, so any number of TRowJoinProbe lanes may probe
// it at once; a SEMI/ANTI probe marks the build rows, so such a table has one
// lane.
//
// Ownership: Destroy over the table's own zeroed scratch and pair buffer frees
// only the arenas and the table. Build batches are retained only when a build
// row carries a StringView into them (TRowJoinKernels::RetainsBuildBatches).
class TRowJoinTable {
public:
    explicit TRowJoinTable(TRowJoinKernels kernels);
    TRowJoinTable(const TRowJoinTable&) = delete;
    TRowJoinTable& operator=(const TRowJoinTable&) = delete;
    ~TRowJoinTable();

    // Before Seal. Takes ownership of one reference.
    void Add(TRowSet batch);
    // Builds the pointer table over the arena; no Add may follow.
    void Seal();
    bool Sealed() const { return Sealed_; }

    const TRowJoinKernels& Kernels() const { return Kernels_; }

    // input: the build rows' input in the owner's operator counters; the
    // build task of a shared table has only that one input.
    void SetInstrumentation(TOperatorInstrumentation* instrumentation, size_t input = 0);
    void ObserveInstrumentation() const;

    // The kernel ABI takes mutable pointers; a probe of a sealed table only
    // reads them, apart from the marks of a SEMI/ANTI join.
    void* ArenaState() const;
    void* NullKeyArenaState() const;
    void* TableState() const;

private:
    void EnsureInit();
    bool Dispatch(ERowJoinKernelOp op, TRowSet* batch, TRowArenaState& arena);

private:
    TRowJoinKernels Kernels_;
    TOperatorInstrumentation* Instrumentation_ = nullptr;
    size_t InstrumentationInput_ = 0;
    TRowArenaState Arena_;
    TRowArenaState NullKeyArena_;
    TRowTableState Table_;
    std::vector<TRowSet> BuildBatches_;
    bool Initialized_ = false;
    bool Sealed_ = false;
};

// One probe lane of a sealed row join table. It owns its probe scratch, pair
// buffer and output queue, so lanes over one table share nothing mutable.
class TRowJoinProbe {
public:
    // useInputHash: TRowSet::Hash of the probe batches holds this join's key
    // hash. Otherwise the kernel hashes the keys itself; a hash left by
    // another key must not be read.
    TRowJoinProbe(std::shared_ptr<const TRowJoinTable> table, bool useInputHash);
    TRowJoinProbe(const TRowJoinProbe&) = delete;
    TRowJoinProbe& operator=(const TRowJoinProbe&) = delete;
    ~TRowJoinProbe();

    // Probes one batch and materializes its matches before releasing it.
    // Takes ownership of one reference. A SEMI/ANTI join marks the build rows
    // instead and emits nothing.
    void Probe(TRowSet batch);
    bool Next(TRowSet& output);

    // SEMI/ANTI, once the probe side has ended: emits the build rows the join
    // selects by their marks, one output batch per call; false when all are
    // out. The lane must be the only one over its table.
    bool EmitMarked(TRowSet& output);

private:
    bool Dispatch(ERowJoinKernelOp op, TRowSet* batch, void* arena);
    TRowSet Materialize(TRowSet* stream, int64_t start);

private:
    std::shared_ptr<const TRowJoinTable> Table_;
    bool UseInputHash_ = false;
    TRowScratchState Scratch_;
    TJoinPairBuffer PairBuffer_;
    std::deque<TRowSet> ReadyOutput_;
    // The kernel allocated this lane's scratch or pairs, so Destroy must run.
    bool Used_ = false;
    bool MarksEmitted_ = false;
    // Next pair of the EmitByMark output to materialize.
    int64_t EmitCursor_ = 0;
};

// Equi-join over the row join table: one TRowJoinTable and one TRowJoinProbe
// lane over it. The build side is appended to the table until it ends, the
// table is sealed, then every probe batch is looked up and materialized while
// it is alive. The build side is fixed by lowering; the processor never
// switches roles, which is what lets it keep one compact table instead of two.
// An outer join builds its null-supplying side, and the probe kernel pads the
// preserved rows without a match, so the output of a probe batch is complete
// when it is released and nothing is left to emit at the end.
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
class TRowJoinProcessor {
public:
    using TFetch = std::function<EJoinFetchResult(TRowSet&)>;

    TRowJoinProcessor(TRowJoinKernels kernels, EJoinBuildSide buildSide, size_t probeHoldBytes);
    TRowJoinProcessor(const TRowJoinProcessor&) = delete;
    TRowJoinProcessor& operator=(const TRowJoinProcessor&) = delete;
    ~TRowJoinProcessor();

    EJoinProcessorResult Process(
        const TFetch& left,
        const TFetch& right,
        TRowSet& output);

    // Either side until the build side ends, then the probe side. The build
    // side only, while the held probe batches are at the budget.
    EJoinBuildSide RequiredInputSide() const;
    void SetInstrumentation(TOperatorInstrumentation* instrumentation);
    void ObserveInstrumentation();

private:
    // Pulls one build batch; true when the build side made progress or ended.
    bool PullBuildBatch(const TFetch& build);
    // Pulls one probe batch: kept in PendingProbe_ while the table is not
    // sealed, probed otherwise. True when the probe side made progress or ended.
    bool PullProbeBatch(const TFetch& probe);
    // The probe side may be read ahead of the build: the budget is not reached.
    bool CanHoldProbe() const;
    // Payload bytes of the batch, by the instrumentation measure kernel.
    uint64_t ProbeBatchBytes(const TRowSet& batch) const;
    size_t ProbeInput() const;
    // After the probe side ended: emits the build rows a SEMI/ANTI join
    // selects by their marks, one output batch per call.
    EJoinProcessorResult Finish(TRowSet& output);

private:
    EJoinBuildSide BuildSide_;
    TOperatorInstrumentation* Instrumentation_ = nullptr;
    // The probe lane holds the table, so it is declared after it.
    std::shared_ptr<TRowJoinTable> Table_;
    TRowJoinProbe Probe_;
    // A probe batch held for the table, with the bytes measured when it was put here.
    struct THeldProbe {
        TRowSet Batch;
        uint64_t Bytes;
    };
    std::deque<THeldProbe> PendingProbe_;
    size_t ProbeHoldBytes_;
    uint64_t HeldProbeBytes_ = 0;
    // The probe input ended; batches may still wait in PendingProbe_.
    bool ProbeDone_ = false;
};

} // namespace NQdb
