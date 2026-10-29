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

// The digits go to the kernel's scratch arena, like the bytes of `||`; `__arena__` is
// the placeholder the codegen binds to it. One runtime function serves every width.
TExprPtr IntegerToString(const TExprPtr& operand, const TTypePtr&, const TTypePtr&,
    TLocation loc)
{
    return Call("qdb_string_from_i64", {Ident("__arena__", loc), CastToI64(operand, loc)}, loc);
}

TExprPtr BoolToString(const TExprPtr& operand, const TTypePtr&, const TTypePtr&, TLocation loc) {
    return Call("qdb_string_from_bool", {operand}, loc);
}

// Qumir lowers a bool-to-number cast as a sign extension of the i1, so TRUE would become
// -1 (and -1.0 for a float). Select 1 or 0 instead.
TExprPtr BoolToNumber(const TExprPtr& operand, const TTypePtr&, const TTypePtr& to,
    TLocation loc)
{
    auto literal = [&](int64_t value) -> TExprPtr {
        return std::make_shared<TCastExpr>(loc, std::make_shared<TNumberExpr>(loc, value), to);
    };
    return std::make_shared<TIfExpr>(loc, operand, literal(1), literal(0));
}

// qdb_sql_date is the `cast` operator StringView -> DATE of qumirdb.oz; the rule keeps a
// string column cast to DATE on it after the planner has erased DATE to i32, and erases
// the nominal DATE it returns the same way.
TExprPtr StringToDate(const TExprPtr& operand, const TTypePtr&, const TTypePtr&, TLocation loc) {
    return std::make_shared<TCastExpr>(loc, Call("qdb_sql_date", {operand}, loc),
        std::make_shared<TIntegerType>(TIntegerType::I32));
}

// A DECIMAL is a BinInt of unscaled digits; the conversions below come from qumirdb.oz
// and take the scale as a literal. qdb_decimal_from_* round the value to the target
// scale, qdb_decimal_to_* round half away from zero and saturate.
TExprPtr IntegerToDecimal(const TExprPtr& operand, const TTypePtr&, const TTypePtr& to,
    TLocation loc)
{
    return Call("qdb_decimal_from_i64", {CastToI64(operand, loc), Int64(ScaleOf(to), loc)}, loc);
}

TExprPtr FloatToDecimal(const TExprPtr& operand, const TTypePtr&, const TTypePtr& to,
    TLocation loc)
{
    return Call("qdb_decimal_from_f64", {operand, Int64(ScaleOf(to), loc)}, loc);
}

// Only the scale changes the digits: the same scale is the identity, a larger one
// multiplies by a power of ten. A scale down would have to round and is not implemented.
TExprPtr DecimalToDecimal(const TExprPtr& operand, const TTypePtr& from, const TTypePtr& to,
    TLocation loc)
{
    const int32_t fromScale = ScaleOf(from);
    const int32_t toScale = ScaleOf(to);
    if (fromScale == toScale) {
        return operand;
    }
    if (fromScale > toScale) {
        throw NQumir::TError(
            "decimal scale down cast is not implemented: " +
            std::to_string(fromScale) + " -> " + std::to_string(toScale));
    }
    return Call("qdb_decimal_scale_up", {operand, Int64(toScale - fromScale, loc)}, loc);
}

TExprPtr DecimalToFloat(const TExprPtr& operand, const TTypePtr& from, const TTypePtr&,
    TLocation loc)
{
    return Call("qdb_decimal_to_f64", {operand, Int64(ScaleOf(from), loc)}, loc);
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
    return Call(bits == 32 ? "qdb_decimal_to_i32" : "qdb_decimal_to_i64",
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

// One row per conversion Qumir does not perform itself. A new cast is a new row and,
// when it needs code, a function in qumirdb.oz (or an extern in qumirdb_runtime.cpp
// with its counterpart in browser_runtime.js). The rows with a DECIMAL side are applied
// by ExpandDecimal, the others by ExpandNullable (see cast_rules.h).
const TCastRule CastRules[] = {
    {EKind::Integer, EKind::String, IntegerToString},
    {EKind::Bool, EKind::String, BoolToString},
    {EKind::Bool, EKind::Integer, BoolToNumber},
    {EKind::Bool, EKind::Float, BoolToNumber},
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

// A cast to VARCHAR without a rule has no conversion at all: Qumir's only `cast`
// candidate to StringView is the generic nullable wrapper, whose error misleads.
bool IsUnsupportedToString(EKind from, EKind to) {
    return to == EKind::String && from != EKind::String && from != EKind::Other;
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
            "CAST from " + TypeDiagnosticName(from) + " to VARCHAR is not implemented");
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
