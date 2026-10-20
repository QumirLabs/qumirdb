#include <qdb/plan/passes/late_materialization.h>

#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/late_materialize.h>
#include <qdb/plan/ops/limit.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/sort.h>
#include <qdb/plan/ops/source.h>

#include <qumir/parser/ast.h>
#include <qumir/parser/type.h>

#include <cmath>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace NQdb {
namespace {

using namespace NQumir::NAst;

std::string BareName(const std::string& name) {
    const auto dot = name.rfind('.');
    return dot == std::string::npos ? name : name.substr(dot + 1);
}

struct TRowChain {
    std::shared_ptr<TSourceOperator> Source;
    std::unordered_set<std::string> EarlyColumns;
};

std::optional<TRowChain> MatchRowChain(const TOperatorPtr& root) {
    TRowChain result;
    TOperatorPtr current = root;
    while (auto filter = TMaybeOp<TFilterOperator>(current)) {
        for (const auto& column : filter.Cast()->ComputeReferencedColumns()) {
            result.EarlyColumns.insert(BareName(column));
        }
        current = filter.Cast()->Input();
    }
    auto source = TMaybeOp<TSourceOperator>(current);
    if (!source) {
        return std::nullopt;
    }
    result.Source = source.Cast();
    return result;
}

std::optional<std::vector<TLateMaterializeColumn>> DirectOutputColumns(
    const TProjectOperator& project,
    const TSourceOperator& source,
    const TProjectOperator* bridge = nullptr)
{
    auto output = project.OutputColumns();
    if (!output || output->Fields.size() != project.Projections().size()) {
        return std::nullopt;
    }

    const std::string prefix = source.GetAlias().empty()
        ? std::string{}
        : source.GetAlias() + ".";
    std::unordered_set<std::string> physicalColumns;
    for (const auto& column : source.GetSource().Schema().Columns) {
        physicalColumns.insert(std::string(column.Name));
    }

    std::unordered_map<std::string, std::string> bridgedPhysical;
    if (bridge) {
        auto bridgeOutput = bridge->OutputColumns();
        if (!bridgeOutput ||
            bridgeOutput->Fields.size() != bridge->Projections().size())
        {
            return std::nullopt;
        }
        for (size_t i = 0; i < bridge->Projections().size(); ++i) {
            const auto& projection = bridge->Projections()[i];
            auto ident = TMaybeNode<TIdentExpr>(projection.Expression);
            if (!ident || !projection.ColumnAliases.empty() ||
                FlattenedProjectionArity(projection) != 1)
            {
                return std::nullopt;
            }
            if (!prefix.empty() && ident.Cast()->Name.rfind(prefix, 0) != 0) {
                return std::nullopt;
            }
            const std::string physical = BareName(ident.Cast()->Name);
            if (!physicalColumns.contains(physical) ||
                physical == InternalRowIdColumnName)
            {
                return std::nullopt;
            }
            bridgedPhysical.emplace(bridgeOutput->Fields[i].first, physical);
        }
    }

    std::vector<TLateMaterializeColumn> result;
    result.reserve(project.Projections().size());
    for (size_t i = 0; i < project.Projections().size(); ++i) {
        const auto& projection = project.Projections()[i];
        if (!projection.ColumnAliases.empty() ||
            FlattenedProjectionArity(projection) != 1)
        {
            return std::nullopt;
        }
        auto ident = TMaybeNode<TIdentExpr>(projection.Expression);
        if (!ident) {
            return std::nullopt;
        }
        std::string physical;
        if (bridge) {
            auto it = bridgedPhysical.find(ident.Cast()->Name);
            if (it == bridgedPhysical.end()) {
                return std::nullopt;
            }
            physical = it->second;
        } else {
            if (!prefix.empty() && ident.Cast()->Name.rfind(prefix, 0) != 0) {
                return std::nullopt;
            }
            physical = BareName(ident.Cast()->Name);
        }
        if (!physicalColumns.contains(physical) ||
            physical == InternalRowIdColumnName)
        {
            return std::nullopt;
        }
        result.push_back({
            .PhysicalName = physical,
            .OutputName = output->Fields[i].first,
            .Type = output->Fields[i].second,
        });
    }
    return result;
}

bool HasPhysicalRowIdCollision(const TSourceOperator& source) {
    for (const auto& column : source.GetSource().Schema().Columns) {
        if (column.Name == InternalRowIdColumnName) {
            return true;
        }
    }
    return false;
}

void SetReason(TLateMaterializationDiagnostics* diagnostics, std::string reason) {
    if (diagnostics) {
        diagnostics->Reason = std::move(reason);
    }
}

struct TRewriteCandidate {
    std::shared_ptr<TProjectOperator> OutputProject;
    std::shared_ptr<TProjectOperator> OrderingProject;
    std::shared_ptr<TTopSortOperator> TopSort;
    std::shared_ptr<TSortOperator> Sort;
    std::shared_ptr<TLimitOperator> Limit;
    TOperatorPtr RowChainRoot;
    uint64_t MaxOutputRows = 0;
};

std::optional<TRewriteCandidate> MatchCandidate(const TOperatorPtr& root) {
    TRewriteCandidate result;
    TOperatorPtr bounded = root;
    if (auto outerProject = TMaybeOp<TProjectOperator>(bounded)) {
        result.OutputProject = outerProject.Cast();
        bounded = outerProject.Cast()->Input();
    }

    TOperatorPtr orderingInput;
    if (auto top = TMaybeOp<TTopSortOperator>(bounded)) {
        if (top.Cast()->Limit() <= 0) {
            return std::nullopt;
        }
        result.TopSort = top.Cast();
        result.MaxOutputRows = static_cast<uint64_t>(top.Cast()->Limit());
        orderingInput = top.Cast()->Input();
    } else if (auto limit = TMaybeOp<TLimitOperator>(bounded)) {
        if (limit.Cast()->Limit() <= 0) {
            return std::nullopt;
        }
        result.Limit = limit.Cast();
        result.MaxOutputRows = static_cast<uint64_t>(limit.Cast()->Limit());
        orderingInput = limit.Cast()->Input();
        if (auto sort = TMaybeOp<TSortOperator>(orderingInput)) {
            result.Sort = sort.Cast();
            orderingInput = sort.Cast()->Input();
        }
    } else {
        return std::nullopt;
    }

    if (result.TopSort || result.Sort) {
        auto orderingProject = TMaybeOp<TProjectOperator>(orderingInput);
        if (!orderingProject) {
            return std::nullopt;
        }
        result.OrderingProject = orderingProject.Cast();
        result.RowChainRoot = orderingProject.Cast()->Input();
        if (!result.OutputProject) {
            result.OutputProject = orderingProject.Cast();
        }
    } else {
        // PushDownLimits produces Project(Limit(row-chain)) for an unordered
        // query. Without that outer projection there is no output mapping to
        // restore after the locator-only scan.
        if (!result.OutputProject) {
            return std::nullopt;
        }
        result.OrderingProject = result.OutputProject;
        result.RowChainRoot = std::move(orderingInput);
    }
    return result;
}

} // namespace

