#include <qdb/plan/build/relation.h>

#include <qdb/plan/ops/project.h>
#include <qdb/plan/passes/typing.h>
#include <qdb/plan/sql_lowering.h>

#include <algorithm>
#include <memory>
#include <unordered_map>
#include <utility>

namespace NQdb {
namespace NBuild {

namespace NAst = NQumir::NAst;
using NQumir::TError;
using NQumir::TLocation;

std::vector<std::string> ColumnNames(const TOperatorPtr& plan) {
    std::vector<std::string> names;
    if (auto&& schema = plan->OutputColumns()) {
        for (const auto& [name, _] : schema->Fields) {
            names.push_back(name);
        }
    }
    return names;
}

std::expected<void, TError> Annotate(const TOperatorPtr& plan, const NKernel::TAnnotationContext& annotation) {
    try {
        AnnotateOperatorType(plan, annotation);
    } catch (const TError& error) {
        return std::unexpected(error);
    }
    return {};
}

std::expected<TOperatorPtr, TError> RenameOutputs(
    TOperatorPtr plan,
    const std::vector<std::string>& names,
    const NKernel::TAnnotationContext& annotation)
{
    const auto columns = ColumnNames(plan);
    if (columns == names) {
        return plan;
    }
    if (auto project = TMaybeOp<TProjectOperator>(plan)) {
        size_t offset = 0;
        for (auto& spec : project.Cast()->MutableProjections()) {
            spec.Name = names[offset];
            for (size_t field = 0; field < spec.ColumnAliases.size(); ++field) {
                spec.ColumnAliases[field] = names[offset + field];
            }
            offset += FlattenedProjectionArity(spec);
        }
    } else {
        std::vector<TProjectionSpec> renamed;
        for (size_t i = 0; i < columns.size(); ++i) {
            renamed.push_back({.Name = names[i], .Expression = Ident({}, columns[i])});
        }
        plan = std::make_shared<TProjectOperator>(std::move(plan), std::move(renamed));
    }
    if (auto annotated = Annotate(plan, annotation); !annotated) {
        return std::unexpected(annotated.error());
    }
    return plan;
}

std::expected<TRelation, TError> SeparateRight(
    const TRelation& left,
    TRelation right,
    const NKernel::TAnnotationContext& annotation)
{
    const auto leftNames = ColumnNames(left.Plan);
    const std::unordered_set<std::string> taken(leftNames.begin(), leftNames.end());
    auto names = ColumnNames(right.Plan);
    if (std::ranges::none_of(names, [&](const auto& name) { return taken.contains(name); })) {
        return right;
    }
    TFreshNames fresh;
    for (const auto& name : leftNames) {
        fresh.Reserve(name);
    }
    for (const auto& name : names) {
        fresh.Reserve(name);
    }
    for (auto& name : names) {
        if (taken.contains(name)) {
            name = fresh.Next("__scope");
        }
    }
    auto plan = RenameOutputs(std::move(right.Plan), names, annotation);
    if (!plan) {
        return std::unexpected(plan.error());
    }
    right.Plan = std::move(*plan);
    return right;
}

TRelation CarryLabels(const TRelation& input, TOperatorPtr plan) {
    std::unordered_map<std::string, std::string> labels;
    const auto columns = ColumnNames(input.Plan);
    for (size_t i = 0; i < columns.size(); ++i) {
        labels.emplace(columns[i], input.Labels[i]);
    }
    TRelation relation{.Plan = std::move(plan), .QualifiedOnly = input.QualifiedOnly};
    for (const auto& name : ColumnNames(relation.Plan)) {
        auto label = labels.find(name);
        relation.Labels.push_back(label == labels.end() ? std::string() : label->second);
    }
    return relation;
}

TRelation JoinRelations(TRelation left, TRelation right, std::vector<TJoinKey> keys, EJoinType type, NAst::TExprPtr residual) {
    TRelation joined{.Plan = std::make_shared<TJoinOperator>(
        left.Plan, right.Plan, std::move(keys), type, std::move(residual))};
    joined.Labels = std::move(left.Labels);
    joined.QualifiedOnly = std::move(left.QualifiedOnly);
    if (type == EJoinType::LeftSemi || type == EJoinType::LeftAnti) {
        return joined;
    }
    if (type == EJoinType::RightSemi || type == EJoinType::RightAnti) {
        joined.Labels = std::move(right.Labels);
        joined.QualifiedOnly = std::move(right.QualifiedOnly);
        return joined;
    }
    joined.Labels.insert(joined.Labels.end(), right.Labels.begin(), right.Labels.end());
    joined.QualifiedOnly.insert(right.QualifiedOnly.begin(), right.QualifiedOnly.end());
    return joined;
}

std::expected<void, TError> AttachDependent(
    TRelation& tree,
    uint32_t scope,
    TRelation right,
    EDependentJoinKind kind,
    NAst::TExprPtr predicate,
    std::string marker,
    const TLocation& location,
    const NKernel::TAnnotationContext& annotation)
{
    auto output = ComputeDependentJoinOutputType(tree.Plan->OutputColumns(),
        right.Plan->OutputColumns(), kind, predicate, marker, annotation);
    if (!output) {
        return std::unexpected(TError(location, output.error()));
    }
    auto join = std::make_shared<TDependentJoinOperator>(tree.Plan, right.Plan, kind, scope,
        std::move(predicate), std::move(marker), annotation);
    join->Location = location;
    tree.Plan = std::move(join);
    switch (kind) {
        case EDependentJoinKind::Inner:
        case EDependentJoinKind::Left:
            tree.Labels.insert(tree.Labels.end(), right.Labels.begin(), right.Labels.end());
            tree.QualifiedOnly.insert(right.QualifiedOnly.begin(), right.QualifiedOnly.end());
            break;
        case EDependentJoinKind::Mark:
        case EDependentJoinKind::Single:
            tree.Labels.emplace_back();
            break;
        case EDependentJoinKind::Semi:
        case EDependentJoinKind::Anti:
            break;
    }
    return {};
}

} // namespace NBuild
} // namespace NQdb
