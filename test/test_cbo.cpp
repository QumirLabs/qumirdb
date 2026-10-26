#include <gtest/gtest.h>
#include "mock_source.h"

#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/operator.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/ops/stats.h>
#include <qdb/plan/passes/cbo/dpccp.h>
#include <qdb/plan/passes/const_fold.h>
#include <qdb/plan/passes/estimate_stats.h>
#include <qdb/plan/passes/unbound_vars.h>
#include <qdb/plan/build.h>
#include <qdb/plan/pipeline.h>
#include <qdb/plan/plan_print.h>
#include <qdb/sql/parser.h>

#include <qumir/parser/type.h>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

using namespace NQdb;

namespace {

using TSubset = uint32_t;

struct TColumnSpec {
    std::string Name;
    uint64_t Ndv;
    // Integer values in [Min, Max] with an equi-depth histogram of `Buckets`
    // buckets, uniform over the range; no range when Buckets is 0.
    int64_t Min = 0;
    int64_t Max = 0;
    size_t Buckets = 0;
};

struct TLeafSpec {
    uint64_t Rows;
    std::vector<TColumnSpec> Cols;
};

// Owns mock sources; builds leaf operators with Stats_ set.
struct TFixture {
    std::vector<std::unique_ptr<TMockSource>> Sources;
    std::vector<TOperatorPtr> Leaves;

    TOperatorPtr Add(const TLeafSpec& spec) {
        std::vector<std::string> names;
        for (const auto& col : spec.Cols) {
            names.push_back(col.Name);
        }
        Sources.push_back(std::make_unique<TMockSource>(names));
        auto op = std::make_shared<TSourceOperator>(*Sources.back(), std::string{});
        auto stats = std::make_shared<TStats>();
        stats->RowCount = spec.Rows;
        for (const auto& col : spec.Cols) {
            auto cs = std::make_shared<TStats::TColumnStats>();
            cs->Ndv = col.Ndv;
            if (col.Buckets > 0) {
                cs->MinValue = std::bit_cast<uint64_t>(col.Min);
                cs->MaxValue = std::bit_cast<uint64_t>(col.Max);
                for (size_t i = 0; i <= col.Buckets; ++i) {
                    const int64_t value = col.Min
                        + (col.Max - col.Min) * static_cast<int64_t>(i) / static_cast<int64_t>(col.Buckets);
                    cs->Histogram.push_back(std::bit_cast<uint64_t>(value));
                }
            }
            stats->ColumnStats[col.Name] = cs;
        }
        op->Stats_ = stats;
        Leaves.push_back(op);
        return op;
    }
};

NQumir::NAst::TExprPtr Compare(const char* op, const std::string& left, const std::string& right) {
    return std::make_shared<NQumir::NAst::TBinaryExpr>(
        NQumir::TLocation{}, NQumir::NAst::TOperator(op),
        std::make_shared<NQumir::NAst::TIdentExpr>(NQumir::TLocation{}, left),
        std::make_shared<NQumir::NAst::TIdentExpr>(NQumir::TLocation{}, right));
}

// Independent Cout model - mirrors dpccp's cost, used to check optimality. A
// set of leaves has one cardinality: the largest join estimate over its
// splits into two connected sets. A join is keyed by the equivalence classes
// of the edge columns that have columns on both sides, and a residual scales
// the join that first has all its leaves.
struct TModel {
    struct TMember {
        size_t Leaf;
        std::string Column;
        double Width;
    };

    struct TResidual {
        TSubset Leaves;
        NQumir::NAst::TExprPtr Predicate;
    };

    const std::vector<TOperatorPtr>& Leaves;
    std::map<const IOperator*, size_t> LeafIdx;
    std::vector<TSubset> Neighbors;
    std::vector<std::vector<TMember>> Classes;
    std::vector<TResidual> Residuals;
    // A join tree of each connected set, carrying the set's statistics.
    mutable std::map<TSubset, TOperatorPtr> Sets;
    mutable TConstFolder Folder;

