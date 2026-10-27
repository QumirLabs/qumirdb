#include <qdb/plan/passes/cte_reuse.h>

#include <qdb/plan/clone_operator.h>
#include <qdb/plan/ops/cte_consumer.h>
#include <qdb/plan/passes/equijoin.h>
#include <qdb/plan/passes/estimate_stats.h>
#include <qdb/plan/passes/predicate_requirements.h>
#include <qdb/plan/passes/typing.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/project.h>

#include <cassert>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace NQdb {

using namespace NQumir::NAst;

namespace {

using TMemo = std::unordered_map<TCteDefinition*, TOperatorPtr>;
using TMaterializedMap = std::unordered_map<TCteDefinition*, TCteMaterializationPtr>;

struct TResolveState {
    const TCteReuseDecisions& Decisions;
    TMemo Inlined;
    TMaterializedMap Materialized;
};

TOperatorPtr Resolve(const TOperatorPtr& op, TResolveState& state);

TOperatorPtr ResolveInline(const TCteDefinitionPtr& def, TResolveState& state) {
    auto it = state.Inlined.find(def.get());
    if (it == state.Inlined.end()) {
        // Resolve a clone so the canonical definition (its nested TCteRefs) stays
        // intact for the reuse phase.
        it = state.Inlined.emplace(
            def.get(), Resolve(CloneOperator(def->Plan), state)).first;
    }
    return CloneOperator(it->second);
}

// Narrows the spool to the union of columns demanded by all consumers, in the
// producer's own field order.
TOperatorPtr WrapBoundaryProjection(
    TOperatorPtr plan, const std::unordered_set<std::string>& required) {
    auto&& schema = plan->OutputColumns();
    if (!schema) {
        throw std::runtime_error(
            "cte boundary projection: producer output is not a struct");
    }
    if (required.empty()) {
        // Rowsets need >=1 physical column; keep the full schema until a
        // row-count carrier exists.
        return plan;
    }

    std::vector<TProjectionSpec> specs;
    std::vector<std::pair<std::string, TTypePtr>> fields;
    for (const auto& [name, type] : schema->Fields) {
        if (!required.contains(name)) {
            continue;
        }
        specs.push_back(TProjectionSpec{
            .Name = name,
            .Expression = std::make_shared<TIdentExpr>(NQumir::TLocation{}, name),
        });
        fields.emplace_back(name, type);
    }
    // Every demanded column must exist in the producer schema; a mismatch means
    // demand analysis and the producer plan have diverged.
    if (specs.size() != required.size()) {
        throw std::runtime_error(
            "cte boundary projection: demanded column missing from producer schema");
    }
    if (specs.size() == schema->Fields.size()) {
        return plan;
    }

    auto project = std::make_shared<TProjectOperator>(std::move(plan), std::move(specs));
    project->Type = std::make_shared<TFunctionType>(
        std::vector<TTypePtr>{project->Input()->OutputColumns()},
        std::make_shared<TStructType>(std::move(fields)));
    return project;
}

TOperatorPtr ResolveMaterialized(const TCteDefinitionPtr& def, TResolveState& state) {
    auto it = state.Materialized.find(def.get());
    TCteMaterializationPtr mat;
    if (it != state.Materialized.end()) {
        mat = it->second;
    } else {
        mat = std::make_shared<TCteMaterialization>();
        state.Materialized.emplace(def.get(), mat);
        mat->Plan = WrapBoundaryProjection(
            Resolve(CloneOperator(def->Plan), state),
            state.Decisions.at(def.get()).RequiredOutputs);
    }
    return std::make_shared<TCteConsumer>(def, mat);
}

TOperatorPtr Resolve(const TOperatorPtr& op, TResolveState& state) {
    if (!op) {
        return op;
    }
    if (auto maybeRef = TMaybeOp<TCteRef>(op)) {
        auto def = maybeRef.Cast()->Def();
        switch (state.Decisions.at(def.get()).Mode) {
            case ECteReuseMode::Inline:
                return ResolveInline(def, state);
            case ECteReuseMode::Materialize:
                return ResolveMaterialized(def, state);
        }
        std::unreachable();
    }
    for (auto& input : op->MutableInputs()) {
        input = Resolve(input, state);
    }
    return op;
}

} // namespace

