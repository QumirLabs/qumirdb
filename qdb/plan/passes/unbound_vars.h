#pragma once

#include <qumir/parser/ast.h>

#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace NQdb {

struct TColumnReference;

// Returns the set of free (unbound) variable names in expr.
// Names introduced by let-bindings, fun params, var declarations, and for-loop
// variables are considered bound and excluded from the result.
// Unbound names in a kernel expression correspond to input column references.
std::unordered_set<std::string> FindUnboundVars(const NQumir::NAst::TExprPtr& expr);

// The identifier nodes behind FindUnboundVars, in AST order (pre-order, left to
// right), so a diagnostic can point at the first reference.
std::vector<std::shared_ptr<NQumir::NAst::TIdentExpr>> FindUnboundIdents(
    const NQumir::NAst::TExprPtr& expr);

// Appends an operator column reference for each free identifier of `expr`, in
// AST order.
void AppendColumnReferences(
    std::vector<TColumnReference>& references,
    const NQumir::NAst::TExprPtr& expr);

} // namespace NQdb
