#include <qdb/plan/passes/sql_binding.h>

#include <qdb/plan/clone_expr.h>
#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/cte_ref.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/limit.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/sort.h>
#include <qdb/plan/ops/union.h>
#include <qdb/plan/ops/window.h>
#include <qdb/plan/passes/qualify_columns.h>
#include <qdb/plan/passes/typing.h>
#include <qdb/plan/sql_lowering.h>
#include <qdb/sql/ast.h>

#include <qumir/parser/core/printer.h>

#include <algorithm>
#include <iterator>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace NQdb {
namespace {

namespace NAst = NQumir::NAst;
using NQumir::TError;

struct TNameBinding {
    std::shared_ptr<NAst::TStructType> Schema;
    std::vector<std::string> Labels;
};

// Resolves SQL names against the input labels, qualified or bare. Physical
// names are not SQL names: a hidden one is an unknown column unless the caller
// allows unresolved names. Returns whether every name was resolved; other
// unresolved names, such as builder helpers, are left for later scopes.
bool BindColumns(NAst::TExprPtr& expression, const TNameBinding& input, bool allowUnresolved = false) {
    std::vector<std::pair<std::string, NAst::TTypePtr>> fields;
    std::unordered_map<std::string, std::optional<std::string>> names;
    auto addName = [&](const std::string& label, const std::string& physical) {
        if (!names.emplace(label, physical).second) {
            names[label].reset();
        }
    };
    for (size_t i = 0; i < input.Labels.size(); ++i) {
        const auto& label = input.Labels[i];
        fields.emplace_back(label, input.Schema->Fields[i].second);
        addName(label, input.Schema->Fields[i].first);
        if (const auto dot = label.rfind('.'); dot != std::string::npos) {
            addName(label.substr(dot + 1), input.Schema->Fields[i].first);
        }
    }
    QualifyExpression(expression, NAst::TStructType(std::move(fields)));
    bool resolved = true;
    auto rewrite = [&](const auto& self, NAst::TExprPtr& node) -> void {
        if (!node) {
            return;
        }
        if (auto ident = NAst::TMaybeNode<NAst::TIdentExpr>(node)) {
            const auto& name = ident.Cast()->Name;
            if (auto found = names.find(name); found != names.end()) {
                if (!found->second) {
                    throw TError(node->Location, "input column is ambiguous: " + name);
                }
                ident.Cast()->Name = *found->second;
                return;
            }
            if (!allowUnresolved && FieldType(input.Schema.get(), name)) {
                throw TError(node->Location, "unknown input column: " + name);
            }
            resolved = false;
            return;
        }
        if (auto call = NAst::TMaybeNode<NAst::TCallExpr>(node)) {
            for (auto& argument : call.Cast()->Args) {
                self(self, argument);
            }
            return;
        }
        for (auto* child : node->MutableChildren()) {
            self(self, *child);
        }
    };
    rewrite(rewrite, expression);
    return resolved;
}

struct TGroupingResolution {
    TNameBinding Input;
    std::shared_ptr<TAggCollector> Aggregates;
    std::unordered_map<std::string, std::string> Computed;
    TGroupingRewriteInfo Grouping;
};

struct TBoundPlan {
    TOperatorPtr Plan;
    std::vector<std::string> Labels;
    std::unordered_map<std::string, size_t> SelectedExpressions;
    std::vector<std::optional<std::string>> Identities;
    std::shared_ptr<TProjectOperator> SelectProject;
    std::optional<TNameBinding> SelectInput;
    std::optional<TGroupingResolution> Grouping;
    std::optional<TGroupingResolution> OrderGrouping;
    bool Distinct = false;
};

std::vector<std::string> ColumnNames(const TOperatorPtr& plan) {
    std::vector<std::string> names;
    for (const auto& [name, _] : plan->OutputColumns()->Fields) {
        names.push_back(name);
    }
    return names;
}

// Columns of a star with the given prefix (empty for a bare "*"): output name and
// a reference to the physical input column.
std::vector<TProjectionSpec> StarColumns(const std::string& prefix, const TNameBinding& input) {
    std::vector<TProjectionSpec> columns;
    for (size_t i = 0; i < input.Labels.size(); ++i) {
        const auto& name = input.Labels[i];
        if (!prefix.empty() && !name.starts_with(prefix)) {
            continue;
        }
        const auto dot = name.rfind('.');
        columns.push_back({.Name = dot == std::string::npos ? name : name.substr(dot + 1),
            .Expression = Ident({}, input.Schema->Fields[i].first)});
    }
    return columns;
}

std::vector<TSelectItemBinding> ExpandItems(
    const std::vector<TSelectItemBinding>& items, const TNameBinding& input)
{
    std::vector<TSelectItemBinding> expanded;
    for (const auto& item : items) {
        if (!item.StarPrefix) {
            expanded.push_back(item);
            continue;
        }
        // Star columns keep their prefix: they already reference physical
        // input columns and must not go through SQL name lookup again.
        for (auto& column : StarColumns(*item.StarPrefix, input)) {
            expanded.push_back({.Name = std::move(column.Name), .Expression = std::move(column.Expression),
                .StarPrefix = item.StarPrefix});
        }
    }
    return expanded;
}

// The expression must already be bound against the grouping input.
NAst::TExprPtr RewriteGroupedExpression(NAst::TExprPtr expression, const TGroupingResolution& resolution) {
    auto aggregated = resolution.Aggregates->Rewrite(std::move(expression));
    if (!aggregated) {
        throw aggregated.error();
    }
    expression = SubstituteGroupKeys(std::move(*aggregated), resolution.Computed);
    auto rewritten = RewriteGroupingCalls(std::move(expression), resolution.Grouping);
    if (!rewritten) {
        throw rewritten.error();
    }
    return std::move(*rewritten);
}

class TSqlBindingResolver {
public:
    explicit TSqlBindingResolver(const NKernel::TAnnotationContext& context)
        : Context_(context)
    {}

