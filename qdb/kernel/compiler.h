#pragma once

#include <qdb/io/io.h>
#include <qdb/kernel/generated.h>
#include <qdb/kernel/spec.h>
#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/join.h>

#include <qumir/parser/type.h>
#include <qumir/runner/runner_llvm.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace NQdb {

class TExternalCatalogSnapshot;

// Kernel generation appends TGeneratedKernels to Sink (when set); BindNow
// additionally JIT-finalizes them immediately so the returned dispatches are
// live (the default; deferred callers finalize via JitFinalizeKernels).
struct TKernelCompilerOptions {
    std::ostream* Diagnostics = nullptr;
    std::string Stage;
    TExecStageId ExecStageId = InvalidExecStageId;
    std::vector<TGeneratedKernel>* Sink = nullptr;
    bool BindNow = true;
    std::shared_ptr<const TExternalCatalogSnapshot> ExternalCatalog;
    std::shared_ptr<TRuntimeContext> RuntimeContext;
};

// Shared kernel-pipeline options; caller overrides NativeCode/TargetTriple.
NQumir::TLLVMRunnerOptions KernelRunnerOptions();

// Native-JIT kernel compilation (prepends `use qumirdb`, links runtime
// symbols); one function pointer per entry. The pointers live as long as
// `runner`.
std::unordered_map<std::string, void*> CompileKernelAst(
    NQumir::TLLVMRunner& runner,
    NQumir::NAst::TExprPtr ast,
    const std::vector<std::string>& entryNames,
    std::string* error);

std::unordered_map<std::string, void*> CompileKernelAst(
    NQumir::TLLVMRunner& runner,
    NQumir::NFrontend::TComposeResult composed,
    const std::vector<std::string>& entryNames,
    std::string* error);

// Cache-backed native-JIT compilation: cacheable dependency symbols are loaded
// from / persisted to `cacheDir`; the query kernel is recompiled and linked.
// Entries stay valid while the returned lifetime handle is held.
NQumir::NCodeGen::TLlvmRunner::TLinkedModule CompileKernelAstCached(
    NQumir::TLLVMRunner& runner,
    NQumir::NAst::TExprPtr ast,
    const std::vector<std::string>& entryNames,
    const std::string& cacheDir,
    std::string* error);

// Like the native CompileKernelAst, but emits a target object (per
// runner.Options.TargetTriple) instead of JIT. Returns object bytes.
std::optional<std::string> CompileKernelAstToObject(
    NQumir::TLLVMRunner& runner,
    NQumir::NAst::TExprPtr ast,
    const std::vector<std::string>& entryNames,
    std::string* error);

std::optional<std::string> CompileKernelAstToObject(
    NQumir::TLLVMRunner& runner,
    NQumir::NFrontend::TComposeResult composed,
    const std::vector<std::string>& entryNames,
    std::string* error);

// Cache-backed object compilation for external linkers. Cacheable dependencies
// are returned as existing paths or fresh blobs; KernelObject is always rebuilt.
std::optional<NQumir::TLLVMRunner::TCachedObjectModule>
CompileKernelAstToObjectsCached(
    NQumir::TLLVMRunner& runner,
    NQumir::NAst::TExprPtr ast,
    const std::vector<std::string>& entryNames,
    const std::string& cacheDir,
    std::string* error);

struct TSortRadixKeyInput {
    int32_t ColumnIndex = 0;
    NQumir::NAst::TTypePtr Type;
    bool Desc = false;
    bool NullsFirst = false;
};

// Generates a heap_less overload taking the row store as its context, so heap
// items stay bare packed row ids. It reads the given sort columns from each
// row and compares them in lexicographic SQL order. Equal keys use packed row
// id order as a stable tie break.
NQumir::NAst::TExprPtr BuildSortRowIdLessAst(
    const std::vector<TSortRadixKeyInput>& keys);

// Heap Top-K program (entry qdb_top_k_select). It keeps the first K packed row
// ids in SQL sort order and returns their count. The tail holds unspecified
// values, not the remaining row ids.
NQumir::NAst::TExprPtr BuildHeapTopKProgramAst(
    const std::vector<TSortRadixKeyInput>& keys);

