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
    reinterpret_cast<const void*>(&qdb_regexp_replace),
    reinterpret_cast<const void*>(&qdb_date_year),
    reinterpret_cast<const void*>(&qdb_sql_date),
    reinterpret_cast<const void*>(&qdb_sql_interval),
    reinterpret_cast<const void*>(&qdb_round),
    reinterpret_cast<const void*>(&qdb_abs_i32),
    reinterpret_cast<const void*>(&qdb_abs_i64),
    reinterpret_cast<const void*>(&qdb_fabs),
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

std::vector<TExternalType> BuildQumirDbExternalTypes() {
    auto i8Type = std::make_shared<TIntegerType>(TIntegerType::I8);
    auto i32Type = std::make_shared<TIntegerType>(TIntegerType::I32);
    auto i64Type = std::make_shared<TIntegerType>();
    auto u8Type = std::make_shared<TIntegerType>(TIntegerType::U8);
    auto voidPtrType = std::make_shared<TPointerType>(i64Type);
    auto ptrU8Type = std::make_shared<TPointerType>(u8Type);
    auto ptrI8Type = std::make_shared<TPointerType>(i8Type);

    auto makeStringHandleType = [&]() {
        return std::make_shared<TStructType>(
            std::vector<std::pair<std::string, TTypePtr>>{
                {"Data", ptrU8Type},
                {"Size", i64Type},
            });
    };
    auto stringViewType = makeStringHandleType();
    auto ownedStringType = makeStringHandleType();

    auto columnType = std::make_shared<TStructType>(
        std::vector<std::pair<std::string, TTypePtr>>{
            {"Data", ptrI8Type},
            {"DataBitOffset", i32Type},
            {"Mask", ptrU8Type},
            {"MaskBitOffset", i32Type},
            {"Offsets", voidPtrType},
            {"OffsetWidth", u8Type},
        });
    auto columnNamedType = std::make_shared<TNamedType>("TColumn", columnType);
    auto ptrColumnType = std::make_shared<TPointerType>(columnNamedType);

    auto u64Type = std::make_shared<TIntegerType>(TIntegerType::U64);
    auto ptrU64Type = std::make_shared<TPointerType>(u64Type);
    auto rowSetType = std::make_shared<TStructType>(
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

    auto ptrI64Type = std::make_shared<TPointerType>(i64Type);
    auto ptrPtrI64Type = std::make_shared<TPointerType>(ptrI64Type);
    auto ptrPtrU8Type = std::make_shared<TPointerType>(ptrU8Type);

    auto hashTableType = std::make_shared<TStructType>(
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
        });

    auto pairBufferType = std::make_shared<TStructType>(
        std::vector<std::pair<std::string, TTypePtr>>{
            {"Count", i64Type},
            {"Capacity", i64Type},
            {"Data", ptrI64Type},
        });

    return {
        { .Name = "TColumn", .Type = columnType },
        { .Name = "TRowSet", .Type = rowSetType },
        { .Name = "HashTable", .Type = hashTableType },
        { .Name = "PairBuffer", .Type = pairBufferType },
        { .Name = "StringView", .Type = stringViewType },
        { .Name = "OwnedString", .Type = ownedStringType },
    };
}

const std::vector<TExternalType>& QumirDbExternalTypes() {
    static const auto types = BuildQumirDbExternalTypes();
    return types;
}

void EnsureQumirDbRuntimeSymbolsLinked() {
    (void)QumirDbRuntimeSymbolAnchors;
}

} // namespace NRegistry
} // namespace NQumir
