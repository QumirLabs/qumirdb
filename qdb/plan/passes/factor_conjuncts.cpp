#include "factor_conjuncts.h"

#include "flatten_conjuncts.h"
#include "flatten_disjuncts.h"

#include <qumir/parser/core/printer.h>

#include <string>
#include <unordered_set>
#include <vector>

namespace NQdb {

using namespace NQumir::NAst;

namespace {

bool IsCommutative(const TOperator& op) {
    return op == "==" || op == "!=" || op == "&&" || op == "||" || op == "+" || op == "*";
}

// Canonical structural key: equal atoms map to the same key. Operands of
// commutative operators are ordered, so `a == b` and `b == a` match.
std::string Key(const TExprPtr& expr) {
    if (auto binary = TMaybeNode<TBinaryExpr>(expr)) {
        auto node = binary.Cast();
        std::string left = Key(node->Left);
        std::string right = Key(node->Right);
        if (IsCommutative(node->Operator) && right < left) {
            std::swap(left, right);
        }
        return "(" + node->Operator.ToString() + " " + left + " " + right + ")";
    }
    return NCore::PrintAst(expr);
}

TExprPtr Conjoin(const std::vector<TExprPtr>& atoms) {
    TExprPtr result;
    for (const auto& atom : atoms) {
        result = result
            ? NQumir::MakeIntrusive<TBinaryExpr>(atom->Location, TOperator("&&"), result, atom)
            : atom;
    }
    return result;
}

TExprPtr Disjoin(const std::vector<TExprPtr>& parts) {
    TExprPtr result;
    for (const auto& part : parts) {
        result = result
            ? NQumir::MakeIntrusive<TBinaryExpr>(part->Location, TOperator("||"), result, part)
            : part;
    }
    return result;
}

} // namespace

void FactorConjuncts(const TExprPtr& expr, std::vector<TExprPtr>& out) {
    std::vector<TExprPtr> conjuncts;
    FlattenConjuncts(expr, conjuncts);

    for (const auto& conj : conjuncts) {
        std::vector<TExprPtr> disjuncts;
        FlattenDisjuncts(conj, disjuncts);
        if (disjuncts.size() < 2) {
            out.push_back(conj);
            continue;
        }

        // Atoms of every disjunct.
        std::vector<std::vector<TExprPtr>> atomsPer;
        atomsPer.reserve(disjuncts.size());
        for (const auto& disjunct : disjuncts) {
            std::vector<TExprPtr> atoms;
            FlattenConjuncts(disjunct, atoms);
            atomsPer.push_back(std::move(atoms));
        }

        // Common atoms = those of the first disjunct present in every disjunct.
        std::vector<TExprPtr> common;
        std::unordered_set<std::string> commonKeys;
        for (const auto& atom : atomsPer.front()) {
            std::string key = Key(atom);
            if (commonKeys.count(key)) {
                continue;
            }
            bool inAll = true;
            for (size_t i = 1; i < atomsPer.size() && inAll; ++i) {
                bool found = false;
                for (const auto& other : atomsPer[i]) {
                    if (Key(other) == key) {
                        found = true;
                        break;
                    }
                }
                inAll = found;
            }
            if (inAll) {
                common.push_back(atom);
                commonKeys.insert(std::move(key));
            }
        }

        if (common.empty()) {
            out.push_back(conj);
            continue;
        }

        // Hoist the common atoms as conjuncts.
        for (const auto& atom : common) {
            out.push_back(atom);
        }

        // Rebuild the disjunction from the per-disjunct remainder. If any branch
        // is fully covered by the common atoms, the whole OR is implied by them.
        std::vector<TExprPtr> reducedParts;
        bool implied = false;
        for (const auto& atoms : atomsPer) {
            std::vector<TExprPtr> rest;
            for (const auto& atom : atoms) {
                if (!commonKeys.count(Key(atom))) {
                    rest.push_back(atom);
                }
            }
            if (rest.empty()) {
                implied = true;
                break;
            }
            reducedParts.push_back(Conjoin(rest));
        }
        if (!implied) {
            out.push_back(Disjoin(reducedParts));
        }
    }
}

} // namespace NQdb
