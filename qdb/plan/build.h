#pragma once

#include <qdb/kernel/annotate_type.h>
#include <qdb/plan/ops/operator.h>
#include <qdb/sql/ast.h>

#include <qumir/error.h>

#include <expected>
#include <functional>
#include <string_view>

namespace NQdb {

using TTableSourceFactory =
    std::function<std::expected<TOperatorPtr, NQumir::TError>(std::string_view table)>;

// Builds an unqualified, untyped logical plan from a parsed SQL query. The
// annotation context types expressions the builder inspects, such as aggregate
// arguments that call external functions.
std::expected<TOperatorPtr, NQumir::TError> BuildPlan(
    const NQdb::NSql::TSqlNodePtr& query,
    const TTableSourceFactory& sources,
    const NKernel::TAnnotationContext& annotation = {});

} // namespace NQdb
