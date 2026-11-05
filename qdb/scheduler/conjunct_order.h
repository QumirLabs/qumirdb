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

} // namespace NScheduler
} // namespace NQdb
