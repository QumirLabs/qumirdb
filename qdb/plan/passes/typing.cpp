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
#include <qdb/plan/types/date.h>
#include <qdb/plan/types/nullable.h>
#include <qdb/plan/types/decimal.h>
#include <qdb/plan/types/type_text.h>
#include <qdb/kernel/annotate_type.h>

#include <qumir/error.h>
#include <qumir/parser/core/printer.h>
#include <qumir/parser/type.h>

#include <algorithm>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace NQdb {

using namespace NQumir::NAst;

namespace {

bool IsIntType(const TTypePtr& t) {
    return static_cast<bool>(MaybeIntegerType(t));
}
bool IsFloatType(const TTypePtr& t) {
    return static_cast<bool>(TMaybeType<TFloatType>(t));
}

// Narrowest integer type holding both operands, as in DuckDB: the wider of two
// integers of one signedness, and for mixed signedness the signed type wider than
// the unsigned operand (UINTEGER with INTEGER gives BIGINT). nullptr when no
// 128-bit-or-narrower type holds both.
TTypePtr UnifyIntegers(NQumir::NAst::TTypeFactory& types, const TIntegerType& a, const TIntegerType& b) {
    using EKind = TIntegerType::EKind;
    if (a.IsSigned() == b.IsSigned()) {
        return types.MakeInteger(a.BitWidth() >= b.BitWidth() ? a.Width : b.Width);
    }
    const auto& sig = a.IsSigned() ? a : b;
    const auto& uns = a.IsSigned() ? b : a;
    if (sig.BitWidth() > uns.BitWidth()) {
        return types.MakeInteger(sig.Width);
    }
    switch (uns.BitWidth()) {
        case 8: return types.MakeInteger(EKind::I16);
        case 16: return types.MakeInteger(EKind::I32);
        case 32: return types.MakeInteger(EKind::I64);
        case 64: return types.MakeInteger(EKind::I128);
        default: return nullptr;
    }
}

// Common numeric type for a UNION column across branches: any decimal -> decimal
// (scale = max branch scale, precision widened to 38 to avoid overflow when a float/int
// branch is cast in); else two floats -> the wider (REAL with DOUBLE is DOUBLE), a float with
// an integer -> the float, as in PostgreSQL; else the wider integer. Returns nullptr if the
// base types are not a compatible numeric mix (caller reports the mismatch).
TTypePtr UnifyUnionBase(NQumir::NAst::TTypeFactory& types, const TTypePtr& a, const TTypePtr& b) {
    if (!a || !b) {
        return nullptr;
    }
    // PostgreSQL has no DATE with a number.
    if (IsDateType(a) || IsDateType(b)) {
        return IsDateType(a) && IsDateType(b) ? a : nullptr;
    }
    if (a == b) {
        return a;
    }
    auto da = DecimalSpecOfValueType(a);
    auto db = DecimalSpecOfValueType(b);
    const bool numA = da || IsFloatType(a) || IsIntType(a);
    const bool numB = db || IsFloatType(b) || IsIntType(b);
    if (!numA || !numB) return nullptr;
    if (da || db) {
        int32_t scale = std::max(da ? da->Scale : 0, db ? db->Scale : 0);
        return MakeDecimalType(types, {MaxDecimalPrecision, scale});
    }
    if (IsFloatType(a) && IsFloatType(b)) {
        const int aBits = TMaybeType<TFloatType>(a).Cast()->BitWidth();
        return aBits >= TMaybeType<TFloatType>(b).Cast()->BitWidth() ? a : b;
    }
    if (IsFloatType(a) || IsFloatType(b)) return IsFloatType(a) ? a : b;
    return UnifyIntegers(types, *MaybeIntegerType(a).Cast(), *MaybeIntegerType(b).Cast());
}

// Unify two UNION column types (nullability + numeric base). nullptr -> incompatible.
TTypePtr UnifyUnionColumn(NQumir::NAst::TTypeFactory& types, const TTypePtr& a, const TTypePtr& b) {
    auto base = UnifyUnionBase(types, UnwrapNullableType(a), UnwrapNullableType(b));
    if (!base) return nullptr;
    return (IsNullableType(a) || IsNullableType(b))
        ? MakeNullableType(types, base) : base;
}

// An operator reads only columns its inputs produce. Kernel type inference types
// any other name like a bare NULL, so a misspelled column would silently change
// the result (a comparison over it is NULL, coalesce() picks its fallback) instead
// of failing the query. Checked before the operator itself is typed, so the
// unknown name is reported rather than a type inference failure it causes.
void RequireInputColumns(const TOperatorPtr& op) {
    std::unordered_set<std::string> available;
    std::unordered_map<std::string, size_t> bareNameCounts;
    for (const auto& input : op->Inputs()) {
        for (const auto& [name, _] : input->OutputColumns()->Fields) {
            available.insert(name);
            ++bareNameCounts[BareColumnName(name)];
        }
    }
    for (const auto& reference : op->ColumnReferences()) {
        if (available.contains(reference.Name)) {
            continue;
        }
        // QualifyColumns leaves a bare name unresolved when it matches several
        // qualified columns, as in a self-join.
        const auto bare = bareNameCounts.find(reference.Name);
        const bool ambiguous = reference.Name.find('.') == std::string::npos
            && bare != bareNameCounts.end()
            && bare->second > 1;
        const auto message = (ambiguous)
            ? "ambiguous input column: " + reference.Name
            : "unknown input column: " + reference.Name;
        // Planner-generated identifiers carry no source location.
        if (reference.Ident && reference.Ident->Location.Line > 0) {
            throw NQumir::TError(reference.Ident->Location, message);
        }
        throw NQumir::TError(message);
    }
}

// Which output columns of a UNION branch are untyped NULLs: a projected expression that
// derives from bare NULLs only (see NKernel::IsUntypedNullValue).
std::vector<bool> UntypedNullColumns(
    NQumir::NAst::TTypeFactory& types,
    const TOperatorPtr& branch,
    const NKernel::TAnnotationContext& context)
{
    auto schema = branch->OutputColumns();
    std::vector<bool> result(schema ? schema->Fields.size() : 0, false);
    auto proj = TMaybeOp<TProjectOperator>(branch);
    if (!proj || proj.Cast()->Projections().size() != result.size()) {
        return result;
    }
    auto input = proj.Cast()->Input()->OutputColumns();
    for (size_t i = 0; input && i < result.size(); ++i) {
        const auto& expr = proj.Cast()->Projections()[i].Expression;
        result[i] = !TMaybeNode<TIdentExpr>(expr)
            && NKernel::IsUntypedNullValue(types, expr, *input, context);
    }
    return result;
}

// Types the subtree bottom-up and returns whether all of it is typed. A join
// whose sides output the same column name, as two unaliased derived tables can,
// stays untyped: column pruning may still drop the colliding columns, and building
// the execution plan reports the collision otherwise. The operators above such a
// join are typed against incomplete schemas, so their references are not checked
// until then.
bool AnnotateSubtree(
    NQumir::NAst::TTypeFactory& types,
    const TOperatorPtr& root,
    const NKernel::TAnnotationContext& context)
{
    bool typed = true;
    for (const auto& input : root->Inputs()) {
        typed = AnnotateSubtree(types, input, context) && typed;
    }
    if (typed) {
        RequireInputColumns(root);
    }
    AnnotateOperatorType(types, root, context);
    return typed && root->OutputColumns();
}

} // namespace

