#include <qdb/plan/passes/cbo/dpccp.h>

#include <qdb/plan/ops/join.h>
#include <qdb/plan/passes/estimate_stats.h>
#include <qdb/plan/passes/unbound_vars.h>

#include <qumir/parser/type.h>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>

namespace NQdb {
namespace NCbo {
namespace {

using TSubset = uint32_t; // bitmask over leaf indices

// A key equality between columns of two leaves.
struct TGraphEdge {
    size_t A;
    size_t B;
    std::string ColumnA;
    std::string ColumnB;
    double KeyWidth;
};

// A conjunct other than a key equality, over the leaves it reads.
struct TGraphResidual {
    TSubset Leaves;
    NQumir::NAst::TExprPtr Predicate;
};

struct TDpEntry {
    TOperatorPtr Tree;
    double Cost = 0.0;
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
        TFoldedBounds& foldedBounds)
        : Leaves_(leaves)
        , N_(leaves.size())
        , Neighbors_(leaves.size(), 0)
        , FoldedBounds_(foldedBounds)
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
                Leaves_[i],
                Leaves_[i]->Stats_ ? Leaves_[i]->Stats_->Cost : 0.0,
            };
        }

        for (TSubset s = 1; s <= full; ++s) {
            if (std::popcount(s) < 2 || !Connected(s)) {
                continue;
            }
            auto stats = SubsetStats(s, dp);
            if (!stats) {
                continue;
            }
            const double jc = static_cast<double>(stats->RowCount);
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
                if (!dp[s].Tree || cost < dp[s].Cost) {
                    auto tree = MakeJoin(dp[s1].Tree, dp[s2].Tree);
                    auto treeStats = std::make_shared<TStats>(*stats);
                    treeStats->Cost = cost;
                    tree->Stats_ = std::move(treeStats);
                    dp[s] = {std::move(tree), cost};
                    card[s] = jc;
                    width[s] = jw;
                }
            }
        }
        return dp[full].Tree; // null when the graph is disconnected
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
        for (const auto& e : edges) {
            auto la = owner.find(e.LeftCol);
            auto lb = owner.find(e.RightCol);
            if (la == owner.end() || lb == owner.end() || la->second == lb->second) {
                continue;
            }
            const size_t a = la->second;
            const size_t b = lb->second;
            const double keyWidth = std::max(
                ColumnWidth(Leaves_[a], e.LeftCol),
                ColumnWidth(Leaves_[b], e.RightCol));
            Edges_.push_back({a, b, e.LeftCol, e.RightCol, keyWidth});
            Neighbors_[a] |= TSubset{1} << b;
            Neighbors_[b] |= TSubset{1} << a;
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
            }
        }
    }

    // The plan's estimate of a join of two subsets: its keys are the edges
    // between them, its residual the conjuncts that the join is the first to
    // see whole. The estimate reads the inputs' statistics, so a key a join
    // has already selected is estimated as such.
    //
    // Each split of `s` estimates the same rows from different statistics, and
    // their estimates differ. DP needs one cardinality per set of relations,
    // and the largest one keeps an order from being chosen for an estimate the
    // other orders contradict.
    TStatsPtr SubsetStats(TSubset s, const std::vector<TDpEntry>& dp) const {
        TStatsPtr largest;
        const TSubset lowest = s & (~s + 1);
        for (TSubset s1 = (s - 1) & s; s1; s1 = (s1 - 1) & s) {
            const TSubset s2 = s ^ s1;
            // A join is estimated alike for either input order.
            if (!(s1 & lowest) || !dp[s1].Tree || !dp[s2].Tree) {
                continue;
            }
            auto join = std::make_shared<TJoinOperator>(
                dp[s1].Tree, dp[s2].Tree, CrossingKeys(s1, s2), EJoinType::Inner,
                CrossingResidual(s1, s2));
            auto stats = EstimateJoinStats(join, FoldedBounds_);
            if (stats && (!largest || stats->RowCount > largest->RowCount)) {
                largest = std::move(stats);
            }
        }
        return largest;
    }

    std::vector<TJoinKey> CrossingKeys(TSubset s1, TSubset s2) const {
        std::vector<TJoinKey> keys;
        for (const auto& e : Edges_) {
            const TSubset a = TSubset{1} << e.A;
            const TSubset b = TSubset{1} << e.B;
            if ((a & s1) && (b & s2)) {
                keys.push_back({e.ColumnA, e.ColumnB});
            } else if ((a & s2) && (b & s1)) {
                keys.push_back({e.ColumnB, e.ColumnA});
            }
        }
        return keys;
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

    double CrossingKeyWidth(TSubset s1, TSubset s2) const {
        double width = 0.0;
        for (const auto& e : Edges_) {
            const TSubset a = TSubset{1} << e.A;
            const TSubset b = TSubset{1} << e.B;
            if (((a & s1) && (b & s2)) || ((a & s2) && (b & s1))) {
                width += e.KeyWidth;
            }
        }
        return width;
    }

    static TOperatorPtr MakeJoin(const TOperatorPtr& l, const TOperatorPtr& r) {
        return std::make_shared<TJoinOperator>(
            l, r, std::vector<TJoinKey>{}, EJoinType::Inner, nullptr);
    }

    const std::vector<TOperatorPtr>& Leaves_;
    size_t N_;
    std::vector<TSubset> Neighbors_;
    std::vector<TGraphEdge> Edges_;
    std::vector<TGraphResidual> Residuals_;
    TFoldedBounds& FoldedBounds_;
};

} // namespace

TOperatorPtr DpccpJoinOrder(
    const std::vector<TOperatorPtr>& leaves,
    const std::vector<TJoinEdge>& edges,
    const std::vector<NQumir::NAst::TExprPtr>& residuals,
    TFoldedBounds& foldedBounds)
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
    return TDpccp(leaves, edges, residuals, foldedBounds).Solve();
}

} // namespace NCbo
} // namespace NQdb
