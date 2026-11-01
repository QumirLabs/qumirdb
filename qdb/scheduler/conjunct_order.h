#pragma once

#include <qumir/parser/ast.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace NQdb {
namespace NScheduler {

// A conjunct of a filter over a scan.
struct TScanConjunct {
    NQumir::NAst::TExprPtr Predicate;
    // The scan columns it reads.
    std::vector<std::string> Columns;
    // The estimated fraction of rows it keeps.
    double Selectivity = 1.0;
    // It calls an external function, which may fail on a row an earlier conjunct rejects.
    bool MayFail = false;
};

// Conjuncts evaluated once the scan has decoded NewColumns as well as the earlier ones.
struct TScanFilterStep {
    std::vector<std::string> NewColumns;
    std::vector<NQumir::NAst::TExprPtr> Conjuncts;
};

// Orders the conjuncts of a filter over a scan: next comes the one whose new columns cost
// the least to decode (`columnBytes`: bytes per row of every column), and of costs within a
// factor of two the most selective. A conjunct that may fail goes after all that cannot.
std::vector<TScanFilterStep> OrderScanConjuncts(
    std::vector<TScanConjunct> conjuncts,
    const std::unordered_map<std::string, double>& columnBytes);

// A step of a staged scan after stage 0, by its estimated selectivity and its cost in
// bytes per row.
struct TRankedStep {
    double Selectivity = 1.0;
    double Cost = 0.0;
};

// Where runtime filters go among the later stages of a staged scan: each before the first
// stage of lower rank (1 - s) / c, the order of expensive predicates by Hellerstein and
// Stonebraker. Returns for each filter the number of stages before it.
std::vector<size_t> PlaceRuntimeFilters(
    const std::vector<TRankedStep>& stages,
    const std::vector<TRankedStep>& filters);

} // namespace NScheduler
} // namespace NQdb