    TModel(
        const std::vector<TOperatorPtr>& leaves,
        const std::vector<NCbo::TJoinEdge>& edges,
        const std::vector<NQumir::NAst::TExprPtr>& residuals)
        : Leaves(leaves)
        , Neighbors(leaves.size(), 0)
    {
        std::map<std::string, size_t> owner;
        for (size_t i = 0; i < Leaves.size(); ++i) {
            LeafIdx[Leaves[i].get()] = i;
            auto* schema = static_cast<NQumir::NAst::TStructType*>(Leaves[i]->OutputColumns().get());
            for (const auto& [name, type] : schema->Fields) {
                owner[name] = i;
            }
        }
        // Classes by repeated merging, in the order the columns appear.
        std::vector<std::set<std::string>> classes;
        std::vector<std::string> columns;
        for (const auto& e : edges) {
            size_t a = owner.at(e.LeftCol);
            size_t b = owner.at(e.RightCol);
            Neighbors[a] |= TSubset{1} << b;
            Neighbors[b] |= TSubset{1} << a;
            for (const auto& column : {e.LeftCol, e.RightCol}) {
                if (std::ranges::find(columns, column) == columns.end()) {
                    columns.push_back(column);
                }
            }
            std::set<std::string> merged{e.LeftCol, e.RightCol};
            std::erase_if(classes, [&](const std::set<std::string>& cls) {
                if (cls.contains(e.LeftCol) || cls.contains(e.RightCol)) {
                    merged.insert(cls.begin(), cls.end());
                    return true;
                }
                return false;
            });
            classes.push_back(std::move(merged));
        }
        std::map<size_t, size_t> classOf; // index in `classes` -> index in Classes
        for (const auto& column : columns) {
            size_t cls = 0;
            while (!classes[cls].contains(column)) {
                ++cls;
            }
            auto [it, inserted] = classOf.try_emplace(cls, Classes.size());
            if (inserted) {
                Classes.emplace_back();
            }
            size_t leaf = owner.at(column);
            Classes[it->second].push_back({leaf, column, NQdb::ColumnWidth(Leaves[leaf], column)});
        }
        for (const auto& predicate : residuals) {
            TSubset mask = 0;
            for (const auto& column : FindUnboundVars(predicate)) {
                mask |= TSubset{1} << owner.at(column);
            }
            Residuals.push_back({mask, predicate});
        }
    }

    double LeafRows(size_t idx) const {
        return static_cast<double>(Leaves[idx]->Stats_->RowCount);
    }

    double LeafWidth(size_t idx) const {
        return NQdb::EstimateTypeWidth(Leaves[idx]->OutputColumns());
    }

    double LeafCost(size_t idx) const {
        return Leaves[idx]->Stats_ ? Leaves[idx]->Stats_->Cost : 0.0;
    }

    std::vector<std::pair<const TMember*, const TMember*>> CrossingClasses(TSubset a, TSubset b) const {
        std::vector<std::pair<const TMember*, const TMember*>> crossing;
        for (const auto& members : Classes) {
            const TMember* left = nullptr;
            const TMember* right = nullptr;
            for (const auto& member : members) {
                TSubset bit = TSubset{1} << member.Leaf;
                if (!left && (bit & a)) {
                    left = &member;
                } else if (!right && (bit & b)) {
                    right = &member;
                }
            }
            if (left && right) {
                crossing.emplace_back(left, right);
            }
        }
        return crossing;
    }

    NQumir::NAst::TExprPtr CrossingResidual(TSubset a, TSubset b) const {
        NQumir::NAst::TExprPtr conjunction;
        for (const auto& r : Residuals) {
            if ((r.Leaves & (a | b)) != r.Leaves || (r.Leaves & a) == r.Leaves || (r.Leaves & b) == r.Leaves) {
                continue;
            }
            conjunction = conjunction
                ? std::make_shared<NQumir::NAst::TBinaryExpr>(
                    NQumir::TLocation{}, NQumir::NAst::TOperator("&&"), conjunction, r.Predicate)
                : r.Predicate;
        }
        return conjunction;
    }