// Radix composite sort program. With a materialize schema it exports one
// stage-level entry qdb_sort_run: optionally sorts packed TRowId values
// in-place, then materializes a requested slice into a kernel-owned TRowSet.
// Throws on an unsupported key/output type.
NQumir::NAst::TExprPtr BuildRadixSortProgramAst(
    const std::vector<TSortRadixKeyInput>& keys,
    const NQumir::NAst::TStructType* materializeType = nullptr);

// Nullable-key variant of qdb_sort_run.
NQumir::NAst::TExprPtr BuildRadixSortNullableProgramAst(
    const std::vector<TSortRadixKeyInput>& keys,
    const NQumir::NAst::TStructType* materializeType = nullptr);

// Top-sort program (entry qdb_top_sort_update): optionally sorts incoming batch
// row ids with the same radix cascade as full sort, merges the current top-K
// state with that sorted batch, and with a materialize schema writes the new
// state into a kernel-owned TRowSet. pick_src/pick_idx are scratch buffers, not
// host output.
NQumir::NAst::TExprPtr BuildTopSortMergeProgramAst(
    const std::vector<TSortRadixKeyInput>& keys,
    const NQumir::NAst::TStructType* materializeType = nullptr);

// One window function to materialize as an appended output column. Func is the
// internal kernel variant ("rank", "sum_prefix", "sum_partition",
// "avg_partition", "max_prefix").
struct TWindowFuncInput {
    std::string Func;
    int32_t ArgColumn = -1;
    NQumir::NAst::TTypePtr ResultType;
    // For running sum/max (sum_prefix/max_prefix): RANGE frame (each row shares
    // the running value through the last order-key peer) vs ROWS (row-by-row).
    bool PeerInclusive = false;
};

// Window program (entry qdb_window_run, same ABI as qdb_sort_run): optionally
// sorts the packed TRowId permutation by `keys`, then materializes the [start,
// limit) slice into a kernel-owned TRowSet whose columns are the input columns
// (in sorted order) followed by one computed column per window function.
NQumir::NAst::TExprPtr BuildWindowProgramAst(
    const std::vector<TSortRadixKeyInput>& keys,
    const NQumir::NAst::TStructType& inputType,
    const std::vector<TSortRadixKeyInput>& partitionKeys,
    const std::vector<TSortRadixKeyInput>& orderKeys,
    const std::vector<TWindowFuncInput>& funcs);

// agg_dispatch(ref HashTable ht, ref TRowSet batch, i64 arg, i64 op) -> i64
//   op == 0: init(ht, capacity = arg)
//   op == 1: update ht from *batch (arg ignored)
//   otherwise: destroy(ht)
// `ht` must point to a caller-owned, zero-initialized buffer of
// TKernelCompiler::HashTableSize bytes (the HashTable layout from
// modules/qumirdb.cpp).
using TAggregateDispatch = std::function<int64_t(void* ht, TRowSet* batch, int64_t arg, int64_t op)>;

// Finalizes ht into a complete TRowSet and destroys ht.
using TAggregateFinishRowSet = std::function<int64_t(void* ht, TRowSet* output)>;

enum class EAggregateOutputKeyKind {
    Fixed,
    String,
};

struct TAggregateOutputKey {
    EAggregateOutputKeyKind Kind = EAggregateOutputKeyKind::Fixed;
    bool IsNullable = false;
    size_t Size = 0;
    size_t Alignment = 0;
};

struct TAggregateOutputAgg {
    bool IsNullable = false;
};

// The compiled kernels for one Aggregation query: Dispatch handles
// init/update/destroy of the HashTable (via agg_dispatch's op-codes),
// Finalize copies the dense group keys/aggregate buffers to output arrays.
struct TAggregateKernels {
    TAggregateDispatch Dispatch;
    TAggregateFinishRowSet FinishRowSet;
    size_t NumAggs = 0;
    std::vector<int64_t> StringMetadataBuffers;
    std::vector<TAggregateOutputKey> OutputKeys;
    std::vector<TAggregateOutputAgg> OutputAggs;
};