    void Run(TOperatorPtr& plan) {
        // A generated name must not take a name that a later scope chooses itself.
        ReserveNames(plan);
        for (const auto& definition : CollectCteDefinitions(plan)) {
            ReserveNames(definition->Plan);
        }
        for (const auto& definition : CollectCteDefinitions(plan)) {
            AssignSourceAliases(definition->Plan);
            auto bound = Resolve(definition->Plan);
            definition->Plan = bound.Plan;
            definition->OutputType = bound.Plan->OutputColumns();
            Definitions_.emplace(definition.get(), std::move(bound));
        }
        AssignSourceAliases(plan);
        auto bound = Resolve(plan);
        if (bound.Grouping) {
            throw TError("unresolved aggregate is missing its SELECT scope");
        }
        plan = PublicOutputs(std::move(bound));
    }

private:
    void ReserveNames(const TOperatorPtr& plan) {
        if (auto project = TMaybeOp<TProjectOperator>(plan)) {
            for (const auto& spec : project.Cast()->Projections()) {
                BoundNames_.insert(spec.Name);
                BoundNames_.insert(spec.ColumnAliases.begin(), spec.ColumnAliases.end());
            }
            if (const auto& aliases = project.Cast()->OutputAliases(); aliases && aliases->Columns) {
                BoundNames_.insert(aliases->Columns->begin(), aliases->Columns->end());
            }
        }
        for (const auto& input : plan->Inputs()) {
            ReserveNames(input);
        }
    }

    void Type(const TOperatorPtr& plan) {
        for (const auto& input : plan->Inputs()) {
            if (!Typed_.contains(input.get())) {
                Type(input);
            }
        }
        QualifyOperatorColumns(plan);
        AnnotateOperatorType(plan, Context_);
        const auto schema = plan->OutputColumns();
        if (!schema) {
            throw TError("SQL binding could not determine the output columns of " + std::string(plan->RelName()));
        }
        Typed_.insert(plan.get());
        for (const auto& [name, _] : schema->Fields) {
            BoundNames_.insert(name);
        }
    }

    // A physical name no SQL scope uses, for outputs whose labels collide.
    std::string NextScopeName() {
        std::string name;
        do {
            name = "__scope_" + std::to_string(NextScopeColumn_++);
        } while (!BoundNames_.insert(name).second);
        return name;
    }

    // FROM items may expose equal physical names, for example two unaliased
    // derived tables. The labels stay; the right input gets unique physical names.
    TBoundPlan SeparateJoinInput(TBoundPlan right, const TBoundPlan& left) {
        std::unordered_set<std::string> taken;
        for (const auto& [name, _] : left.Plan->OutputColumns()->Fields) {
            taken.insert(name);
        }
        const auto fields = right.Plan->OutputColumns()->Fields;
        const auto collides = [&](const auto& field) { return taken.contains(field.first); };
        if (right.Grouping || std::ranges::none_of(fields, collides)) {
            return right;
        }
        std::vector<TProjectionSpec> renamed;
        for (const auto& [name, _] : fields) {
            renamed.push_back({.Name = taken.contains(name) ? NextScopeName() : name, .Expression = Ident({}, name)});
        }
        auto project = std::make_shared<TProjectOperator>(right.Plan, std::move(renamed));
        Type(project);
        return {.Plan = project, .Labels = std::move(right.Labels)};
    }