    const TOperatorPtr& Set(TSubset s) const {
        if (std::popcount(s) == 1) {
            return Leaves[std::countr_zero(s)];
        }
        auto it = Sets.find(s);
        if (it != Sets.end()) {
            return it->second;
        }
        TOperatorPtr largest;
        const TSubset lowest = s & (~s + 1);
        for (TSubset s1 = (s - 1) & s; s1; s1 = (s1 - 1) & s) {
            TSubset s2 = s ^ s1;
            if (!(s1 & lowest) || !Connected(s1) || !Connected(s2)) {
                continue;
            }
            std::vector<TJoinKey> keys;
            for (const auto& [left, right] : CrossingClasses(s1, s2)) {
                keys.push_back({left->Column, right->Column});
            }
            auto join = std::make_shared<TJoinOperator>(
                Set(s1), Set(s2), std::move(keys), EJoinType::Inner, CrossingResidual(s1, s2));
            join->Stats_ = NQdb::EstimateJoinStats(join, Folder);
            if (!largest || join->Stats_->RowCount > largest->Stats_->RowCount) {
                largest = join;
            }
        }
        return Sets[s] = largest;
    }

    double Card(TSubset s) const {
        return static_cast<double>(Set(s)->Stats_->RowCount);
    }

    double CrossingKeyWidth(TSubset a, TSubset b) const {
        double width = 0.0;
        for (const auto& [left, right] : CrossingClasses(a, b)) {
            width += std::max(left->Width, right->Width);
        }
        return width;
    }

    bool Connected(TSubset s) const {
        TSubset visited = s & (~s + 1);
        TSubset frontier = visited;
        while (frontier) {
            TSubset next = 0;
            for (TSubset m = frontier; m; m &= m - 1) {
                next |= Neighbors[std::countr_zero(m)];
            }
            next &= s & ~visited;
            visited |= next;
            frontier = next;
        }
        return visited == s;
    }

    struct TEval {
        TSubset Mask;
        double Card;
        double Cost;
        double Width;
    };

    // EstimateJoinCost of a concrete tree (mirrors dpccp's cost).
    TEval Eval(const TOperatorPtr& node) const {
        if (auto join = TMaybeOp<TJoinOperator>(node)) {
            auto left = Eval(join.Cast()->Left());
            auto right = Eval(join.Cast()->Right());
            double card = Card(left.Mask | right.Mask);
            double width = left.Width + right.Width;
            double cost = NQdb::EstimateJoinCost(
                left.Cost, right.Cost, left.Card, right.Card, card, width,
                CrossingKeyWidth(left.Mask, right.Mask));
            return {left.Mask | right.Mask, card, cost, width};
        }
        size_t idx = LeafIdx.at(node.get());
        return {TSubset{1} << idx, LeafRows(idx), LeafCost(idx), LeafWidth(idx)};
    }

    // Brute-force optimum over all bushy trees for the leaf set `s`.
    double BruteMin(
        TSubset s,
        std::vector<double>& cost,
        std::vector<double>& card,
        std::vector<double>& width) const
    {
        if (std::popcount(s) == 1) {
            size_t i = std::countr_zero(s);
            card[s] = LeafRows(i);
            width[s] = LeafWidth(i);
            return cost[s] = LeafCost(i);
        }
        if (cost[s] >= 0) {
            return cost[s];
        }
        double best = std::numeric_limits<double>::infinity();
        for (TSubset s1 = (s - 1) & s; s1; s1 = (s1 - 1) & s) {
            TSubset s2 = s ^ s1;
            if (!Connected(s1) || !Connected(s2)) {
                continue;
            }
            double c1 = BruteMin(s1, cost, card, width);
            double c2 = BruteMin(s2, cost, card, width);
            double jc = Card(s);
            double jw = width[s1] + width[s2];
            double c = NQdb::EstimateJoinCost(
                c1, c2, card[s1], card[s2], jc, jw, CrossingKeyWidth(s1, s2));
            if (c < best) {
                best = c;
                card[s] = jc;
                width[s] = jw;
            }
        }
        return cost[s] = best;
    }
};

// DPccp result must be an optimal-cost tree covering every leaf, and its
// estimate must be the model's estimate of the whole set.
void ExpectOptimal(
    const TFixture& fx,
    const std::vector<NCbo::TJoinEdge>& edges,
    const std::vector<NQumir::NAst::TExprPtr>& residuals = {})
{
    TConstFolder folder;
    auto tree = NCbo::DpccpJoinOrder(fx.Leaves, edges, residuals, folder);
    ASSERT_NE(tree, nullptr);

    TModel model(fx.Leaves, edges, residuals);
    auto ev = model.Eval(tree);
    TSubset full = (TSubset{1} << fx.Leaves.size()) - 1;
    EXPECT_EQ(ev.Mask, full) << "tree does not cover all leaves";
    ASSERT_TRUE(tree->Stats_);
    EXPECT_EQ(static_cast<double>(tree->Stats_->RowCount), model.Card(full));

    std::vector<double> cost(full + 1, -1.0);
    std::vector<double> card(full + 1, 0.0);
    std::vector<double> width(full + 1, 0.0);
    double best = model.BruteMin(full, cost, card, width);
    EXPECT_NEAR(ev.Cost, best, best * 1e-9 + 1.0) << "DP tree is not optimal";
}

} // namespace

