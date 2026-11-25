#pragma once

#include <qdb/plan/types/nullable.h>

#include <qumir/parser/type.h>
#include <qumir/parser/type_factory.h>

#include <cstdint>
#include <cstdio>
#include <string>

namespace NQdb {

// DATE is a day count, an i32 in a column: `<named DATE [i32]>` over that i32. A
// min/max aggregate keeps its value in the i64 state of every integer aggregate, so its
// DATE is an I64 one. The argument keeps the two apart.
inline NQumir::NAst::TTypePtr MakeDateType(
    NQumir::NAst::TTypeFactory& types,
    NQumir::NAst::TIntegerType::EKind kind = NQumir::NAst::TIntegerType::I32)
{
    const NQumir::NAst::TTypePtr storage = types.MakeInteger(kind);
    return types.MakeNamed("DATE", storage, {NQumir::NAst::TGenericArg::TypeArg(storage)});
}

// qumirdb.oz's `(type DATE i32)`, the target of a SQL cast to DATE. Built complete,
// as the qumirdb.oz declaration completes it.
inline NQumir::NAst::TTypePtr DateCastTarget(NQumir::NAst::TTypeFactory& types) {
    return types.MakeNamed("DATE", types.MakeInteger(NQumir::NAst::TIntegerType::I32));
}

// The storage integer of a DATE value type, else null.
inline const NQumir::NAst::TIntegerType* DateStorageType(const NQumir::NAst::TTypePtr& type) {
    auto named = NQumir::NAst::TMaybeType<NQumir::NAst::TNamedType>(type);
    if (!named) {
        return nullptr;
    }
    auto date = named.Cast();
    if (date->Name != "DATE" || date->TypeArgs.size() != 1) {
        return nullptr;
    }
    return NQumir::NAst::TMaybeType<NQumir::NAst::TIntegerType>(date->TypeArgs[0].Type).Cast();
}

inline bool IsDateType(const NQumir::NAst::TTypePtr& type) {
    return DateStorageType(UnwrapNullableType(type)) != nullptr;
}

// Qumir's UnwrapNamedType that keeps a DATE: a DATE is an integer to move and compare,
// but its rules and output are its own.
inline NQumir::NAst::TTypePtr UnwrapNamedValueType(const NQumir::NAst::TTypePtr& type) {
    return DateStorageType(type) ? type : NQumir::NAst::UnwrapNamedType(type);
}

// An integer type, or the storage integer of a DATE: DATE moves and compares as one.
inline NQumir::NAst::TMaybeType<NQumir::NAst::TIntegerType> MaybeIntegerType(
    const NQumir::NAst::TTypePtr& type)
{
    if (const auto* storage = DateStorageType(type)) {
        return NQumir::NAst::TMaybeType<NQumir::NAst::TIntegerType>(storage);
    }
    return NQumir::NAst::TMaybeType<NQumir::NAst::TIntegerType>(type);
}

// The day count of the DATE cell `row` of a column of type `type` (IsDateType).
inline int64_t DateCellDays(const char* data, int64_t row, const NQumir::NAst::TTypePtr& type) {
    return DateStorageType(UnwrapNullableType(type))->Width == NQumir::NAst::TIntegerType::I64
        ? reinterpret_cast<const int64_t*>(data)[row]
        : reinterpret_cast<const int32_t*>(data)[row];
}

// The ISO 8601 date of a day count since 1970-01-01 in the proleptic Gregorian calendar,
// as qdb_string_from_date prints it: at least four year digits and a minus sign when
// negative. Howard Hinnant's civil_from_days ("chrono-Compatible Low-Level Date
// Algorithms").
inline std::string FormatDate(int64_t days) {
    const int64_t z = days + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    const int day = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    const int month = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    const int64_t year = yoe + era * 400 + (month <= 2);
    char buf[40];
    const int size = std::snprintf(buf, sizeof(buf), year < 0 ? "-%04lld-%02d-%02d" : "%04lld-%02d-%02d",
        static_cast<long long>(year < 0 ? -year : year), month, day);
    return std::string(buf, static_cast<size_t>(size));
}

} // namespace NQdb
