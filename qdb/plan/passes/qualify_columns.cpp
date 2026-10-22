#include <qdb/plan/passes/qualify_columns.h>

#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/limit.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/sort.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/ops/union.h>
#include <qdb/plan/ops/window.h>

#include <qumir/parser/ast.h>
#include <qumir/parser/type.h>

#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <vector>

namespace NQdb {

using namespace NQumir::NAst;

namespace {

// Extract a lowercase stem from a path like "/data/nation.parquet" or "__NATION__".
std::string ExtractStem(const std::string& path) {
    auto lastSlash = path.rfind('/');
    std::string name = (lastSlash != std::string::npos) ? path.substr(lastSlash + 1) : path;
    auto dot = name.rfind('.');
    if (dot != std::string::npos) name = name.substr(0, dot);
    auto start = name.find_first_not_of('_');
    auto end   = name.find_last_not_of('_');
    if (start == std::string::npos) return name;
    name = name.substr(start, end - start + 1);
    std::transform(name.begin(), name.end(), name.begin(),
        [](unsigned char c){ return std::tolower(c); });
    return name;
}

// Collect all TSourceOperator nodes in pre-order.
void CollectSources(const TOperatorPtr& op,
    std::vector<std::shared_ptr<TSourceOperator>>& out)
{
    if (auto maybe = TMaybeOp<TSourceOperator>(op)) {
        out.push_back(maybe.Cast());
        return;
    }
    for (const auto& input : op->Inputs()) {
        CollectSources(input, out);
    }
}

// Is `table` an alias `stem_<digits>` of `stem` (the suffix AssignSourceAliases gives a
// duplicated source: `time_dim` -> `time_dim_0`)?
bool IsAliasOfStem(std::string_view table, std::string_view stem) {
    if (table.size() <= stem.size() + 1 || table.substr(0, stem.size()) != stem
        || table[stem.size()] != '_')
    {
        return false;
    }
    for (size_t i = stem.size() + 1; i < table.size(); ++i) {
        if (table[i] < '0' || table[i] > '9') return false;
    }
    return true;
}

// Qualify `name` against `schema`, matching by the bare column part. A dotted reference
// (`time_dim.t_time_sk`) is remapped to the field's actual table alias when the source was
// renamed (`time_dim` -> `time_dim_0`) - otherwise the reference no longer matches the
// schema and equi-join edges over it get dropped. Returns `name` if no unique match.
std::string QualifyIdent(const TStructType& schema, const std::string& name) {
    const auto dotPos = name.rfind('.');
    const std::string_view bare = (dotPos != std::string::npos)
        ? std::string_view(name).substr(dotPos + 1) : std::string_view(name);
    const std::string_view prefix = (dotPos != std::string::npos)
        ? std::string_view(name).substr(0, dotPos) : std::string_view();

    std::string result;
    int matches = 0;
    for (const auto& [fieldName, _] : schema.Fields) {
        if (fieldName == name) return name; // already qualified to this schema
        const auto fdot = fieldName.rfind('.');
        const std::string_view fbare = (fdot != std::string::npos)
            ? std::string_view(fieldName).substr(fdot + 1)
            : std::string_view(fieldName);
        if (fbare != bare) continue;
        if (dotPos != std::string::npos) {
            const std::string_view ftable = (fdot != std::string::npos)
                ? std::string_view(fieldName).substr(0, fdot) : std::string_view();
            if (ftable != prefix && !IsAliasOfStem(ftable, prefix)) continue;
        }
        result = fieldName;
        ++matches;
    }
    if (matches == 1) return result;
    return name; // zero or ambiguous -> keep original (best-effort)
}

// Recursively rename TIdentExpr leaves in an expression tree.
void QualifyExpr(TExprPtr& expr, const TStructType& schema) {
    if (!expr) return;
    if (auto ident = TMaybeNode<TIdentExpr>(expr)) {
        ident.Cast()->Name = QualifyIdent(schema, ident.Cast()->Name);
        return;
    }
    for (TExprPtr* child : expr->MutableChildren()) {
        QualifyExpr(*child, schema);
    }
}

// Bottom-up pass: returns the qualified output schema for each node.
// Also modifies the node in-place (source Type, join keys, expression idents, etc.)
std::shared_ptr<TStructType> QualifyColumnsImpl(const TOperatorPtr& op) {

    // -- Source --------------------------------------------------------------
    if (auto maybe = TMaybeOp<TSourceOperator>(op)) {
        auto src = maybe.Cast();
        const std::string& alias = src->GetAlias();
        const auto& schema = src->GetSource().Schema();

        std::vector<std::pair<std::string, TTypePtr>> newFields;
        newFields.reserve(
            schema.Columns.size() + (src->EmitsRowId() ? 1 : 0));
        for (const auto& col : schema.Columns) {
            newFields.emplace_back(alias + "." + std::string(col.Name), col.Type);
        }
        if (src->EmitsRowId()) {
            newFields.emplace_back(
                src->RowIdColumn(),
                std::make_shared<TIntegerType>(TIntegerType::U64));
        }
        auto qStruct = std::make_shared<TStructType>(std::move(newFields));

        // Update the source's TFunctionType::ReturnType in-place.
        if (auto* fun = static_cast<TFunctionType*>(src->Type.get())) {
            fun->ReturnType = qStruct;
        } else {
            src->Type = std::make_shared<TFunctionType>(
                std::vector<TTypePtr>{}, qStruct);
        }
        return qStruct;
    }

    // -- Filter --------------------------------------------------------------
    if (auto maybe = TMaybeOp<TFilterOperator>(op)) {
        auto flt = maybe.Cast();
        auto childSchema = QualifyColumnsImpl(flt->Input());
        if (childSchema) {
            QualifyExpr(flt->MutablePredicate(), *childSchema);
        }
        return childSchema;
    }

    // -- Sort / TopSort / Limit ---------------------------------------------
    if (auto maybe = TMaybeOp<TSortOperator>(op)) {
        auto sort = maybe.Cast();
        auto childSchema = QualifyColumnsImpl(sort->Input());
        if (childSchema) {
            for (auto& key : sort->MutableKeys()) {
                key.Column = QualifyIdent(*childSchema, key.Column);
            }
        }
        return childSchema;
    }

    if (auto maybe = TMaybeOp<TTopSortOperator>(op)) {
        auto topSort = maybe.Cast();
        auto childSchema = QualifyColumnsImpl(topSort->Input());
        if (childSchema) {
            for (auto& key : topSort->MutableKeys()) {
                key.Column = QualifyIdent(*childSchema, key.Column);
            }
        }
        return childSchema;
    }

    if (auto maybe = TMaybeOp<TLimitOperator>(op)) {
        auto limit = maybe.Cast();
        return QualifyColumnsImpl(limit->Input());
    }

    // -- Join ----------------------------------------------------------------
    if (auto maybe = TMaybeOp<TJoinOperator>(op)) {
        auto join = maybe.Cast();
        auto leftSchema  = QualifyColumnsImpl(join->Left());
        auto rightSchema = QualifyColumnsImpl(join->Right());

        // Update equi-join keys.
        if (leftSchema && rightSchema) {
            for (auto& key : join->MutableKeys()) {
                key.Left  = QualifyIdent(*leftSchema,  key.Left);
                key.Right = QualifyIdent(*rightSchema, key.Right);
            }
        }

        // Update optional residual predicate (uses merged schema).
        if (join->MutableFilter() && leftSchema && rightSchema) {
            std::vector<std::pair<std::string, TTypePtr>> merged = leftSchema->Fields;
            for (auto& f : rightSchema->Fields) merged.push_back(f);
            TStructType mergedStruct(std::move(merged));
            QualifyExpr(join->MutableFilter(), mergedStruct);
        }

        // Build the qualified output schema by join type: semi/anti joins expose
        // only their surviving side (matching ComputeJoinOutputType), so an outer
        // predicate cannot accidentally bind to the eliminated side's columns.
        if (!leftSchema || !rightSchema) return nullptr;
        std::vector<std::pair<std::string, TTypePtr>> outFields;
        switch (join->JoinType()) {
            case EJoinType::LeftSemi:
            case EJoinType::LeftAnti:
                outFields = leftSchema->Fields;
                break;
            case EJoinType::RightSemi:
            case EJoinType::RightAnti:
                outFields = rightSchema->Fields;
                break;
            case EJoinType::LeftMark:
                // Untyped, as ComputeJoinOutputType rejects it for now.
                return nullptr;
            default:
                outFields = leftSchema->Fields;
                for (auto& f : rightSchema->Fields) outFields.push_back(f);
                break;
        }
        auto outSchema = std::make_shared<TStructType>(std::move(outFields));

        // Update the join's TFunctionType::ReturnType so AnnotateTypes can skip it if needed.
        // (AnnotateTypes will re-compute it anyway, so this just ensures a non-null ReturnType
        //  is available during the QualifyColumns bottom-up walk for ancestor nodes.)
        if (auto* fun = static_cast<TFunctionType*>(join->Type.get())) {
            fun->ReturnType = outSchema;
        }
        return outSchema;
    }

    // -- Project -------------------------------------------------------------
    if (auto maybe = TMaybeOp<TProjectOperator>(op)) {
        auto proj = maybe.Cast();
        auto childSchema = QualifyColumnsImpl(proj->Input());
        if (childSchema) {
            for (auto& spec : proj->MutableProjections()) {
                QualifyExpr(spec.Expression, *childSchema);
            }
        }
        // Output schema is user-defined names; types will be filled by AnnotateTypes.
        std::vector<std::pair<std::string, TTypePtr>> outFields;
        for (const auto& spec : proj->Projections()) {
            if (spec.ColumnAliases.empty()) {
                outFields.emplace_back(spec.Name, nullptr);
            } else {
                for (const auto& name : spec.ColumnAliases) {
                    outFields.emplace_back(name, nullptr);
                }
            }
        }
        return std::make_shared<TStructType>(std::move(outFields));
    }

    // -- Aggregate -----------------------------------------------------------
    if (auto maybe = TMaybeOp<TAggregateOperator>(op)) {
        auto agg = maybe.Cast();
        auto childSchema = QualifyColumnsImpl(agg->Input());
        if (childSchema) {
            for (auto& key : agg->MutableGroupKeys()) {
                key = QualifyIdent(*childSchema, key);
            }
            for (auto& spec : agg->MutableAggs()) {
                QualifyExpr(spec.Arg, *childSchema);
            }
        }
        // Output schema is user-defined; types filled by AnnotateTypes.
        std::vector<std::pair<std::string, TTypePtr>> outFields;
        for (const auto& key : agg->GroupKeys()) {
            outFields.emplace_back(key, nullptr);
        }
        for (const auto& spec : agg->Aggs()) {
            outFields.emplace_back(spec.Name, nullptr);
        }
        return std::make_shared<TStructType>(std::move(outFields));
    }

    // -- Window --------------------------------------------------------------
    // Preserves all input columns and appends one per function; qualify the
    // partition/order key names and the function arguments against the input.
    if (auto maybe = TMaybeOp<TWindowOperator>(op)) {
        auto window = maybe.Cast();
        auto childSchema = QualifyColumnsImpl(window->Input());
        if (childSchema) {
            for (auto& key : window->MutablePartitionKeys()) {
                key = QualifyIdent(*childSchema, key);
            }
            for (auto& key : window->MutableOrderKeys()) {
                key.Column = QualifyIdent(*childSchema, key.Column);
            }
            for (auto& func : window->MutableFunctions()) {
                QualifyExpr(func.Arg, *childSchema);
            }
        }
        std::vector<std::pair<std::string, TTypePtr>> outFields;
        if (childSchema) {
            outFields = childSchema->Fields;
        }
        for (const auto& func : window->Functions()) {
            outFields.emplace_back(func.Name, nullptr);
        }
        return std::make_shared<TStructType>(std::move(outFields));
    }

    // -- Union -----------------------------------------------------------------
    // Qualify every branch; the union's output schema follows the first branch
    // (set-op columns are positional). Returning a non-null schema here is what
    // lets a semi-join above a decorrelated EXISTS-in-OR keep a valid right
    // schema - otherwise the join returns null and its ancestors stop qualifying.
    if (auto maybe = TMaybeOp<TUnionAllOperator>(op)) {
        auto un = maybe.Cast();
        std::shared_ptr<TStructType> first;
        for (auto& branch : un->MutableInputs()) {
            auto branchSchema = QualifyColumnsImpl(branch);
            if (!first) first = branchSchema;
        }
        return first;
    }

    // Unknown operator: recurse into inputs to qualify sources beneath it.
    for (const auto& input : op->Inputs()) {
        QualifyColumnsImpl(input);
    }
    return nullptr;
}

} // namespace

// ----------------------------------------------------------------------------

void AssignSourceAliases(const TOperatorPtr& root) {
    std::vector<std::shared_ptr<TSourceOperator>> sources;
    CollectSources(root, sources);

    // Count stems (only for sources without an explicit alias).
    std::unordered_map<std::string, int> stemCount;
    for (auto& src : sources) {
        if (src->GetAlias().empty()) {
            stemCount[ExtractStem(src->SourcePath())]++;
        }
    }

    // Assign aliases: unique stem -> bare stem, duplicate stem -> stem_0, stem_1, ...
    std::unordered_map<std::string, int> stemIndex;
    for (auto& src : sources) {
        if (!src->GetAlias().empty()) continue; // explicit alias already set
        std::string stem = ExtractStem(src->SourcePath());
        if (stemCount[stem] == 1) {
            src->SetAlias(stem);
        } else {
            int idx = stemIndex[stem]++;
            src->SetAlias(stem + "_" + std::to_string(idx));
        }
    }
}

void QualifyColumns(const TOperatorPtr& root) {
    QualifyColumnsImpl(root);
}

} // namespace NQdb