void AnnotateTypes(
    NQumir::NAst::TTypeFactory& types,
    const TOperatorPtr& root,
    const NKernel::TAnnotationContext& context)
{
    AnnotateSubtree(types, root, context);
}

void AnnotateOperatorType(
    NQumir::NAst::TTypeFactory& types,
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
        root->Type = types.MakeFunction(
            std::vector<TTypePtr>{},
            StructTypeFromSchema(types, src->TableSchema()));
        if (src->EmitsRowId()) {
            src->EnableRowId(types);
        }
        return;
    }

    if (auto maybe = TMaybeOp<TCteRef>(root)) {
        // Re-derive from the definition, whose schema may have shifted while it
        // was optimized (e.g. set-op branch coercion).
        root->Type = types.MakeFunction(
            std::vector<TTypePtr>{}, maybe.Cast()->Def()->OutputType);
        return;
    }

    if (auto maybe = TMaybeOp<TFilterOperator>(root)) {
        auto schema = maybe.Cast()->Input()->OutputColumns();
        root->Type = types.MakeFunction(
            std::vector<TTypePtr>{schema},
            schema);
        return;
    }

    if (auto maybe = TMaybeOp<TSortOperator>(root)) {
        auto sort = maybe.Cast();
        auto schema = sort->Input()->OutputColumns();
        root->Type = types.MakeFunction(
            std::vector<TTypePtr>{schema},
            schema);
        return;
    }

    if (auto maybe = TMaybeOp<TTopSortOperator>(root)) {
        auto sort = maybe.Cast();
        auto schema = sort->Input()->OutputColumns();
        root->Type = types.MakeFunction(
            std::vector<TTypePtr>{schema},
            schema);
        return;
    }

    if (auto maybe = TMaybeOp<TLimitOperator>(root)) {
        auto schema = maybe.Cast()->Input()->OutputColumns();
        root->Type = types.MakeFunction(
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
        root->Type = types.MakeFunction(
            std::vector<TTypePtr>{late->Input()->OutputColumns()},
            types.MakeStruct(std::move(fields)));
        return;
    }

    if (auto maybe = TMaybeOp<TProjectOperator>(root)) {
        auto proj = maybe.Cast();
        auto&& inputStruct = proj->Input()->OutputColumns();
        std::vector<std::pair<std::string, TTypePtr>> outFields;
        if (inputStruct) {
            for (auto& spec : proj->MutableProjections()) {
                TTypePtr fieldType = nullptr;
                if (auto ident = TMaybeNode<TIdentExpr>(spec.Expression)) {
                    for (auto& [name, type] : inputStruct->Fields) {
                        if (name == ident.Cast()->Name) { fieldType = type; break; }
                    }
                } else {
                    // Computed column: rule-based SQL type inference (no rewrite).
                    fieldType = NKernel::AnnotateExprType(
                        types, spec.Expression, *inputStruct, context);
                }
                auto structType = TMaybeType<TStructType>(
                    UnwrapNamedValueType(UnwrapNullableType(fieldType)));
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
        root->Type = types.MakeFunction(
            std::vector<TTypePtr>{proj->Input()->OutputColumns()},
            types.MakeStruct(std::move(outFields)));
        return;
    }

    if (auto maybe = TMaybeOp<TAggregateOperator>(root)) {
        auto agg = maybe.Cast();
        auto inputSchema = agg->Input()->OutputColumns();
        root->Type = types.MakeFunction(
            std::vector<TTypePtr>{inputSchema},
            ComputeAggregateOutputType(types, inputSchema, agg->GroupKeys(), agg->Aggs(),
                !agg->GroupingSets().empty(), !agg->EmptyInputGroups().empty()));
        return;
    }

    if (auto maybe = TMaybeOp<TWindowOperator>(root)) {
        auto window = maybe.Cast();
        auto inputSchema = window->Input()->OutputColumns();
        root->Type = types.MakeFunction(
            std::vector<TTypePtr>{inputSchema},
            ComputeWindowOutputType(types, inputSchema, window->Functions()));
        return;
    }

    if (auto maybe = TMaybeOp<TJoinOperator>(root)) {
        auto join = maybe.Cast();
        auto leftSchema = join->Left()->OutputColumns();
        auto rightSchema = join->Right()->OutputColumns();
        auto output = ComputeJoinOutputType(types, leftSchema, rightSchema, join->JoinType());
        root->Type = types.MakeFunction(
            std::vector<TTypePtr>{leftSchema, rightSchema},
            output ? *output : nullptr);
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
        auto* first = static_cast<const TStructType*>(paramTypes[0]);
        // Unified per-column types (names from the first branch). Numeric branches are
        // promoted to a common type (any decimal -> decimal), and a column is nullable if
        // any branch's is. CoerceSetOpBranches then inserts casts so every branch
        // physically produces this layout (the union just routes branch rowsets).
        // A branch column that is an untyped NULL (`SELECT NULL AS x`) takes its type from the
        // other branches and only makes the column nullable; a column NULL in every branch
        // keeps the first branch's NULL[INTEGER].
        std::vector<std::pair<std::string, TTypePtr>> unified;
        if (first && !first->Fields.empty()) {
            unified = first->Fields;
            std::vector<bool> typed(unified.size(), false);
            std::vector<bool> sawNull(unified.size(), false);
            for (size_t b = 0; b < inputs.size(); ++b) {
                auto* other = static_cast<const TStructType*>(paramTypes[b]);
                // A branch with no resolved schema means an upstream typing failure;
                // let that surface on its own rather than reporting a count mismatch.
                if (!other || other->Fields.empty()) {
                    continue;
                }
                if (first->Fields.size() != other->Fields.size()) {
                    throw NQumir::TError("UNION ALL branches must have the same number of columns");
                }
                const auto nulls = UntypedNullColumns(types, inputs[b], context);
                for (size_t i = 0; i < first->Fields.size(); ++i) {
                    if (nulls[i]) {
                        sawNull[i] = true;
                    } else if (!typed[i]) {
                        unified[i].second = other->Fields[i].second;
                        typed[i] = true;
                    } else if (auto merged = UnifyUnionColumn(types, unified[i].second, other->Fields[i].second)) {
                        unified[i].second = merged;
                    } else {
                        throw NQumir::TError(
                            "UNION ALL branch " + std::to_string(b) + " column " +
                            std::to_string(i) + " type mismatch");
                    }
                }
            }
            for (size_t i = 0; i < unified.size(); ++i) {
                if (typed[i] && sawNull[i] && !IsNullableType(unified[i].second)) {
                    unified[i].second = MakeNullableType(types, unified[i].second);
                }
            }
        }
        auto output = (first && !first->Fields.empty())
            ? types.MakeStruct(std::move(unified))
            : inputs[0]->OutputColumns();
        root->Type = types.MakeFunction(std::move(paramTypes), std::move(output));
        return;
    }
}

// After types are known, wrap each UNION ALL branch whose column types differ from the
// unified output in a projection that casts them, so every branch physically emits the
// same layout (the union operator only routes branch rowsets, it does not coerce).
void CoerceSetOpBranches(NQumir::NAst::TTypeFactory& types, const TOperatorPtr& root, const NKernel::TAnnotationContext& context) {
    for (const auto& input : root->Inputs()) {
        CoerceSetOpBranches(types, input, context);
    }
    auto un = TMaybeOp<TUnionAllOperator>(root);
    if (!un) return;
    auto* fun = static_cast<const TFunctionType*>(root->Type);
    if (!fun) return;
    auto* unified = static_cast<const TStructType*>(fun->ReturnType);
    if (!unified || unified->Fields.empty()) return;

    for (auto& branch : un.Cast()->MutableInputs()) {
        auto branchSchema = branch->OutputColumns();
        auto* bs = static_cast<const TStructType*>(branchSchema);
        if (!bs || bs->Fields.size() != unified->Fields.size()) continue;

        // An untyped-NULL column is cast in the branch's own projection, where the cast
        // makes it the NULL of the unified type; the branch schema still says NULL[INTEGER].
        const auto nulls = UntypedNullColumns(types, branch, context);
        bool needs = false;
        for (size_t i = 0; i < bs->Fields.size(); ++i) {
            const auto& bt = bs->Fields[i].second;
            const auto& ut = unified->Fields[i].second;
            if (!bt || !ut || bt == ut) continue;
            if (nulls[i]) {
                auto& expr = TMaybeOp<TProjectOperator>(branch).Cast()->MutableProjections()[i].Expression;
                expr = NQumir::MakeIntrusive<TCastExpr>(NQumir::TLocation{}, expr, ut);
            } else {
                needs = true;
            }
        }
        if (!needs) continue;

        std::vector<TProjectionSpec> projs;
        projs.reserve(bs->Fields.size());
        for (size_t i = 0; i < bs->Fields.size(); ++i) {
            const std::string& colName = bs->Fields[i].first;
            TExprPtr expr = NQumir::MakeIntrusive<TIdentExpr>(NQumir::TLocation{}, colName);
            const auto& bt = bs->Fields[i].second;
            const auto& ut = unified->Fields[i].second;
            if (!nulls[i] && bt && ut && bt != ut) {
                expr = NQumir::MakeIntrusive<TCastExpr>(NQumir::TLocation{}, expr, ut);
            }
            projs.push_back({.Name = colName, .Expression = std::move(expr)});
        }
        branch = NQumir::MakeIntrusive<TProjectOperator>(types, branch, std::move(projs));
    }
}

namespace {

// Wraps `input` in a projection that passes every column through and adds
// `alias` = CAST(`column` AS `target`), keeping the original column's type for
// the operators above the join.
TOperatorPtr AddCastedKey(
    NQumir::NAst::TTypeFactory& types,
    const TOperatorPtr& input, const std::string& column, const TTypePtr& target, const std::string& alias)
{
    auto projections = PassThroughProjections(*input->OutputColumns());
    projections.push_back({
        .Name = alias,
        .Expression = NQumir::MakeIntrusive<TCastExpr>(NQumir::TLocation{},
            NQumir::MakeIntrusive<TIdentExpr>(NQumir::TLocation{}, column), target),
    });
    return NQumir::MakeIntrusive<TProjectOperator>(types, input, std::move(projections));
}

TTypePtr ColumnType(const TOperatorPtr& input, const std::string& column) {
    for (const auto& [name, type] : static_cast<const TStructType*>(input->OutputColumns())->Fields) {
        if (name == column) {
            return type;
        }
    }
    return nullptr;
}

// Decimal digits an integer type needs, so that it fits DECIMAL(digits, 0).
int32_t IntegerDecimalDigits(const TIntegerType& type) {
    switch (type.BitWidth()) {
        case 8: return 3;
        case 16: return 5;
        case 32: return 10;
        case 64: return type.IsSigned() ? 19 : 20;
        default: return MaxDecimalPrecision;
    }
}

// Type both join key sides are cast to so that equal values have equal bytes:
// DOUBLE for a float key against any other number, as PostgreSQL's `=` compares REAL only
// with REAL; a DATE for a DATE key; the common integer type; or for a decimal key
// (against a decimal or an integer) the decimal with the larger scale and the larger
// integer part. nullptr when the sides already share a type or no common key type exists;
// throws when the common decimal would need more than 38 digits, as a wrapping cast would
// equate unequal keys.
TTypePtr CommonKeyType(NQumir::NAst::TTypeFactory& types, const TTypePtr& left, const TTypePtr& right) {
    if (left == right) {
        return nullptr;
    }
    auto leftInt = MaybeIntegerType(left);
    auto rightInt = MaybeIntegerType(right);
    // PostgreSQL compares a DATE with a TIMESTAMP as that day's midnight, and qdb types
    // a TIMESTAMP as its day count: both sides key on the day count of the wider integer.
    if (IsDateType(left) || IsDateType(right)) {
        if (!leftInt || !rightInt) {
            return nullptr;
        }
        const auto days = UnifyIntegers(types, *leftInt.Cast(), *rightInt.Cast());
        return days ? MakeDateType(types, MaybeIntegerType(days).Cast()->Width) : nullptr;
    }
    const auto isNumber = [](const TTypePtr& type) {
        return IsIntType(type) || IsFloatType(type) || DecimalSpecOfValueType(type);
    };
    if ((IsFloatType(left) || IsFloatType(right)) && isNumber(left) && isNumber(right)) {
        return types.MakeFloat(TFloatType::F64);
    }
    if (leftInt && rightInt) {
        return UnifyIntegers(types, *leftInt.Cast(), *rightInt.Cast());
    }
    const auto asDecimal = [](const TTypePtr& type) -> std::optional<TDecimalSpec> {
        if (auto integer = MaybeIntegerType(type)) {
            return TDecimalSpec{.Precision = IntegerDecimalDigits(*integer.Cast()), .Scale = 0};
        }
        return DecimalSpecOfValueType(type);
    };
    const auto leftSpec = asDecimal(left);
    const auto rightSpec = asDecimal(right);
    if (!leftSpec || !rightSpec) {
        return nullptr;
    }
    const int32_t scale = std::max(leftSpec->Scale, rightSpec->Scale);
    const int32_t integerDigits = std::max(
        leftSpec->Precision - leftSpec->Scale, rightSpec->Precision - rightSpec->Scale);
    if (integerDigits + scale > MaxDecimalPrecision) {
        throw NQumir::TError("join key types " + SqlTypeName(left) + " and " + SqlTypeName(right) +
            " have no common DECIMAL type within " + std::to_string(MaxDecimalPrecision) + " digits");
    }
    return MakeDecimalType(types, {integerDigits + scale, scale});
}

// The expression that the projection right below the join computes key `column`
// from, or null for a column of the input.
TExprPtr ComputedKey(const TOperatorPtr& input, const std::string& column) {
    auto maybeProject = TMaybeOp<TProjectOperator>(input);
    if (!maybeProject) {
        return nullptr;
    }
    for (const auto& projection : maybeProject.Cast()->Projections()) {
        if (projection.Name == column) {
            return TMaybeNode<TIdentExpr>(projection.Expression) ? nullptr : projection.Expression;
        }
    }
    return nullptr;
}

// A computed key column has an internal name, so key types without a common type
// are reported with the expressions of the query. The join kernel reports a pair
// of plain columns by name.
void RequireComparableKeys(
    const TJoinOperator& join, const TJoinKey& key, const TTypePtr& leftType, const TTypePtr& rightType)
{
    const auto leftExpr = ComputedKey(join.Left(), key.Left);
    const auto rightExpr = ComputedKey(join.Right(), key.Right);
    if ((!leftExpr && !rightExpr) || leftType == rightType) {
        return;
    }
    auto side = [](const TExprPtr& expr, const std::string& column, const TTypePtr& type) {
        return (expr ? NQumir::NAst::NCore::PrintAst(expr) : "'" + column + "'") + " is " + SqlTypeName(type);
    };
    throw NQumir::TError((leftExpr ? leftExpr : rightExpr)->Location,
        "join key types incompatible: left " + side(leftExpr, key.Left, leftType)
        + ", right " + side(rightExpr, key.Right, rightType));
}

// `aliasCounter` spans the whole plan: the types of the joins below are stale
// until re-annotation, so an alias cannot be checked against their schemas.
void CoerceJoinKeys(NQumir::NAst::TTypeFactory& types, TOperatorPtr& root, size_t& aliasCounter) {
    for (auto& input : root->MutableInputs()) {
        CoerceJoinKeys(types, input, aliasCounter);
    }
    auto join = TMaybeOp<TJoinOperator>(root);
    if (!join) {
        return;
    }
    bool coerced = false;
    // A semi or anti join outputs one input as is, so nothing above it drops a key
    // alias added to that input (INTERSECT, EXCEPT); project the original columns back.
    auto restored = PassThroughProjections(*root->OutputColumns());
    for (auto& key : join.Cast()->MutableKeys()) {
        const auto leftType = ColumnType(join.Cast()->Left(), key.Left);
        const auto rightType = ColumnType(join.Cast()->Right(), key.Right);
        if (!leftType || !rightType) {
            continue;
        }
        const auto common = CommonKeyType(types, UnwrapNullableType(leftType), UnwrapNullableType(rightType));
        if (!common) {
            RequireComparableKeys(*join.Cast(), key, UnwrapNullableType(leftType), UnwrapNullableType(rightType));
            continue;
        }
        // A side that already has the common type is left alone; the other side is cast.
        for (const bool isLeft : {true, false}) {
            const auto& sideType = isLeft ? leftType : rightType;
            if (UnwrapNullableType(sideType) == common) {
                continue;
            }
            auto& column = isLeft ? key.Left : key.Right;
            auto& side = isLeft ? join.Cast()->MutableLeft() : join.Cast()->MutableRight();
            const auto target = IsNullableType(sideType)
                ? MakeNullableType(types, common) : common;
            const auto alias = "__join_key_" + std::to_string(aliasCounter++) + "__";
            coerced = true;
            side = AddCastedKey(types, side, column, target, alias);
            column = alias;
        }
    }
    if (!coerced) {
        return;
    }
    switch (join.Cast()->JoinType()) {
        case EJoinType::LeftSemi:
        case EJoinType::RightSemi:
        case EJoinType::LeftAnti:
        case EJoinType::RightAnti:
            root = NQumir::MakeIntrusive<TProjectOperator>(types, root, std::move(restored));
            break;
        default:
            break;
    }
}

} // namespace

void CoerceJoinKeys(NQumir::NAst::TTypeFactory& types, TOperatorPtr& root) {
    size_t aliasCounter = 0;
    CoerceJoinKeys(types, root, aliasCounter);
}

} // namespace NQdb
