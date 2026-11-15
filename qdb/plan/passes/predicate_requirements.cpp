#include <qdb/plan/passes/predicate_requirements.h>

#include <qdb/plan/clone_expr.h>
#include <qdb/plan/junction.h>
#include <qdb/plan/passes/flatten_conjuncts.h>
#include <qdb/plan/passes/unbound_vars.h>

#include <qumir/parser/core/printer.h>

#include <algorithm>
#include <map>
#include <string>
#include <unordered_set>
#include <utility>

namespace NQdb {

using namespace NQumir::NAst;

namespace {

using TColumns = std::vector<std::string>;
using TByColumns = std::map<TColumns, std::vector<TExprPtr>>;

} // namespace

TExprPtr ConjoinPredicates(const std::vector<TExprPtr>& predicates) {
    std::vector<TExprPtr> cloned;
    cloned.reserve(predicates.size());
    for (const auto& predicate : predicates) {
        if (predicate) {
            cloned.push_back(CloneExpr(predicate));
        }
    }
    return Junction(TOperator("&&"), std::move(cloned));
}

TExprPtr BuildPredicateSuperset(
    const TStructType& output,
    const std::vector<TExprPtr>& alternatives)
{
    if (alternatives.empty()
        || std::ranges::any_of(alternatives, [](const auto& predicate) {
            return !predicate;
        }))
    {
        return nullptr;
    }

    std::unordered_set<std::string> outputNames;
    for (const auto& [name, type] : output.Fields) {
        outputNames.insert(name);
    }

    std::vector<TByColumns> grouped(alternatives.size());
    for (size_t i = 0; i < alternatives.size(); ++i) {
        std::vector<TExprPtr> conjuncts;
        FlattenConjuncts(alternatives[i], conjuncts);
        for (const auto& conjunct : conjuncts) {
            auto variables = FindUnboundVars(conjunct);
            if (!std::ranges::all_of(variables, [&](const auto& name) {
                    return outputNames.contains(name);
                }))
            {
                continue;
            }
            TColumns columns(variables.begin(), variables.end());
            std::ranges::sort(columns);
            grouped[i][std::move(columns)].push_back(conjunct);
        }
    }

    std::vector<TExprPtr> commonGroups;
    for (const auto& [columns, unused] : grouped.front()) {
        (void)unused;
        if (!std::ranges::all_of(grouped, [&](const auto& byColumns) {
                return byColumns.contains(columns);
            }))
        {
            continue;
        }

        std::vector<TExprPtr> perConsumer;
        std::unordered_set<std::string> seen;
        for (const auto& byColumns : grouped) {
            auto conjoined = ConjoinPredicates(byColumns.at(columns));
            const auto printed = NQumir::NAst::NCore::PrintAst(conjoined);
            if (seen.insert(printed).second) {
                perConsumer.push_back(std::move(conjoined));
            }
        }
        commonGroups.push_back(Junction(TOperator("||"), std::move(perConsumer)));
    }
    return Junction(TOperator("&&"), std::move(commonGroups));
}

} // namespace NQdb
