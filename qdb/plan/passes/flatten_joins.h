#pragma once

#include <qdb/plan/ops/operator.h>

namespace NQdb {

// Normalizes maximal inner-join regions to the comma-join shape a keyless
// left-deep chain under a single filter so the reorderer can permute them.
TOperatorPtr FlattenInnerJoins(NQumir::NAst::TTypeFactory& types, TOperatorPtr root);

} // namespace NQdb
