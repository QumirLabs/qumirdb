#include "equijoin.h"

#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/limit.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/sort.h>
#include <qdb/plan/ops/union.h>
#include <qdb/plan/ops/window.h>
#include <qdb/plan/clone_expr.h>
#include <qdb/plan/passes/unbound_vars.h>
#include <qdb/utils/union_find.h>

#include "factor_conjuncts.h"
#include "flatten_conjuncts.h"
#include "flatten_disjuncts.h"
#include "const_fold.h"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace NQdb {

using namespace NQumir::NAst;

namespace {

// PushOnly pushes single-side predicates to leaves but leaves joins empty-key;
// ExtractKeys additionally lifts equalities into join keys.
enum class EMode { PushOnly, ExtractKeys };

struct TConjuct {
    TExprPtr Expr = nullptr;
    bool equiv = false;
    std::string left;
    std::string right;
};

TExprPtr ConjuctExpr(const TConjuct& conj) {
    if (conj.Expr) {
        return conj.Expr;
    } else {
        return
            std::make_shared<TBinaryExpr>(NQumir::TLocation{}, TOperator("=="),
                std::make_shared<TIdentExpr>(NQumir::TLocation{}, conj.left),
                std::make_shared<TIdentExpr>(NQumir::TLocation{}, conj.right));
    }
}

TExprPtr Conjoin(const std::vector<TConjuct>& conjucts) {
    TExprPtr result;
    for (const auto& conj : conjucts) {
        auto expr = ConjuctExpr(conj);
        result = result
            ? std::make_shared<TBinaryExpr>(expr->Location, TOperator("&&"), result, expr)
            : expr;
    }
    return result;
}

TOperatorPtr Materialize(TOperatorPtr result, const std::vector<TConjuct>& conjucts) {
    return conjucts.empty()
        ? result
        : std::make_shared<TFilterOperator>(std::move(result), Conjoin(conjucts));
}

std::vector<TJoinKey> ExtractJoinKeys(const std::set<std::string>& leftCols, std::set<std::string> rightCols, TUnionFind<std::string>& unionFind)
{
    std::vector<TJoinKey> joinKeys;
    for (const auto& left : leftCols) {
        std::optional<std::string> found;
        for (const auto& right : rightCols) {
            if (unionFind.Connected(left, right)) {
                found = right;
                break;
            }
        }
        if (found) {
            joinKeys.emplace_back(left, *found);
            rightCols.erase(*found);
        }
    }
    return joinKeys;
}

void ExtractConjucts(std::vector<TConjuct>& ret, const TExprPtr& expr) {
    std::vector<TExprPtr> conjucts;
    FactorConjuncts(expr, conjucts);
    for (auto conj : conjucts) {
        auto& cur = ret.emplace_back(TConjuct{conj});
        auto maybeBinary = TMaybeNode<TBinaryExpr>(conj);
        if (!maybeBinary) {
            continue;
        }
        auto binary = maybeBinary.Cast();
        if (binary->Operator != "==") {
            continue;
        }
        auto identLeft = TMaybeNode<TIdentExpr>(binary->Left);
        auto identRight = TMaybeNode<TIdentExpr>(binary->Right);
        if (!identLeft) {
            continue;
        }
        if (!identRight) {
            continue;
        }
        cur.equiv = true;
        cur.left = identLeft.Cast()->Name;
        cur.right = identRight.Cast()->Name;
    }
}

std::unordered_set<std::string> JoinColumns(int side, const std::shared_ptr<TJoinOperator>& join) {
    std::unordered_set<std::string> ret;
    auto sideInput = side == 0
        ? join->Left()
        : join->Right();
    auto&& schema = sideInput->OutputColumns();
    if (!schema) {
        return ret;
    }

    for (const auto& [name, _] : schema->Fields) {
        ret.insert(name);
    }
    return ret;
}

std::unordered_set<std::string> ColumnsOf(const TConjuct& conj) {
    if (conj.equiv) {
        return {conj.left, conj.right};
    }
    return FindUnboundVars(conj.Expr);
}

bool Covers(
    const std::unordered_set<std::string>& cols,
    const std::unordered_set<std::string>& side)
{
    for (const auto& col : cols) {
        if (side.count(col) == 0) {
            return false;
        }
    }
    return true;
}

// True when every column shares the same qualifier (alias) prefix, i.e. the
// conjunct constrains a single relation. Only these are pushed to leaves in
// PushOnly mode; multi-relation conjuncts (join edges) stay in the top filter.
bool SingleRelation(const TConjuct& conj) {
    std::string prefix;
    bool first = true;
    for (const auto& col : ColumnsOf(conj)) {
        auto dot = col.find('.');
        std::string rel = dot == std::string::npos ? col : col.substr(0, dot);
        if (first) {
            prefix = rel;
            first = false;
        } else if (rel != prefix) {
            return false;
        }
    }
    return !first;
}

TExprPtr Combine(const std::vector<TExprPtr>& parts, TOperator op) {
    TExprPtr result;
    for (const auto& part : parts) {
        result = result
            ? std::make_shared<TBinaryExpr>(part->Location, op, result, part)
            : part;
    }
    return result;
}

// Weakens a disjunction into a single-side necessary condition: per disjunct,
// keep only the atoms over `side`; if a disjunct constrains no `side` column,
// nothing can be derived. Returns null when there is nothing to push.
TExprPtr DeriveSideFilter(const TExprPtr& expr, const std::unordered_set<std::string>& side) {
    std::vector<TExprPtr> disjuncts;
    FlattenDisjuncts(expr, disjuncts);
    if (disjuncts.size() < 2) {
        return nullptr;
    }

    std::vector<TExprPtr> parts;
    for (const auto& disjunct : disjuncts) {
        std::vector<TExprPtr> atoms;
        FlattenConjuncts(disjunct, atoms);

        std::vector<TExprPtr> kept;
        for (const auto& atom : atoms) {
            if (Covers(FindUnboundVars(atom), side)) {
                kept.push_back(atom);
            }
        }
        if (kept.empty()) {
            return nullptr;
        }
        parts.push_back(Combine(kept, TOperator("&&")));
    }
    return Combine(parts, TOperator("||"));
}

struct TRemapTarget {
    std::string Name;
    NQumir::NAst::TExprPtr Expr;
};

void RemapIdents(NQumir::NAst::TExprPtr& expr, const std::unordered_map<std::string, TRemapTarget>& renameFromTo) {
    if (auto maybeIdent = TMaybeNode<NQumir::NAst::TIdentExpr>(expr)) {
        auto ident = maybeIdent.Cast();
        auto name = ident->Name;
        auto maybeRename = renameFromTo.find(name);
        if (maybeRename != renameFromTo.end()) {
            auto target = maybeRename->second;
            if (target.Expr) {
                // Clone: the substituted expression is shared with its source
                // (a projection spec / branch constant) and must stay independent
                // of further in-place rewrites of the pushed predicate.
                expr = CloneExpr(target.Expr);
            } else {
                ident->Name = target.Name;
            }
        }
        return;
    }

    for (const auto& child : expr->MutableChildren()) {
        RemapIdents(*child, renameFromTo);
    }
}

struct TContext {
    std::vector<TConjuct> Conjucts;
    EMode Mode = EMode::ExtractKeys;
};

TOperatorPtr Process(TOperatorPtr node, TContext ctx);

TOperatorPtr ProcessAggregate(std::shared_ptr<TAggregateOperator> aggregate, TContext ctx) {
    if (!aggregate->GroupingSets().empty()) {
        aggregate->MutableInput() = Process(aggregate->Input(), {{}, ctx.Mode});
        return Materialize(aggregate, ctx.Conjucts);
    }

    std::unordered_set<std::string> groupCols(
        aggregate->GroupKeys().begin(), aggregate->GroupKeys().end());

    // A global aggregate owns its group even when the input is empty, so a
    // predicate on it (HAVING NULL) must not be pushed below it.
    std::vector<TConjuct> pushable;
    std::vector<TConjuct> keep;
    for (auto& conj : ctx.Conjucts) {
        if (!aggregate->IsGlobal() && !groupCols.empty() && Covers(ColumnsOf(conj), groupCols)) {
            pushable.push_back(std::move(conj));
        } else {
            keep.push_back(std::move(conj));
        }
    }

    aggregate->MutableInput() = Process(aggregate->Input(), {pushable, ctx.Mode});
    return Materialize(aggregate, keep);
}

TOperatorPtr ProcessLimit(std::shared_ptr<TLimitOperator> limit, TContext ctx) {
    limit->MutableInput() = Process(limit->Input(), {{}, ctx.Mode});
    return Materialize(limit, ctx.Conjucts);
}

// In PushOnly mode a project is not a barrier: a predicate over its output
// columns is rewritten into the input namespace (each output column replaced by
// its defining projection expression) and pushed below.
TOperatorPtr ProcessProject(std::shared_ptr<TProjectOperator> project, TContext ctx) {
    // ExtractKeys keeps the barrier: its conjuncts carry equi-key info (the
    // `equiv` flag lifted into join keys downstream); rewriting `col == col`
    // through the projection would flatten it to a plain predicate and lose keys.
    if (ctx.Mode != EMode::PushOnly) {
        project->MutableInput() = Process(project->Input(), {{}, ctx.Mode});
        return Materialize(project, ctx.Conjucts);
    }

    std::unordered_map<std::string, TRemapTarget> rewrite;
    for (const auto& spec : project->Projections()) {
        rewrite[spec.Name] = TRemapTarget{.Expr = spec.Expression};
    }

    std::vector<TConjuct> pushable;
    std::vector<TConjuct> keep;
    for (auto& conj : ctx.Conjucts) {
        bool mapped = true;
        for (const auto& col : ColumnsOf(conj)) {
            if (!rewrite.count(col)) {
                mapped = false;
                break;
            }
        }
        if (!mapped) {
            keep.push_back(std::move(conj));
            continue;
        }
        auto rewritten = CloneExpr(ConjuctExpr(conj));
        RemapIdents(rewritten, rewrite);
        pushable.push_back(TConjuct{std::move(rewritten)});
    }

    project->MutableInput() = Process(project->Input(), {std::move(pushable), ctx.Mode});
    return Materialize(project, keep);
}

TOperatorPtr ProcessSort(std::shared_ptr<TSortOperator> sort, TContext ctx) {
    sort->MutableInput() = Process(sort->Input(), {{}, ctx.Mode});
    return Materialize(sort, ctx.Conjucts);
}

TOperatorPtr ProcessTopSort(std::shared_ptr<TTopSortOperator> topSort, TContext ctx) {
    topSort->MutableInput() = Process(topSort->Input(), {{}, ctx.Mode});
    return Materialize(topSort, ctx.Conjucts);
}

// A window is a partial barrier: a predicate over PARTITION BY columns only can
// pass below it (dropping whole partitions never changes a per-partition window
// result), while predicates over its output columns or non-partition inputs stay
// above.
TOperatorPtr ProcessWindow(std::shared_ptr<TWindowOperator> window, TContext ctx) {
    std::unordered_set<std::string> partitionCols(
        window->PartitionKeys().begin(), window->PartitionKeys().end());

    std::vector<TConjuct> pushable;
    std::vector<TConjuct> keep;
    for (auto& conj : ctx.Conjucts) {
        if (!partitionCols.empty() && Covers(ColumnsOf(conj), partitionCols)) {
            pushable.push_back(std::move(conj));
        } else {
            keep.push_back(std::move(conj));
        }
    }

    window->MutableInput() = Process(window->Input(), {std::move(pushable), ctx.Mode});
    return Materialize(window, keep);
}

// A closed projection is a constant within its branch, so it can be substituted
// for the column in a pushed predicate and then const-folded (e.g. the
// `'s'`/`'c'`/`'w'` sale_type tags of a UNION ALL). Returns nullptr when the
// branch has no top Project, `pos` is out of range, or the projection still
// references input columns.
TExprPtr ClosedBranchProjection(const TOperatorPtr& branch, size_t pos) {
    auto project = TMaybeOp<TProjectOperator>(branch);
    if (!project) {
        return nullptr;
    }
    const auto& projections = project.Cast()->Projections();
    if (pos >= projections.size()) {
        return nullptr;
    }
    const auto& expr = projections[pos].Expression;
    if (!expr || !FindUnboundVars(expr).empty()) {
        return nullptr;
    }
    return expr;
}

TOperatorPtr ProcessUnion(std::shared_ptr<TUnionAllOperator> unionAll, TContext ctx) {
    auto&& outFields = TMaybeType<NQumir::NAst::TStructType>(unionAll->OutputColumns()).Cast()->Fields;
    // 1: build column name -> column id mapping
    std::unordered_map<std::string, int> columnNameToId;
    for (const auto& [field, _] : outFields) {
        columnNameToId[field] = columnNameToId.size();
    }

    auto branches = unionAll->Inputs();
    std::vector<TOperatorPtr> newInputs; newInputs.reserve(branches.size());
    for (const auto& branch : branches) {
        std::unordered_map<std::string, TRemapTarget> remapFromTo;
        auto&& branchFields = TMaybeType<NQumir::NAst::TStructType>(branch->OutputColumns()).Cast()->Fields;
        std::vector<TConjuct> branchConjucts;
        branchConjucts.reserve(ctx.Conjucts.size());
        std::vector<NQumir::NAst::TExprPtr> branchConstColumns(branchFields.size());
        for (int i = 0; i < branchConstColumns.size(); ++i) {
            branchConstColumns[i] = ClosedBranchProjection(branch, i);
        }

        bool branchIsDead = false;
        // 2. build cloned predicates with remapped columns
        for (const auto& conj : ctx.Conjucts) {
            for (const auto& col : ColumnsOf(conj)) {
                auto idx = columnNameToId.at(col);
                if (branchConstColumns[idx]) {
                    remapFromTo[col] = TRemapTarget {
                        .Expr = branchConstColumns[idx]
                    };
                } else {
                    remapFromTo[col] = TRemapTarget {
                        .Name = branchFields[idx].first
                    };
                }
            }

            auto cloned = CloneExpr(conj.Expr);
            RemapIdents(cloned, remapFromTo);
            cloned = ConstFold(cloned);
            std::optional<bool> branchValue;
            if (auto maybeNumber = TMaybeNode<TNumberExpr>(cloned)) {
                auto number = maybeNumber.Cast();
                if (number->IsFloat()) {
                    branchValue = number->FloatValue != 0.0;
                } else {
                    branchValue = number->IntValue != 0;
                }
            }

            if (branchValue) {
                if (*branchValue == false) {
                    branchIsDead = true;
                    break;
                }
            } else {
                auto newConj = conj; newConj.Expr = std::move(cloned);
                branchConjucts.push_back(std::move(newConj));
            }
        }

        if (!branchIsDead) {
            newInputs.emplace_back(Process(branch, {std::move(branchConjucts), ctx.Mode}));
        }
    }

    if (newInputs.empty()) {
        // all branches folded => empty operator
        return std::make_shared<TLimitOperator>(branches.front(), 0);
    }
    if (newInputs.size() == 1) {
        return newInputs[0];
    }
    unionAll->SetInputs(std::move(newInputs));
    return Materialize(unionAll, {});
}

// Outer joins are excluded: pushing onto the null-extended side changes results.
bool IsRedistributable(EJoinType type) {
    return type == EJoinType::Inner
        || type == EJoinType::LeftSemi || type == EJoinType::LeftAnti
        || type == EJoinType::RightSemi || type == EJoinType::RightAnti;
}

TOperatorPtr ProcessJoin(std::shared_ptr<TJoinOperator> join, TContext ctx)
{
    if (ctx.Mode == EMode::PushOnly) {
        auto leftCols = JoinColumns(0, join);
        auto rightCols = JoinColumns(1, join);
        std::vector<TConjuct> leftConjucts;
        std::vector<TConjuct> rightConjucts;
        std::vector<TConjuct> keep;
        for (const auto& conj : ctx.Conjucts) {
            auto cols = ColumnsOf(conj);
            if (Covers(cols, leftCols)) {
                leftConjucts.emplace_back(conj);
            } else if (Covers(cols, rightCols)) {
                rightConjucts.emplace_back(conj);
            } else {
                keep.emplace_back(conj);
            }
        }
        join->MutableLeft() = Process(join->Left(), {std::move(leftConjucts), ctx.Mode});
        join->MutableRight() = Process(join->Right(), {std::move(rightConjucts), ctx.Mode});
        return Materialize(join, keep);
    }

    // An anti join returns the rows of one input that match nothing, and its
    // own condition decides what matches. A conjunct of that condition may
    // narrow only the other input; one that reads just the preserved input
    // stays in the residual. Predicates from above read only the output, which
    // is the preserved input, and go there.
    const bool preservesLeft = join->JoinType() == EJoinType::LeftAnti;
    const bool preservesRight = join->JoinType() == EJoinType::RightAnti;
    std::vector<TConjuct> above;
    if (preservesLeft || preservesRight) {
        above = std::exchange(ctx.Conjucts, {});
    }

    // Fold the join's own residual into the pool so explicit JOIN ON and
    // decorrelated semi/anti correlations feed key extraction too.
    std::vector<TConjuct>& pool = ctx.Conjucts;
    if (join->Filter()) {
        ExtractConjucts(pool, join->Filter());
        join->MutableFilter() = nullptr;
    }

    TUnionFind<std::string> unionFind;
    for (const auto& conj : pool) {
        if (conj.equiv) {
            unionFind.Union(conj.left, conj.right);
        }
    }

    auto leftCols = JoinColumns(0, join);
    auto rightCols = JoinColumns(1, join);

    auto addEquivConjuct = [](std::vector<TConjuct>& out, const std::string& left, const std::string& right) {
        if (left != right) {
            out.emplace_back(TConjuct{nullptr, true, left, right});
        }
    };

    std::vector<TConjuct> newConjucts;
    std::vector<std::string> equivCols;
    std::unordered_set<std::string> seenEquivCols;
    for (const auto& conj : pool) {
        if (!conj.equiv) {
            newConjucts.emplace_back(conj);
            continue;
        }
        if (seenEquivCols.insert(conj.left).second) {
            equivCols.push_back(conj.left);
        }
        if (seenEquivCols.insert(conj.right).second) {
            equivCols.push_back(conj.right);
        }
    }

    auto& keys = join->MutableKeys();
    std::unordered_set<std::string> assignedEquivCols;
    for (const auto& root : equivCols) {
        if (assignedEquivCols.count(root)) {
            continue;
        }

        std::vector<std::string> cls;
        for (const auto& col : equivCols) {
            if (unionFind.Connected(root, col)) {
                cls.push_back(col);
                assignedEquivCols.insert(col);
            }
        }

        std::vector<std::string> left;
        std::vector<std::string> right;
        for (const auto& col : cls) {
            if (leftCols.count(col)) {
                left.push_back(col);
            } else if (rightCols.count(col)) {
                right.push_back(col);
            } else {
                // The column is part of an equi-join class but exists in neither
                // input schema - it cannot become a key nor be pushed to a child,
                // so the edge would be silently dropped (turning the join into an
                // accidental cross product). This means an upstream column stayed
                // unqualified/unresolved; fail loudly instead of producing a plan
                // that materializes a cartesian.
                throw std::runtime_error(
                    "ExtractEquiJoins: equi-join column '" + col +
                    "' is absent from both join inputs (unqualified or unresolved "
                    "column); the join edge would be dropped");
            }
        }

        if (!left.empty() && !right.empty()) {
            keys.emplace_back(left.front(), right.front());

            for (size_t i = 1; i < left.size(); ++i) {
                addEquivConjuct(newConjucts, left.front(), left[i]);
            }
            for (size_t i = 1; i < right.size(); ++i) {
                addEquivConjuct(newConjucts, right.front(), right[i]);
            }
        } else if (left.size() > 1) {
            for (size_t i = 1; i < left.size(); ++i) {
                addEquivConjuct(newConjucts, left.front(), left[i]);
            }
        } else if (right.size() > 1) {
            for (size_t i = 1; i < right.size(); ++i) {
                addEquivConjuct(newConjucts, right.front(), right[i]);
            }
        }
    }

    std::vector<TConjuct> leftConjucts;
    std::vector<TConjuct> rightConjucts;
    std::vector<TConjuct> residualConjucts;
    for (const auto& conj : newConjucts) {
        auto cols = ColumnsOf(conj);
        if (!preservesLeft && Covers(cols, leftCols)) {
            leftConjucts.emplace_back(conj);
        } else if (!preservesRight && Covers(cols, rightCols)) {
            rightConjucts.emplace_back(conj);
        } else {
            residualConjucts.emplace_back(conj);
            if (conj.Expr && !preservesLeft) {
                if (auto derived = DeriveSideFilter(conj.Expr, leftCols)) {
                    leftConjucts.emplace_back(TConjuct{derived});
                }
            }
            if (conj.Expr && !preservesRight) {
                if (auto derived = DeriveSideFilter(conj.Expr, rightCols)) {
                    rightConjucts.emplace_back(TConjuct{derived});
                }
            }
        }
    }
    if (preservesLeft) {
        leftConjucts.insert(leftConjucts.end(), above.begin(), above.end());
    } else if (preservesRight) {
        rightConjucts.insert(rightConjucts.end(), above.begin(), above.end());
    }

    if (!residualConjucts.empty()) {
        join->MutableFilter() = Conjoin(residualConjucts);
    }

    join->MutableLeft() = Process(join->Left(), {std::move(leftConjucts), ctx.Mode});
    join->MutableRight() = Process(join->Right(), {std::move(rightConjucts), ctx.Mode});
    return join;
}

// Outer join. WHERE predicates from above may only be pushed onto the preserved
// side (Left keeps left, Right keeps right, Full neither). The ON residual is
// different: a non-key ON conjunct constraining only the null-extended side can
// be filtered on that side before the join - equivalent for an outer join, and
// it keeps the conjunct out of the join residual.
TOperatorPtr ProcessOuterJoin(std::shared_ptr<TJoinOperator> join, TContext ctx) {
    EJoinType type = join->JoinType();
    auto leftCols = JoinColumns(0, join);
    auto rightCols = JoinColumns(1, join);

    std::vector<TConjuct> leftConjucts;
    std::vector<TConjuct> rightConjucts;

    // Pull equi keys out of the ON residual (keys + residual together stay equal
    // to the original ON); push the null-extended side's own conjuncts down to it.
    if (ctx.Mode == EMode::ExtractKeys && join->Filter()) {
        std::vector<TConjuct> on;
        ExtractConjucts(on, join->Filter());

        TUnionFind<std::string> unionFind;
        for (const auto& conj : on) {
            if (conj.equiv) {
                unionFind.Union(conj.left, conj.right);
            }
        }

        std::unordered_set<std::string> used;
        auto& keys = join->MutableKeys();
        // Ordered traversal keeps key selection and composite key layout stable
        // across standard libraries, including runtime-filter key projections.
        const std::set<std::string> orderedLeftCols(leftCols.begin(), leftCols.end());
        std::set<std::string> orderedRightCols(rightCols.begin(), rightCols.end());
        for (auto& key : ExtractJoinKeys(orderedLeftCols, std::move(orderedRightCols), unionFind)) {
            used.insert(key.Left);
            used.insert(key.Right);
            keys.push_back(std::move(key));
        }

        std::vector<TConjuct> residual;
        for (const auto& conj : on) {
            if (conj.equiv && used.count(conj.left) && used.count(conj.right)) {
                continue;
            }
            auto cols = ColumnsOf(conj);
            if (type == EJoinType::Left && Covers(cols, rightCols)) {
                rightConjucts.emplace_back(conj);
            } else if (type == EJoinType::Right && Covers(cols, leftCols)) {
                leftConjucts.emplace_back(conj);
            } else {
                residual.emplace_back(conj);
            }
        }
        join->MutableFilter() = residual.empty() ? nullptr : Conjoin(residual);
    }

    std::vector<TConjuct> keep;
    for (const auto& conj : ctx.Conjucts) {
        auto cols = ColumnsOf(conj);
        if (type == EJoinType::Left && Covers(cols, leftCols)) {
            leftConjucts.emplace_back(conj);
        } else if (type == EJoinType::Right && Covers(cols, rightCols)) {
            rightConjucts.emplace_back(conj);
        } else {
            keep.emplace_back(conj);
        }
    }

    join->MutableLeft() = Process(join->Left(), {std::move(leftConjucts), ctx.Mode});
    join->MutableRight() = Process(join->Right(), {std::move(rightConjucts), ctx.Mode});
    return Materialize(join, keep);
}

TOperatorPtr Process(TOperatorPtr node, TContext ctx) {
    if (auto maybeFilter = TMaybeOp<TFilterOperator>(node)) {
        auto filter = maybeFilter.Cast();
        if (ctx.Mode == EMode::PushOnly) {
            // Push only single-relation predicates down; keep the rest (join
            // edges) materialized here so the join chain stays intact.
            std::vector<TConjuct> all;
            ExtractConjucts(all, filter->Predicate());
            std::vector<TConjuct> kept;
            for (auto& conj : all) {
                if (SingleRelation(conj)) {
                    ctx.Conjucts.push_back(std::move(conj));
                } else {
                    kept.push_back(std::move(conj));
                }
            }
            auto inner = Process(filter->Input(), std::move(ctx));
            return Materialize(inner, kept);
        }
        ExtractConjucts(ctx.Conjucts, filter->Predicate());
        // drop the filter: its conjuncts move down into keys / lower nodes
        return Process(filter->Input(), std::move(ctx));
    } else if (auto maybeJoin = TMaybeOp<TJoinOperator>(node)) {
        auto join = maybeJoin.Cast();
        return IsRedistributable(join->JoinType())
            ? ProcessJoin(join, std::move(ctx))
            : ProcessOuterJoin(join, std::move(ctx));
    } else if (auto maybeProject = TMaybeOp<TProjectOperator>(node)) {
        auto project = maybeProject.Cast();
        return ProcessProject(project, std::move(ctx));
    } else if (auto maybeAggregate = TMaybeOp<TAggregateOperator>(node)) {
        auto aggregate = maybeAggregate.Cast();
        return ProcessAggregate(aggregate, std::move(ctx));
    } else if (auto maybeLimit = TMaybeOp<TLimitOperator>(node)) {
        auto limit = maybeLimit.Cast();
        return ProcessLimit(limit, std::move(ctx));
    } else if (auto maybeSort = TMaybeOp<TSortOperator>(node)) {
        auto sort = maybeSort.Cast();
        return ProcessSort(sort, std::move(ctx));
    } else if (auto maybeTopSort = TMaybeOp<TTopSortOperator>(node)) {
        auto topSort = maybeTopSort.Cast();
        return ProcessTopSort(topSort, std::move(ctx));
    } else if (auto maybeWindow = TMaybeOp<TWindowOperator>(node)) {
        auto window = maybeWindow.Cast();
        return ProcessWindow(window, std::move(ctx));
    } else if (auto maybeUnion = TMaybeOp<TUnionAllOperator>(node)) {
        auto unionAll = maybeUnion.Cast();
        return ProcessUnion(unionAll, std::move(ctx));
    }

    return Materialize(node, ctx.Conjucts);
}

bool IsNullJoinKey(
    const TConjuct& conjunct,
    const TJoinOperator& join,
    bool rightKey)
{
    auto call = TMaybeNode<TCallExpr>(conjunct.Expr);
    if (!call || call.Cast()->Args.size() != 1) {
        return false;
    }
    auto callee = TMaybeNode<TIdentExpr>(call.Cast()->Callee);
    auto column = TMaybeNode<TIdentExpr>(call.Cast()->Args.front());
    if (!callee || callee.Cast()->Name != "qdb_is_null" || !column) {
        return false;
    }
    return std::ranges::any_of(join.Keys(), [&](const TJoinKey& key) {
        return !key.NullsEqual
            && (rightKey ? key.Right : key.Left) == column.Cast()->Name;
    });
}

TOperatorPtr RewriteAntiJoins(
    TOperatorPtr root,
    const std::unordered_set<std::string>& needed,
    bool& changed)
{
    if (auto filter = TMaybeOp<TFilterOperator>(root)) {
        auto join = TMaybeOp<TJoinOperator>(filter.Cast()->Input());
        if (join && (join.Cast()->JoinType() == EJoinType::Left
            || join.Cast()->JoinType() == EJoinType::Right))
        {
            // TPC-DS Q78's store-sales CTE includes:
            // SELECT ss_item_sk, sum(ss_quantity) FROM store_sales
            // LEFT JOIN store_returns ON sr_ticket_number = ss_ticket_number
            //                        AND sr_item_sk = ss_item_sk
            // WHERE sr_ticket_number IS NULL GROUP BY ss_item_sk
            // is an anti join: ordinary equality cannot match a NULL key,
            // even when the source key is nullable. A nullable payload such
            // as returns.comment is not a marker: matched rows may have NULL there.
            // Keep the whole ON condition, including any residual, and reject
            // the rewrite if a consumer or another WHERE conjunct needs returns.*.
            // RIGHT JOIN is the same rule with inputs and key pairs swapped;
            // normalize it to LeftAnti, which the executor already supports.
            const bool swapped = join.Cast()->JoinType() == EJoinType::Right;
            std::vector<TConjuct> conjuncts;
            ExtractConjucts(conjuncts, filter.Cast()->Predicate());
            std::vector<TConjuct> remaining;
            bool hasMarker = false;
            for (const auto& conjunct : conjuncts) {
                if (IsNullJoinKey(conjunct, *join.Cast(), !swapped)) {
                    hasMarker = true;
                } else {
                    remaining.push_back(conjunct);
                }
            }
            const auto preservedColumns = JoinColumns(swapped ? 1 : 0, join.Cast());
            if (hasMarker && Covers(needed, preservedColumns)
                && std::ranges::all_of(remaining, [&](const TConjuct& conjunct) {
                    return Covers(ColumnsOf(conjunct), preservedColumns);
                }))
            {
                auto keys = join.Cast()->Keys();
                if (swapped) {
                    for (auto& key : keys) {
                        std::swap(key.Left, key.Right);
                    }
                }
                auto anti = std::make_shared<TJoinOperator>(
                    swapped ? join.Cast()->Right() : join.Cast()->Left(),
                    swapped ? join.Cast()->Left() : join.Cast()->Right(), std::move(keys),
                    EJoinType::LeftAnti, join.Cast()->Filter());
                anti->Location = join.Cast()->Location;
                root = Materialize(std::move(anti), remaining);
                changed = true;
            }
        }
    }

    auto inputs = root->MutableInputs();
    for (size_t index = 0; index < inputs.size(); ++index) {
        inputs[index] = RewriteAntiJoins(
            inputs[index], root->RequiredColumnsForChild(index, needed), changed);
    }
    return root;
}

} // namespace

TOperatorPtr PushDownPredicates(TOperatorPtr root) {
    return Process(root, {{}, EMode::PushOnly});
}

TOperatorPtr ExtractEquiJoins(TOperatorPtr root) {
    return Process(root, {{}, EMode::ExtractKeys});
}

bool RewriteOuterJoinAsAnti(TOperatorPtr& root) {
    if (!root) {
        return false;
    }
    std::unordered_set<std::string> needed;
    for (const auto& [name, type] : root->OutputColumns()->Fields) {
        needed.insert(name);
    }
    bool changed = false;
    root = RewriteAntiJoins(root, needed, changed);
    return changed;
}

} // namespace NQdb
