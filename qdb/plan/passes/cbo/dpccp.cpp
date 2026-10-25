#include <qdb/plan/passes/cbo/dpccp.h>

#include <qdb/plan/ops/join.h>
#include <qdb/plan/passes/estimate_stats.h>

#include <qumir/parser/type.h>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <map>
#include <memory>
#include <unordered_map>
#include <utility>

namespace NQdb {
namespace NCbo {
namespace {

using TSubset = uint32_t; // bitmask over leaf indices

struct TGraphEdge {
    size_t A;
    size_t B;
    double Sel;
    double KeyWidth;
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
    TDpccp(const std::vector<TOperatorPtr>& leaves, const std::vector<TJoinEdge>& edges)
        : Leaves_(leaves)
        , N_(leaves.size())
        , Neighbors_(leaves.size(), 0)
    {
        BuildGraph(edges);
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
            for (TSubset s1 = (s - 1) & s; s1; s1 = (s1 - 1) & s) {
                const TSubset s2 = s ^ s1;
                if (!dp[s1].Tree || !dp[s2].Tree) {
                    continue; // a side is disconnected
                }
                const double jc = card[s1] * card[s2] * CrossingSel(s1, s2);
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
                    auto stats = std::make_shared<TStats>();
                    stats->RowCount = std::max<uint64_t>(1, static_cast<uint64_t>(jc));
                    stats->Cost = cost;
                    tree->Stats_ = std::move(stats);
                    dp[s] = {std::move(tree), cost};
                    card[s] = jc;
                    width[s] = jw;
                }
            }
        }
        return dp[full].Tree; // null when the graph is disconnected
    }

private:
    double LeafRows(size_t i) const {
        return Leaves_[i]->Stats_ ? static_cast<double>(Leaves_[i]->Stats_->RowCount) : 0.0;
    }

    void BuildGraph(const std::vector<TJoinEdge>& edges) {
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
        // Group keys per relation pair, then ask the estimate propagator for the
        // pairwise join cardinality; its ratio to the cross product is the pair's
        // fixed selectivity (keeps the DP cost split-independent, composite keys
        // collapsed by the propagator's row-count clamp).
        std::map<std::pair<size_t, size_t>, std::vector<TJoinKeyStats>> pairKeys;
        std::map<std::pair<size_t, size_t>, double> pairKeyWidths;
        for (const auto& e : edges) {
            auto la = owner.find(e.LeftCol);
            auto lb = owner.find(e.RightCol);
            if (la == owner.end() || lb == owner.end() || la->second == lb->second) {
                continue;
            }
            const size_t a = la->second;
            const size_t b = lb->second;
            auto keyStats = EstimateJoinKey(Leaves_[a], e.LeftCol, Leaves_[b], e.RightCol);
            const double wa = ColumnWidth(Leaves_[a], e.LeftCol);
            const double wb = ColumnWidth(Leaves_[b], e.RightCol);
            const size_t i = std::min(a, b);
            const size_t j = std::max(a, b);
            if (a != i) {
                std::swap(keyStats.Left, keyStats.Right);
            }
            pairKeys[{i, j}].push_back(keyStats);
            pairKeyWidths[{i, j}] += std::max(wa, wb);
            Neighbors_[a] |= TSubset{1} << b;
            Neighbors_[b] |= TSubset{1} << a;
        }
        for (const auto& [key, keys] : pairKeys) {
            const double ri = LeafRows(key.first);
            const double rj = LeafRows(key.second);
            const double rows = EstimateEquiJoin(ri, rj, keys).Rows;
            const double sel = ri * rj > 0.0 ? rows / (ri * rj) : 1.0;
            Edges_.push_back({key.first, key.second, sel, pairKeyWidths[key]});
        }
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

    double CrossingSel(TSubset s1, TSubset s2) const {
        double sel = 1.0;
        for (const auto& e : Edges_) {
            const TSubset a = TSubset{1} << e.A;
            const TSubset b = TSubset{1} << e.B;
            if (((a & s1) && (b & s2)) || ((a & s2) && (b & s1))) {
                sel *= e.Sel;
            }
        }
        return sel;
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
};

} // namespace

TOperatorPtr DpccpJoinOrder(
    const std::vector<TOperatorPtr>& leaves,
    const std::vector<TJoinEdge>& edges)
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
    return TDpccp(leaves, edges).Solve();
}

} // namespace NCbo
} // namespace NQdb
