#pragma once

#include <qumir/parser/ast.h>

#include <vector>

namespace NQdb {

// The AND ("&&") or the OR ("||") of `parts` in their order, as a balanced tree
// (both are associative in 3VL), so a long conjunction stays shallow. A node is
// located at its first part. Null for no parts.
NQumir::NAst::TExprPtr Junction(NQumir::NAst::TOperator op, std::vector<NQumir::NAst::TExprPtr> parts);

} // namespace NQdb
