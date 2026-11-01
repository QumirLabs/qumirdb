#include <qdb/plan/build/binder.h>

#include <qdb/plan/ops/dependent_join.h>
#include <qdb/plan/sql_lowering.h>

#include <utility>

namespace NQdb {
namespace NBuild {

namespace NAst = NQumir::NAst;
using NQumir::TError;
using NQumir::TLocation;

std::expected<std::optional<size_t>, TError> FindColumn(
    const TRelation& relation,
    const std::string& name,
    const TLocation& location)
{
    const auto nameDot = name.rfind('.');
    std::optional<size_t> found;
    bool knownQualifier = false;
    for (size_t i = 0; i < relation.Labels.size(); ++i) {
        const auto& label = relation.Labels[i];
        const auto labelDot = label.rfind('.');
        bool match = label == name;
        if (nameDot != std::string::npos) {
            knownQualifier = knownQualifier || (labelDot != std::string::npos
                && label.compare(0, labelDot, name, 0, nameDot) == 0 && labelDot == nameDot);
        } else if (labelDot != std::string::npos && !relation.QualifiedOnly.contains(label)) {
            match = match || label.compare(labelDot + 1, std::string::npos, name) == 0;
        }
        if (label.empty() || !match) {
            continue;
        }
        if (found) {
            return std::unexpected(TError(location, "ambiguous column: " + name));
        }
        found = i;
    }
    if (!found && knownQualifier) {
        return std::unexpected(TError(location, "unknown column: " + name));
    }
    return found;
}

std::expected<NAst::TExprPtr, TError> Resolve(
    TBlock& block,
    const std::shared_ptr<NAst::TIdentExpr>& ident,
    bool inAggregate)
{
    for (TBlock* scope = &block; scope; scope = scope->Parent) {
        if (!scope->Tree.Plan) {
            continue;
        }
        auto index = FindColumn(scope->Tree, ident->Name, ident->Location);
        if (!index) {
            return std::unexpected(index.error());
        }
        if (!*index && scope->Ungrouped) {
            auto ungrouped = FindColumn(*scope->Ungrouped, ident->Name, ident->Location);
            if (ungrouped && *ungrouped) {
                return std::unexpected(TError(ident->Location, inAggregate
                    ? "an aggregate over columns of an enclosing query is not supported yet"
                    : "column " + ident->Name + " must appear in GROUP BY or be used in an aggregate function"));
            }
        }
        if (!*index) {
            continue;
        }
        const auto column = ColumnNames(scope->Tree.Plan)[**index];
        if (scope == &block) {
            ident->Name = column;
            return ident;
        }
        for (TBlock* inner = &block; inner != scope; inner = inner->Parent) {
            inner->OuterScopes.emplace(scope->Id, ident->Location);
        }
        return std::make_shared<TOuterRefExpr>(ident->Location, scope->Id, column,
            FieldType(scope->Tree.Plan->OutputColumns().get(), column));
    }
    return std::unexpected(TError(ident->Location, "unknown column: " + ident->Name));
}

std::string SourceAlias(const NSql::TSqlTableName& table, std::unordered_set<std::string>& taken) {
    if (table.Alias) {
        taken.insert(*table.Alias);
        return *table.Alias;
    }
    const auto stem = ToLower(table.Name.back());
    auto alias = stem;
    for (size_t i = 1; !taken.insert(alias).second; ++i) {
        alias = stem + "_" + std::to_string(i);
    }
    return alias;
}

TRelation TableRelation(const std::shared_ptr<TSourceOperator>& source, const std::string& qualifier) {
    TRelation relation{.Plan = source};
    for (const auto& column : source->GetSource().Schema().Columns) {
        relation.Labels.push_back(qualifier + "." + std::string(column.Name));
    }
    return relation;
}

std::expected<TRelation, TError> Relabel(
    TRelation relation,
    std::vector<std::string> labels,
    const NKernel::TAnnotationContext& annotation)
{
    if (labels.size() != ColumnNames(relation.Plan).size()) {
        return std::unexpected(TError("column alias count does not match output columns"));
    }
    auto names = labels;
    if (std::unordered_set<std::string>(names.begin(), names.end()).size() != names.size()) {
        TFreshNames fresh;
        for (auto& name : names) {
            name = fresh.Next("__scope");
        }
    }
    auto plan = RenameOutputs(std::move(relation.Plan), names, annotation);
    if (!plan) {
        return std::unexpected(plan.error());
    }
    return TRelation{.Plan = std::move(*plan), .Labels = std::move(labels)};
}

std::expected<TRelation, TError> AliasRelation(
    TRelation relation,
    const std::string& alias,
    const NKernel::TAnnotationContext& annotation)
{
    std::vector<std::string> labels;
    for (const auto& label : relation.Labels) {
        labels.push_back(alias + "." + BareColumnName(label));
    }
    return Relabel(std::move(relation), std::move(labels), annotation);
}

TCteScope* FindCteScope(TCteScope* scope, const std::string& key) {
    for (; scope; scope = scope->Parent) {
        if (scope->Definitions.contains(key)) {
            return scope;
        }
    }
    return nullptr;
}

} // namespace NBuild
} // namespace NQdb