    TBoundPlan Ordinary(TOperatorPtr plan) {
        Type(plan);
        return {.Plan = plan, .Labels = ColumnNames(plan)};
    }

    TOperatorPtr PublicOutputs(TBoundPlan bound) {
        const auto fields = bound.Plan->OutputColumns()->Fields;
        std::vector<TProjectionSpec> projections;
        bool changed = false;
        for (size_t i = 0; i < fields.size(); ++i) {
            projections.push_back({.Name = bound.Labels[i], .Expression = Ident({}, fields[i].first)});
            changed |= fields[i].first != bound.Labels[i];
        }
        if (!changed) {
            return bound.Plan;
        }
        auto project = std::make_shared<TProjectOperator>(bound.Plan, std::move(projections));
        Type(project);
        return project;
    }

    TBoundPlan Aggregate(std::shared_ptr<TAggregateOperator> aggregate, TBoundPlan child) {
        auto binding = *aggregate->Binding();
        auto input = child.Plan;
        const auto schema = input->OutputColumns();
        const TNameBinding inputNames{schema, child.Labels};
        auto collector = std::make_shared<TAggCollector>(schema.get(), binding.Sets.size() <= 1, Context_);
        auto bind = [&](NAst::TExprPtr& expression) {
            BindColumns(expression, inputNames);
        };
        for (const auto& item : binding.Select->Items) {
            if (!item.StarPrefix) {
                if (auto collected = collector->Collect(item.Expression, bind); !collected) {
                    throw collected.error();
                }
            }
        }
        if (binding.Select->Having) {
            if (auto collected = collector->Collect(binding.Select->Having, bind); !collected) {
                throw collected.error();
            }
        }
        auto specs = collector->TakeSpecs();
        for (const auto& spec : specs) {
            if (HasGroupingCall(spec.Arg)) {
                throw TError(spec.Arg->Location, "GROUPING() inside aggregate arguments is not supported");
            }
        }
        const auto visible = Bound_.at(binding.Select->Input.get());
        auto selected = ExpandItems(binding.Select->Items, {visible.Plan->OutputColumns(), visible.Labels});
        std::unordered_map<std::string, NAst::TExprPtr> aliases;
        std::unordered_set<std::string> ambiguous;
        for (auto& item : selected) {
            if (item.Hidden) {
                continue;
            }
            if (!item.Opaque && !item.StarPrefix) {
                item.Expression = CloneExpr(item.Expression);
                BindColumns(item.Expression, inputNames);
            }
            if (!item.ImplicitName && item.ColumnAliases.empty()) {
                if (!aliases.emplace(item.Name, item.Expression).second) {
                    ambiguous.insert(item.Name);
                }
            }
        }
        auto keys = std::move(binding.Keys);
        std::vector<TGroupKey> resolved;
        std::vector<size_t> remap;
        std::unordered_map<std::string, size_t> expressions;
        std::unordered_set<std::string> reserved;
        for (const auto& [name, _] : schema->Fields) {
            reserved.insert(name);
        }
        for (const auto& spec : specs) {
            reserved.insert(spec.Name);
        }
        size_t nextGroupKey = 0;
        for (auto& key : keys) {
            key.Expression = CloneExpr(key.Expression);
            // Input columns take precedence; only an unknown name may be a SELECT alias.
            auto ident = NAst::TMaybeNode<NAst::TIdentExpr>(key.Expression);
            if (!BindColumns(key.Expression, inputNames, static_cast<bool>(ident)) && ident) {
                const auto name = ident.Cast()->Name;
                if (ambiguous.contains(name)) {
                    throw TError(key.Expression->Location, "GROUP BY alias is ambiguous: " + name);
                }
                auto alias = aliases.find(name);
                if (alias == aliases.end()) {
                    throw TError(key.Expression->Location, "unknown input column: " + name);
                }
                key.Expression = alias->second;
            }
            if (HasOpaqueExpression(key.Expression) || HasGroupingCall(key.Expression)) {
                throw TError(key.Expression->Location, "GROUP BY key must refer to a scalar input expression");
            }
            if (HasAggregateCall(key.Expression)) {
                throw TError(key.Expression->Location, "GROUP BY key cannot refer to an aggregate");
            }
            key.Expression = CloneExpr(key.Expression);
            if (auto ident = NAst::TMaybeNode<NAst::TIdentExpr>(key.Expression)) {
                key.Name = ident.Cast()->Name;
                key.Computed = false;
            } else {
                key.Computed = true;
                if (!reserved.insert(key.Name).second) {
                    do {
                        key.Name = "gb_" + std::to_string(nextGroupKey++);
                    } while (!reserved.insert(key.Name).second);
                }
            }
            const auto canonical = NAst::NCore::PrintAst(key.Expression);
            auto [it, inserted] = expressions.emplace(canonical, resolved.size());
            remap.push_back(it->second);
            if (inserted) {
                resolved.push_back(std::move(key));
            }
        }
        auto sets = std::move(binding.Sets);
        for (auto& set : sets) {
            std::vector<size_t> mapped;
            for (const auto index : set) {
                if (std::ranges::find(mapped, remap[index]) == mapped.end()) {
                    mapped.push_back(remap[index]);
                }
            }
            set = std::move(mapped);
        }
        NarrowPlainGroupKeys(resolved, sets, binding.HasGroupingSyntax);
        TGroupingResolution resolution{.Input = inputNames, .Aggregates = collector,
            .Grouping = MakeGroupingRewriteInfo(resolved, sets, binding.HasGroupingSyntax)};
        for (const auto& key : resolved) {
            if (key.Computed) {
                resolution.Computed.emplace(NAst::NCore::PrintAst(key.Expression), key.Name);
            }
        }
        const bool multiSet = sets.size() > 1;
        std::vector<std::string> names;
        for (const auto& key : resolved) {
            names.push_back(key.Name);
        }
        if (collector->DistinctColumn()) {
            if (multiSet) {
                throw TError("GROUPING SETS with COUNT(DISTINCT) is not supported yet");
            }
            if (!resolution.Computed.empty()) {
                throw TError("GROUP BY on an expression with COUNT(DISTINCT) is not supported yet");
            }
            const auto& distinct = *collector->DistinctColumn();
            auto dedup = names;
            if (std::ranges::find(dedup, distinct) == dedup.end()) {
                dedup.push_back(distinct);
            }
            std::vector<TAggregateSpec> partials;
            if (collector->HasNonDistinct()) {
                std::vector<TAggregateSpec> combined;
                for (auto& spec : specs) {
                    if (collector->IsDistinctSpec(spec.Name)) {
                        combined.push_back(std::move(spec));
                        continue;
                    }
                    if (spec.Arg && !NAst::TMaybeNode<NAst::TIdentExpr>(spec.Arg)) {
                        throw TError("mixing DISTINCT with a computed aggregate argument is not supported");
                    }
                    const auto partial = "partial_" + spec.Name;
                    partials.push_back({.Name = partial, .Func = spec.Func, .Arg = spec.Arg});
                    combined.push_back({.Name = spec.Name,
                        .Func = spec.Func == "count" ? "sum" : spec.Func, .Arg = Ident({}, partial)});
                }
                specs = std::move(combined);
            }
            input = std::make_shared<TAggregateOperator>(input, std::move(dedup), std::move(partials));
            Type(input);
        }
        const bool global = names.empty();
        bool materialize = global || multiSet || !resolution.Computed.empty();
        for (const auto& spec : specs) {
            materialize |= spec.Arg && !NAst::TMaybeNode<NAst::TIdentExpr>(spec.Arg);
        }
        if (materialize) {
            std::vector<TProjectionSpec> arguments;
            std::unordered_set<std::string> used;
            for (const auto& key : resolved) {
                if (used.insert(key.Name).second) {
                    arguments.push_back({.Name = key.Name, .Expression = key.Expression});
                }
            }
            std::unordered_set<std::string> occupied = used;
            for (const auto& [name, _] : input->OutputColumns()->Fields) {
                occupied.insert(name);
            }
            size_t next = 0;
            for (auto& spec : specs) {
                if (!spec.Arg) {
                    continue;
                }
                if (auto ident = NAst::TMaybeNode<NAst::TIdentExpr>(spec.Arg);
                    ident && !(global && ident.Cast()->Name == "__group__"))
                {
                    if (used.insert(ident.Cast()->Name).second) {
                        arguments.push_back({.Name = ident.Cast()->Name, .Expression = spec.Arg});
                    }
                } else {
                    std::string name;
                    do {
                        name = "arg_" + std::to_string(next++);
                    } while (!occupied.insert(name).second);
                    used.insert(name);
                    arguments.push_back({.Name = name, .Expression = spec.Arg});
                    spec.Arg = Ident(spec.Arg->Location, name);
                }
            }
            if (global) {
                // The existing scheduler and estimates recognize this marker.
                // A colliding input argument was materialized under another name.
                arguments.push_back({.Name = "__group__", .Expression = I64Literal({}, 1)});
                names.push_back("__group__");
            }
            input = std::make_shared<TProjectOperator>(std::move(input), std::move(arguments));
        }
        auto bound = std::make_shared<TAggregateOperator>(input, std::move(names), std::move(specs));
        if (multiSet) {
            bound->MutableGroupingSets() = std::move(sets);
        }
        auto result = Ordinary(bound);
        result.Grouping = std::move(resolution);
        return result;
    }

