#include <qdb/plan/passes/cbo/dpccp.h>

#include <qdb/plan/ops/join.h>
#include <qdb/plan/passes/estimate_stats.h>
#include <qdb/plan/passes/unbound_vars.h>
#include <qdb/utils/union_find.h>

#include <qumir/parser/type.h>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace NQdb {
namespace NCbo {
namespace {

using TSubset = uint32_t; // bitmask over leaf indices

// A leaf's column that a key equality names.
struct TClassMember {
    size_t Leaf;
    std::string Column;
    double Width;
};

// A conjunct other than a key equality, over the leaves it reads.
struct TGraphResidual {
    TSubset Leaves;
    NQumir::NAst::TExprPtr Predicate;
};

struct TDpEntry {
    // The cheapest join tree of the subset, keyed as the plan keys it and
    // carrying the subset's statistics.
    TOperatorPtr Tree;
    double Cost = 0.0;
    // The leaves of the left input of the tree's top join; 0 for a leaf.
    TSubset Left = 0;
};

// DP over subsets ordered by increasing bitmask, so every proper subset is
// solved before its supersets. Optimal bushy tree; DPccp's csg-cmp enumeration
// (see header link) is a drop-in optimization for large n.
class TDpccp {
public:
    TDpccp(
        const std::vector<TOperatorPtr>& leaves,
        const std::vector<TJoinEdge>& edges,
        const std::vector<NQumir::NAst::TExprPtr>& residuals,
        TConstFolder& folder)
        : Leaves_(leaves)
        , N_(leaves.size())
        , Neighbors_(leaves.size(), 0)
        , Folder_(folder)
    {
        BuildGraph(edges, residuals);
    }

