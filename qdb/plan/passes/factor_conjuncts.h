#pragma once

#include <qumir/parser/ast.h>

#include <vector>

namespace NQdb {

// Splits `expr` into top-level conjuncts, additionally factoring atoms common to
// every branch of a disjunction:  (a AND x) OR (a AND y) == a AND (x OR y).  The
// hoisted common atoms and the reduced disjunction are appended to `out`. This
// exposes a join equality buried inside an OR (e.g. TPC-H q19) to key
// extraction.
void FactorConjuncts(const NQumir::NAst::TExprPtr& expr, std::vector<NQumir::NAst::TExprPtr>& out);

} // namespace NQdb
