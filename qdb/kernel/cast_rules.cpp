#include <qdb/kernel/cast_rules.h>

#include <qdb/plan/types/decimal.h>

#include <qumir/error.h>

#include <string>

namespace NQdb {
namespace NKernel {

using namespace NQumir::NAst;
using NQumir::TLocation;

namespace {

enum class EKind {
    Bool,
    Integer,
    Float,
    String,
    Date,
    Decimal,
    Other,
};

// Classifies a value type in either spelling a cast sees: the planner's (TStringType,
// TIntegerType, a decimal) or qumirdb.oz's (StringView, DATE).
EKind KindOf(const TTypePtr& type) {
    if (!type) {
        return EKind::Other;
    }
    if (IsDecimalType(type)) {
        return EKind::Decimal;
    }
    if (auto named = TMaybeType<TNamedType>(type)) {
        const std::string& name = named.Cast()->Name;
        if (name == "StringView") {
            return EKind::String;
        }
        if (name == "DATE" || name == "TIMESTAMP") {
            return EKind::Date;
        }
        return KindOf(named.Cast()->UnderlyingType);
    }
    if (TMaybeType<TStringType>(type)) {
        return EKind::String;
    }
    if (TMaybeType<TBoolType>(type)) {
        return EKind::Bool;
    }
    if (TMaybeType<TIntegerType>(type)) {
        return EKind::Integer;
    }
    if (TMaybeType<TFloatType>(type)) {
        return EKind::Float;
    }
    return EKind::Other;
}

TExprPtr Ident(const std::string& name, TLocation loc) {
    return std::make_shared<TIdentExpr>(loc, name);
}

TExprPtr Call(const std::string& name, std::vector<TExprPtr> args, TLocation loc) {
    return std::make_shared<TCallExpr>(loc, Ident(name, loc), std::move(args));
}

TExprPtr Int64(int64_t value, TLocation loc) {
    auto literal = std::make_shared<TNumberExpr>(loc, value);
    literal->Type = std::make_shared<TIntegerType>();
    return literal;
}

TExprPtr CastToI64(const TExprPtr& operand, TLocation loc) {
    return std::make_shared<TCastExpr>(loc, operand, std::make_shared<TIntegerType>());
}

// The scale of a DECIMAL type a rule converts from or to; the kinds guarantee it is one.
int32_t ScaleOf(const TTypePtr& type) {
    return DecimalSpecOfValueType(type)->Scale;
}

int32_t PrecisionOf(const TTypePtr& type) {
    return DecimalSpecOfValueType(type)->Precision;
}

// The digits an integer type can hold; 64-bit takes 20 for u64.
int32_t IntegerDigits(const TTypePtr& type) {
    switch (TMaybeType<TIntegerType>(UnwrapNamedType(type)).Cast()->BitWidth()) {
        case 8:
            return 3;
        case 16:
            return 5;
        case 32:
            return 10;
    }
    return 20;
}

// The digits go to the kernel's scratch arena, like the bytes of `||`; `__arena__` is
// the placeholder the codegen binds to it. One runtime function serves every signed
// width and one every unsigned width: a u64 above 2^63-1 has no i64 to widen to.
TExprPtr IntegerToString(const TExprPtr& operand, const TTypePtr& from, const TTypePtr&,
    TLocation loc)
{
    const bool isSigned = TMaybeType<TIntegerType>(UnwrapNamedType(from)).Cast()->IsSigned();
    auto wide = std::make_shared<TCastExpr>(loc, operand,
        std::make_shared<TIntegerType>(isSigned ? TIntegerType::I64 : TIntegerType::U64));
    return Call(isSigned ? "qdb_string_from_i64" : "qdb_string_from_u64",
        {Ident("__arena__", loc), std::move(wide)}, loc);
}

// qdb_sql_date is the `cast` operator StringView -> DATE of qumirdb.oz; the rule keeps a
// string column cast to DATE on it after the planner has erased DATE to i32, and erases
// the nominal DATE it returns the same way.
TExprPtr StringToDate(const TExprPtr& operand, const TTypePtr&, const TTypePtr&, TLocation loc) {
    return std::make_shared<TCastExpr>(loc, Call("qdb_sql_date", {operand}, loc),
        std::make_shared<TIntegerType>(TIntegerType::I32));
}

// qdb_string_from_date formats the day count as an ISO date into the kernel's arena. The
// operand is cast to DATE first: the rule is chosen by the nominal type the query wrote,
// but the planner has erased the value it carries to i32, and the i32 -> DATE `cast`
// operator of qumirdb.oz restores the type the function takes.
TExprPtr DateToString(const TExprPtr& operand, const TTypePtr&, const TTypePtr&, TLocation loc) {
    auto date = std::make_shared<TCastExpr>(loc, operand,
        std::make_shared<TNamedType>("DATE", nullptr));
    return Call("qdb_string_from_date", {Ident("__arena__", loc), std::move(date)}, loc);
}

bool IsDecimal64(const TTypePtr& type) {
    return DecimalStorageOf(*DecimalSpecOfValueType(type)) == EDecimalStorage::I64;
}

// A DECIMAL is its unscaled digits in a plain i64 (precision <= 18) or in a BinInt; the
// conversions below come from qumirdb.oz and take the scale as a literal.
// qdb_decimal*_from_* round the value to the target scale, qdb_decimal*_to_* round half
// away from zero and saturate.
TExprPtr IntegerToDecimal(const TExprPtr& operand, const TTypePtr& from, const TTypePtr& to,
    TLocation loc)
{
    if (!IsDecimal64(to)) {
        return Call("qdb_decimal_from_i64", {CastToI64(operand, loc), Int64(ScaleOf(to), loc)}, loc);
    }
    // Digits that do not fit the precision raise, like DuckDB, instead of wrapping in i64.
    const bool fits = IntegerDigits(from) <= PrecisionOf(to) - ScaleOf(to);
    return fits
        ? Call("qdb_decimal64_scale_up", {CastToI64(operand, loc), Int64(ScaleOf(to), loc)}, loc)
        : Call("qdb_decimal64_scale_up_checked", {CastToI64(operand, loc),
            Int64(ScaleOf(to), loc), Int64(PrecisionOf(to), loc)}, loc);
}

TExprPtr FloatToDecimal(const TExprPtr& operand, const TTypePtr&, const TTypePtr& to,
    TLocation loc)
{
    if (!IsDecimal64(to)) {
        return Call("qdb_decimal_from_f64", {operand, Int64(ScaleOf(to), loc)}, loc);
    }
    return Call("qdb_decimal64_from_f64",
        {operand, Int64(ScaleOf(to), loc), Int64(PrecisionOf(to), loc)}, loc);
}

// Only the scale changes the digits: the same scale is the identity, a larger one
// multiplies by a power of ten, in the wider of the two storages. A scale down would have
// to round and is not implemented.
TExprPtr DecimalToDecimal(const TExprPtr& operand, const TTypePtr& from, const TTypePtr& to,
    TLocation loc)
{
    const int32_t fromScale = ScaleOf(from);
    const int32_t toScale = ScaleOf(to);
    if (fromScale > toScale) {
        throw NQumir::TError(
            "decimal scale down cast is not implemented: " +
            std::to_string(fromScale) + " -> " + std::to_string(toScale));
    }
    const int32_t delta = toScale - fromScale;
    if (IsDecimal64(from) && IsDecimal64(to)) {
        // A target with fewer integer digits can overflow: it raises instead of wrapping.
        if (PrecisionOf(to) - toScale < PrecisionOf(from) - fromScale) {
            return Call("qdb_decimal64_scale_up_checked",
                {operand, Int64(delta, loc), Int64(PrecisionOf(to), loc)}, loc);
        }
        return delta == 0
            ? operand
            : Call("qdb_decimal64_scale_up", {operand, Int64(delta, loc)}, loc);
    }
    TExprPtr wide = IsDecimal64(from)
        ? Call("qdb_decimal_from_unscaled", {operand}, loc)
        : operand;
    if (delta != 0) {
        wide = Call("qdb_decimal_scale_up", {std::move(wide), Int64(delta, loc)}, loc);
    }
    return IsDecimal64(to)
        ? Call("qdb_decimal64_narrow_checked", {std::move(wide), Int64(PrecisionOf(to), loc)}, loc)
        : wide;
}

TExprPtr DecimalToFloat(const TExprPtr& operand, const TTypePtr& from, const TTypePtr&,
    TLocation loc)
{
    return Call(IsDecimal64(from) ? "qdb_decimal64_to_f64" : "qdb_decimal_to_f64",
        {operand, Int64(ScaleOf(from), loc)}, loc);
}

TExprPtr DecimalToInteger(const TExprPtr& operand, const TTypePtr& from, const TTypePtr& to,
    TLocation loc)
{
    // Only i32 and i64 are SQL integer types.
    const int bits = TMaybeType<TIntegerType>(UnwrapNamedType(to)).Cast()->BitWidth();
    if (bits != 32 && bits != 64) {
        throw NQumir::TError("decimal cast to a " + std::to_string(bits) +
            "-bit integer is not implemented; only INTEGER and BIGINT exist");
    }
    const std::string prefix = IsDecimal64(from)
        ? "qdb_decimal64_to_"
        : "qdb_decimal_to_";
    return Call(prefix + (bits == 32 ? "i32" : "i64"),
        {operand, Int64(ScaleOf(from), loc)}, loc);
}

struct TCastRule {
    EKind From;
    EKind To;
    // Builds the conversion of a plain `operand` of type `from` (kind From) to the cast
    // target `to` (kind To).
    TExprPtr (*Build)(const TExprPtr& operand, const TTypePtr& from, const TTypePtr& to,
        TLocation loc);
};

// One row per conversion Qumir does not perform itself and a `cast` operator of
// qumirdb.oz cannot: integer -> VARCHAR and DATE -> VARCHAR need the kernel's arena,
// which a one-argument operator cannot receive, and a DECIMAL conversion needs the scale
// of the types. VARCHAR -> DATE is an operator (qdb_sql_date), the row only survives the
// planner's erasure of DATE to i32. A cast a plain operator can express (boolean ->
// number, boolean -> VARCHAR) belongs in qumirdb.oz instead; ExpandNullable spells the
// target of a cast to VARCHAR as StringView so that such an operator is a candidate.
// The rows with a DECIMAL side are applied by ExpandDecimal, the others by
// ExpandNullable (see cast_rules.h).
const TCastRule CastRules[] = {
    {EKind::Integer, EKind::String, IntegerToString},
    {EKind::Date, EKind::String, DateToString},
    {EKind::String, EKind::Date, StringToDate},
    {EKind::Integer, EKind::Decimal, IntegerToDecimal},
    {EKind::Float, EKind::Decimal, FloatToDecimal},
    {EKind::Decimal, EKind::Decimal, DecimalToDecimal},
    {EKind::Decimal, EKind::Float, DecimalToFloat},
    {EKind::Decimal, EKind::Integer, DecimalToInteger},
};

bool HasDecimalSide(EKind from, EKind to) {
    return from == EKind::Decimal || to == EKind::Decimal;
}

const TCastRule* FindCastRule(EKind from, EKind to) {
    for (const auto& rule : CastRules) {
        if (rule.From == from && rule.To == to) {
            return &rule;
        }
    }
    return nullptr;
}

// A cast to VARCHAR that neither a row nor a `cast` operator of qumirdb.oz converts
// (DOUBLE, DECIMAL; issue #393) is rejected here with the SQL type names: Qumir would
// report the unresolved cast only when the kernel is compiled.
bool IsUnsupportedToString(EKind from, EKind to) {
    return to == EKind::String && (from == EKind::Float || from == EKind::Decimal);
}

// The SQL spelling of a type in the rejection: Qumir's diagnostic name of a DECIMAL is
// its SQL name already, that of DOUBLE is `f64`.
std::string SqlTypeName(const TTypePtr& type) {
    return TMaybeType<TFloatType>(UnwrapNamedType(type)) ? "DOUBLE" : TypeDiagnosticName(type);
}

} // namespace

TExprPtr ApplyCastRule(
    const TTypePtr& from,
    const TTypePtr& to,
    const TExprPtr& operand,
    TLocation loc)
{
    const EKind fromKind = KindOf(from);
    const EKind toKind = KindOf(to);
    if (!HasDecimalSide(fromKind, toKind)) {
        if (const TCastRule* rule = FindCastRule(fromKind, toKind)) {
            return rule->Build(operand, from, to, loc);
        }
    }
    if (IsUnsupportedToString(fromKind, toKind)) {
        throw NQumir::TError(loc,
            "CAST from " + SqlTypeName(from) + " to VARCHAR is not implemented");
    }
    return nullptr;
}

TExprPtr ApplyDecimalCastRule(
    const TTypePtr& from,
    const TTypePtr& to,
    const TExprPtr& operand,
    TLocation loc)
{
    const EKind fromKind = KindOf(from);
    const EKind toKind = KindOf(to);
    if (!HasDecimalSide(fromKind, toKind)) {
        return nullptr;
    }
    if (const TCastRule* rule = FindCastRule(fromKind, toKind)) {
        return rule->Build(operand, from, to, loc);
    }
    return nullptr;
}

bool HasCastRule(const TTypePtr& from, const TTypePtr& to) {
    const EKind fromKind = KindOf(from);
    const EKind toKind = KindOf(to);
    return (!HasDecimalSide(fromKind, toKind) && FindCastRule(fromKind, toKind))
        || IsUnsupportedToString(fromKind, toKind);
}

} // namespace NKernel
} // namespace NQdb