    TOperatorPtr Solve() {
        const TSubset full = (TSubset{1} << N_) - 1;
        std::vector<double> card(full + 1, 0.0);
        std::vector<double> width(full + 1, 0.0);
        std::vector<TDpEntry> dp(full + 1);

        for (size_t i = 0; i < N_; ++i) {
            const TSubset s = TSubset{1} << i;
            card[s] = Leaves_[i]->Stats_ ? static_cast<double>(Leaves_[i]->Stats_->RowCount) : 0.0;
            width[s] = EstimateTypeWidth(Leaves_[i]->OutputColumns());
            dp[s] = {
                .Tree = Leaves_[i],
                .Cost = Leaves_[i]->Stats_ ? Leaves_[i]->Stats_->Cost : 0.0,
            };
        }

        for (TSubset s = 1; s <= full; ++s) {
            if (std::popcount(s) < 2 || !Connected(s)) {
                continue;
            }
            const TSubset largest = LargestSplit(s, dp);
            if (!largest) {
                continue;
            }
            // Only the rows of each split were estimated; the statistics of
            // the set are those of the largest split.
            auto largestJoin = Join(largest, s ^ largest, dp);
            auto stats = EstimateJoinStats(largestJoin, Folder_, &EstimatedColumns_);
            const double jc = static_cast<double>(stats->RowCount);
            TSubset best = 0;
            double bestCost = 0.0;
            for (TSubset s1 = (s - 1) & s; s1; s1 = (s1 - 1) & s) {
                const TSubset s2 = s ^ s1;
                if (!dp[s1].Tree || !dp[s2].Tree) {
                    continue; // a side is disconnected
                }
                const double jw = width[s1] + width[s2];
                const double cost = EstimateJoinCost(
                    dp[s1].Cost,
                    dp[s2].Cost,
                    card[s1],
                    card[s2],
                    jc,
                    jw,
                    CrossingKeyWidth(s1, s2));
                if (!best || cost < bestCost) {
                    best = s1;
                    bestCost = cost;
                }
            }
            auto tree = best == largest
                ? std::move(largestJoin)
                : Join(best, s ^ best, dp);
            stats->Cost = bestCost;
            tree->Stats_ = std::move(stats);
            dp[s] = {.Tree = std::move(tree), .Cost = bestCost, .Left = best};
            card[s] = jc;
            width[s] = width[best] + width[s ^ best];
        }
        return dp[full].Tree
            ? Unkeyed(full, dp)
            : nullptr; // the graph is disconnected
    }

private:
    void BuildGraph(
        const std::vector<TJoinEdge>& edges,
        const std::vector<NQumir::NAst::TExprPtr>& residuals)
    {
        std::unordered_map<std::string, size_t> owner;
        for (size_t i = 0; i < N_; ++i) {
            auto&& schema = Leaves_[i]->OutputColumns();
            if (!schema) {
                continue;
            }
            for (const auto& [name, type] : schema->Fields) {
                owner.emplace(name, i);
            }
        }
        // The equalities form equivalence classes of columns, and a join
        // between two subsets is keyed by every class with columns on both
        // sides, as ExtractEquiJoins keys it.
        TUnionFind<std::string> classes;
        std::vector<std::string> columns;
        std::unordered_set<std::string> seen;
        for (const auto& e : edges) {
            auto la = owner.find(e.LeftCol);
            auto lb = owner.find(e.RightCol);
            if (la == owner.end() || lb == owner.end() || la->second == lb->second) {
                continue;
            }
            Neighbors_[la->second] |= TSubset{1} << lb->second;
            Neighbors_[lb->second] |= TSubset{1} << la->second;
            classes.Union(e.LeftCol, e.RightCol);
            for (const auto& column : {e.LeftCol, e.RightCol}) {
                if (seen.insert(column).second) {
                    columns.push_back(column);
                }
            }
        }
        std::unordered_map<int, size_t> classIndex;
        for (const auto& column : columns) {
            auto [it, inserted] = classIndex.try_emplace(classes.Find(column), Classes_.size());
            if (inserted) {
                Classes_.emplace_back();
            }
            const size_t leaf = owner.at(column);
            Classes_[it->second].push_back({leaf, column, ColumnWidth(Leaves_[leaf], column)});
            EstimatedColumns_.insert(column);
        }
        for (const auto& predicate : residuals) {
            TSubset leaves = 0;
            for (const auto& column : FindUnboundVars(predicate)) {
                auto it = owner.find(column);
                if (it == owner.end()) {
                    leaves = 0; // it reads a relation outside the graph
                    break;
                }
                leaves |= TSubset{1} << it->second;
            }
            if (std::popcount(leaves) >= 2) {
                Residuals_.push_back({leaves, predicate});
                for (auto& column : FindUnboundVars(predicate)) {
                    EstimatedColumns_.insert(std::move(column));
                }
            }
        }
    }

    // The split of `s` with the largest estimate, by its left input.
    //
    // Each split of `s` estimates the same rows from different statistics, and
    // their estimates differ. DP needs one cardinality per set of relations,
    // and the largest one keeps an order from being chosen for an estimate the
    // other orders contradict. Each join is estimated as the plan estimates it:
    // its keys are the classes the two subsets share, its residual the
    // conjuncts that the join is the first to see whole, and the estimate reads
    // the inputs' statistics, so a key a join has already selected is
    // estimated as such.
    TSubset LargestSplit(TSubset s, const std::vector<TDpEntry>& dp) const {
        TSubset largest = 0;
        uint64_t largestRows = 0;
        const TSubset lowest = s & (~s + 1);
        for (TSubset s1 = (s - 1) & s; s1; s1 = (s1 - 1) & s) {
            const TSubset s2 = s ^ s1;
            // A join is estimated alike for either input order.
            if (!(s1 & lowest) || !dp[s1].Tree || !dp[s2].Tree) {
                continue;
            }
            const uint64_t rows = EstimateInnerJoinRows(
                dp[s1].Tree,
                dp[s2].Tree,
                CrossingKeys(s1, s2),
                CrossingResidual(s1, s2),
                Folder_);
            if (!largest || rows > largestRows) {
                largest = s1;
                largestRows = rows;
            }
        }
        return largest;
    }

    std::shared_ptr<TJoinOperator> Join(TSubset s1, TSubset s2, const std::vector<TDpEntry>& dp) const {
        return std::make_shared<TJoinOperator>(
            dp[s1].Tree,
            dp[s2].Tree,
            CrossingKeys(s1, s2),
            EJoinType::Inner,
            CrossingResidual(s1, s2));
    }

