#pragma once

#include <qumir/parser/ast.h>

namespace NQdb {
namespace NKernel {

// SQL CASTs that neither Qumir nor a `cast` operator of qumirdb.oz converts (integer ->
// VARCHAR, VARCHAR -> DATE, number <-> DECIMAL, ...) are rows of one table in cast_rules.cpp. A
// rule only converts a plain (non-nullable) value; the nullability of the operand is
// handled around it by the pass that applies it.
//
// Two passes apply the table. ExpandNullable asks it for every cast of a plain value
// through ApplyCastRule. The rows with a DECIMAL side are left to ExpandDecimal
// (ApplyDecimalCastRule): the lowered call yields the scale-less BinInt storage type, so
// the cast must be lowered where the scale of its result is still known to the
// arithmetic around it.
//
// `from` is the operand's value type, `to` the cast target as the query wrote it, both
// without nullability. The target keeps its spelling (`DATE`, `StringView`, a planner
// type, a DECIMAL with its scale) because the planner erases DATE to i32 otherwise; so
// does the operand type when the operand is itself a cast to DATE (see
// NominalCastType in annotate_type.cpp). A DATE column or literal is an i32 to the
// planner (issue #226) and takes the integer rows.

// The conversion of `operand` when a rule without a DECIMAL side covers the cast, else
// nullptr: Qumir's own `cast` or a `cast` operator of qumirdb.oz applies then. Throws a
// TError naming the SQL types for a cast to VARCHAR that neither implements; Qumir would
// report the unresolved cast only when the kernel is compiled.
NQumir::NAst::TExprPtr ApplyCastRule(
    const NQumir::NAst::TTypePtr& from,
    const NQumir::NAst::TTypePtr& to,
    const NQumir::NAst::TExprPtr& operand,
    NQumir::TLocation loc);

// The conversion of `operand` when a rule with a DECIMAL side covers the cast, else
// nullptr. Throws a TError for a DECIMAL conversion a rule covers but cannot perform
// (a scale down, an integer width that is not a SQL type).
NQumir::NAst::TExprPtr ApplyDecimalCastRule(
    const NQumir::NAst::TTypePtr& from,
    const NQumir::NAst::TTypePtr& to,
    const NQumir::NAst::TExprPtr& operand,
    NQumir::TLocation loc);

// True when ApplyCastRule rewrites or rejects this cast, so the expression must go
// through ExpandNullable even when nothing in it is nullable.
bool HasCastRule(const NQumir::NAst::TTypePtr& from, const NQumir::NAst::TTypePtr& to);

} // namespace NKernel
} // namespace NQdb
