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

// The digits go to the kernel's scratch arena, like the bytes of `||`; `__arena__` is
// the placeholder the codegen binds to it. One runtime function serves every width.
TExprPtr IntegerToString(const TExprPtr& operand, const TTypePtr&, TLocation loc) {
    auto wide = std::make_shared<TCastExpr>(loc, operand, std::make_shared<TIntegerType>());
    return Call("qdb_string_from_i64", {Ident("__arena__", loc), std::move(wide)}, loc);
}

TExprPtr BoolToString(const TExprPtr& operand, const TTypePtr&, TLocation loc) {
    return Call("qdb_string_from_bool", {operand}, loc);
}

// Qumir lowers a bool-to-number cast as a sign extension of the i1, so TRUE would become
// -1 (and -1.0 for a float). Select 1 or 0 instead.
TExprPtr BoolToNumber(const TExprPtr& operand, const TTypePtr& to, TLocation loc) {
    auto literal = [&](int64_t value) -> TExprPtr {
        return std::make_shared<TCastExpr>(loc, std::make_shared<TNumberExpr>(loc, value), to);
    };
    return std::make_shared<TIfExpr>(loc, operand, literal(1), literal(0));
}

// qdb_sql_date is the `cast` operator StringView -> DATE of qumirdb.oz; the rule keeps a
// string column cast to DATE on it after the planner has erased DATE to i32, and erases
// the nominal DATE it returns the same way.
TExprPtr StringToDate(const TExprPtr& operand, const TTypePtr&, TLocation loc) {
    return std::make_shared<TCastExpr>(loc, Call("qdb_sql_date", {operand}, loc),
        std::make_shared<TIntegerType>(TIntegerType::I32));
}

struct TCastRule {
    EKind From;
    EKind To;
    // Builds the conversion of a plain `operand` of kind From to the cast target `to`.
    TExprPtr (*Build)(const TExprPtr& operand, const TTypePtr& to, TLocation loc);
};

// One row per conversion Qumir does not perform itself. A new cast is a new row and,
// when it needs code, a function in qumirdb.oz (or an extern in qumirdb_runtime.cpp
// with its counterpart in browser_runtime.js).
const TCastRule CastRules[] = {
    {EKind::Integer, EKind::String, IntegerToString},
    {EKind::Bool, EKind::String, BoolToString},
    {EKind::Bool, EKind::Integer, BoolToNumber},
    {EKind::Bool, EKind::Float, BoolToNumber},
    {EKind::String, EKind::Date, StringToDate},
};

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
    if (const TCastRule* rule = FindCastRule(fromKind, toKind)) {
        return rule->Build(operand, to, loc);
    }
    if (IsUnsupportedToString(fromKind, toKind)) {
        throw NQumir::TError(loc,
            "CAST from " + TypeDiagnosticName(from) + " to VARCHAR is not implemented");
    }
    return nullptr;
}

bool HasCastRule(const TTypePtr& from, const TTypePtr& to) {
    const EKind fromKind = KindOf(from);
    const EKind toKind = KindOf(to);
    return FindCastRule(fromKind, toKind) || IsUnsupportedToString(fromKind, toKind);
}

} // namespace NKernel
} // namespace NQdb