TEST(Cbo, ChainOptimal) {
    TFixture fx;
    fx.Add({1000, {{"a_id", 1000}}});
    fx.Add({500, {{"b_a", 500}, {"b_c", 50}}});
    fx.Add({20, {{"c_id", 20}}});
    fx.Add({100, {{"d_a", 100}}});
    // a-b, b-c, a-d
    ExpectOptimal(fx, {{"a_id", "b_a"}, {"b_c", "c_id"}, {"a_id", "d_a"}});
}

TEST(Cbo, StarOptimal) {
    TFixture fx;
    fx.Add({1000000, {{"f_d1", 1000000}, {"f_d2", 1000000}, {"f_d3", 1000000}}}); // fact
    fx.Add({10, {{"d1_id", 10}}});
    fx.Add({500, {{"d2_id", 500}}});
    fx.Add({50000, {{"d3_id", 50000}}});
    ExpectOptimal(fx, {{"f_d1", "d1_id"}, {"f_d2", "d2_id"}, {"f_d3", "d3_id"}});
}

TEST(Cbo, CycleOptimal) {
    TFixture fx;
    fx.Add({300, {{"a_id", 300}, {"a_d", 300}}});
    fx.Add({40, {{"b_a", 40}, {"b_c", 40}}});
    fx.Add({900, {{"c_id", 900}}});
    fx.Add({70, {{"d_id", 70}}});
    // a-b, b-c, c... actually cycle a-b, b-c, a-d, plus close: c-... use a-b,b-c,a-c,a-d
    ExpectOptimal(fx, {{"a_id", "b_a"}, {"b_c", "c_id"}, {"a_id", "c_id"}, {"a_d", "d_id"}});
}

TEST(Cbo, PkFkChainFiveOptimal) {
    TFixture fx;
    fx.Add({8000000, {{"ps_sk", 100000}, {"ps_pk", 200000}}});
    fx.Add({100000, {{"s_sk", 100000}, {"s_nk", 25}}});
    fx.Add({25, {{"n_nk", 25}, {"n_rk", 5}}});
    fx.Add({5, {{"r_rk", 5}}});
    fx.Add({200000, {{"p_pk", 200000}}});
    ExpectOptimal(fx, {
        {"ps_sk", "s_sk"}, {"s_nk", "n_nk"}, {"n_rk", "r_rk"}, {"ps_pk", "p_pk"}});
}

