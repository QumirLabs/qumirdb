#pragma once

#include <qdb/kernel/join_key.h>
#include <qdb/plan/ops/join.h>

#include <qumir/parser/ast.h>
#include <qumir/parser/type.h>

#include <string>

namespace NQdb::NKernel {

// Generates jt_insert_key_only: iterates a batch, extracts the key, and
// inserts only the key into the own table (no row-ID storage via jb_append).
// Used for the right side of SEMI/ANTI joins. Signature is identical to
// GenJoinProcessAst so the same TProcessFn ABI can be reused; `opp` and
// `pairs` parameters are present but unused in the generated body.
NQumir::NAst::TExprPtr GenJoinInsertKeyOnlyAst(
    const TJoinKeyDescriptor& key,
    const std::string& funcName,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr hashTableType,
    NQumir::NAst::TTypePtr pairBufferType,
    NQumir::NAst::TTypePtr stringViewType);

// Generates jt_finalize_semi / jt_finalize_anti: iterates own.GroupKeys,
// probes opp for each key, and pushes matching (SEMI) or non-matching (ANTI)
// left RowIds to pairs (right_row_id = -1; jt_materialize ignores it because
// semi/anti output schema contains only left columns).
NQumir::NAst::TExprPtr GenJoinFinalizeSemiAntiAst(
    const TJoinKeyDescriptor& key,
    bool isAnti,
    const std::string& funcName,
    NQumir::NAst::TTypePtr hashTableType,
    NQumir::NAst::TTypePtr pairBufferType);

// Generates one side's process function (jt_process_left / jt_process_right):
// reads the side's key columns, assembles the <named Key>, and calls the
// generic jt_emit_and_insert for each selected row. Reuses BuildColumnValueAst
// for the per-row key materialization (same path as the aggregation dispatch).
//
// Signature of the generated function:
//   <funcName>(own: <ref HashTable>, opp: <ref HashTable>,
//              batch: <ref TRowSet>, key_columns: <ptr i64>,
//              batch_idx: i64, pairs: <ref PairBuffer>) -> bool
//
// hasPrecomputedHash: read batch.Hash[i] instead of calling rh_hash(key) -
// set when this batch comes straight from a hash shuffle keyed on the same
// join columns (plan_lowerer.cpp decides this). markMatched marks the matched
// rows of both tables, for a FULL join with a residual.
NQumir::NAst::TExprPtr GenJoinProcessAst(
    const TJoinKeyDescriptor& key,
    bool isLeft,
    const std::string& funcName,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr hashTableType,
    NQumir::NAst::TTypePtr pairBufferType,
    NQumir::NAst::TTypePtr stringViewType,
    bool hasPrecomputedHash = false,
    bool markMatched = false);

// Generates one side's insert-only function (same ABI as GenJoinProcessAst):
// reads keys from the batch and inserts row ids into the own table without
// probing the opposite table. Used by residual SEMI/ANTI for the left build.
NQumir::NAst::TExprPtr GenJoinInsertRowsOnlyAst(
    const TJoinKeyDescriptor& key,
    bool isLeft,
    const std::string& funcName,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr hashTableType,
    NQumir::NAst::TTypePtr pairBufferType,
    NQumir::NAst::TTypePtr stringViewType,
    bool hasPrecomputedHash = false);

// Generates one side's probe-only function (jt_probe_left_stream /
// jt_probe_right_stream): reads stream batch keys, probes the already-built
// opposite table, and emits pairs without inserting stream rows into any table.
// markMatched also marks the matched build rows, for a streaming outer join
// that emits the unmarked ones at EOF.
NQumir::NAst::TExprPtr GenJoinProbeAst(
    const TJoinKeyDescriptor& key,
    bool isLeft,
    const std::string& funcName,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr hashTableType,
    NQumir::NAst::TTypePtr pairBufferType,
    NQumir::NAst::TTypePtr stringViewType,
    bool hasPrecomputedHash = false,
    bool markMatched = false);

NQumir::NAst::TExprPtr GenJoinProbeSemiAst(
    const TJoinKeyDescriptor& key,
    bool isAnti,
    const std::string& funcName,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr hashTableType,
    NQumir::NAst::TTypePtr pairBufferType,
    NQumir::NAst::TTypePtr stringViewType,
    bool hasPrecomputedHash = false);

// Generates the mark-join left stream probe (same ABI as GenJoinProbeSemiAst):
// probes the complete right key set and pushes (left row id, marker code) for
// every selected row, NULL keys included (join_mark.oz).
NQumir::NAst::TExprPtr GenMarkJoinProbeAst(
    const TJoinKeyDescriptor& key,
    const std::string& funcName,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr hashTableType,
    NQumir::NAst::TTypePtr pairBufferType,
    NQumir::NAst::TTypePtr stringViewType,
    bool hasPrecomputedHash = false);

// Generates residual SEMI/ANTI right-side probe: reads right batch keys, probes
// the left build table, applies jt_residual_filter, and marks matched left row
// ids in the matched-id table instead of emitting output pairs.
NQumir::NAst::TExprPtr GenJoinProbeMarkAst(
    const TJoinKeyDescriptor& key,
    bool isLeft,
    const std::string& funcName,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr hashTableType,
    NQumir::NAst::TTypePtr pairBufferType,
    NQumir::NAst::TTypePtr stringViewType,
    bool hasPrecomputedHash = false);

// Generates the cacheable rowset hash worker for shuffle:
//   <funcName>(batch: <ref TRowSet>, hashes: <ptr u64>,
//              key_columns: <ptr i64>, witness: <ptr LookupKey>) -> bool
// Fills hashes[i] for every row i in batch.Selection (null = every row) and
// leaves the other slots as the caller passed them, so a reader must check
// the selection before it reads a hash. The unused witness makes the physical
// key type part of automatic cache mangling.
// Operator-neutral; GenJoinHashBatchAst below shims join keys into it.
NQumir::NAst::TExprPtr GenKeyHashBatchAst(
    const TAggregateKeyDescriptor& key,
    const std::string& funcName,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr stringViewType);

NQumir::NAst::TExprPtr GenJoinHashBatchAst(
    const TJoinKeyDescriptor& key,
    const std::string& funcName,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr stringViewType);

// Thin entrypoint forwarding to the batch function named `batchFuncName`.
NQumir::NAst::TExprPtr GenKeyHashEntrypointAst(
    const TAggregateKeyDescriptor& key,
    const std::string& funcName,
    const std::string& batchFuncName,
    NQumir::NAst::TTypePtr rowSetType);

// Thin wrapper: forwards to "jt_hash_batch" (join's fixed batch func name).
NQumir::NAst::TExprPtr GenJoinHashEntrypointAst(
    const TJoinKeyDescriptor& key,
    const std::string& funcName,
    NQumir::NAst::TTypePtr rowSetType);

// Generates the rh_hash / rh_key_equal overloads for the join key type, reusing
// the aggregation key-ops generator (GenKeyOperationFunDecls).
std::vector<NQumir::NAst::TExprPtr> GenJoinKeyOpsFunDecls(const TJoinKeyDescriptor& key);

// Generates the key_owned_bytes / key_clone_owned overloads for the join key
// type (owned-string cloning), reusing the aggregation ownership generator.
std::vector<NQumir::NAst::TExprPtr> GenJoinKeyOwnershipFunDecls(
    const TJoinKeyDescriptor& key);

// Emits the (type ...) declarations that make the named Key type(s) known to
// the compiler. Must be prepended to the program before any use of the Key.
std::vector<NQumir::NAst::TExprPtr> GenKeyTypeDecls(const TAggregateKeyDescriptor& key);
std::vector<NQumir::NAst::TExprPtr> GenJoinKeyTypeDecls(const TJoinKeyDescriptor& key);

// Output columns of the join materializer: the left fields, followed by the
// right fields, by the mark-join marker, or by nothing (semi/anti).
enum class EJoinOutputColumns {
    Left,
    LeftRight,
    LeftMarker,
};

// Generates the output materializer entrypoint:
//   jt_materialize(pairs, left_store, right_store, stream_left, stream_right,
//                  start, limit, out) -> i64
// Gathers up to `limit` pairs beginning at `start` from the pair buffer into a
// complete output TRowSet: allocates every buffer with qdb_alloc (recorded in
// an owners list stored in out.Private), decodes packed row ids (id == -1 is
// outer-join null padding; batch index -1 reads the stream_* batch instead of
// the store), and calls cacheable physical-type workers for fixed-width, bool,
// BinInt, and string columns. `output` selects the columns; the marker column
// reads the marker code from the right half of each pair. Source column
// positions and pair sides are runtime worker arguments, while fixed-width
// worker types are selected through a pointer witness. Returns the number of
// rows materialized (0 leaves `out` untouched). The caller assigns out.Destroy
// (the kernel sets it to null).
NQumir::NAst::TExprPtr GenJoinMaterializeAst(
    const NQumir::NAst::TStructType& leftType,
    const NQumir::NAst::TStructType& rightType,
    EJoinOutputColumns output,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr pairBufferType,
    NQumir::NAst::TTypePtr stringViewType);

// Generates the single external join entrypoint:
//   jt_dispatch(left, right, batch, batch_idx, pairs, left_store, right_store,
//               left_key_columns, right_key_columns, arg, op) -> bool
// It dispatches init/update/stream/finalize/destroy to the internal generated
// and library helpers. Join type and key layout are compile-time constants.
//
// LeftMark: UpdateLeft stores left rows, UpdateRight builds the key-only right
// set. A marker needs the complete right set, so StreamLeft and Finalize may
// run only after the last UpdateRight. StreamRight, probing right rows against
// stored left rows without keeping them, is unsupported and returns false.
NQumir::NAst::TExprPtr GenJoinDispatchAst(
    const TJoinKeyDescriptor& key,
    EJoinType type,
    bool hasResidual,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr hashTableType,
    NQumir::NAst::TTypePtr pairBufferType);

// Row join table (kernel/join/join_rows.oz): the layout of one build row.
// Every field of the build side is a payload column, in field order; the
// LookupKey struct sits at KeyOffset, after the hash/next word.
struct TRowJoinColumn {
    enum class EKind {
        Fixed,
        Bool,
        BinInt,
        String,
    };

