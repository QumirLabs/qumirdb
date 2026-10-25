#pragma once

#include <qdb/plan/ops/operator.h>

#include <qumir/parser/ast.h>
#include <qumir/parser/type.h>

#include <memory>
#include <string>
#include <unordered_map>
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

// Fallback NDV for a join key column that has no statistics.
constexpr double UnknownNdv = 100.0;

struct TJoinCardinality {
    double Rows = 0.0;
    double NdvL = 1.0; // \prod ndv over left key columns, clamped to leftRows
    double NdvR = 1.0;
};

// One side of an equi-join key pair.
struct TJoinKeySide {
    double Ndv = UnknownNdv;
    // Fraction of the side's rows whose key lies in the range of values both
    // sides' keys span; 1 when a range is unknown.
    double SharedFraction = 1.0;
};

struct TJoinKeyStats {
    TJoinKeySide Left;
    TJoinKeySide Right;
};

// Statistics of the key pair `leftColumn = rightColumn` of two inputs whose
// stats are estimated.
TJoinKeyStats EstimateJoinKey(
    const TOperatorPtr& left,
    const std::string& leftColumn,
    const TOperatorPtr& right,
    const std::string& rightColumn);

// Inner equi-join cardinality over the range both sides' keys span: rows =
// sharedL*sharedR / max(domainL, domainR), where a side's domain is its key ndv
// inside that range and its shared rows are the rows there. See "Join
// cardinality" in docs/arch/optimizations.md.
TJoinCardinality EstimateEquiJoin(
    double leftRows,
    double rightRows,
    const std::vector<TJoinKeyStats>& keys);

} // namespace NQdb
