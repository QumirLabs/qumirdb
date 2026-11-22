#pragma once

#include <qdb/plan/ops/operator.h>

namespace NQdb {

// Deep-copies the expressions embedded in an operator subtree in place (the nodes
// must already be distinct). Used after a structural clone so two copies do not
// share mutable expression nodes.
void CloneOperatorExprs(const TOperatorPtr& op);

// Independent deep copy of an operator subtree (new nodes + cloned expressions).
TOperatorPtr CloneOperator(NQumir::NAst::TTypeFactory& types, const TOperatorPtr& op);

} // namespace NQdb
