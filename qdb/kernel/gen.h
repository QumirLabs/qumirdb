#pragma once

#include <qdb/kernel/aggregate_key.h>

#include <qumir/parser/ast.h>
#include <qumir/parser/type.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace NQdb {
namespace NKernel {

enum class EAggValueKind {
    Int64,
    Float64,
    BinInt,
    String,
    // FALSE < TRUE: the state is the i64 0/1, the output a bitmap column.
    Bool,
};

// Per-reducer layout for the generic aggregation kernel. One TAggReducerInfo
// per requested aggregate (same order/size as `funcs`/`aggs`); ValidBufIdx
// (if >= 0) names an extra internal AggBuffers slot ("valid count") used to
// skip NULL reducer arguments and to mark NULL output for sum/min/max when a
// group has zero non-null contributions. NumAggBuffers is the total number of
// physical AggBuffers slots the hash table must allocate.
//
// BinInt uses two i64 buffers (Lo/Hi). String MIN/MAX also use two buffers:
// owned pointer and packed size/capacity metadata.
struct TAggReducerInfo {
    std::string Func;
    bool HasArg = false;            // agg.Arg != nullptr
    bool NeedsValidity = false;     // arg nullable && HasArg
    bool IsNullableOutput = false;  // NeedsValidity && Func in {sum,min,max}
    int ValueBufIdx = -1;           // main value state buffer
    int ExtraBufIdx = -1;           // BinInt high word or string metadata
    int ValidBufIdx = -1;           // -1 if IsNullableOutput == false
    EAggValueKind ValueKind = EAggValueKind::Int64;
    int ArgColumnIndex = -1;        // input column index of this agg's arg; -1 = count(*)

    bool IsFloat() const {
        return ValueKind == EAggValueKind::Float64;
    }

    bool IsBinInt() const {
        return ValueKind == EAggValueKind::BinInt;
    }

    bool IsString() const {
        return ValueKind == EAggValueKind::String;
    }

