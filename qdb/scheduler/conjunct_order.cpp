#include <qdb/scheduler/conjunct_order.h>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <unordered_set>
#include <utility>

namespace NQdb {
namespace NScheduler {

namespace {

// Bytes per row are a coarse proxy of the decoding cost, so close costs defer to selectivity.
constexpr double CloseCostRatio = 2.0;

class TStepBuilder {
public:
    explicit TStepBuilder(const std::unordered_map<std::string, double>& columnBytes)
        : ColumnBytes_(columnBytes)
    {}

    double NewCost(const TScanConjunct& conjunct) const {
        double cost = 0.0;
        for (const auto& column : conjunct.Columns) {
            if (!Decoded_.contains(column)) {
                cost += ColumnBytes_.at(column);
            }
        }
        return cost;
    }

    void Add(TScanConjunct conjunct) {
        std::vector<std::string> newColumns;
        for (auto& column : conjunct.Columns) {
            if (Decoded_.insert(column).second) {
                newColumns.push_back(std::move(column));
            }
        }
        if (Steps_.empty() || !newColumns.empty()) {
            Steps_.push_back({.NewColumns = std::move(newColumns)});
        }
        Steps_.back().Conjuncts.push_back(std::move(conjunct.Predicate));
    }

    std::vector<TScanFilterStep> Finish() {
        return std::move(Steps_);
    }

private:
    const std::unordered_map<std::string, double>& ColumnBytes_;
    std::unordered_set<std::string> Decoded_;
    std::vector<TScanFilterStep> Steps_;
};

} // namespace

std::vector<TScanFilterStep> OrderScanConjuncts(
    std::vector<TScanConjunct> conjuncts,
    const std::unordered_map<std::string, double>& columnBytes)
{
    TStepBuilder builder(columnBytes);
    std::vector<TScanConjunct> safe;
    std::vector<TScanConjunct> mayFail;
    for (auto& conjunct : conjuncts) {
        (conjunct.MayFail ? mayFail : safe).push_back(std::move(conjunct));
    }
    while (!safe.empty()) {
        std::vector<double> costs;
        double minCost = std::numeric_limits<double>::infinity();
        for (const auto& conjunct : safe) {
            costs.push_back(builder.NewCost(conjunct));
            minCost = std::min(minCost, costs.back());
        }
        size_t next = safe.size();
        for (size_t i = 0; i < safe.size(); ++i) {
            if (costs[i] > minCost * CloseCostRatio) {
                continue;
            }
            if (next == safe.size() || safe[i].Selectivity < safe[next].Selectivity) {
                next = i;
            }
        }
        builder.Add(std::move(safe[next]));
        safe.erase(safe.begin() + static_cast<std::ptrdiff_t>(next));
    }
    for (auto& conjunct : mayFail) {
        builder.Add(std::move(conjunct));
    }
    return builder.Finish();
}

} // namespace NScheduler
} // namespace NQdb