// A conjunct other than a key equality scales the join that brings its
// relations together, as it scales the join's estimate in the plan, and does
// not connect relations.
TEST(Cbo, ResidualScalesTheJoinItCompletes) {
    TFixture fx;
    fx.Add({1000, {{"a_b", 100}, {"a_x", 2}}});
    fx.Add({100, {{"b_id", 100}, {"b_c", 10}}});
    fx.Add({10, {{"c_id", 10}, {"c_y", 2}}});
    auto residual = std::make_shared<NQumir::NAst::TBinaryExpr>(
        NQumir::TLocation{}, NQumir::NAst::TOperator("!="),
        std::make_shared<NQumir::NAst::TIdentExpr>(NQumir::TLocation{}, "a_x"),
        std::make_shared<NQumir::NAst::TIdentExpr>(NQumir::TLocation{}, "c_y"));
    TConstFolder folder;
    auto tree = NCbo::DpccpJoinOrder(
        fx.Leaves, {{"a_b", "b_id"}, {"b_c", "c_id"}}, {residual}, folder);
    ASSERT_TRUE(tree && tree->Stats_);
    // 1000 rows find their b and their c; a_x != c_y keeps half of them.
    EXPECT_EQ(tree->Stats_->RowCount, 500u);

    TFixture pair;
    pair.Add({1000, {{"a_x", 2}}});
    pair.Add({10, {{"c_y", 2}}});
    EXPECT_EQ(NCbo::DpccpJoinOrder(pair.Leaves, {}, {residual}, folder), nullptr);
}

TEST(Cbo, DisconnectedReturnsNull) {
    TFixture fx;
    fx.Add({100, {{"a_id", 100}}});
    fx.Add({100, {{"b_id", 100}}}); // isolated
    fx.Add({100, {{"c_a", 100}}});
    TConstFolder folder;
    EXPECT_EQ(NCbo::DpccpJoinOrder(fx.Leaves, {{"a_id", "c_a"}}, {}, folder), nullptr);
}

TEST(Cbo, SingleLeafReturnsLeaf) {
    TFixture fx;
    auto leaf = fx.Add({100, {{"a_id", 100}}});
    TConstFolder folder;
    EXPECT_EQ(NCbo::DpccpJoinOrder(fx.Leaves, {}, {}, folder), leaf);
}

TEST(Cbo, FallbackAboveMaxRelations) {
    TFixture fx;
    for (size_t i = 0; i < NCbo::MaxRelations + 1; ++i) {
        fx.Add({100, {{"c" + std::to_string(i), 100}}});
    }
    TConstFolder folder;
    EXPECT_EQ(NCbo::DpccpJoinOrder(fx.Leaves, {}, {}, folder), nullptr);
}

// Random connected graphs (spanning tree + extra edges) must all be optimal.
// Their key columns have ranges and histograms, so range alignment and the
// histograms dropped by joins shape the estimates; an extra edge may reuse a
// column, which puts three or more columns in one equivalence class; and a
// residual may compare two leaves.
TEST(Cbo, RandomGraphsOptimal) {
    uint32_t seed = 987654321;
    auto rnd = [&](int lo, int hi) {
        seed = seed * 1103515245u + 12345u;
        return lo + static_cast<int>((seed & 0x7fffffff) % static_cast<uint32_t>(hi - lo + 1));
    };
    auto column = [&](std::string name, uint64_t rows) {
        TColumnSpec spec{std::move(name), static_cast<uint64_t>(rnd(1, static_cast<int>(rows)))};
        if (rnd(0, 2) > 0) {
            spec.Min = rnd(0, 1000);
            spec.Max = spec.Min + rnd(0, 2000);
            spec.Buckets = rnd(1, 8);
            spec.Ndv = std::min<uint64_t>(spec.Ndv, spec.Max - spec.Min + 1);
        }
        return spec;
    };
    for (int iter = 0; iter < 200; ++iter) {
        size_t n = rnd(2, 6);
        std::vector<uint64_t> rows(n);
        std::vector<std::vector<TColumnSpec>> cols(n);
        for (size_t i = 0; i < n; ++i) {
            rows[i] = rnd(1, 100000);
            cols[i].push_back(column("v" + std::to_string(i), rows[i]));
        }
        std::vector<std::pair<size_t, size_t>> pairs;
        for (size_t i = 1; i < n; ++i) {                 // spanning tree -> connected
            pairs.emplace_back(i, static_cast<size_t>(rnd(0, static_cast<int>(i) - 1)));
        }
        for (int k = 0, extra = rnd(0, static_cast<int>(n)); k < extra; ++k) {
            size_t i = rnd(0, static_cast<int>(n) - 1);
            size_t j = rnd(0, static_cast<int>(n) - 1);
            if (i != j) {
                pairs.emplace_back(i, j);
            }
        }
        std::vector<NCbo::TJoinEdge> edges;
        for (size_t e = 0; e < pairs.size(); ++e) {
            auto [i, j] = pairs[e];
            std::string cl = "e" + std::to_string(e) + "l";
            std::string cr = "e" + std::to_string(e) + "r";
            if (cols[i].size() > 1 && rnd(0, 3) == 0) {
                cl = cols[i][rnd(1, static_cast<int>(cols[i].size()) - 1)].Name;
            } else {
                cols[i].push_back(column(cl, rows[i]));
            }
            cols[j].push_back(column(cr, rows[j]));
            edges.push_back({cl, cr});
        }
        std::vector<NQumir::NAst::TExprPtr> residuals;
        for (int k = 0, count = rnd(0, 2); k < count && n > 2; ++k) {
            size_t i = rnd(0, static_cast<int>(n) - 1);
            size_t j = (i + rnd(1, static_cast<int>(n) - 1)) % n;
            static const char* ops[] = {"<", ">=", "!="};
            residuals.push_back(Compare(
                ops[rnd(0, 2)],
                cols[i][rnd(0, static_cast<int>(cols[i].size()) - 1)].Name,
                cols[j][rnd(0, static_cast<int>(cols[j].size()) - 1)].Name));
        }
        TFixture fx;
        for (size_t i = 0; i < n; ++i) {
            fx.Add({rows[i], cols[i]});
        }
        ExpectOptimal(fx, edges, residuals);
    }
}

