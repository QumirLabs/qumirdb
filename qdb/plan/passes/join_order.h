#pragma once

#include <qdb/plan/ops/operator.h>
#include <qdb/plan/passes/estimate_stats.h>

#include <string>
#include <vector>

namespace NQdb {

struct TJoinReorderChainDiagnostics {
    size_t LeafCount = 0;
    size_t EdgeCount = 0;
    bool EnableCbo = true;
    bool UsedCbo = false;
    std::string Strategy;
    std::string Reason;
};

struct TJoinReorderDiagnostics {
    bool EnableCbo = true;
    bool UsedCbo = false;
    std::vector<TJoinReorderChainDiagnostics> Chains;
};

// Reorders inner-join chains (comma joins) into a connected left-deep order
// using the equalities of the governing WHERE filter, so that only genuinely
// disconnected relations stay cross-joined. Each relation is attached next to a
// relation it shares an equality with, so the subsequent equi-join extraction
// can lift a key for every connected adjacency.
//
// Runs after QualifyColumns, AnnotateTypes and EstimateStats (it needs leaf
// output schemas to map columns to relations, and leaf statistics) and before
// ExtractEquiJoins. `foldedBounds` is the query's memo of folded constants.
// Returns the new root.
TOperatorPtr ReorderJoins(
    TOperatorPtr root,
    TFoldedBounds& foldedBounds,
    bool enableCbo = true,
    TJoinReorderDiagnostics* diagnostics = nullptr);

// Heuristic semi/anti-join pushdown. A LeftSemi/LeftAnti join filters its build
// (left) side by existence in the probe side, so keeping it above a
// cardinality-expanding inner join forces the semi to buffer that expanded
// intermediate (semi/anti are blocking on the build side). When the semi's key
// and residual columns all come from ONE side of an inner join below it, the
// semi is pushed onto that side:
//   (A join B) semijoin C  ->  (A semijoin C) join B
// Applied recursively so the semi sinks as far as it can. This is what lets e.g.
// TPC-H Q18 apply its `o_orderkey IN (...)` filter on orders (~|orders| rows)
// instead of on customer join orders join lineitem (~|lineitem| rows).
//
// Runs after ExtractEquiJoins and AnnotateTypes (needs join keys and output
// schemas); re-annotate afterwards since it restructures the join tree.
TOperatorPtr PushDownSemiJoins(TOperatorPtr root);

} // namespace NQdb