enum class EJoinKernelOp : int64_t {
    Init = 0,
    UpdateLeft = 1,
    UpdateRight = 2,
    StreamLeft = 3,
    StreamRight = 4,
    Finalize = 5,
    Destroy = 6,
    // LEFT/RIGHT join that streamed against its preserved side: emit the build
    // rows the stream probes left unmarked.
    FinalizeStream = 7,
};

using TJoinDispatch = std::function<bool(
    void* left,
    void* right,
    TRowSet* batch,
    int64_t batchIdx,
    void* pairs,
    TRowSet* leftStore,
    TRowSet* rightStore,
    int64_t arg,
    int64_t op)>;

// jt_materialize(pairs, leftStore, rightStore, streamLeft, streamRight,
//                start, limit, out) -> i64.
// Materializes up to `limit` pairs starting at `start` into a complete output
// TRowSet allocated inside the kernel (out->Private holds the owners list; the
// caller assigns out->Destroy). streamLeft/streamRight back pairs whose packed
// batch index is -1 (rows probed from a live stream batch); pass any valid
// pointer when no such pairs exist. Returns the number of rows written, 0 when
// the buffer is exhausted (out is left untouched).
using TJoinMaterialize = std::function<int64_t(
    void* pairs,
    TRowSet* leftStore,
    TRowSet* rightStore,
    TRowSet* streamLeft,
    TRowSet* streamRight,
    int64_t start,
    int64_t limit,
    TRowSet* out)>;

// The compiled kernels for one symmetric hash join. Each side's hash map is a
// caller-owned, zero-initialized HashTable buffer (HashTableSize); the output
// pair buffer is a zero-initialized buffer of PairBufferSize. Key-column
// indices are supplied by the bound dispatch closure; join-kind dispatch stays
// baked into the generated jt_dispatch function.
struct TJoinKernels {
    // jt_dispatch(left, right, batch, batchIdx, pairs, leftStore, rightStore,
    //             arena, leftKeyColumns, rightKeyColumns, arg, op) -> bool.
    // The closure binds the key columns and a string arena for the call.
    // Init uses arg as initial capacity. Residual SEMI/ANTI Finalize uses arg
    // as left-store batch count so the kernel can scan selected left rows.
    // UpdateRight/Finalize are specialized by the compiled join type, so host
    // code does not select jt_insert_key_only vs jt_process_right or
    // left/right outer finalization itself.
    TJoinDispatch Dispatch;

    // Output materialization: the per-column gather (types, null masks, string
    // payloads, outer-join null padding) is baked into the generated
    // jt_materialize, so host code only cursors over the pair buffer.
    TJoinMaterialize Materialize;
};

enum class ECrossJoinKernelOp : int64_t {
    Emit = 0,
    Destroy = 1,
    EmitUnmatchedRight = 2,
};

using TCrossJoinDispatch = std::function<bool(
    TRowSet* leftStore,
    int64_t batchIdx,
    TRowSet* rightStore,
    int64_t rightBatchCount,
    uint8_t* rightMatched,
    void* pairs,
    int64_t op)>;

struct TCrossJoinKernels {
    // xj_dispatch(leftStore, batchIdx, rightStore, rightBatchCount,
    //             rightMatched, pairs, arena, op) -> bool, the closure
    //             binding a string arena for the call.
    // Emit appends the canonical row-id pairs of left batch `batchIdx` against
    // the buffered right store, as the compiled join type emits them. RIGHT and
    // FULL joins also flag the matched right rows in rightMatched, one byte per
    // right row in store order, which EmitUnmatchedRight reads after the last
    // left batch. Destroy frees PairBuffer storage.
    TCrossJoinDispatch Dispatch;

    // Same generated materializer as equi-join: cross emits row-id pairs and
    // host code only cursors over PairBuffer.
    TJoinMaterialize Materialize;
};

