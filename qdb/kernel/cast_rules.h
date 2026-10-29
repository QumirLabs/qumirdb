#pragma once

#include <qumir/parser/ast.h>

namespace NQdb {
namespace NKernel {

// SQL CASTs that Qumir does not convert itself (integer -> VARCHAR, BOOLEAN -> number,
// VARCHAR -> DATE, ...) are rows of a table in cast_rules.cpp. ExpandNullable asks the
// table for every cast of a plain (non-nullable) value; the nullability of the operand
// is handled around the rule, so a rule only converts a value.
//
// `from` is the operand's value type, `to` the cast target as the query wrote it, both
// without nullability. The target keeps its spelling (`DATE`, `StringView`, a planner
// type) because the planner erases DATE to i32 otherwise.

// The conversion of `operand` when a rule covers the cast, else nullptr: Qumir's own
// `cast` applies then. Throws a TError naming the types for a cast to VARCHAR that no
// rule implements; Qumir's error there names a generic candidate and misleads.
NQumir::NAst::TExprPtr ApplyCastRule(
    const NQumir::NAst::TTypePtr& from,
    const NQumir::NAst::TTypePtr& to,
    const NQumir::NAst::TExprPtr& operand,
    NQumir::TLocation loc);

// True when ApplyCastRule rewrites or rejects this cast, so the expression must go
// through ExpandNullable even when nothing in it is nullable.
bool HasCastRule(const NQumir::NAst::TTypePtr& from, const NQumir::NAst::TTypePtr& to);

} // namespace NKernel
} // namespace NQdb
