#include <qdb/plan/ops/dependent_join.h>

#include <qdb/plan/ops/cte_ref.h>
#include <qdb/plan/passes/unbound_vars.h>
#include <qdb/plan/types/nullable.h>

#include <qumir/parser/type.h>

#include <unordered_set>

namespace NQdb {

using namespace NQumir::NAst;

namespace {

using TFields = std::vector<std::pair<std::string, TTypePtr>>;

TFields Fields(const TTypePtr& type, bool nullable) {
    TFields out;
    auto structType = TMaybeType<TStructType>(type);
    if (!structType) {
        return out;
    }
    for (const auto& [name, fieldType] : structType.Cast()->Fields) {
        out.emplace_back(name, nullable && !IsNullableType(fieldType)
            ? std::make_shared<TNullable>(fieldType)
            : fieldType);
    }
    return out;
}

TTypePtr MarkerType(
    const TTypePtr& left,
    const TTypePtr& right,
    const TExprPtr& predicate,
    const NKernel::TAnnotationContext& annotation)
{
    TTypePtr marker = std::make_shared<TBoolType>();
    if (!predicate) {
        return marker;
    }
    auto both = Fields(left, false);
    for (auto& field : Fields(right, false)) {
        both.push_back(std::move(field));
    }
    const auto predicateType = NKernel::AnnotateExprType(predicate, TStructType(std::move(both)), &annotation);
    return IsNullableType(predicateType)
        ? std::make_shared<TNullable>(marker)
        : marker;
}

bool FindDependentJoin(
    const TOperatorPtr& op,
    std::unordered_set<const IOperator*>& visited,
    TOperatorPtr& found)
{
    if (!op || !visited.insert(op.get()).second) {
        return false;
    }
    if (TMaybeOp<TDependentJoinOperator>(op)) {
        found = op;
        return true;
    }
    for (const auto& input : op->Inputs()) {
        if (FindDependentJoin(input, visited, found)) {
            return true;
        }
    }
    return false;
}

} // namespace

std::string_view DependentJoinKindName(EDependentJoinKind kind) {
    switch (kind) {
        case EDependentJoinKind::Inner: return "inner";
        case EDependentJoinKind::Left: return "left";
        case EDependentJoinKind::Semi: return "semi";
        case EDependentJoinKind::Anti: return "anti";
        case EDependentJoinKind::Mark: return "mark";
        case EDependentJoinKind::Single: return "single";
    }
    return "inner";
}

std::expected<TTypePtr, NQumir::TError> ComputeDependentJoinOutputType(
    const TTypePtr& left,
    const TTypePtr& right,
    EDependentJoinKind kind,
    const TExprPtr& predicate,
    const std::string& marker,
    const NKernel::TAnnotationContext& annotation)
{
    auto fields = Fields(left, false);
    switch (kind) {
        case EDependentJoinKind::Inner:
        case EDependentJoinKind::Left:
        case EDependentJoinKind::Single: {
            for (auto& field : Fields(right, kind != EDependentJoinKind::Inner)) {
                fields.push_back(std::move(field));
            }
            break;
        }
        case EDependentJoinKind::Mark: {
            try {
                fields.emplace_back(marker, MarkerType(left, right, predicate, annotation));
            } catch (const NQumir::TError& error) {
                return std::unexpected(error);
            }
            break;
        }
        case EDependentJoinKind::Semi:
        case EDependentJoinKind::Anti:
            break;
    }
    std::unordered_set<std::string> seen;
    for (const auto& [name, _] : fields) {
        if (!seen.insert(name).second) {
            return std::unexpected(NQumir::TError(
                "dependent join output column name '" + name + "' appears on both sides"));
        }
    }
    return std::make_shared<TStructType>(std::move(fields));
}

TDependentJoinOperator::TDependentJoinOperator(
    TOperatorPtr left,
    TOperatorPtr right,
    EDependentJoinKind kind,
    uint32_t scope,
    TExprPtr predicate,
    std::string marker,
    const NKernel::TAnnotationContext& annotation)
    : Inputs_{std::move(left), std::move(right)}
    , Kind_(kind)
    , Scope_(scope)
    , Predicate_(std::move(predicate))
    , Marker_(std::move(marker))
{
    auto leftSchema = Left()->OutputColumns();
    auto rightSchema = Right()->OutputColumns();
    auto output = ComputeDependentJoinOutputType(
        leftSchema, rightSchema, Kind_, Predicate_, Marker_, annotation);
    // The builder checks the output with ComputeDependentJoinOutputType first; an
    // unchecked construction leaves the output type null rather than throwing.
    Type = std::make_shared<TFunctionType>(
        std::vector<TTypePtr>{leftSchema, rightSchema},
        output ? *output : nullptr);
}

std::vector<TColumnReference> TDependentJoinOperator::ColumnReferences() const {
    std::vector<TColumnReference> references;
    AppendColumnReferences(references, Predicate_);
    return references;
}

std::unordered_set<std::string> TDependentJoinOperator::RequiredColumnsForChild(
    size_t childIdx, const std::unordered_set<std::string>&) const
{
    std::unordered_set<std::string> required;
    if (auto&& schema = Inputs_[childIdx]->OutputColumns()) {
        for (const auto& [name, _] : schema->Fields) {
            required.insert(name);
        }
    }
    return required;
}

const std::string TDependentJoinOperator::ToString() const {
    return "(rel dependent-join " + Left()->ToString() + " " + Right()->ToString()
        + " (" + std::string(DependentJoinKindName(Kind_)) + "))";
}

const std::string TOuterRefExpr::ToString() const {
    return "(outer " + std::to_string(Scope) + " " + Column + ")";
}

void PrintOuterRef(TExpr& expr, NCore::TPrinter& printer, NCore::TPrintFrame) {
    auto& ref = static_cast<TOuterRefExpr&>(expr);
    printer.GetOut() << "(outer " << ref.Scope << ' ';
    printer.PrintIdentifier(ref.Column);
    printer.GetOut() << ')';
}

std::expected<void, NQumir::TError> RequireNoDependentJoins(const TOperatorPtr& plan) {
    std::unordered_set<const IOperator*> visited;
    TOperatorPtr found;
    bool hasJoin = FindDependentJoin(plan, visited, found);
    for (const auto& definition : CollectCteDefinitions(plan)) {
        hasJoin = hasJoin || FindDependentJoin(definition->Plan, visited, found);
    }
    if (!hasJoin) {
        return {};
    }
    return std::unexpected(NQumir::TError(found->Location,
        "dependent join is not supported before unnesting"));
}

} // namespace NQdb
