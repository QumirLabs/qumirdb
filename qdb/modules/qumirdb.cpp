#include "qumirdb.h"

#include <qdb/modules/qumirdb_runtime.h>

#include <qumir/parser/ast.h>

namespace NQumir {
namespace NRegistry {

using namespace NAst;

const void* const QumirDbRuntimeSymbolAnchors[] __attribute__((used)) = {
    reinterpret_cast<const void*>(&qdb_alloc),
    reinterpret_cast<const void*>(&qdb_free),
    reinterpret_cast<const void*>(&qdb_filter_string_compare),
    reinterpret_cast<const void*>(&qdb_string_hash_bytes),
    reinterpret_cast<const void*>(&qdb_string_view_sql_like),
    reinterpret_cast<const void*>(&qdb_like_equals),
    reinterpret_cast<const void*>(&qdb_like_prefix),
    reinterpret_cast<const void*>(&qdb_like_suffix),
    reinterpret_cast<const void*>(&qdb_like_contains),
    reinterpret_cast<const void*>(&qdb_substring),
    reinterpret_cast<const void*>(&qdb_string_concat),
    reinterpret_cast<const void*>(&qdb_string_arena_reset),
    reinterpret_cast<const void*>(&qdb_string_arena_alloc),
    reinterpret_cast<const void*>(&qdb_regexp_replace),
    reinterpret_cast<const void*>(&qdb_date_year),
    reinterpret_cast<const void*>(&qdb_sql_date),
    reinterpret_cast<const void*>(&qdb_sql_interval),
    reinterpret_cast<const void*>(&qdb_round),
    reinterpret_cast<const void*>(&qdb_abs_i8),
    reinterpret_cast<const void*>(&qdb_abs_i16),
    reinterpret_cast<const void*>(&qdb_abs_i32),
    reinterpret_cast<const void*>(&qdb_abs_i64),
    reinterpret_cast<const void*>(&qdb_fabs),
    reinterpret_cast<const void*>(&qdb_decimal64_overflow),
    reinterpret_cast<const void*>(&qdb_decimal_from_i64),
    reinterpret_cast<const void*>(&qdb_decimal_from_f64),
    reinterpret_cast<const void*>(&qdb_decimal_scale_up),
    reinterpret_cast<const void*>(&qdb_decimal_add),
    reinterpret_cast<const void*>(&qdb_decimal_sub),
    reinterpret_cast<const void*>(&qdb_decimal_neg),
    reinterpret_cast<const void*>(&qdb_decimal_mul_i64),
    reinterpret_cast<const void*>(&qdb_decimal_div_i64),
    reinterpret_cast<const void*>(&qdb_decimal_div),
    reinterpret_cast<const void*>(&qdb_decimal_eq),
    reinterpret_cast<const void*>(&qdb_decimal_ne),
    reinterpret_cast<const void*>(&qdb_decimal_lt),
    reinterpret_cast<const void*>(&qdb_decimal_le),
    reinterpret_cast<const void*>(&qdb_decimal_gt),
    reinterpret_cast<const void*>(&qdb_decimal_ge),
};

namespace {

using TFields = std::vector<std::pair<std::string, TTypePtr>>;

TTypePtr ColumnType(TTypeFactory& types) {
    auto i32Type = types.MakeInteger(TIntegerType::I32);
    auto i64Type = types.MakeInteger();
    auto u8Type = types.MakeInteger(TIntegerType::U8);
    return types.MakeStruct(TFields{
        {"Data", types.MakePointer(types.MakeInteger(TIntegerType::I8))},
        {"DataBitOffset", i32Type},
        {"Mask", types.MakePointer(u8Type)},
        {"MaskBitOffset", i32Type},
        {"Offsets", types.MakePointer(i64Type)},
        {"OffsetWidth", u8Type},
    });
}

TTypePtr RowSetType(TTypeFactory& types) {
    auto i64Type = types.MakeInteger();
    auto ptrU8Type = types.MakePointer(types.MakeInteger(TIntegerType::U8));
    auto voidPtrType = types.MakePointer(i64Type);
    auto ptrColumnType = types.MakePointer(types.MakeNamed("TColumn", ColumnType(types)));
    return types.MakeStruct(TFields{
        {"Columns", ptrColumnType},
        {"ColumnCount", i64Type},
        {"RowCount", i64Type},
        {"Selection", ptrU8Type},
        {"Hash", types.MakePointer(types.MakeInteger(TIntegerType::U64))},
        {"Destroy", voidPtrType},
        {"Private", voidPtrType},
        {"RefCount", i64Type},
    });
}

TTypePtr StringHandleType(TTypeFactory& types) {
    return types.MakeStruct(TFields{
        {"Data", types.MakePointer(types.MakeInteger(TIntegerType::U8))},
        {"Size", types.MakeInteger()},
    });
}

TTypePtr PairBufferType(TTypeFactory& types) {
    auto i64Type = types.MakeInteger();
    return types.MakeStruct(TFields{
        {"Count", i64Type},
        {"Capacity", i64Type},
        {"Data", types.MakePointer(i64Type)},
    });
}

// Row table (kernel/row_table.oz); mirrored by TRowJoinProcessor's
// state structs in exec/row_join_exec.h.
TTypePtr RowArenaType(TTypeFactory& types) {
    auto i64Type = types.MakeInteger();
    auto ptrPtrU8Type = types.MakePointer(types.MakePointer(types.MakeInteger(TIntegerType::U8)));
    return types.MakeStruct(TFields{
        {"Blocks", ptrPtrU8Type},
        {"BlockCount", i64Type},
        {"BlockCapacity", i64Type},
        {"BlockRows", i64Type},
        {"RowBytes", i64Type},
        {"Rows", i64Type},
        {"CurRows", i64Type},
    });
}

TTypePtr RowTableType(TTypeFactory& types) {
    auto i64Type = types.MakeInteger();
    auto u64Type = types.MakeInteger(TIntegerType::U64);
    return types.MakeStruct(TFields{
        {"Entries", types.MakePointer(u64Type)},
        {"Capacity", i64Type},
        {"Mask", u64Type},
        {"Shift", i64Type},
        {"Rows", i64Type},
        {"Keys", i64Type},
    });
}

TTypePtr RowScratchType(TTypeFactory& types) {
    auto i64Type = types.MakeInteger();
    auto ptrU64Type = types.MakePointer(types.MakeInteger(TIntegerType::U64));
    auto ptrI64Type = types.MakePointer(i64Type);
    return types.MakeStruct(TFields{
        {"Hashes", ptrU64Type},
        {"Salts", ptrU64Type},
        {"Offsets", ptrU64Type},
        {"Entries", ptrU64Type},
        {"Rows", ptrI64Type},
        {"Work", ptrI64Type},
        {"Keys", types.MakePointer(types.MakeInteger(TIntegerType::U8))},
        {"Capacity", i64Type},
    });
}

TTypePtr HashTableType(TTypeFactory& types) {
    auto i64Type = types.MakeInteger();
    auto ptrU8Type = types.MakePointer(types.MakeInteger(TIntegerType::U8));
    auto ptrI64Type = types.MakePointer(i64Type);
    auto ptrPtrU8Type = types.MakePointer(ptrU8Type);
    return types.MakeStruct(TFields{
        {"Keys", ptrU8Type},
        {"Ctrl", ptrU8Type},
        {"SlotId", ptrI64Type},
        {"GroupKeys", ptrU8Type},
        {"AggBuffers", types.MakePointer(ptrI64Type)},
        {"OwnedBlocks", ptrPtrU8Type},
        {"OwnedBlockCount", i64Type},
        {"OwnedBlockCapacity", i64Type},
        {"Capacity", i64Type},
        {"Size", i64Type},
        {"NumAggs", i64Type},
        {"NumKeys", i64Type},
        {"KeySize", i64Type},
        {"RowArena", types.MakeNamed("RowArena", RowArenaType(types))},
        {"RowTable", types.MakeNamed("RowTable", RowTableType(types))},
        {"RowScratch", types.MakeNamed("RowScratch", RowScratchType(types))},
        {"RowSwitchGroups", i64Type},
        {"RowExpectedGroups", i64Type},
        {"RowRebuilds", i64Type},
        {"RowMovedFromSlots", i64Type},
    });
}

struct TTypeBuilder {
    std::string_view Name;
    TTypePtr (*Build)(TTypeFactory&);
};

// The types qumirdb.oz declares, built on demand.
constexpr TTypeBuilder QumirDbTypeBuilders[] = {
    {"TColumn", ColumnType},
    {"TRowSet", RowSetType},
    {"HashTable", HashTableType},
    {"PairBuffer", PairBufferType},
    {"RowArena", RowArenaType},
    {"RowTable", RowTableType},
    {"RowScratch", RowScratchType},
    {"StringView", StringHandleType},
    {"OwnedString", StringHandleType},
};

} // namespace

std::vector<TExternalType> QumirDbExternalTypes(TTypeFactory& types) {
    std::vector<TExternalType> result;
    for (const auto& builder : QumirDbTypeBuilders) {
        result.push_back({ .Name = std::string(builder.Name), .Type = builder.Build(types) });
    }
    return result;
}

NAst::TTypePtr QumirDbNamedType(NAst::TTypeFactory& types, std::string_view name) {
    for (const auto& builder : QumirDbTypeBuilders) {
        if (builder.Name == name) {
            return types.MakeNamed(std::string(name), builder.Build(types));
        }
    }
    throw std::logic_error("qumirdb.oz declares no type " + std::string(name));
}

void EnsureQumirDbRuntimeSymbolsLinked() {
    (void)QumirDbRuntimeSymbolAnchors;
}

} // namespace NRegistry
} // namespace NQumir