    TBoundPlan Select(std::shared_ptr<TProjectOperator> project, TBoundPlan child) {
        const auto scope = project->SelectBinding();
        const auto visible = Bound_.at(scope->Input.get());
        const TNameBinding visibleNames{visible.Plan->OutputColumns(), visible.Labels};
        const TNameBinding inputNames = child.Grouping
            ? child.Grouping->Input
            : TNameBinding{child.Plan->OutputColumns(), child.Labels};
        auto originals = ExpandItems(scope->Items, visibleNames);
        for (auto& item : originals) {
            if (!item.Opaque) {
                item.Expression = CloneExpr(item.Expression);
            }
        }
        std::vector<TProjectionSpec> projections;
        for (auto spec : project->Projections()) {
            if (spec.StarPrefix) {
                std::ranges::move(StarColumns(*spec.StarPrefix, visibleNames), std::back_inserter(projections));
            } else {
                projections.push_back(std::move(spec));
            }
        }
        TWindowCollector windows;
        for (size_t i = 0; i < projections.size(); ++i) {
            auto& spec = projections[i];
            if (!originals[i].StarPrefix) {
                BindColumns(spec.Expression, inputNames);
            }
            if (child.Grouping) {
                spec.Expression = RewriteGroupedExpression(std::move(spec.Expression), *child.Grouping);
            } else {
                if (HasGroupingCall(spec.Expression)) {
                    throw TError(spec.Expression->Location, "GROUPING() requires GROUPING SETS, ROLLUP, or CUBE");
                }
            }
            auto rewritten = windows.Rewrite(spec.Expression);
            if (!rewritten) {
                throw rewritten.error();
            }
            spec.Expression = std::move(*rewritten);
        }
        auto windowed = windows.Build(child.Plan);
        if (!windowed) {
            throw windowed.error();
        }
        project->MutableInput() = std::move(*windowed);
        project->MutableProjections() = std::move(projections);
        project->SetSelectBinding({});
        Type(project);
        auto labels = ColumnNames(project);
        auto names = labels;
        std::unordered_set<std::string> used(names.begin(), names.end());
        if (used.size() != names.size()) {
            for (const auto& [name, _] : project->Input()->OutputColumns()->Fields) {
                used.insert(name);
            }
            // Physical names stay unique across scopes: sibling SELECTs meet in joins and set operations.
            for (auto& name : names) {
                do {
                    name = "__output_" + std::to_string(NextOutputColumn_++);
                } while (used.contains(name) || !BoundNames_.insert(name).second);
            }
        }
        TBoundPlan result{.Plan = project, .Labels = std::move(labels),
            .SelectProject = project, .SelectInput = inputNames, .OrderGrouping = child.Grouping};
        size_t offset = 0;
        auto& specs = project->MutableProjections();
        for (size_t i = 0; i < specs.size(); ++i) {
            auto& spec = specs[i];
            const auto width = FlattenedProjectionArity(spec);
            if (!width) {
                throw TError(spec.Expression->Location, "a SELECT projection must return at least one column");
            }
            std::optional<std::string> canonical;
            if (!originals[i].Opaque) {
                auto expression = CloneExpr(originals[i].Expression);
                if (!originals[i].StarPrefix) {
                    BindColumns(expression, inputNames);
                }
                canonical = NAst::NCore::PrintAst(expression);
                if (!originals[i].Hidden) {
                    result.SelectedExpressions.emplace(*canonical, offset);
                }
            }
            for (size_t field = 0; field < width; ++field) {
                result.Identities.push_back(canonical
                    ? std::optional<std::string>{*canonical + ":" + std::to_string(field)}
                    : std::nullopt);
            }
            spec.Name = names[offset];
            if (!spec.ColumnAliases.empty()) {
                spec.ColumnAliases.assign(names.begin() + offset, names.begin() + offset + width);
            }
            offset += width;
        }
        Type(project);
        return result;
    }

