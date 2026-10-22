#pragma once

#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/sort.h>

#include <qumir/parser/ast.h>
#include <qumir/parser/type.h>

#include <cstddef>
#include <iosfwd>
#include <string>
#include <vector>

namespace NQdb {
class TExternalCatalogSnapshot;
class TRuntimeContext;
namespace NKernel {

enum class EOperatorKernelKind {
    NonCompute,
    UnaryStreaming,
    UnaryBlocking,
    Binary,
};

struct TKernelColumnRef {
    std::string Name;
    int32_t Index = -1;
    NQumir::NAst::TTypePtr Type;
};

struct TKernelKeySpec {
    std::string Name;
    std::vector<TKernelColumnRef> Columns;
};

struct TKernelEntrypointSpec {
    std::string Name;
    std::string Abi;
};

struct TKernelAggregateSpec {
    std::string Name;
    std::string Func;
    bool HasArg = false;
    TKernelColumnRef Arg;
    NQumir::NAst::TExprPtr ArgExpr;
};

struct TKernelSortKeySpec {
    TKernelColumnRef Column;
    ESortDirection Direction = ESortDirection::Asc;
    ESortNulls Nulls = ESortNulls::Default;
};

struct TKernelJoinKeySpec {
    TKernelColumnRef Left;
    TKernelColumnRef Right;
    bool NullsEqual = false;
};

struct TOperatorKernelSpec {
    EOperatorKernelKind Kind = EOperatorKernelKind::NonCompute;
    std::string OperatorName;
    std::vector<NQumir::NAst::TTypePtr> InputSchemas;
    NQumir::NAst::TTypePtr OutputSchema;
    std::vector<TKernelColumnRef> ReferencedColumns;
    std::vector<NQumir::NAst::TExprPtr> Expressions;
    std::vector<TKernelKeySpec> Keys;
    std::vector<TKernelAggregateSpec> Aggregates;
    std::vector<TKernelSortKeySpec> SortKeys;
    std::vector<TKernelJoinKeySpec> JoinKeys;
    EJoinType JoinType = EJoinType::Inner;
    std::vector<TKernelEntrypointSpec> Entrypoints;
    std::vector<std::string> SourceModules;

    // Set by the caller (plan_lowerer.cpp) when this operator sits directly
    // downstream of a hash shuffle keyed on the same columns: the generated
    // dispatch reads batch.Hash[i] instead of recomputing rh_hash(key).
    bool HasPrecomputedHash = false;
};

std::string_view KernelKindName(EOperatorKernelKind kind);

TOperatorKernelSpec BuildFilterKernelSpec(
    const NQumir::NAst::TStructType& inputType,
    const NQumir::NAst::TExprPtr& predicate,
    std::string entrypointName = "<kernel>",
    const TExternalCatalogSnapshot* externalCatalog = nullptr,
    TRuntimeContext* runtimeContext = nullptr);

TOperatorKernelSpec BuildProjectKernelSpec(
    const NQumir::NAst::TStructType& inputType,
    const std::vector<NQumir::NAst::TExprPtr>& computedExprs,
    const std::vector<NQumir::NAst::TTypePtr>& computedTypes,
    std::string entrypointName = "<project>",
    const TExternalCatalogSnapshot* externalCatalog = nullptr,
    TRuntimeContext* runtimeContext = nullptr);

TOperatorKernelSpec BuildAggregateKernelSpec(
    const NQumir::NAst::TStructType& inputType,
    const std::vector<std::string>& groupKeys,
    const std::vector<TAggregateSpec>& aggs,
    const TExternalCatalogSnapshot* externalCatalog = nullptr);

TOperatorKernelSpec BuildSortKernelSpec(
    const NQumir::NAst::TStructType& inputType,
    const std::vector<TSortKey>& keys,
    std::string operatorName = "sort");

// `markerName` names the marker column a LeftMark join appends to the left
// columns; other join types do not use it.
TOperatorKernelSpec BuildJoinKernelSpec(
    const NQumir::NAst::TStructType& leftType,
    const NQumir::NAst::TStructType& rightType,
    const std::vector<TJoinKey>& keys,
    EJoinType type,
    const NQumir::NAst::TExprPtr& residualPredicate = nullptr,
    const std::string& markerName = {});

// A keyless (nested-loop) join. The residual decides matches, so it is given
// only to joins other than Inner, whose residual is a filter over the product.
TOperatorKernelSpec BuildCrossJoinKernelSpec(
    const NQumir::NAst::TStructType& leftType,
    const NQumir::NAst::TStructType& rightType,
    EJoinType type = EJoinType::Inner,
    const NQumir::NAst::TExprPtr& residualPredicate = nullptr);

void PrintKernelSpec(std::ostream& out, const TOperatorKernelSpec& spec);

} // namespace NKernel
} // namespace NQdb
