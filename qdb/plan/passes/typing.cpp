#include <qdb/plan/passes/typing.h>

#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/late_materialize.h>
#include <qdb/plan/ops/limit.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/sort.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/ops/cte_ref.h>
#include <qdb/plan/ops/union.h>
#include <qdb/plan/ops/window.h>
#include <qdb/plan/types/nullable.h>
#include <qdb/plan/types/decimal.h>
#include <qdb/kernel/annotate_type.h>

#include <qumir/error.h>
#include <qumir/parser/type.h>

#include <stdexcept>
#include <string>
#include <unordered_set>

namespace NQdb {

using namespace NQumir::NAst;

namespace {

bool IsIntType(const TTypePtr& t) { return static_cast<bool>(TMaybeType<TIntegerType>(t)); }
bool IsFloatType(const TTypePtr& t) { return static_cast<bool>(TMaybeType<TFloatType>(t)); }

// Common numeric type for a UNION column across branches: any decimal -> decimal
// (scale = max branch scale, precision widened to 38 to avoid overflow when a float/int
// branch is cast in); else any float -> float; else keep. Returns nullptr if the base
// types are not a compatible numeric mix (caller reports the mismatch).
TTypePtr UnifyUnionBase(const TTypePtr& a, const TTypePtr& b) {
    if (!a || !b) return nullptr;
    if (a->ToString() == b->ToString()) return a;
    auto da = DecimalSpecOfValueType(a);
    auto db = DecimalSpecOfValueType(b);
    const bool numA = da || IsFloatType(a) || IsIntType(a);
    const bool numB = db || IsFloatType(b) || IsIntType(b);
    if (!numA || !numB) return nullptr;
    if (da || db) {
        int32_t scale = std::max(da ? da->Scale : 0, db ? db->Scale : 0);
        return std::make_shared<TDecimal>(MaxDecimalPrecision, scale);
    }
    if (IsFloatType(a) || IsFloatType(b)) return std::make_shared<TFloatType>();
    return a; // both integer
}

// Unify two UNION column types (nullability + numeric base). nullptr -> incompatible.
TTypePtr UnifyUnionColumn(const TTypePtr& a, const TTypePtr& b) {
    auto base = UnifyUnionBase(UnwrapNullableType(a), UnwrapNullableType(b));
    if (!base) return nullptr;
    return (IsNullableType(a) || IsNullableType(b))
        ? std::static_pointer_cast<TType>(std::make_shared<TNullable>(base)) : base;
}

// An operator reads only columns its inputs produce. Kernel type inference types
// any other name like a bare NULL, so a misspelled column would silently change
// the result (a comparison over it is NULL, coalesce() picks its fallback) instead
// of failing the query. Checked before the operator itself is typed, so the
// unknown name is reported rather than a type inference failure it causes.
void RequireInputColumns(const TOperatorPtr& op) {
    std::unordered_set<std::string> available;
    for (const auto& input : op->Inputs()) {
        auto&& schema = input->OutputColumns();
        if (!schema) {
            throw std::runtime_error(
                std::string(op->RelName()) + " typing: an input has no schema");
        }
        for (const auto& [name, _] : schema->Fields) {
            available.insert(name);
        }
    }
    for (const auto& reference : op->ColumnReferences()) {
        if (available.contains(reference.Name)) {
            continue;
        }
        const auto message = "unknown input column: " + reference.Name;
        // Planner-generated identifiers carry no source location.
        if (reference.Ident && reference.Ident->Location.Line > 0) {
            throw NQumir::TError(reference.Ident->Location, message);
        }
        throw NQumir::TError(message);
    }
}

void ResolveProjectionSortKeys(
    std::vector<TSortKey>& keys,
    const TOperatorPtr& input)
{
    auto project = TMaybeOp<TProjectOperator>(input);
    auto&& output = input->OutputColumns();
    if (!project || !output) {
        return;
    }
    for (auto& key : keys) {
        if (!key.ProjectionIndex) {
            continue;
        }
        const auto& projections = project.Cast()->Projections();
        if (*key.ProjectionIndex >= projections.size()) {
            throw NQumir::TError("ORDER BY projection index is out of range");
        }
        size_t outputIndex = 0;
        for (size_t i = 0; i < *key.ProjectionIndex; ++i) {
            outputIndex += FlattenedProjectionArity(projections[i]);
        }
        if (outputIndex >= output->Fields.size()) {
            throw NQumir::TError("ORDER BY flattened projection is out of range");
        }
        key.Column = output->Fields[outputIndex].first;
        key.ProjectionIndex.reset();
    }
}

} // namespace

void AnnotateTypes(
    const TOperatorPtr& root,
    const NKernel::TAnnotationContext& context)
{
    // Bottom-up: children first.
    for (const auto& input : root->Inputs()) {
        AnnotateTypes(input, context);
    }

    RequireInputColumns(root);
    AnnotateOperatorType(root, context);
}

void AnnotateOperatorType(
    const TOperatorPtr& root,
    const NKernel::TAnnotationContext& context)
{
    if (auto maybe = TMaybeOp<TSourceOperator>(root)) {
        auto src = maybe.Cast();
        // If QualifyColumns has already set a qualified schema, keep it.
        if (!src->GetAlias().empty()) {
            // EnableRowId updates the already-qualified source type eagerly.
            return;
        }
        root->Type = std::make_shared<TFunctionType>(
            std::vector<TTypePtr>{},
            StructTypeFromSchema(src->GetSource().Schema()));
        if (src->EmitsRowId()) {
            src->EnableRowId();
        }
        return;
    }

    if (auto maybe = TMaybeOp<TCteRef>(root)) {
        // Re-derive from the definition, whose schema may have shifted while it
        // was optimized (e.g. set-op branch coercion).
        root->Type = std::make_shared<TFunctionType>(
            std::vector<TTypePtr>{}, maybe.Cast()->Def()->OutputType);
        return;
    }

    if (auto maybe = TMaybeOp<TFilterOperator>(root)) {
        auto schema = maybe.Cast()->Input()->OutputColumns();
        root->Type = std::make_shared<TFunctionType>(
            std::vector<TTypePtr>{schema},
            schema);
        return;
    }

    if (auto maybe = TMaybeOp<TSortOperator>(root)) {
        auto sort = maybe.Cast();
        ResolveProjectionSortKeys(sort->MutableKeys(), sort->Input());
        auto schema = sort->Input()->OutputColumns();
        root->Type = std::make_shared<TFunctionType>(
            std::vector<TTypePtr>{schema},
            schema);
        return;
    }

    if (auto maybe = TMaybeOp<TTopSortOperator>(root)) {
        auto sort = maybe.Cast();
        ResolveProjectionSortKeys(sort->MutableKeys(), sort->Input());
        auto schema = sort->Input()->OutputColumns();
        root->Type = std::make_shared<TFunctionType>(
            std::vector<TTypePtr>{schema},
            schema);
        return;
    }

    if (auto maybe = TMaybeOp<TLimitOperator>(root)) {
        auto schema = maybe.Cast()->Input()->OutputColumns();
        root->Type = std::make_shared<TFunctionType>(
            std::vector<TTypePtr>{schema},
            schema);
        return;
    }

    if (auto maybe = TMaybeOp<TLateMaterializeOperator>(root)) {
        auto late = maybe.Cast();
        std::vector<std::pair<std::string, TTypePtr>> fields;
        fields.reserve(late->Columns().size());
        for (const auto& column : late->Columns()) {
            fields.emplace_back(column.OutputName, column.Type);
        }
        root->Type = std::make_shared<TFunctionType>(
            std::vector<TTypePtr>{late->Input()->OutputColumns()},
            std::make_shared<TStructType>(std::move(fields)));
        return;
    }

    if (auto maybe = TMaybeOp<TProjectOperator>(root)) {
        auto proj = maybe.Cast();
        auto&& inputStruct = proj->Input()->OutputColumns();
        std::vector<std::pair<std::string, TTypePtr>> outFields;
        if (inputStruct) {
            for (auto& spec : proj->MutableProjections()) {
                TTypePtr fieldType;
                if (auto ident = TMaybeNode<TIdentExpr>(spec.Expression)) {
                    for (auto& [name, type] : inputStruct->Fields) {
                        if (name == ident.Cast()->Name) { fieldType = type; break; }
                    }
                } else {
                    // Computed column: rule-based SQL type inference (no rewrite).
                    fieldType = NKernel::AnnotateExprType(
                        spec.Expression, *inputStruct, &context);
                }
                auto structType = TMaybeType<TStructType>(
                    UnwrapNamedType(UnwrapNullableType(fieldType)));
                if (structType && IsNullableType(fieldType)) {
                    throw NQumir::TError(spec.Expression->Location,
                        "nullable struct-return projections are not supported");
                }
                if (structType && spec.RejectUnaliasedStructForDistinct) {
                    throw NQumir::TError(spec.Expression->Location,
                        "SELECT DISTINCT over a struct-return projection requires AS (...)");
                }
                auto nextColName = [&] {
                    return "col" + std::to_string(outFields.size());
                };
                if (!structType) {
                    if (!spec.ColumnAliases.empty()) {
                        throw NQumir::TError(spec.Expression->Location,
                            "a column alias list requires a struct-return projection");
                    }
                    if (spec.ImplicitName) {
                        spec.Name = nextColName();
                    }
                    spec.ImplicitName = false;
                    outFields.emplace_back(spec.Name, fieldType);
                    continue;
                }
                if (spec.ColumnAliases.empty() && !spec.ImplicitName) {
                    throw NQumir::TError(spec.Expression->Location,
                        "a struct-return projection cannot use a scalar alias");
                }
                // Preserve the grouped type after OutputColumns is flattened.
                // Scalar expressions keep their original AST type metadata
                // (notably planner-only DECIMAL casts).
                spec.Expression->Type = fieldType;
                const auto structure = structType.Cast();
                const auto& aliases = spec.ColumnAliases;
                if (!aliases.empty() && aliases.size() != structure->Fields.size()) {
                    throw NQumir::TError(spec.Expression->Location,
                        "struct-return projection has "
                        + std::to_string(structure->Fields.size())
                        + " fields but " + std::to_string(aliases.size())
                        + " column aliases were specified");
                }
                spec.Name = aliases.empty() ? nextColName() : aliases.front();
                const auto offset = outFields.size();
                for (size_t i = 0; i < structure->Fields.size(); ++i) {
                    outFields.emplace_back(
                        aliases.empty() ? nextColName() : aliases[i],
                        structure->Fields[i].second);
                }
                if (spec.ColumnAliases.empty()) {
                    for (size_t i = offset; i < outFields.size(); ++i) {
                        spec.ColumnAliases.push_back(outFields[i].first);
                    }
                }
                spec.ImplicitName = false;
            }
        }
        root->Type = std::make_shared<TFunctionType>(
            std::vector<TTypePtr>{proj->Input()->OutputColumns()},
            std::make_shared<TStructType>(std::move(outFields)));
        return;
    }

    if (auto maybe = TMaybeOp<TAggregateOperator>(root)) {
        auto agg = maybe.Cast();
        auto inputSchema = agg->Input()->OutputColumns();
        root->Type = std::make_shared<TFunctionType>(
            std::vector<TTypePtr>{inputSchema},
            ComputeAggregateOutputType(inputSchema, agg->GroupKeys(), agg->Aggs(),
                !agg->GroupingSets().empty()));
        return;
    }

    if (auto maybe = TMaybeOp<TWindowOperator>(root)) {
        auto window = maybe.Cast();
        auto inputSchema = window->Input()->OutputColumns();
        root->Type = std::make_shared<TFunctionType>(
            std::vector<TTypePtr>{inputSchema},
            ComputeWindowOutputType(inputSchema, window->Functions()));
        return;
    }

    if (auto maybe = TMaybeOp<TJoinOperator>(root)) {
        auto join = maybe.Cast();
        auto leftSchema = join->Left()->OutputColumns();
        auto rightSchema = join->Right()->OutputColumns();
        auto output = ComputeJoinOutputType(leftSchema, rightSchema, join->JoinType());
        if (!output) {
            throw output.error();
        }
        root->Type = std::make_shared<TFunctionType>(
            std::vector<TTypePtr>{leftSchema, rightSchema},
            *output);
        return;
    }

    if (auto maybe = TMaybeOp<TUnionAllOperator>(root)) {
        auto un = maybe.Cast();
        const auto& inputs = un->Inputs();
        std::vector<TTypePtr> paramTypes;
        paramTypes.reserve(inputs.size());
        for (const auto& input : inputs) {
            paramTypes.push_back(input->OutputColumns());
        }
        auto* first = static_cast<TStructType*>(paramTypes[0].get());
        // Unified per-column types (names from the first branch). Numeric branches are
        // promoted to a common type (any decimal -> decimal), and a column is nullable if
        // any branch's is. CoerceSetOpBranches then inserts casts so every branch
        // physically produces this layout (the union just routes branch rowsets).
        std::vector<std::pair<std::string, TTypePtr>> unified;
        if (first && !first->Fields.empty()) {
            unified = first->Fields;
        }
        for (size_t b = 1; b < inputs.size(); ++b) {
            auto* other = static_cast<TStructType*>(paramTypes[b].get());
            // A branch with no resolved schema means an upstream typing failure;
            // let that surface on its own rather than reporting a count mismatch.
            if (!first || !other || first->Fields.empty() || other->Fields.empty()) {
                continue;
            }
            if (first->Fields.size() != other->Fields.size()) {
                throw NQumir::TError("UNION ALL branches must have the same number of columns");
            }
            for (size_t i = 0; i < first->Fields.size(); ++i) {
                auto merged = UnifyUnionColumn(unified[i].second, other->Fields[i].second);
                if (!merged) {
                    throw NQumir::TError(
                        "UNION ALL branch " + std::to_string(b) + " column " +
                        std::to_string(i) + " type mismatch");
                }
                unified[i].second = merged;
            }
        }
        auto output = (first && !first->Fields.empty())
            ? std::static_pointer_cast<TType>(std::make_shared<TStructType>(std::move(unified)))
            : inputs[0]->OutputColumns();
        root->Type = std::make_shared<TFunctionType>(std::move(paramTypes), std::move(output));
        return;
    }
}

// After types are known, wrap each UNION ALL branch whose column types differ from the
// unified output in a projection that casts them, so every branch physically emits the
// same layout (the union operator only routes branch rowsets, it does not coerce).
void CoerceSetOpBranches(const TOperatorPtr& root) {
    for (const auto& input : root->Inputs()) {
        CoerceSetOpBranches(input);
    }
    auto un = TMaybeOp<TUnionAllOperator>(root);
    if (!un) return;
    auto* fun = static_cast<TFunctionType*>(root->Type.get());
    if (!fun) return;
    auto* unified = static_cast<TStructType*>(fun->ReturnType.get());
    if (!unified || unified->Fields.empty()) return;

    for (auto& branch : un.Cast()->MutableInputs()) {
        auto branchSchema = branch->OutputColumns();
        auto* bs = static_cast<TStructType*>(branchSchema.get());
        if (!bs || bs->Fields.size() != unified->Fields.size()) continue;

        bool needs = false;
        for (size_t i = 0; i < bs->Fields.size(); ++i) {
            const auto& bt = bs->Fields[i].second;
            const auto& ut = unified->Fields[i].second;
            if (bt && ut && bt->ToString() != ut->ToString()) { needs = true; break; }
        }
        if (!needs) continue;

        std::vector<TProjectionSpec> projs;
        projs.reserve(bs->Fields.size());
        for (size_t i = 0; i < bs->Fields.size(); ++i) {
            const std::string& colName = bs->Fields[i].first;
            TExprPtr expr = std::make_shared<TIdentExpr>(NQumir::TLocation{}, colName);
            const auto& bt = bs->Fields[i].second;
            const auto& ut = unified->Fields[i].second;
            if (bt && ut && bt->ToString() != ut->ToString()) {
                expr = std::make_shared<TCastExpr>(NQumir::TLocation{}, expr, ut);
            }
            projs.push_back({.Name = colName, .Expression = std::move(expr)});
        }
        branch = std::make_shared<TProjectOperator>(branch, std::move(projs));
    }
}

} // namespace NQdb