    TBoundPlan Sort(std::shared_ptr<TSortOperator> sort, TBoundPlan child) {
        const auto fields = child.Plan->OutputColumns()->Fields;
        std::vector<TSortKey> keys;
        std::vector<TProjectionSpec> hidden;
        std::unordered_set<std::string> used;
        for (const auto& [name, _] : fields) {
            used.insert(name);
        }
        if (child.SelectProject) {
            for (const auto& [name, _] : child.SelectProject->Input()->OutputColumns()->Fields) {
                used.insert(name);
            }
        }
        size_t next = 0;
        for (const auto& key : sort->UnresolvedKeys()) {
            std::optional<size_t> index;
            if (auto ident = NAst::TMaybeNode<NAst::TIdentExpr>(key.Expression)) {
                for (size_t i = 0; i < child.Labels.size(); ++i) {
                    if (child.Labels[i] != ident.Cast()->Name) {
                        continue;
                    }
                    if (index && (child.Identities.empty() || !child.Identities[i]
                        || child.Identities[*index] != child.Identities[i]))
                    {
                        throw TError(key.Expression->Location,
                            "ORDER BY output column is ambiguous: " + ident.Cast()->Name);
                    }
                    index = i;
                }
            }
            NAst::TExprPtr expression;
            if (!index && !HasOpaqueExpression(key.Expression)) {
                expression = CloneExpr(key.Expression);
                if (child.SelectInput) {
                    BindColumns(expression, *child.SelectInput);
                }
                if (auto found = child.SelectedExpressions.find(NAst::NCore::PrintAst(expression));
                    found != child.SelectedExpressions.end())
                {
                    index = found->second;
                }
            }
            std::string name;
            if (index) {
                name = fields[*index].first;
            } else {
                if (HasOpaqueExpression(key.Expression)) {
                    throw TError(key.Expression->Location, "ORDER BY on a window or subquery must reference its select alias");
                }
                if (child.Distinct) {
                    throw TError(key.Expression->Location, "ORDER BY expressions must appear in the SELECT DISTINCT output");
                }
                if (!child.SelectProject) {
                    throw TError(key.Expression->Location, "ORDER BY on an expression is not supported here");
                }
                if (HasAggregateCall(expression) || HasGroupingCall(expression)) {
                    throw TError(key.Expression->Location, "ORDER BY on an aggregate not in the select list is not supported");
                }
                if (child.OrderGrouping) {
                    expression = RewriteGroupedExpression(std::move(expression), *child.OrderGrouping);
                }
                do {
                    name = "__sort_" + std::to_string(next++);
                } while (!used.insert(name).second);
                hidden.push_back({.Name = name, .Expression = std::move(expression)});
            }
            keys.push_back({.Column = name, .Direction = key.Direction, .Nulls = key.Nulls});
        }
        if (!hidden.empty()) {
            for (auto& spec : hidden) {
                child.SelectProject->MutableProjections().push_back(std::move(spec));
            }
            Type(child.Plan);
        }
        sort->MutableInput() = child.Plan;
        sort->MutableKeys() = std::move(keys);
        sort->MutableUnresolvedKeys().clear();
        child.Plan = sort;
        if (!hidden.empty()) {
            std::vector<TProjectionSpec> visible;
            for (const auto& [name, _] : fields) {
                visible.push_back({.Name = name, .Expression = Ident({}, name)});
            }
            child.Plan = std::make_shared<TProjectOperator>(sort, std::move(visible));
        }
        Type(child.Plan);
        child.SelectProject.reset();
        return child;
    }

