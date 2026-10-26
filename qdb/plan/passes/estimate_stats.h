#pragma once

#include <qdb/plan/ops/operator.h>

#include <qumir/parser/ast.h>
#include <qumir/parser/type.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace NQdb {

// Closed comparison bounds already folded to numbers, by printed expression;
// null for a bound that is not a number. Folding compiles the bound and the
// planner estimates a plan several times, so the estimates of one query share
// one set and each bound is folded once.
using TFoldedBounds = std::unordered_map<
    std::string, std::shared_ptr<NQumir::NAst::TNumberExpr>>;

TStatsPtr EstimateStats(TOperatorPtr op);
TStatsPtr EstimateStats(TOperatorPtr op, TFoldedBounds& foldedBounds);

class TJoinOperator;
struct TJoinKey;

// Statistics of `join` from the estimated stats of its inputs, which the join
// order search also uses for the joins it considers. With `columns`, only
// those columns get statistics: the search reads the ones of its keys and
// residuals.
TStatsPtr EstimateJoinStats(
    const std::shared_ptr<TJoinOperator>& join,
    TFoldedBounds& foldedBounds,
    const std::unordered_set<std::string>* columns = nullptr);

// The row count EstimateJoinStats gives an inner join of `left` and `right`
// on `keys` with `residual` (null for none), without the join's column
// statistics: the join order search estimates every split it considers.
uint64_t EstimateInnerJoinRows(
    const TOperatorPtr& left,
    const TOperatorPtr& right,
    const std::vector<TJoinKey>& keys,
    const NQumir::NAst::TExprPtr& residual,
    TFoldedBounds& foldedBounds);

// Width estimate used by the CBO cost model. Strings are costed as the
// materialized StringView (ptr + size), not as their payload bytes.
double EstimateTypeWidth(const NQumir::NAst::TTypePtr& type);

// Width of a named output column of `op` (8.0 when unknown).
double ColumnWidth(const TOperatorPtr& op, const std::string& name);

// Browser/native neutral join work model. Units are abstract byte-ish work:
// child costs + key scan work + materialized output width + per-output-batch
// overhead. The batch overhead intentionally mirrors the browser join
// materialization granularity so bushy plans that create many wide rowsets are
// visible to CBO.
double EstimateJoinCost(
    double leftCost,
    double rightCost,
    double leftRows,
    double rightRows,
    double outputRows,
    double outputRowWidth,
    double keyWidth);

} // namespace NQdb