enum class ERowJoinKernelOp : int64_t {
    Init = 0,
    // Append the selected rows of `batch` (the build side) to the arena.
    Append = 1,
    // Build the pointer table over the arena; no Append may follow.
    Build = 2,
    // Probe `batch` (the probe side) and emit pairs of (stream row id, row
    // pointer) in (left, right) order. The row pointer of an unmatched row of
    // an outer join is null.
    Probe = 3,
    Destroy = 4,
};

// jr_dispatch(arena, table, scratch, pairs, batch, key_columns, op) -> bool.
// The bound closure passes the build-side key columns for Append and the
// probe-side key columns for Probe; the other operations ignore `batch`.
using TRowJoinDispatch = std::function<bool(
    void* arena,
    void* table,
    void* scratch,
    void* pairs,
    TRowSet* batch,
    int64_t op)>;

// jr_materialize(pairs, stream, start, limit, out) -> i64: like jt_materialize,
// but the build side of every pair is a row pointer into the arena and the
// probe side a stream row id of `stream`.
using TRowJoinMaterialize = std::function<int64_t(
    void* pairs,
    TRowSet* stream,
    int64_t start,
    int64_t limit,
    TRowSet* out)>;

// The compiled kernels of an inner or outer equi-join over the row join table
// (kernel/join/join_rows.oz). The caller owns zero-initialized JoinArena,
// JoinTable, JoinScratch and PairBuffer buffers of the sizes below.
struct TRowJoinKernels {
    TRowJoinDispatch Dispatch;
    TRowJoinMaterialize Materialize;
    // String payload rows point into the build batches, which the caller then
    // keeps alive until Destroy.
    bool RetainsBuildBatches = false;
};

struct TJoinHashKernels {
    using TDispatch = std::function<bool(TRowSet* batch, uint64_t* hashes)>;

    TDispatch Left;
    TDispatch Right;
};

// Compiles qumir core-lang kernel sources to LLVM JIT function pointers.
class TKernelCompiler {
public:
    explicit TKernelCompiler(std::ostream* diagnostics = nullptr)
        : TKernelCompiler(TKernelCompilerOptions{.Diagnostics = diagnostics})
    {}

    explicit TKernelCompiler(TKernelCompilerOptions options)
        : Diagnostics_(options.Diagnostics)
        , Stage_(std::move(options.Stage))
        , ExecStageId_(options.ExecStageId)
        , Sink_(options.Sink)
        , BindNow_(options.BindNow)
        , ExternalCatalog_(std::move(options.ExternalCatalog))
        , RuntimeContext_(std::move(options.RuntimeContext))
    {
        Opts_ = KernelRunnerOptions();
        Opts_.NativeCode = true;
        Opts_.EnablePerfJitEventListener = true;
        Opts_.PrintIr = Diagnostics_ != nullptr;
        Opts_.PrintLlvm = Diagnostics_ != nullptr;
    }

    // Filter dispatch: called per batch to fill the selection buffer
    // rowSet.Selection. inputSelection is the mask the batch arrived with
    // (null = every row); rows outside it get 0 and are not evaluated.
    // A plain struct, so a call has no type-erased indirection. The entry
    // point is read from Slot on every call, because JIT finalization fills it
    // after this object is created and copied.
    struct TFilterDispatch {
        void operator()(TRowSet& rowSet, const uint8_t* inputSelection) const;

        std::shared_ptr<TKernelSlot> Slot;
        // Payloads the kernel points into (literal strings).
        std::shared_ptr<void> Storage;
        std::shared_ptr<TRuntimeContext> Runtime;
    };

    // Project dispatch: called per batch; for each computed column k and row i,
    // writes outBuffers[k][i] = <expr_k>. outBuffers has one pointer per computed
    // projection, each to a caller-owned buffer of RowCount * sizeof(computed
    // type). Only computed (non-ident) columns go through this kernel.
    using TProjectDispatch =
        std::function<void(TRowSet* in, void** outBuffers, void* arena)>;