    TBoundPlan Resolve(TOperatorPtr plan) {
        const auto original = plan.get();
        if (auto cached = Bound_.find(original); cached != Bound_.end()) {
            return cached->second;
        }
        auto result = ResolveOperator(std::move(plan));
        Bound_.emplace(original, result);
        return result;
    }

    TBoundPlan ResolveOperator(TOperatorPtr plan) {
        if (auto reference = TMaybeOp<TCteRef>(plan)) {
            const auto found = Definitions_.find(reference.Cast()->Def().get());
            if (found == Definitions_.end()) {
                return Ordinary(plan);
            }
            auto result = found->second;
            result.Plan = plan;
            result.SelectProject.reset();
            result.SelectedExpressions.clear();
            result.Grouping.reset();
            Type(plan);
            return result;
        }
        if (plan->Inputs().empty()) {
            return Ordinary(plan);
        }
        std::vector<TBoundPlan> children;
        for (auto& input : plan->MutableInputs()) {
            children.push_back(Resolve(input));
            input = children.back().Plan;
        }
        if (auto aggregate = TMaybeOp<TAggregateOperator>(plan)) {
            if (aggregate.Cast()->Binding()) {
                return Aggregate(aggregate.Cast(), std::move(children[0]));
            }
            if (aggregate.Cast()->GroupsAllColumns()) {
                auto child = std::move(children[0]);
                aggregate.Cast()->MutableGroupKeys() = ColumnNames(child.Plan);
                aggregate.Cast()->SetGroupsAllColumns(false);
                Type(plan);
                child.Plan = plan;
                child.Distinct = true;
                return child;
            }
        }
        if (auto project = TMaybeOp<TProjectOperator>(plan)) {
            if (project.Cast()->SelectBinding()) {
                return Select(project.Cast(), std::move(children[0]));
            }
            if (project.Cast()->OutputAliases()) {
                auto child = std::move(children[0]);
                auto aliases = *project.Cast()->OutputAliases();
                auto labels = aliases.Columns ? *aliases.Columns : child.Labels;
                const auto fields = child.Plan->OutputColumns()->Fields;
                if (labels.size() != fields.size()) {
                    throw TError("column alias count does not match query output");
                }
                std::vector<TProjectionSpec> renamed;
                for (size_t i = 0; i < fields.size(); ++i) {
                    if (!aliases.Qualifier.empty()) {
                        const auto dot = labels[i].rfind('.');
                        labels[i] = aliases.Qualifier + "."
                            + (dot == std::string::npos ? labels[i] : labels[i].substr(dot + 1));
                    }
                    renamed.push_back({.Name = labels[i], .Expression = Ident({}, fields[i].first)});
                }
                std::unordered_set<std::string> used;
                for (const auto& spec : renamed) {
                    used.insert(spec.Name);
                }
                if (used.size() != renamed.size()) {
                    for (auto& spec : renamed) {
                        spec.Name = NextScopeName();
                    }
                }
                if (aliases.Columns && child.SelectProject && child.Plan == child.SelectProject) {
                    // Rename the SELECT outputs in place: a separate renaming
                    // projection would survive optimization as an extra operator.
                    size_t offset = 0;
                    for (auto& spec : child.SelectProject->MutableProjections()) {
                        spec.Name = renamed[offset].Name;
                        for (size_t field = 0; field < spec.ColumnAliases.size(); ++field) {
                            spec.ColumnAliases[field] = renamed[offset + field].Name;
                        }
                        offset += FlattenedProjectionArity(spec);
                    }
                    Type(child.Plan);
                    return {.Plan = child.Plan, .Labels = std::move(labels), .Grouping = std::move(child.Grouping)};
                }
                project.Cast()->MutableProjections() = std::move(renamed);
                project.Cast()->MutableOutputAliases().reset();
                Type(plan);
                return {.Plan = plan, .Labels = std::move(labels), .Grouping = std::move(child.Grouping)};
            }
        }
        if (auto sort = TMaybeOp<TSortOperator>(plan); sort && !sort.Cast()->UnresolvedKeys().empty()) {
            return Sort(sort.Cast(), std::move(children[0]));
        }
        if (auto join = TMaybeOp<TJoinOperator>(plan); join && join.Cast()->Binding()) {
            const auto left = children[0].Plan->OutputColumns();
            const auto right = children[1].Plan->OutputColumns();
            if (left->Fields.size() != right->Fields.size()) {
                throw TError("set operation branches must have the same number of columns");
            }
            for (size_t i = 0; i < left->Fields.size(); ++i) {
                join.Cast()->MutableKeys().push_back({.Left = left->Fields[i].first,
                    .Right = right->Fields[i].first, .NullsEqual = true});
            }
            join.Cast()->MutableBinding().reset();
            Type(plan);
            return {.Plan = plan, .Labels = std::move(children[0].Labels)};
        }
        if (TMaybeOp<TUnionAllOperator>(plan)) {
            Type(plan);
            return {.Plan = plan, .Labels = std::move(children[0].Labels)};
        }
        if (TMaybeOp<TLimitOperator>(plan)) {
            Type(plan);
            auto child = std::move(children[0]);
            child.Plan = plan;
            child.SelectProject.reset();
            return child;
        }
        if (TMaybeOp<TJoinOperator>(plan)) {
            children[1] = SeparateJoinInput(std::move(children[1]), children[0]);
        }
        std::optional<TGroupingResolution> grouping;
        for (size_t i = 0; i < children.size(); ++i) {
            plan->MutableInputs()[i] = children[i].Plan;
            if (children[i].Grouping) {
                if (grouping) {
                    throw TError("multiple unresolved grouping scopes reach one operator");
                }
                grouping = children[i].Grouping;
            }
        }
        if (grouping) {
            if (auto filter = TMaybeOp<TFilterOperator>(plan)) {
                BindColumns(filter.Cast()->MutablePredicate(), grouping->Input);
                filter.Cast()->MutablePredicate() = RewriteGroupedExpression(filter.Cast()->Predicate(), *grouping);
            } else if (auto project = TMaybeOp<TProjectOperator>(plan)) {
                for (auto& spec : project.Cast()->MutableProjections()) {
                    BindColumns(spec.Expression, grouping->Input);
                    spec.Expression = RewriteGroupedExpression(std::move(spec.Expression), *grouping);
                }
            } else if (auto join = TMaybeOp<TJoinOperator>(plan); join && join.Cast()->Filter()) {
                BindColumns(join.Cast()->MutableFilter(), grouping->Input);
                join.Cast()->MutableFilter() = RewriteGroupedExpression(join.Cast()->Filter(), *grouping);
            }
        }
        std::vector<std::string> labels;
        for (const auto& child : children) {
            labels.insert(labels.end(), child.Labels.begin(), child.Labels.end());
        }
        std::vector<std::pair<std::string, NAst::TTypePtr>> fields;
        for (const auto& child : children) {
            const auto schema = child.Plan->OutputColumns();
            fields.insert(fields.end(), schema->Fields.begin(), schema->Fields.end());
        }
        TNameBinding names{std::make_shared<NAst::TStructType>(std::move(fields)), labels};
        // Expressions above a grouping scope were bound before their aggregate rewrite.
        if (auto filter = TMaybeOp<TFilterOperator>(plan); filter && !grouping) {
            BindColumns(filter.Cast()->MutablePredicate(), names);
        } else if (auto join = TMaybeOp<TJoinOperator>(plan)) {
            if (join.Cast()->Filter() && !grouping) {
                BindColumns(join.Cast()->MutableFilter(), names);
            }
            for (auto& key : join.Cast()->MutableKeys()) {
                auto left = Ident({}, key.Left);
                auto right = Ident({}, key.Right);
                BindColumns(left, {children[0].Plan->OutputColumns(), children[0].Labels});
                BindColumns(right, {children[1].Plan->OutputColumns(), children[1].Labels});
                key.Left = NAst::TMaybeNode<NAst::TIdentExpr>(left).Cast()->Name;
                key.Right = NAst::TMaybeNode<NAst::TIdentExpr>(right).Cast()->Name;
            }
            switch (join.Cast()->JoinType()) {
                case EJoinType::LeftSemi:
                case EJoinType::LeftAnti:
                    labels = children[0].Labels;
                    break;
                case EJoinType::RightSemi:
                case EJoinType::RightAnti:
                    labels = children[1].Labels;
                    break;
                default:
                    break;
            }
        } else if (auto project = TMaybeOp<TProjectOperator>(plan)) {
            for (auto& spec : project.Cast()->MutableProjections()) {
                if (!grouping) {
                    BindColumns(spec.Expression, names);
                }
            }
            labels = ColumnNames(plan);
        }
        Type(plan);
        if (TMaybeOp<TProjectOperator>(plan)) {
            labels = ColumnNames(plan);
        }
        return {.Plan = plan, .Labels = std::move(labels), .Grouping = std::move(grouping)};
    }