// A join of a composite foreign key whose column another relation's column
// equals: lineitem's supplier key is also supplier's. Whichever column of the
// class keys the join, the key pairs join lineitem and partsupp, and DPccp
// estimates the join as the plan estimates it.
TEST(Cbo, CompositeKeyThroughAnEquatedColumn) {
    TFixture fx;
    fx.Add({60000, {{"l_partkey", 2000}, {"l_suppkey", 100}}});
    fx.Add({100, {{"s_suppkey", 100}}});
    fx.Add({8000, {{"ps_partkey", 2000}, {"ps_suppkey", 100}}});
    const std::vector<NCbo::TJoinEdge> edges{
        {"s_suppkey", "l_suppkey"}, {"ps_suppkey", "l_suppkey"}, {"ps_partkey", "l_partkey"}};
    ExpectOptimal(fx, edges);

    TConstFolder folder;
    auto lineitemSupplier = std::make_shared<TJoinOperator>(
        fx.Leaves[0], fx.Leaves[1], std::vector<TJoinKey>{{"l_suppkey", "s_suppkey"}},
        EJoinType::Inner, nullptr);
    lineitemSupplier->Stats_ = EstimateJoinStats(lineitemSupplier, folder);
    // Every lineitem row has its partsupp row: the composite key keeps the
    // larger input, whichever supplier key column is the key.
    for (const auto* suppkey : {"l_suppkey", "s_suppkey"}) {
        const std::vector<TJoinKey> keys{{suppkey, "ps_suppkey"}, {"l_partkey", "ps_partkey"}};
        EXPECT_EQ(EstimateInnerJoinRows(lineitemSupplier, fx.Leaves[2], keys, nullptr, folder), 60000u)
            << suppkey;
    }
}

namespace {

class TWidthSource final : public TMockSource {
public:
    TWidthSource(
        std::vector<std::string> names,
        std::vector<NQumir::NAst::TTypePtr> types,
        uint64_t rows,
        std::vector<uint64_t> ndvs)
        : TMockSource(std::move(names), {}, std::move(types))
        , Stats_(std::make_shared<TStats>())
    {
        Stats_->RowCount = rows;
        for (size_t i = 0; i < Names.size(); ++i) {
            auto column = std::make_shared<TStats::TColumnStats>();
            column->Ndv = ndvs[i];
            Stats_->ColumnStats[Names[i]] = std::move(column);
        }
    }