    using TSortRadixCompositeDispatch = std::function<int64_t(
        TRowSet* store, int64_t* rowIds, int64_t* work, uint32_t* counts,
        int64_t n, bool* descs, bool* nullsFirsts, bool doSort,
        int64_t start, int64_t limit, TRowSet* output)>;

    using TSortRadixCompositeNullableDispatch = std::function<int64_t(
        TRowSet* store, int64_t* rowIds, int64_t* work, uint32_t* counts,
        int64_t n, bool* descs, bool* nullsFirsts, bool doSort,
        int64_t start, int64_t limit, TRowSet* output)>;

    using TTopSortDispatch = std::function<int64_t(
        TRowSet* state,
        TRowSet* batch,
        int64_t* rowIds,
        int64_t* work,
        uint32_t* counts,
        int64_t n,
        bool sortInput,
        uint8_t* pickSrc,
        uint32_t* pickIdx,
        int64_t limit,
        TRowSet* output)>;

    using THeapTopKDispatch = std::function<int64_t(
        TRowSet* store,
        int64_t* rowIds,
        int64_t size,
        int64_t limit)>;

    // sizeof(HashTable) per modules/qumirdb.cpp's layout - callers of
    // CompileAggregate must allocate a zero-initialized buffer this large
    // for `ht`.
    static constexpr size_t HashTableSize = 104;

    // The symmetric hash join reuses the aggregation HashTable (HashTableSize)
    // as each side's hash map. sizeof(PairBuffer) per modules/qumirdb.cpp -
    // callers of CompileJoin allocate a zero-initialized buffer this large for
    // the output pair buffer.
    static constexpr size_t PairBufferSize = 24;

    // sizeof(JoinArena) / sizeof(JoinTable) / sizeof(JoinScratch) per
    // modules/qumirdb.cpp - callers of CompileRowJoin allocate zero-initialized
    // buffers this large.
    static constexpr size_t JoinArenaSize = 56;
    static constexpr size_t JoinTableSize = 48;
    static constexpr size_t JoinScratchSize = 64;

    // sizeof(TRowSet) per io.h / modules/qumirdb.oz - generated kernels that
    // pack TRowSet values into a byte buffer (e.g. top-sort's materialize
    // store) size the allocation off this instead of a bare literal.
    static constexpr size_t RowSetSize = 64;

    std::shared_ptr<TKernelSlot> CompileInstrumentation();

    // Compiles a filter kernel for the given input struct type and predicate.
    // Returns a dispatch lambda that calls the compiled kernel.
    TFilterDispatch CompileFilter(const NKernel::TOperatorKernelSpec& spec);

    // Entry points follow RuntimeFilterKernelEntrypoints.
    std::shared_ptr<TKernelSlot> CompileRuntimeFilter();

    // Compiles a project kernel for the computed (non-ident) projections.
    // computedExprs and computedTypes are parallel; computedTypes[k] is the
    // inferred output type of computedExprs[k] (see AnnotateExprType). The
    // kernel writes each expression to its output buffer, casting to that type.
    TProjectDispatch CompileProject(const NKernel::TOperatorKernelSpec& spec);

    TSortRadixCompositeDispatch CompileRadixSortComposite(
        const std::vector<TSortRadixKeyInput>& keys,
        const NQumir::NAst::TStructType* materializeType);
    TSortRadixCompositeNullableDispatch CompileRadixSortCompositeNullable(
        const std::vector<TSortRadixKeyInput>& keys,
        const NQumir::NAst::TStructType* materializeType);
    TTopSortDispatch CompileTopSort(
        const std::vector<TSortRadixKeyInput>& keys,
        const NQumir::NAst::TStructType* materializeType = nullptr);
    THeapTopKDispatch CompileHeapTopK(
        const std::vector<TSortRadixKeyInput>& keys);