    // The tree of `s` without keys and residuals: ExtractEquiJoins places
    // them in the plan.
    TOperatorPtr Unkeyed(TSubset s, const std::vector<TDpEntry>& dp) const {
        if (std::popcount(s) == 1) {
            return dp[s].Tree;
        }
        auto join = std::make_shared<TJoinOperator>(
            Unkeyed(dp[s].Left, dp),
            Unkeyed(s ^ dp[s].Left, dp),
            std::vector<TJoinKey>{},
            EJoinType::Inner,
            nullptr);
        join->Stats_ = dp[s].Tree->Stats_;
        return join;
    }

    // A column of each class that has columns on both sides.
    template <class TFn>
    void ForEachCrossingClass(TSubset s1, TSubset s2, TFn&& fn) const {
        for (const auto& members : Classes_) {
            const TClassMember* left = nullptr;
            const TClassMember* right = nullptr;
            for (const auto& member : members) {
                const TSubset leaf = TSubset{1} << member.Leaf;
                if (!left && (leaf & s1)) {
                    left = &member;
                } else if (!right && (leaf & s2)) {
                    right = &member;
                }
            }
            if (left && right) {
                fn(*left, *right);
            }
        }
    }

    std::vector<TJoinKey> CrossingKeys(TSubset s1, TSubset s2) const {
        std::vector<TJoinKey> keys;
        ForEachCrossingClass(s1, s2, [&](const TClassMember& left, const TClassMember& right) {
            keys.push_back({left.Column, right.Column});
        });
        return keys;
    }

    double CrossingKeyWidth(TSubset s1, TSubset s2) const {
        double width = 0.0;
        ForEachCrossingClass(s1, s2, [&](const TClassMember& left, const TClassMember& right) {
            width += std::max(left.Width, right.Width);
        });
        return width;
    }

    // The residuals over leaves of both subsets and of no other relation.
    NQumir::NAst::TExprPtr CrossingResidual(TSubset s1, TSubset s2) const {
        const TSubset s = s1 | s2;
        NQumir::NAst::TExprPtr conjunction;
        for (const auto& r : Residuals_) {
            if ((r.Leaves & s) != r.Leaves || (r.Leaves & s1) == r.Leaves || (r.Leaves & s2) == r.Leaves) {
                continue;
            }
            conjunction = conjunction
                ? std::make_shared<NQumir::NAst::TBinaryExpr>(
                    r.Predicate->Location, NQumir::NAst::TOperator("&&"), conjunction, r.Predicate)
                : r.Predicate;
        }
        return conjunction;
    }

    bool Connected(TSubset s) const {
        TSubset visited = s & (~s + 1); // lowest set bit
        TSubset frontier = visited;
        while (frontier) {
            TSubset next = 0;
            for (TSubset m = frontier; m; m &= m - 1) {
                next |= Neighbors_[std::countr_zero(m)];
            }
            next &= s & ~visited;
            visited |= next;
            frontier = next;
        }
        return visited == s;
    }

    const std::vector<TOperatorPtr>& Leaves_;
    size_t N_;
    std::vector<TSubset> Neighbors_;
    std::vector<std::vector<TClassMember>> Classes_;
    std::vector<TGraphResidual> Residuals_;
    // The columns whose statistics the estimates of joins read: those of the
    // classes and of the residuals. A subset's statistics keep only them.
    std::unordered_set<std::string> EstimatedColumns_;
    TConstFolder& Folder_;
};

} // namespace

TOperatorPtr DpccpJoinOrder(
    const std::vector<TOperatorPtr>& leaves,
    const std::vector<TJoinEdge>& edges,
    const std::vector<NQumir::NAst::TExprPtr>& residuals,
    TConstFolder& folder)
{
    if (leaves.empty()) {
        return nullptr;
    }
    if (leaves.size() == 1) {
        return leaves.front();
    }
    if (leaves.size() > MaxRelations) {
        return nullptr;
    }
    return TDpccp(leaves, edges, residuals, folder).Solve();
}

} // namespace NCbo
} // namespace NQdb