    const TStatsPtr Stats() const override {
        return Stats_;
    }

private:
    TStatsPtr Stats_;
};

struct TWidthPlan {
    std::string Text;
    std::set<std::string> FirstPair;
    bool UsedCbo = false;
    size_t OutputColumns = 0;
};

TWidthPlan PlanWithPayload(size_t payloadColumns, bool demandPayload) {
    using namespace NQumir::NAst;
    auto integer = std::make_shared<TIntegerType>();
    auto string = std::make_shared<TStringType>();
    std::vector<std::string> aNames{"id"};
    std::vector<TTypePtr> aTypes{integer};
    std::vector<uint64_t> aNdvs{100};
    std::string select = "select a.id, c.payload";
    for (size_t i = 0; i < payloadColumns; ++i) {
        auto name = "payload_" + std::to_string(i);
        aNames.push_back(name);
        aTypes.push_back(string);
        aNdvs.push_back(100);
        if (demandPayload) {
            select += ", a." + name;
        }
    }
    TWidthSource a(std::move(aNames), std::move(aTypes), 100, std::move(aNdvs));
    TWidthSource b({"a_id", "c_id"}, {integer, integer}, 1000, {100, 1000});
    TWidthSource c({"id", "payload"}, {integer, string}, 1000, {1000, 1000});
    std::map<std::string, ISource*> sources{{"a", &a}, {"b", &b}, {"c", &c}};
    std::istringstream sql(select
        + " from a, b, c where a.id = b.a_id and b.c_id = c.id");
    NSql::TTokenStream tokens(sql);
    auto parsed = NSql::TParser{}.Parse(tokens);
    if (!parsed) {
        throw std::runtime_error(parsed.error().ToString());
    }
    auto built = BuildPlan(*parsed, [&](std::string_view table)
        -> std::expected<TOperatorPtr, NQumir::TError>
    {
        return std::make_shared<TSourceOperator>(
            *sources.at(std::string(table)), std::string(table));
    });
    if (!built) {
        throw std::runtime_error(built.error().ToString());
    }
    auto plan = *built;
    TPlanPassDiagnostics diagnostics;
    ApplyPlanPasses(plan, {.EnableRuntimeFilters = false, .Diagnostics = &diagnostics});
    TWidthPlan result;
    result.UsedCbo = diagnostics.JoinReorder.UsedCbo;
    result.OutputColumns = plan->OutputColumns()->Fields.size();
    std::ostringstream text;
    PrintPlanTree(text, plan);
    result.Text = text.str();
    std::function<std::set<std::string>(const TOperatorPtr&)> leaves =
        [&](const TOperatorPtr& op) {
        if (auto source = TMaybeOp<TSourceOperator>(op)) {
            return std::set<std::string>{source.Cast()->GetAlias()};
        }
        std::set<std::string> names;
        for (const auto& input : op->Inputs()) {
            const auto child = leaves(input);
            names.insert(child.begin(), child.end());
        }
        if (TMaybeOp<TJoinOperator>(op) && names.size() == 2) {
            result.FirstPair = names;
        }
        return names;
    };
    leaves(plan);
    return result;
}

} // namespace

TEST(Cbo, UnusedPayloadDoesNotChangeProductionJoinOrder) {
    const auto narrow = PlanWithPayload(0, false);
    const auto unusedWide = PlanWithPayload(64, false);
    ASSERT_TRUE(narrow.UsedCbo);
    ASSERT_TRUE(unusedWide.UsedCbo);
    EXPECT_EQ(narrow.FirstPair, (std::set<std::string>{"a", "b"}));
    EXPECT_EQ(unusedWide.Text, narrow.Text);
}

TEST(Cbo, DemandedPayloadStillAffectsProductionJoinOrder) {
    const auto unused = PlanWithPayload(64, false);
    const auto demanded = PlanWithPayload(64, true);
    ASSERT_TRUE(unused.UsedCbo);
    ASSERT_TRUE(demanded.UsedCbo);
    EXPECT_EQ(unused.FirstPair, (std::set<std::string>{"a", "b"}));
    EXPECT_EQ(demanded.FirstPair, (std::set<std::string>{"b", "c"}));
    EXPECT_EQ(demanded.OutputColumns, 66);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
