#include <qdb/plan/ops/window.h>

#include <qdb/plan/passes/unbound_vars.h>
#include <qdb/plan/types/nullable.h>
#include <qdb/plan/types/decimal.h>

#include <qumir/parser/ast.h>
#include <qumir/parser/core/printer.h>
#include <qumir/parser/type.h>

#include <algorithm>

namespace NQdb {

using namespace NQumir::NAst;

namespace {

// Ranking functions produce a non-null i64; aggregate-style functions mirror the
// aggregate reducers (integer sum/max widen to i64, avg is f64 unless decimal),
// carrying the argument's nullability.
TTypePtr WindowResultType(const std::string& func, const TTypePtr& argType) {
    if (func == "rank" || func == "row_number" || func == "dense_rank") {
        return std::make_shared<TIntegerType>(); // i64, never null
    }
    if (!argType) {
        return argType;
    }
    const bool nullable = IsNullableType(argType);
    auto wrap = [&](TTypePtr type) -> TTypePtr {
        return nullable ? std::make_shared<TNullable>(std::move(type)) : type;
    };
    if (func == "avg") {
        return ComputeWindowAvgResultType(argType);
    }
    // sum / max: i64 state for every integer argument, else keep the arg type.
    auto inner = nullable ? UnwrapNullableType(argType) : argType;
    if (inner && inner->TypeName() == TIntegerType::TypeId) {
        return wrap(std::make_shared<TIntegerType>());
    }
    return argType;
}

TTypePtr FuncArgType(const TStructType* inputStruct, const TWindowFunc& func) {
    if (!func.Arg) {
        return nullptr;
    }
    if (auto ident = TMaybeNode<TIdentExpr>(func.Arg)) {
        return FieldType(inputStruct, ident.Cast()->Name);
    }
    return nullptr;
}

void AppendBound(std::string& s, const char* tag, const TFrameBound& bound) {
    s += " (";
    s += tag;
    s += " ";
    s += FrameBoundKindName(bound.Kind);
    if (bound.Offset) {
        s += " " + NQumir::NAst::NCore::PrintAst(bound.Offset);
    }
    s += ")";
}

} // namespace

std::string_view WindowFrameModeName(EWindowFrameMode mode) {
    switch (mode) {
        case EWindowFrameMode::Rows:
            return "rows";
        case EWindowFrameMode::Range:
            return "range";
    }
    return "range";
}

std::string_view FrameBoundKindName(EFrameBoundKind kind) {
    switch (kind) {
        case EFrameBoundKind::UnboundedPreceding:
            return "unbounded-preceding";
        case EFrameBoundKind::Preceding:
            return "preceding";
        case EFrameBoundKind::CurrentRow:
            return "current-row";
        case EFrameBoundKind::Following:
            return "following";
        case EFrameBoundKind::UnboundedFollowing:
            return "unbounded-following";
    }
    return "current-row";
}

TTypePtr ComputeWindowAvgResultType(const TTypePtr& argType) {
    if (!argType) {
        return argType;
    }
    const bool nullable = IsNullableType(argType);
    auto wrap = [&](TTypePtr type) -> TTypePtr {
        return nullable ? std::make_shared<TNullable>(std::move(type)) : type;
    };
    if (auto spec = DecimalSpecOfValueType(UnwrapNullableType(argType))) {
        const int32_t intDigits = spec->Precision - spec->Scale;
        const int32_t scale = std::min(
            spec->Scale + WindowAvgExtraScale,
            MaxDecimalPrecision - intDigits);
        const int32_t precision = intDigits + scale;
        return wrap(std::make_shared<TDecimal>(precision, scale));
    }
    return wrap(std::make_shared<TFloatType>());
}

TTypePtr ComputeWindowOutputType(
    const TTypePtr& inputSchema,
    const std::vector<TWindowFunc>& functions)
{
    auto* inputStruct = static_cast<TStructType*>(inputSchema.get());
    std::vector<std::pair<std::string, TTypePtr>> outFields;
    if (inputStruct) {
        outFields = inputStruct->Fields; // pass every input column through
    }
    for (const auto& func : functions) {
        outFields.emplace_back(func.Name,
            WindowResultType(func.Func, FuncArgType(inputStruct, func)));
    }
    return std::make_shared<TStructType>(std::move(outFields));
}

TWindowOperator::TWindowOperator(TOperatorPtr input,
    std::vector<std::string> partitionKeys,
    std::vector<TSortKey> orderKeys,
    std::optional<TWindowFrame> frame,
    std::vector<TWindowFunc> functions)
    : Input_(std::move(input))
    , PartitionKeys_(std::move(partitionKeys))
    , OrderKeys_(std::move(orderKeys))
    , Frame_(std::move(frame))
    , Functions_(std::move(functions))
{
    auto inputSchema = Input_->OutputColumns();
    Type = std::make_shared<TFunctionType>(
        std::vector<TTypePtr>{inputSchema},
        ComputeWindowOutputType(inputSchema, Functions_));
}

std::vector<TColumnReference> TWindowOperator::ColumnReferences() const {
    std::vector<TColumnReference> references;
    for (const auto& func : Functions_) {
        AppendColumnReferences(references, func.Arg);
    }
    for (const auto& key : PartitionKeys_) {
        references.push_back({.Name = key});
    }
    for (const auto& key : OrderKeys_) {
        references.push_back({.Name = key.Column});
    }
    return references;
}

std::unordered_set<std::string> TWindowOperator::RequiredColumnsForChild(
    size_t /*childIdx*/, const std::unordered_set<std::string>& needed) const
{
    auto&& inputStruct = Input_->OutputColumns();
    std::unordered_set<std::string> inputCols;
    if (inputStruct) {
        for (const auto& [name, _] : inputStruct->Fields) {
            inputCols.insert(name);
        }
    }

    std::unordered_set<std::string> required;
    // Forward the parent's needs, but only the ones the child actually owns:
    // the window's own output columns are not produced below it.
    for (const auto& col : needed) {
        if (inputCols.contains(col)) {
            required.insert(col);
        }
    }
    // Always keep this node's own references (partition / order / func args).
    auto own = ComputeReferencedColumns();
    required.insert(own.begin(), own.end());
    return required;
}

const std::string TWindowOperator::ToString() const {
    std::string s = "(rel window " + Input_->ToString();
    if (!PartitionKeys_.empty()) {
        s += " (partition";
        for (const auto& key : PartitionKeys_) {
            s += " " + key;
        }
        s += ")";
    }
    if (!OrderKeys_.empty()) {
        s += " (order";
        for (const auto& key : OrderKeys_) {
            s += " (" + key.Column + " ";
            s += SortDirectionName(key.Direction);
            s += " ";
            s += SortNullsName(key.Nulls);
            s += ")";
        }
        s += ")";
    }
    if (Frame_) {
        s += " (frame ";
        s += WindowFrameModeName(Frame_->Mode);
        AppendBound(s, "start", Frame_->Start);
        AppendBound(s, "end", Frame_->End);
        s += ")";
    }
    for (const auto& func : Functions_) {
        s += " (fn " + func.Name + " " + func.Func;
        if (func.Arg) {
            s += " " + NQumir::NAst::NCore::PrintAst(func.Arg);
        }
        s += ")";
    }
    return s + ")";
}

} // namespace NQdb