    // Window kernel (entry qdb_window_run, reuses the sort dispatch ABI): sorts
    // by `keys` (partition ++ order), then materializes input columns followed
    // by one column per window function.
    TSortRadixCompositeDispatch CompileWindow(
        const std::vector<TSortRadixKeyInput>& keys,
        const NQumir::NAst::TStructType& inputType,
        const std::vector<TSortRadixKeyInput>& partitionKeys,
        const std::vector<TSortRadixKeyInput>& orderKeys,
        const std::vector<TWindowFuncInput>& funcs);

    // Compiles the per-query generic update and finalize programs for `aggs`
    // grouped by `groupKeys`, over rows of `inputType`.
    //
    // Aggregate arguments may be integer/f64/BinInt; String supports MIN/MAX.
    // count(*) has no argument. Unsupported function/type pairs raise TError.
    TAggregateKernels CompileAggregate(
        const NKernel::TOperatorKernelSpec& spec);

    // Compiles the symmetric hash join kernels described by `spec`.
    TJoinKernels CompileJoin(
        const NKernel::TOperatorKernelSpec& spec);

    // Compiles the row join table kernels for an equi-join without a residual
    // whose build side is fixed to the left (buildIsLeft) or right input; the
    // other input is probed as a stream. The join is inner, or LEFT / RIGHT
    // with its null-supplying side built: the preserved probe rows that match
    // nothing are emitted with NULLs for the build side.
    TRowJoinKernels CompileRowJoin(
        const NKernel::TOperatorKernelSpec& spec,
        bool buildIsLeft);

    // Compiles the keyless cross join kernels described by `spec`.
    TCrossJoinKernels CompileCrossJoin(
        const NKernel::TOperatorKernelSpec& spec);

    // Compiles rowset-wide hash helpers for the join keys described by `spec`.
    // Each dispatch fills one uint64 hash per selected input row; the slots of
    // unselected rows are not written and must not be read.
    TJoinHashKernels CompileJoinHash(
        const NKernel::TOperatorKernelSpec& spec);

    // Same rh_hash generator as CompileAggregate's own table, for shuffling.
    std::function<bool(TRowSet*, uint64_t*)> CompileAggregateHash(
        const NQumir::NAst::TStructType& inputType,
        const std::vector<std::string>& groupKeys);

private:
    // Implementation for the equi-key pairs `keys` (parallel left/right column
    // names) over rows of leftType/rightType. String keys are not supported yet.
    //
    // Optional residual filter: when `residualPredicate` is set, it is compiled
    // into the injected `jt_residual_filter` (evaluated per matched pair before
    // emit), reading columns from the inner schema `innerType` (left fields ++
    // right fields, split at `leftFieldCount`). For LeftSemi/LeftAnti with a
    // residual predicate, generated kernels mark matched left row IDs and
    // finalize semi/anti output without host-side scans.
    TJoinKernels CompileJoin(
        const NQumir::NAst::TStructType& leftType,
        const NQumir::NAst::TStructType& rightType,
        const std::vector<TJoinKey>& keys,
        EJoinType type,
        const NQumir::NAst::TExprPtr& residualPredicate = nullptr,
        const NQumir::NAst::TStructType* innerType = nullptr,
        size_t leftFieldCount = 0,
        bool hasPrecomputedHash = false);

    // Wrap a freshly built kernel program: create its slot, append to Sink_,
    // and (if BindNow_) JIT-finalize immediately. Returns the emitted kernel
    // (its Slot backs the caller's dispatch closures).
    TGeneratedKernel EmitKernel(
        std::string name,
        std::vector<std::string> entrypoints,
        NQumir::NAst::TExprPtr ast,
        std::shared_ptr<void> storage = nullptr);

    std::ostream* Diagnostics_ = nullptr;
    std::string Stage_;
    TExecStageId ExecStageId_ = InvalidExecStageId;
    std::vector<TGeneratedKernel>* Sink_ = nullptr;
    bool BindNow_ = true;
    std::shared_ptr<const TExternalCatalogSnapshot> ExternalCatalog_;
    std::shared_ptr<TRuntimeContext> RuntimeContext_;
    NQumir::TLLVMRunnerOptions Opts_;
};

} // namespace NQdb