    EKind Kind = EKind::Fixed;
    int32_t SrcColIdx = 0;
    int64_t Offset = 0;
    // Byte offset of the validity byte; -1 for a column that cannot be NULL.
    int64_t ValidOffset = -1;
    int64_t FixedWidth = 0;
    NQumir::NAst::TTypePtr StorageType;
};

struct TRowJoinLayout {
    int64_t KeyOffset = 8;
    int64_t RowBytes = 0;
    bool HasString = false;
    std::vector<TRowJoinColumn> Columns;
};

TRowJoinLayout BuildRowJoinLayout(
    const NQumir::NAst::TStructType& buildType,
    const TJoinKeyDescriptor& key);

// External types the row join generators refer to by name.
struct TRowJoinGenTypes {
    NQumir::NAst::TTypePtr Column;
    NQumir::NAst::TTypePtr RowSet;
    NQumir::NAst::TTypePtr PairBuffer;
    NQumir::NAst::TTypePtr StringView;
    NQumir::NAst::TTypePtr Arena;
    NQumir::NAst::TTypePtr Table;
    NQumir::NAst::TTypePtr Scratch;
};

// jr_append_batch(arena, batch, key_columns) -> bool: appends every selected
// row with a valid key to the arena with its hash, key and payload.
NQumir::NAst::TExprPtr GenRowJoinAppendAst(
    const TJoinKeyDescriptor& key,
    const TRowJoinLayout& layout,
    const std::string& funcName,
    const TRowJoinGenTypes& types,
    bool hasPrecomputedHash);

// jr_build_table(table, arena) -> bool: sizes the pointer table for the arena
// and inserts every row, chaining rows with equal keys.
NQumir::NAst::TExprPtr GenRowJoinBuildAst(
    const TJoinKeyDescriptor& key,
    const TRowJoinLayout& layout,
    const std::string& funcName,
    const TRowJoinGenTypes& types);

// jr_probe_batch(table, scratch, batch, key_columns, pairs) -> bool: the
// three-pass probe (hashes, salted entry lookup, key comparison of the
// candidates) that pushes one (left, right) pair per matching build row.
NQumir::NAst::TExprPtr GenRowJoinProbeAst(
    const TJoinKeyDescriptor& key,
    const TRowJoinLayout& layout,
    bool probeIsLeft,
    const std::string& funcName,
    const TRowJoinGenTypes& types,
    bool hasPrecomputedHash);

// jr_dispatch(arena, table, scratch, pairs, batch, key_columns, op) -> bool
// (ERowJoinKernelOp).
NQumir::NAst::TExprPtr GenRowJoinDispatchAst(
    const TRowJoinLayout& layout,
    const TRowJoinGenTypes& types);

// jr_materialize(pairs, stream, start, limit, out) -> i64: the probe side's
// columns come from the stream batch through the jt_materialize_* workers,
// the build side's from the rows through the jr_gather_* workers.
NQumir::NAst::TExprPtr GenRowJoinMaterializeAst(
    const NQumir::NAst::TStructType& leftType,
    const NQumir::NAst::TStructType& rightType,
    bool buildIsLeft,
    const TRowJoinLayout& layout,
    const TRowJoinGenTypes& types);

// Generates the nested-loop (keyless) join entrypoint:
//   xj_dispatch(left_store, batch_idx, right_store, right_batch_count,
//               right_matched, pairs, op) -> bool
// It passes the emission flags of `type` (Inner, Left, Right, Full,
// LeftSemi or LeftAnti) to the join_cross.oz helpers as constants.
NQumir::NAst::TExprPtr GenCrossJoinDispatchAst(
    EJoinType type,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr pairBufferType);

} // namespace NQdb::NKernel