    bool IsBool() const {
        return ValueKind == EAggValueKind::Bool;
    }
};

// Per-aggregate argument column descriptor (parallel to funcs). ColumnIndex -1
// means no argument (count(*)). Aggregates may reference different columns.
struct TAggArg {
    int ColumnIndex = -1;
    EAggValueKind ValueKind = EAggValueKind::Int64;
    bool IsNullable = false;
};

struct TAggReducerLayout {
    std::vector<TAggReducerInfo> Reducers;
    size_t NumAggBuffers = 0;
};

// Computes the AggBuffers layout for `funcs`/`hasArg` given whether the
// shared reducer argument column is nullable. When argIsNullable is false,
// NumAggBuffers == funcs.size() and every TAggReducerInfo::NeedsValidity is
// false, preserving the existing one-buffer-per-reducer layout exactly.
TAggReducerLayout BuildAggReducerLayout(
    const std::vector<std::string>& funcs,
    const std::vector<TAggArg>& args);

// Avoid re-wrapping an already same-named type (would self-reference).
inline NQumir::NAst::TTypePtr AsNamed(
    const std::string& name, const NQumir::NAst::TTypePtr& type)
{
    using namespace NQumir::NAst;
    if (auto named = TMaybeType<TNamedType>(type); named && named.Cast()->Name == name) {
        return type;
    }
    return std::make_shared<TNamedType>(name, type);
}

// Generates concrete hash and equality overloads for lookup and stored keys.
std::vector<NQumir::NAst::TExprPtr> GenKeyOperationFunDecls(
    const TAggregateKeyDescriptor& key);

// Generates key_owned_bytes(LookupKey) and key_clone_owned(LookupKey, u8*).
std::vector<NQumir::NAst::TExprPtr> GenKeyOwnershipFunDecls(
    const TAggregateKeyDescriptor& key);

// Builds a zero value of `type`: 0 / 0.0 / false / null pointer, or a struct
// with every field zeroed. Used for the stored_witness argument that binds the
// StoredKey template type at dual-key call sites.
NQumir::NAst::TExprPtr ZeroValueExpr(const NQumir::NAst::TTypePtr& type);

void SubstFieldsInPlace(
    NQumir::NAst::TExprPtr& expr,
    const std::unordered_set<std::string>& fieldNames,
    const NQumir::NAst::TExprPtr& indexIdent);

// Builds a vectorized filter kernel that takes (ref TRowSet) directly.
// fieldIndices maps each field name to its index in TRowSet.Columns.
// Signature: void <kernel>(rowSet, arena, regexHandles, inputSelection).
// rowSet.Selection is the output mask and inputSelection the mask the batch
// arrived with (null = every row); the two must not overlap. A row outside
// inputSelection gets 0 and its columns are not read.
NQumir::NAst::TExprPtr GenFilterKernelAst(
    NQumir::NAst::TExprPtr predicate,
    const NQumir::NAst::TStructType& inputType,
    const std::unordered_map<std::string, int32_t>& fieldIndices,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr stringViewType,
    std::vector<std::shared_ptr<std::string>>& literalStorage);

// Builds `jt_residual_filter` evaluating `predicate` on one (left,right) row
// pair, replacing the always-true default (join/join_residual_default.oz) in the
// kernel program. Signature:
//   jt_residual_filter(left_store: <ptr TRowSet>, right_store: <ptr TRowSet>,
//                      __arena__: <ptr i8>,
//                      stream_left_batch: <ref TRowSet>,
//                      stream_right_batch: <ref TRowSet>,
//                      left_row_id: i64, right_row_id: i64) -> bool
// innerType is the inner join schema (left fields ++ right fields);
// leftFieldCount splits the two sides. Columns are read from the row stores by
// decoding the packed row IDs; batch index -1 reads from the corresponding
// stream batch pointer. Reuses BuildColumnValueAst + the filter truth helpers
// (same path as GenFilterKernelAst). `__arena__` holds the strings the predicate
// builds (`||`); they are released before the filter returns.
NQumir::NAst::TExprPtr GenJoinResidualFilterAst(
    NQumir::NAst::TExprPtr predicate,
    const NQumir::NAst::TStructType& innerType,
    size_t leftFieldCount,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr stringViewType);

// Builds a vectorized project kernel for the COMPUTED columns only.
// Signature: void <kernel>(rowSet, out, arena, regexHandles) - for each
// row i and each computed projection k: out[k][i] = (cast) <expr_k>. Output
// buffer k holds values of computedTypes[k]; the explicit cast pins the buffer
// type. ident projections are NOT here (handled zero-copy by project_exec).
NQumir::NAst::TExprPtr GenProjectKernelAst(
    std::vector<NQumir::NAst::TExprPtr> computedExprs,
    const std::vector<NQumir::NAst::TTypePtr>& computedTypes,
    const NQumir::NAst::TStructType& inputType,
    const std::unordered_map<std::string, int32_t>& fieldIndices,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr stringViewType,
    std::vector<std::shared_ptr<std::string>>& literalStorage);

// Byte layout of a group in the row layout of the aggregate table
// (kernel/aggregation/aggregation_rows.oz): the hash word, the key at
// KeyOffset, then one i64 state word per AggBuffers slot from StateOffset.
// LLVM aligns a key that holds a decimal (an i128) to 16 bytes and may load it
// with instructions that require that alignment, so such a key starts at a
// multiple of 16 in the row and in the lookup scratch, and rows and scratch
// keys keep it by their strides, multiples of KeyAlignment.
struct TAggRowLayout {
    int64_t KeyAlignment = 8;
    int64_t KeyOffset = 0;
    // The stride of the keys in the lookup scratch.
    int64_t KeyStride = 0;
    int64_t StateOffset = 0;
    int64_t RowBytes = 0;
};

// The row layout of an aggregate, or nullopt when it cannot have one: its key
// or a reducer holds a string, or it has groups that exist over an empty input.
std::optional<TAggRowLayout> BuildAggRowLayout(
    const TAggregateKeyDescriptor& key,
    const TAggReducerLayout& layout,
    bool hasEmptyInputGroups);

// Builds the generic aggregation dispatch entry over aht_init/aht_upsert_dual/
// aht_destroy. Key extraction uses the common TColumn materializer and the
// descriptor's borrowed LookupType/stored StoredType representations. Aggregate
// aht_destroy. Each reducer reads its own argument column (layout's
// ArgColumnIndex), so different aggregates may aggregate different columns; the
// reducer applications are inlined per row with per-reducer values.
// hasPrecomputedHash: read batch.Hash[i] instead of calling rh_hash(key) -
// set when the caller knows this batch comes straight from a hash shuffle
// keyed on the same group keys (plan_lowerer.cpp decides this).
// emptyInputGroups: ids (leading integer key values) of the groups the init op creates
// before any row arrives, with all other key columns NULL; see TOperatorKernelSpec.
// rows: the row layout, when the aggregate has one. The dispatch then also
// serves InitRows, SwitchToRowsAt and ExpectRowGroups, sends the batches of a
// table in that layout to agg_rows_update, and moves a swiss table there when
// it must grow past its RowSwitchGroups.
NQumir::NAst::TExprPtr GenGenericAggregateDispatchAst(
    const NQumir::NAst::TStructType& inputType,
    const TAggregateKeyDescriptor& key,
    const TAggReducerLayout& layout,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr hashTableType,
    bool hasPrecomputedHash = false,
    const std::vector<int64_t>& emptyInputGroups = {},
    const std::optional<TAggRowLayout>& rows = std::nullopt);

// Builds agg_rows_update(ht, batch, start) -> i64, the update of a table in
// the row layout from batch row `start` on: chunks of selected rows go through
// agr_find_or_insert, then each row is applied to its group's states. Returns
// 0, or -1 when an allocation fails.
NQumir::NAst::TExprPtr GenGenericAggregateRowsUpdateAst(
    const NQumir::NAst::TStructType& inputType,
    const TAggregateKeyDescriptor& key,
    const TAggReducerLayout& layout,
    const TAggRowLayout& rows,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr hashTableType,
    bool hasPrecomputedHash);

// Builds the byte-oriented finalize ABI. C++ supplies one opaque u8 destination
// per logical key field; the generated wrapper casts each destination to its
// concrete pointer type and projects dense AoS Key values into SoA columns.
// With a row layout, a table in that layout is projected by a scan of its
// arena.
NQumir::NAst::TExprPtr GenGenericAggregateFinalizeAst(
    const TAggregateKeyDescriptor& key,
    const TAggReducerLayout& layout,
    NQumir::NAst::TTypePtr hashTableType,
    NQumir::NAst::TTypePtr columnType = nullptr,
    const std::optional<TAggRowLayout>& rows = std::nullopt);

// Builds agg_finish_rowset(ht, out_rowset) -> i64. The generated function
// allocates output TColumn buffers with qdb_alloc, calls
// agg_measure_outputs/agg_finalize, writes a complete TRowSet into out_rowset, and
// destroys ht before returning. rowLayout: the aggregate has a row layout, so
// ht may be in either layout.
NQumir::NAst::TExprPtr GenGenericAggregateFinishRowSetAst(
    const TAggregateKeyDescriptor& key,
    const TAggReducerLayout& layout,
    NQumir::NAst::TTypePtr hashTableType,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType,
    bool rowLayout = false);

// Measures output Data bytes for key columns and aggregate values. Fixed-width
// outputs report row_count * width; strings report their stored byte lengths.
// Returns ht.Size or -1 on capacity failure.
NQumir::NAst::TExprPtr GenGenericAggregateMeasureAst(
    const TAggregateKeyDescriptor& key,
    const TAggReducerLayout& layout,
    NQumir::NAst::TTypePtr hashTableType);

// Generates N = funcs.size() FunDecls named reduce_0..reduce_{N-1}, each with
// the predefined reducer contract from PLAN_AGGREGATION.md section 4:
//   reduce_i(i64 prev, i64 value, bool is_new) -> i64
// funcs[i] selects the body ("count"|"sum"|"min"|"max"), generated under the
// positional name reduce_i. N is exactly funcs.size(), not a fixed builtin set.
//
// The result is meant for static injection via MergeKernelLibrary before
// kernel compilation: generated table code calls reduce_0..reduce_{N-1}
// directly by name (no function pointers, no runtime dispatch on Func).
std::vector<NQumir::NAst::TExprPtr> GenReducerFunDecls(
    const TAggReducerLayout& layout);

// Generates a single FunDecl:
//   agg_apply_reducers(<ptr <ptr i64>> agg_buffers, i64 dense_slot,
//                       i64 value, bool is_new)
// which for i in [0, numReducers) does
//   agg_buffers[i][dense_slot] = reduce_i(agg_buffers[i][dense_slot], value, is_new)
// - numReducers static, direct, by-name calls to the reduce_0..reduce_{N-1}
// functions from GenReducerFunDecls.
//
// This is the one piece of per-query generated code that the NumAggs-generic
// table library (agg_init/agg_rehash/agg_update/agg_destroy/agg_finalize,
// L2b) calls by static name to update all N aggregate buffers for one dense
// slot. Everything else in that library is agg-count-agnostic and driven by
// ht.NumAggs/ht.AggBuffers at runtime.
NQumir::NAst::TExprPtr GenApplyReducersFunDecl(const TAggReducerLayout& layout);

} // namespace NKernel
} // namespace NQdb