void BindLateMaterializationSources(const TOperatorPtr& root) {
    if (!root) {
        return;
    }
    if (auto late = TMaybeOp<TLateMaterializeOperator>(root)) {
        ResolveLateMaterializationSource(*late.Cast()).Source.EnableRowId();
    }
    for (const auto& input : root->Inputs()) {
        BindLateMaterializationSources(input);
    }
}

TOperatorPtr ApplyLateMaterialization(
    const TOperatorPtr& root,
    const TLateMaterializationSettings& settings,
    TLateMaterializationDiagnostics* diagnostics)
{
    if (!root || TMaybeOp<TLateMaterializeOperator>(root)) {
        return root;
    }
    if (!settings.Enabled) {
        SetReason(diagnostics, "disabled");
        return root;
    }
    if (!std::isfinite(settings.MinSavingsFactor) ||
        settings.MinSavingsFactor < 1.0)
    {
        SetReason(diagnostics, "invalid minimum savings factor");
        return root;
    }

    auto candidate = MatchCandidate(root);
    if (!candidate) {
        return root;
    }
    if (diagnostics) {
        diagnostics->Considered = true;
        diagnostics->Limit = candidate->MaxOutputRows;
    }
    if (candidate->MaxOutputRows > settings.MaxOutputRows) {
        SetReason(diagnostics, "output limit exceeds threshold");
        return root;
    }

    auto chain = MatchRowChain(candidate->RowChainRoot);
    if (!chain) {
        SetReason(diagnostics, "plan is not a single-source row-preserving chain");
        return root;
    }
    auto* lookup = dynamic_cast<IRowLookupSource*>(
        &chain->Source->GetSource());
    if (!lookup) {
        SetReason(diagnostics, "source has no row lookup capability");
        return root;
    }
    if (HasPhysicalRowIdCollision(*chain->Source)) {
        SetReason(diagnostics, "source contains a physical __row_id__ column");
        return root;
    }

    const auto* bridge = candidate->OrderingProject != candidate->OutputProject
        ? candidate->OrderingProject.get()
        : nullptr;
    auto columns = DirectOutputColumns(
        *candidate->OutputProject, *chain->Source, bridge);
    if (!columns || columns->empty()) {
        SetReason(diagnostics, "final projection is not a direct source-column projection");
        return root;
    }

    std::vector<TProjectionSpec> narrowProjections;
    if (candidate->TopSort || candidate->Sort) {
        const auto& sortKeys = candidate->TopSort
            ? candidate->TopSort->Keys()
            : candidate->Sort->Keys();
        std::unordered_map<std::string, size_t> projectionByOutput;
        auto output = candidate->OrderingProject->OutputColumns();
        if (!output) {
            SetReason(diagnostics, "ordering projection has no output schema");
            return root;
        }
        for (size_t i = 0; i < output->Fields.size(); ++i) {
            projectionByOutput.emplace(output->Fields[i].first, i);
        }
        std::unordered_set<size_t> added;
        for (const auto& key : sortKeys) {
            auto it = projectionByOutput.find(key.Column);
            if (it == projectionByOutput.end()) {
                SetReason(diagnostics, "top-sort key is not a direct output column");
                return root;
            }
            if (added.insert(it->second).second) {
                const auto& orderingProjection =
                    candidate->OrderingProject->Projections()[it->second];
                narrowProjections.push_back(orderingProjection);
                auto ident = TMaybeNode<TIdentExpr>(orderingProjection.Expression);
                chain->EarlyColumns.insert(BareName(ident.Cast()->Name));
            }
        }
    }

    std::vector<std::string> earlyColumns(
        chain->EarlyColumns.begin(), chain->EarlyColumns.end());
    std::vector<std::string> fetchColumns;
    fetchColumns.reserve(columns->size());
    for (const auto& column : *columns) {
        fetchColumns.push_back(column.PhysicalName);
    }
    auto cost = lookup->EstimateLookup(
        earlyColumns, fetchColumns, candidate->MaxOutputRows);
    if (!cost) {
        SetReason(diagnostics, "source cannot estimate late-materialization bytes");
        return root;
    }
    if (diagnostics) {
        diagnostics->Cost = *cost;
        diagnostics->EarlyColumnCount = earlyColumns.size();
        diagnostics->FetchColumnCount = fetchColumns.size();
    }
    const long double lateBytes =
        static_cast<long double>(cost->NarrowBytes) +
        static_cast<long double>(cost->LookupBytes);
    if (lateBytes * settings.MinSavingsFactor > cost->EagerBytes) {
        SetReason(diagnostics, "estimated byte savings are below threshold");
        return root;
    }

    chain->Source->EnableRowId();
    const std::string qualifiedLocator = chain->Source->RowIdColumn();
    narrowProjections.push_back({
        .Name = std::string(InternalRowIdColumnName),
        .Expression = std::make_shared<TIdentExpr>(
            NQumir::TLocation{}, qualifiedLocator),
    });
    auto narrow = std::make_shared<TProjectOperator>(
        candidate->RowChainRoot, std::move(narrowProjections));

    TOperatorPtr bounded;
    if (candidate->TopSort) {
        bounded = std::make_shared<TTopSortOperator>(
            std::move(narrow),
            candidate->TopSort->Keys(),
            candidate->TopSort->Limit());
    } else if (candidate->Sort) {
        auto sorted = std::make_shared<TSortOperator>(
            std::move(narrow), candidate->Sort->Keys());
        bounded = std::make_shared<TLimitOperator>(
            std::move(sorted),
            candidate->Limit->Limit(),
            candidate->Limit->Offset());
    } else {
        bounded = std::make_shared<TLimitOperator>(
            std::move(narrow),
            candidate->Limit->Limit(),
            candidate->Limit->Offset());
    }

    if (diagnostics) {
        diagnostics->Applied = true;
        diagnostics->Reason = "applied";
    }
    return std::make_shared<TLateMaterializeOperator>(
        std::move(bounded),
        std::string(InternalRowIdColumnName),
        std::move(*columns));
}

} // namespace NQdb