    std::unordered_set<std::string> BoundNames_;
    size_t NextScopeColumn_ = 0;
    size_t NextOutputColumn_ = 0;
    std::unordered_map<IOperator*, TBoundPlan> Bound_;
    std::unordered_set<IOperator*> Typed_;
    const NKernel::TAnnotationContext& Context_;
    std::unordered_map<TCteDefinition*, TBoundPlan> Definitions_;
};

bool NeedsBinding(const TOperatorPtr& plan) {
    if (auto project = TMaybeOp<TProjectOperator>(plan);
        project && (project.Cast()->SelectBinding() || project.Cast()->OutputAliases()))
    {
        return true;
    }
    if (auto aggregate = TMaybeOp<TAggregateOperator>(plan);
        aggregate && (aggregate.Cast()->Binding() || aggregate.Cast()->GroupsAllColumns()))
    {
        return true;
    }
    if (auto sort = TMaybeOp<TSortOperator>(plan); sort && !sort.Cast()->UnresolvedKeys().empty()) {
        return true;
    }
    if (auto join = TMaybeOp<TJoinOperator>(plan); join && join.Cast()->Binding()) {
        return true;
    }
    return std::ranges::any_of(plan->Inputs(), NeedsBinding);
}

} // namespace

bool HasUnresolvedSqlBindings(const TOperatorPtr& plan) {
    bool pending = NeedsBinding(plan);
    for (const auto& definition : CollectCteDefinitions(plan)) {
        pending |= NeedsBinding(definition->Plan);
    }
    return pending;
}

void ResolveSqlBindings(TOperatorPtr& plan, const NKernel::TAnnotationContext& context) {
    if (HasUnresolvedSqlBindings(plan)) {
        TSqlBindingResolver(context).Run(plan);
    }
}

} // namespace NQdb
