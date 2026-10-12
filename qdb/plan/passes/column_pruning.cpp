#include <qdb/plan/passes/column_pruning.h>

#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/cte_ref.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/source.h>

#include <qumir/parser/type.h>

#include <functional>
#include <stdexcept>
#include <vector>

namespace NQdb {

using namespace NQumir::NAst;

namespace {

bool HasOutputColumn(const TOperatorPtr& op, const std::string& name) {
    if (auto&& st = op->OutputColumns()) {
        for (auto& [field, _] : st->Fields) {
            if (field == name) {
                return true;
            }
        }
    }
    return false;
}

void Prune(const TOperatorPtr& root, const TColumnSet* explicitRootDemand, TCteUsageMap* usage) {
    TColumnSet initial;
    if (explicitRootDemand) {
        initial = *explicitRootDemand;
    } else if (auto&& st = root->OutputColumns()) {
        for (auto& [name, _] : st->Fields) {
            initial.insert(name);
        }
    }

    auto narrowStruct = [](const TTypePtr& type, const TColumnSet& keep) -> TTypePtr {
        auto* st = static_cast<TStructType*>(type.get());
        if (!st) {
            return type;
        }
        std::vector<std::pair<std::string, TTypePtr>> fields;
        for (auto& [name, fieldType] : st->Fields) {
            if (keep.contains(name)) {
                fields.emplace_back(name, fieldType);
            }
        }
        return std::make_shared<TStructType>(std::move(fields));
    };

    std::function<void(const TOperatorPtr&, TColumnSet)> walk =
        [&](const TOperatorPtr& op, TColumnSet needed) {

        auto* fun = static_cast<TFunctionType*>(op->Type.get());

        if (TMaybeOp<TSourceOperator>(op)) {
            auto required = op->RequiredColumnsForChild(0, needed);
            if (fun) {
                auto narrowed = narrowStruct(fun->ReturnType, required);
                fun->ParamTypes = {narrowed};
                fun->ReturnType = narrowed;
            }
            return;
        }

        if (auto ref = TMaybeOp<TCteRef>(op)) {
            if (usage) {
                auto& info = (*usage)[ref.Cast()->Def().get()];
                ++info.StaticRefCount;
                for (const auto& column : needed) {
                    if (HasOutputColumn(op, column)) {
                        info.RequiredOutputs.insert(column);
                    }
                }
            }
            return;
        }

        // Relational-output pruning: drop projected columns the consumer doesn't
        // need, which also narrows this project's own input demand. Skipped when
        // nothing is needed (a zero-column projection would produce empty rowsets).
        if (auto proj = TMaybeOp<TProjectOperator>(op)) {
            auto& specs = proj.Cast()->MutableProjections();
            auto* output = fun
                ? static_cast<TStructType*>(fun->ReturnType.get())
                : nullptr;
            size_t outputIndex = 0;
            size_t kept = 0;
            std::vector<bool> keepSpecs;
            keepSpecs.reserve(specs.size());
            TColumnSet keptOutputs;
            for (const auto& spec : specs) {
                const size_t arity = FlattenedProjectionArity(spec);
                bool keep = false;
                for (size_t i = 0; output && i < arity
                     && outputIndex + i < output->Fields.size(); ++i)
                {
                    keep = keep || needed.contains(output->Fields[outputIndex + i].first);
                }
                if (!output) {
                    keep = needed.contains(spec.Name);
                }
                if (keep) {
                    ++kept;
                    if (output) {
                        for (size_t i = 0; i < arity
                             && outputIndex + i < output->Fields.size(); ++i)
                        {
                            keptOutputs.insert(output->Fields[outputIndex + i].first);
                        }
                    } else {
                        keptOutputs.insert(spec.Name);
                    }
                }
                keepSpecs.push_back(keep);
                outputIndex += arity;
            }
            if (kept > 0 && kept < specs.size()) {
                size_t specIndex = 0;
                std::erase_if(specs, [&](const TProjectionSpec&) {
                    return !keepSpecs[specIndex++];
                });
                if (fun) {
                    // A struct-valued expression is evaluated as one projection;
                    // retaining any of its fields retains the whole flat group.
                    fun->ReturnType = narrowStruct(fun->ReturnType, keptOutputs);
                }
            }
        }

        // Relational-output pruning for aggregates: drop aggregate outputs the
        // consumer doesn't need. Group keys stay - they define the grouping and
        // remain output columns. Dropping an aggregate also removes its argument
        // from this node's input demand.
        if (auto aggOp = TMaybeOp<TAggregateOperator>(op)) {
            auto aggregate = aggOp.Cast();
            auto& aggs = aggregate->MutableAggs();
            size_t kept = 0;
            for (const auto& spec : aggs) {
                if (needed.contains(spec.Name)) {
                    ++kept;
                }
            }
            const bool keepsAnyOutput = kept > 0 || !aggregate->GroupKeys().empty();
            if (kept < aggs.size() && keepsAnyOutput) {
                std::erase_if(aggs, [&](const TAggregateSpec& spec) {
                    return !needed.contains(spec.Name);
                });
                if (fun) {
                    TColumnSet keptOutputs = needed;
                    for (const auto& key : aggregate->GroupKeys()) {
                        keptOutputs.insert(key);
                    }
                    fun->ReturnType = narrowStruct(fun->ReturnType, keptOutputs);
                }
            }
        }

        // Drop from the filter's output the columns needed only by the predicate
        // (e.g. p_name in a LIKE); the predicate still reads them via the child.
        TColumnSet effectiveNeeded = needed;
        if (fun && TMaybeOp<TFilterOperator>(op)) {
            if (auto* rt = static_cast<TStructType*>(fun->ReturnType.get());
                rt && !rt->Fields.empty()) {
                TColumnSet keep;
                for (auto& [name, _] : rt->Fields) {
                    if (needed.contains(name)) {
                        keep.insert(name);
                    }
                }
                // No demand above: keep one column (zero-column rowsets are
                // unsupported), preferring one the predicate already reads.
                if (keep.empty()) {
                    auto referenced = op->ComputeReferencedColumns();
                    std::string technical = rt->Fields.front().first;
                    for (const auto& [name, _] : rt->Fields) {
                        if (referenced.contains(name)) {
                            technical = name;
                            break;
                        }
                    }
                    keep.insert(technical);
                }
                // Propagate the kept column to the child demand.
                effectiveNeeded.insert(keep.begin(), keep.end());
                if (keep.size() < rt->Fields.size()) {
                    fun->ReturnType = narrowStruct(fun->ReturnType, keep);
                }
            }
        }

        auto inputs = op->Inputs();
        for (size_t i = 0; i < inputs.size(); ++i) {
            auto required = op->RequiredColumnsForChild(i, effectiveNeeded);
            if (fun && i < fun->ParamTypes.size()) {
                fun->ParamTypes[i] = narrowStruct(fun->ParamTypes[i], required);
            }
            walk(inputs[i], std::move(required));
        }
    };

    walk(root, initial);
}

} // namespace

void ApplyColumnPruning(const TOperatorPtr& root) {
    Prune(root, nullptr, nullptr);
}

void ApplyColumnPruning(
    const TOperatorPtr& root, const TColumnSet* explicitRootDemand, TCteUsageMap* usage) {
    Prune(root, explicitRootDemand, usage);
}

TCteUsageMap PropagateCteDemands(const TOperatorPtr& main) {
    TCteUsageMap usage;
    ApplyColumnPruning(main, nullptr, &usage);
    auto defs = CollectCteDefinitions(main); // dependency-first
    for (auto it = defs.rbegin(); it != defs.rend(); ++it) {
        // Every collected definition was reached through a TCteRef, so its usage
        // (with a non-zero refcount) must already be recorded.
        auto u = usage.find(it->get());
        if (u == usage.end() || u->second.StaticRefCount == 0) {
            throw std::runtime_error(
                "cte demand: definition reached with no recorded references");
        }
        ApplyColumnPruning((*it)->Plan, &u->second.RequiredOutputs, &usage);
    }
    return usage;
}

} // namespace NQdb