namespace {

// Spool traffic cost per byte, in the abstract byte-work units EstimateStats
// uses for Cost. Rough placeholders - they still need calibration against the
// real memory-vs-scan cost ratio.
constexpr double SpoolWriteCostPerByte = 1.0;
constexpr double SpoolReadCostPerByte = 1.0;

// Bytes actually spooled: the boundary projection narrows the producer to the
// demanded columns (or the full schema when demand is empty), so cost the spool
// over RequiredOutputs, not the definition's full output.
double OutputBytes(const TCteDefinition& def, const std::unordered_set<std::string>& required) {
    const TStatsPtr stats = def.Plan ? def.Plan->Stats_ : nullptr;
    const double rows = stats ? static_cast<double>(stats->RowCount) : 0.0;
    const bool keepFullSchema = required.empty();
    double width = 0.0;
    auto&& schema = def.Plan->OutputColumns();
    assert(schema); // a null schema would silently zero the spool cost
    if (schema) {
        for (const auto& [name, type] : schema->Fields) {
            if (keepFullSchema || required.contains(name)) {
                width += EstimateTypeWidth(type);
            }
        }
    }
    return rows * width;
}

// Inline re-executes the definition per reference; materialize runs it once and
// pays spool traffic (one write + one read per consumer). Pick whichever is
// cheaper. Without a cost estimate keep the safe reuse rule (materialize).
//
// StaticRefCount is the *syntactic* reference count, and decisions are made
// independently on pre-reuse costs. Not modeled yet: nested inline multiplicity
// (inlining a parent multiplies how often its dependencies run) and the reverse
// (materializing a dependency lowers a parent's real cost). A later pass can
// compute effective multiplicity and recompute decisions dependency-aware.
ECteReuseMode ChooseMode(const TCteDefinition& def, const TCteUsageInfo& usage) {
    if (usage.StaticRefCount < 2) {
        return ECteReuseMode::Inline;
    }
    const TStatsPtr stats = def.Plan ? def.Plan->Stats_ : nullptr;
    if (!stats) {
        return ECteReuseMode::Materialize;
    }
    const double n = static_cast<double>(usage.StaticRefCount);
    const double bytes = OutputBytes(def, usage.RequiredOutputs);
    const double inlineCost = n * stats->Cost;
    const double materializeCost = stats->Cost
        + bytes * (SpoolWriteCostPerByte + n * SpoolReadCostPerByte);
    return materializeCost < inlineCost
        ? ECteReuseMode::Materialize
        : ECteReuseMode::Inline;
}

} // namespace

namespace {

// The plan slots that hold the references to `def`: each holds the filter
// directly above a reference or, when there is none, the reference itself.
void CollectReferenceSlots(
    TOperatorPtr& op, const TCteDefinition& def, std::vector<TOperatorPtr*>& out)
{
    if (!op) {
        return;
    }
    if (auto maybeFilter = TMaybeOp<TFilterOperator>(op)) {
        auto maybeRef = TMaybeOp<TCteRef>(maybeFilter.Cast()->Input());
        if (maybeRef && maybeRef.Cast()->Def().get() == &def) {
            out.push_back(&op);
            return;
        }
    }
    if (auto maybeRef = TMaybeOp<TCteRef>(op)) {
        if (maybeRef.Cast()->Def().get() == &def) {
            out.push_back(&op);
        }
        return;
    }
    for (auto& input : op->MutableInputs()) {
        CollectReferenceSlots(input, def, out);
    }
}

// The predicate the definition can apply for its consumers. The only consumer
// owns the definition's rows, so its filter moves into the definition whole.
// Several consumers share the rows: the definition gets the superset of their
// filters, and each consumer keeps its own. A reference without a filter needs
// every row, so nothing is pushed then.
TExprPtr TakePushablePredicate(
    const TCteDefinition& def, const std::vector<TOperatorPtr*>& slots)
{
    if (slots.size() == 1) {
        auto maybeFilter = TMaybeOp<TFilterOperator>(*slots.front());
        if (!maybeFilter) {
            return nullptr;
        }
        auto filter = maybeFilter.Cast();
        *slots.front() = filter->Input();
        return filter->Predicate();
    }
    auto* schema = static_cast<TStructType*>(def.OutputType.get());
    if (!schema) {
        return nullptr;
    }
    std::vector<TExprPtr> predicates;
    for (auto* slot : slots) {
        auto maybeFilter = TMaybeOp<TFilterOperator>(*slot);
        predicates.push_back(maybeFilter ? maybeFilter.Cast()->Predicate() : nullptr);
    }
    return BuildPredicateSuperset(*schema, predicates);
}

} // namespace

void PushConsumerPredicatesIntoDefinitions(
    TOperatorPtr& main,
    const NKernel::TAnnotationContext& context,
    TConstFolder& folder)
{
    auto defs = CollectCteDefinitions(main); // dependency-first
    // Consumers first: a predicate pushed into a definition can reach the
    // references in its plan and go on into the definitions they read.
    for (auto it = defs.rbegin(); it != defs.rend(); ++it) {
        auto& def = **it;
        std::vector<TOperatorPtr*> slots;
        CollectReferenceSlots(main, def, slots);
        for (const auto& other : defs) {
            CollectReferenceSlots(other->Plan, def, slots);
        }
        auto pushable = TakePushablePredicate(def, slots);
        if (!pushable) {
            continue;
        }
        def.Plan = PushDownPredicates(
            std::make_shared<TFilterOperator>(def.Plan, std::move(pushable)), folder);
        AnnotateTypes(def.Plan, context);
        EstimateStats(def.Plan, folder);
    }
}

TCteReuseDecisions ChooseCteReuse(const TCteUsageMap& usage) {
    TCteReuseDecisions decisions;
    for (const auto& [def, info] : usage) {
        if (info.StaticRefCount == 0) {
            throw std::runtime_error("cte reuse: definition recorded with no references");
        }
        decisions.emplace(def, TCteReuseDecision{
            .Mode = ChooseMode(*def, info),
            .RequiredOutputs = info.RequiredOutputs,
        });
    }
    return decisions;
}

TOperatorPtr ApplyCteReuse(TOperatorPtr plan, const TCteReuseDecisions& decisions) {
    TResolveState state{.Decisions = decisions};
    auto resolved = Resolve(std::move(plan), state);
    AssignMaterializationRefCounts(resolved);
    return resolved;
}

} // namespace NQdb
