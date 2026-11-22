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

std::vector<TExternalType> QumirDbExternalTypes(TTypeFactory& types) {
    auto i8Type = types.MakeInteger(TIntegerType::I8);
    auto i32Type = types.MakeInteger(TIntegerType::I32);
    auto i64Type = types.MakeInteger();
    auto u8Type = types.MakeInteger(TIntegerType::U8);
    auto voidPtrType = types.MakePointer(i64Type);
    auto ptrU8Type = types.MakePointer(u8Type);
    auto ptrI8Type = types.MakePointer(i8Type);

    auto makeStringHandleType = [&]() {
        return types.MakeStruct(
            std::vector<std::pair<std::string, TTypePtr>>{
                {"Data", ptrU8Type},
                {"Size", i64Type},
            });
    };
    auto stringViewType = makeStringHandleType();
    auto ownedStringType = makeStringHandleType();

    auto columnType = types.MakeStruct(
        std::vector<std::pair<std::string, TTypePtr>>{
            {"Data", ptrI8Type},
            {"DataBitOffset", i32Type},
            {"Mask", ptrU8Type},
            {"MaskBitOffset", i32Type},
            {"Offsets", voidPtrType},
            {"OffsetWidth", u8Type},
        });
    auto columnNamedType = types.MakeNamed("TColumn", columnType);
    auto ptrColumnType = types.MakePointer(columnNamedType);

    auto u64Type = types.MakeInteger(TIntegerType::U64);
    auto ptrU64Type = types.MakePointer(u64Type);
    auto rowSetType = types.MakeStruct(
        std::vector<std::pair<std::string, TTypePtr>>{
            {"Columns", ptrColumnType},
            {"ColumnCount", i64Type},
            {"RowCount", i64Type},
            {"Selection", ptrU8Type},
            {"Hash", ptrU64Type},
            {"Destroy", voidPtrType},
            {"Private", voidPtrType},
            {"RefCount", i64Type},
        });

    auto ptrI64Type = types.MakePointer(i64Type);
    auto ptrPtrI64Type = types.MakePointer(ptrI64Type);
    auto ptrPtrU8Type = types.MakePointer(ptrU8Type);

    auto pairBufferType = types.MakeStruct(
        std::vector<std::pair<std::string, TTypePtr>>{
            {"Count", i64Type},
            {"Capacity", i64Type},
            {"Data", ptrI64Type},
        });

    // Row table (kernel/row_table.oz); mirrored by TRowJoinProcessor's
    // state structs in exec/row_join_exec.h.
    auto rowArenaType = types.MakeStruct(
        std::vector<std::pair<std::string, TTypePtr>>{
            {"Blocks", ptrPtrU8Type},
            {"BlockCount", i64Type},
            {"BlockCapacity", i64Type},
            {"BlockRows", i64Type},
            {"RowBytes", i64Type},
            {"Rows", i64Type},
            {"CurRows", i64Type},
        });
    auto rowTableType = types.MakeStruct(
        std::vector<std::pair<std::string, TTypePtr>>{
            {"Entries", ptrU64Type},
            {"Capacity", i64Type},
            {"Mask", u64Type},
            {"Shift", i64Type},
            {"Rows", i64Type},
            {"Keys", i64Type},
        });
    auto rowScratchType = types.MakeStruct(
        std::vector<std::pair<std::string, TTypePtr>>{
            {"Hashes", ptrU64Type},
            {"Salts", ptrU64Type},
            {"Offsets", ptrU64Type},
            {"Entries", ptrU64Type},
            {"Rows", ptrI64Type},
            {"Work", ptrI64Type},
            {"Keys", ptrU8Type},
            {"Capacity", i64Type},
        });

    auto hashTableType = types.MakeStruct(
        std::vector<std::pair<std::string, TTypePtr>>{
            {"Keys", ptrU8Type},
            {"Ctrl", ptrU8Type},
            {"SlotId", ptrI64Type},
            {"GroupKeys", ptrU8Type},
            {"AggBuffers", ptrPtrI64Type},
            {"OwnedBlocks", ptrPtrU8Type},
            {"OwnedBlockCount", i64Type},
            {"OwnedBlockCapacity", i64Type},
            {"Capacity", i64Type},
            {"Size", i64Type},
            {"NumAggs", i64Type},
            {"NumKeys", i64Type},
            {"KeySize", i64Type},
            {"RowArena", types.MakeNamed("RowArena", rowArenaType)},
            {"RowTable", types.MakeNamed("RowTable", rowTableType)},
            {"RowScratch", types.MakeNamed("RowScratch", rowScratchType)},
            {"RowSwitchGroups", i64Type},
            {"RowExpectedGroups", i64Type},
            {"RowRebuilds", i64Type},
            {"RowMovedFromSlots", i64Type},
        });

    return {
        { .Name = "TColumn", .Type = columnType },
        { .Name = "TRowSet", .Type = rowSetType },
        { .Name = "HashTable", .Type = hashTableType },
        { .Name = "PairBuffer", .Type = pairBufferType },
        { .Name = "RowArena", .Type = rowArenaType },
        { .Name = "RowTable", .Type = rowTableType },
        { .Name = "RowScratch", .Type = rowScratchType },
        { .Name = "StringView", .Type = stringViewType },
        { .Name = "OwnedString", .Type = ownedStringType },
    };
}

NAst::TTypePtr QumirDbNamedType(NAst::TTypeFactory& types, std::string_view name) {
    for (const auto& type : QumirDbExternalTypes(types)) {
        if (type.Name == name) {
            return types.MakeNamed(std::string(name), type.Type);
        }
    }
    throw std::logic_error("qumirdb.oz declares no type " + std::string(name));
}

void EnsureQumirDbRuntimeSymbolsLinked() {
    (void)QumirDbRuntimeSymbolAnchors;
}

} // namespace NRegistry
} // namespace NQumir
