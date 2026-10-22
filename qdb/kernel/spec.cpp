#include <qdb/kernel/spec.h>

#include <qdb/kernel/annotate_type.h>
#include <qdb/exec/runtime_context.h>
#include <qdb/plan/clone_expr.h>
#include <qdb/plan/passes/unbound_vars.h>
#include <qdb/plan/types/nullable.h>

#include <qumir/parser/core/printer.h>

#include <algorithm>
#include <memory>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace NQdb {
namespace NKernel {

namespace {

std::string TypeName(const NQumir::NAst::TTypePtr& type) {
    return type ? type->ToString() : std::string("<null>");
}

void PrintColumn(std::ostream& out, const TKernelColumnRef& column) {
    out << column.Name << "#" << column.Index << ":" << TypeName(column.Type);
}

const NQumir::NAst::TStringLiteralExpr* StringLiteral(
    const NQumir::NAst::TExprPtr& expr)
{
    using namespace NQumir::NAst;
    TExprPtr current = expr;
    while (auto cast = TMaybeNode<TCastExpr>(current)) {
        current = cast.Cast()->Operand;
    }
    auto literal = TMaybeNode<TStringLiteralExpr>(current);
    return literal ? literal.Cast().get() : nullptr;
}

void LowerRegexCalls(
    NQumir::NAst::TExprPtr& expr,
    TRegexRegistry* registry)
{
    using namespace NQumir::NAst;
    if (!expr) {
        return;
    }
    for (auto* child : expr->MutableChildren()) {
        LowerRegexCalls(*child, registry);
    }

    auto call = TMaybeNode<TCallExpr>(expr);
    if (!call) {
        return;
    }
    auto callee = TMaybeNode<TIdentExpr>(call.Cast()->Callee);
    if (!callee || callee.Cast()->Name != "regexp_replace") {
        return;
    }
    if (call.Cast()->Args.size() != 3) {
        throw std::runtime_error("REGEXP_REPLACE expects 3 arguments");
    }
    const auto* pattern = StringLiteral(call.Cast()->Args[1]);
    const auto* replacement = StringLiteral(call.Cast()->Args[2]);
    if (!pattern || !replacement) {
        throw std::runtime_error(
            "REGEXP_REPLACE pattern and replacement must be constant strings");
    }
    if (!registry) {
        throw std::runtime_error("REGEXP_REPLACE has no query regex registry");
    }

    const size_t id = registry->Register(pattern->Value, replacement->Value);
    auto index = std::make_shared<TNumberExpr>(
        call.Cast()->Location, static_cast<int64_t>(id));
    auto handle = std::make_shared<TIndexExpr>(
        call.Cast()->Location,
        std::make_shared<TIdentExpr>(call.Cast()->Location, "__regexes__"),
        std::move(index));
    auto lowered = std::make_shared<TCallExpr>(
        call.Cast()->Location,
        std::make_shared<TIdentExpr>(call.Cast()->Location, "qdb_regexp_replace"),
        std::vector<TExprPtr>{
            std::make_shared<TIdentExpr>(call.Cast()->Location, "__arena__"),
            std::move(handle),
            call.Cast()->Args[0],
        });
    lowered->Type = expr->Type;
    expr = std::move(lowered);
}

bool ContainsRegexCall(const NQumir::NAst::TExprPtr& expr) {
    using namespace NQumir::NAst;
    if (!expr) {
        return false;
    }
    if (auto call = TMaybeNode<TCallExpr>(expr)) {
        if (auto callee = TMaybeNode<TIdentExpr>(call.Cast()->Callee);
            callee && callee.Cast()->Name == "regexp_replace")
        {
            return true;
        }
    }
    return std::ranges::any_of(expr->Children(), ContainsRegexCall);
}

NQumir::NAst::TExprPtr LowerRegexExpression(
    const NQumir::NAst::TExprPtr& expr,
    TRegexRegistry* registry)
{
    if (!ContainsRegexCall(expr)) {
        return expr;
    }
    auto lowered = CloneExpr(expr);
    LowerRegexCalls(lowered, registry);
    return lowered;
}

void RejectRegexCall(
    const NQumir::NAst::TExprPtr& expr,
    std::string_view kernel)
{
    if (ContainsRegexCall(expr)) {
        throw std::runtime_error(
            "REGEXP_REPLACE is not supported directly in " +
            std::string(kernel) +
            " kernels; materialize it in a project first");
    }
}

} // namespace

std::string_view KernelKindName(EOperatorKernelKind kind) {
    switch (kind) {
        case EOperatorKernelKind::NonCompute:
            return "non-compute";
        case EOperatorKernelKind::UnaryStreaming:
            return "unary-streaming";
        case EOperatorKernelKind::UnaryBlocking:
            return "unary-blocking";
        case EOperatorKernelKind::Binary:
            return "binary";
    }
    return "unknown";
}

TOperatorKernelSpec BuildFilterKernelSpec(
    const NQumir::NAst::TStructType& inputType,
    const NQumir::NAst::TExprPtr& predicate,
    std::string entrypointName,
    const TExternalCatalogSnapshot* externalCatalog,
    TRuntimeContext* runtimeContext)
{
    // Make NULL propagation explicit right before compilation (never in logical planning).
    auto expanded = NKernel::ExpandKernelPredicate(
        predicate, inputType, externalCatalog).first;
    expanded = LowerRegexExpression(
        expanded, runtimeContext ? &runtimeContext->Regexes : nullptr);
    const auto refs = FindUnboundVars(expanded);

    std::vector<TKernelColumnRef> referenced;
    for (int32_t i = 0; i < static_cast<int32_t>(inputType.Fields.size()); ++i) {
        const auto& [name, type] = inputType.Fields[i];
        if (refs.contains(name)) {
            referenced.push_back({.Name = name, .Index = i, .Type = type});
        }
    }

    std::ranges::sort(referenced, [](const auto& lhs, const auto& rhs) {
        return lhs.Index < rhs.Index;
    });

    return TOperatorKernelSpec{
        .Kind = EOperatorKernelKind::UnaryStreaming,
        .OperatorName = "filter",
        .InputSchemas = {std::make_shared<NQumir::NAst::TStructType>(inputType.Fields)},
        .OutputSchema = std::make_shared<NQumir::NAst::TStructType>(inputType.Fields),
        .ReferencedColumns = std::move(referenced),
        .Expressions = {expanded},
        .Entrypoints = {
            {.Name = std::move(entrypointName), .Abi = "void(ref TRowSet)"},
        },
        .SourceModules = {"qumirdb"},
    };
}

TOperatorKernelSpec BuildProjectKernelSpec(
    const NQumir::NAst::TStructType& inputType,
    const std::vector<NQumir::NAst::TExprPtr>& computedExprs,
    const std::vector<NQumir::NAst::TTypePtr>& computedTypes,
    std::string entrypointName,
    const TExternalCatalogSnapshot* externalCatalog,
    TRuntimeContext* runtimeContext)
{
    std::vector<std::pair<std::string, NQumir::NAst::TTypePtr>> outputFields;
    outputFields.reserve(computedTypes.size());
    for (size_t i = 0; i < computedTypes.size(); ++i) {
        outputFields.emplace_back("computed_" + std::to_string(i), computedTypes[i]);
    }

    std::vector<NQumir::NAst::TExprPtr> expanded;
    expanded.reserve(computedExprs.size());
    for (const auto& expr : computedExprs) {
        auto kernelExpr = NKernel::ExpandKernelValue(
            expr, inputType, externalCatalog).first;
        expanded.push_back(LowerRegexExpression(
            kernelExpr, runtimeContext ? &runtimeContext->Regexes : nullptr));
    }

    std::unordered_set<std::string> refs;
    for (const auto& expr : expanded) {
        auto exprRefs = FindUnboundVars(expr);
        refs.insert(exprRefs.begin(), exprRefs.end());
    }

    std::vector<TKernelColumnRef> referenced;
    for (int32_t i = 0; i < static_cast<int32_t>(inputType.Fields.size()); ++i) {
        const auto& [name, type] = inputType.Fields[i];
        if (refs.contains(name)) {
            referenced.push_back({.Name = name, .Index = i, .Type = type});
        }
    }

    std::ranges::sort(referenced, [](const auto& lhs, const auto& rhs) {
        return lhs.Index < rhs.Index;
    });

    return TOperatorKernelSpec{
        .Kind = EOperatorKernelKind::UnaryStreaming,
        .OperatorName = "project-compute",
        .InputSchemas = {std::make_shared<NQumir::NAst::TStructType>(inputType.Fields)},
        .OutputSchema = std::make_shared<NQumir::NAst::TStructType>(std::move(outputFields)),
        .ReferencedColumns = std::move(referenced),
        .Expressions = expanded,
        .Entrypoints = {
            {.Name = std::move(entrypointName), .Abi = "void(ref TRowSet, ptr ptr i8)"},
        },
        .SourceModules = {"qumirdb"},
    };
}

TOperatorKernelSpec BuildAggregateKernelSpec(
    const NQumir::NAst::TStructType& inputType,
    const std::vector<std::string>& groupKeys,
    const std::vector<TAggregateSpec>& aggs,
    const TExternalCatalogSnapshot* externalCatalog,
    std::vector<int64_t> emptyInputGroups)
{
    auto columnRef = [&](const std::string& name) -> TKernelColumnRef {
        for (int32_t i = 0; i < static_cast<int32_t>(inputType.Fields.size()); ++i) {
            const auto& [fieldName, type] = inputType.Fields[i];
            if (fieldName == name) {
                return {.Name = name, .Index = i, .Type = type};
            }
        }
        return {.Name = name};
    };

    std::vector<TKernelColumnRef> groupColumns;
    groupColumns.reserve(groupKeys.size());
    for (const auto& name : groupKeys) {
        groupColumns.push_back(columnRef(name));
    }

    std::vector<TKernelAggregateSpec> aggregateSpecs;
    aggregateSpecs.reserve(aggs.size());
    for (const auto& agg : aggs) {
        RejectRegexCall(agg.Arg, "aggregate");
        // Expand NULL propagation in the argument expression before compilation.
        auto argExpr = agg.Arg
            ? NKernel::ExpandKernelValue(agg.Arg, inputType, externalCatalog).first
            : agg.Arg;
        TKernelAggregateSpec spec{
            .Name = agg.Name,
            .Func = agg.Func,
            .ArgExpr = argExpr,
            .EmptyIsZero = agg.EmptyIsZero,
        };
        if (agg.Arg) {
            auto ident = NQumir::NAst::TMaybeNode<NQumir::NAst::TIdentExpr>(agg.Arg);
            if (ident) {
                spec.HasArg = true;
                spec.Arg = columnRef(ident.Cast()->Name);
            }
        }
        aggregateSpecs.push_back(std::move(spec));
    }

    std::vector<TKernelColumnRef> referenced = groupColumns;
    for (const auto& agg : aggregateSpecs) {
        if (agg.HasArg) {
            referenced.push_back(agg.Arg);
        }
    }
    std::ranges::sort(referenced, [](const auto& lhs, const auto& rhs) {
        return lhs.Index < rhs.Index;
    });
    referenced.erase(std::unique(
        referenced.begin(), referenced.end(),
        [](const auto& lhs, const auto& rhs) { return lhs.Index == rhs.Index; }),
        referenced.end());

    return TOperatorKernelSpec{
        .Kind = EOperatorKernelKind::UnaryBlocking,
        .OperatorName = "aggregate",
        .InputSchemas = {std::make_shared<NQumir::NAst::TStructType>(inputType.Fields)},
        .OutputSchema = ComputeAggregateOutputType(
            std::make_shared<NQumir::NAst::TStructType>(inputType.Fields),
            groupKeys, aggs, false, !emptyInputGroups.empty()),
        .ReferencedColumns = std::move(referenced),
        .Keys = {
            {.Name = "group-keys", .Columns = std::move(groupColumns)},
        },
        .Aggregates = std::move(aggregateSpecs),
        .Entrypoints = {
            {.Name = "agg_dispatch", .Abi = "i64(ptr HashTable, ptr TRowSet, i64, i64)"},
            {.Name = "agg_finish_rowset", .Abi = "i64(ptr HashTable, ptr TRowSet)"},
        },
        .SourceModules = {"qumirdb"},
        .EmptyInputGroups = std::move(emptyInputGroups),
    };
}

TOperatorKernelSpec BuildSortKernelSpec(
    const NQumir::NAst::TStructType& inputType,
    const std::vector<TSortKey>& keys,
    std::string operatorName)
{
    auto columnRef = [&](const std::string& name) -> TKernelColumnRef {
        for (int32_t i = 0; i < static_cast<int32_t>(inputType.Fields.size()); ++i) {
            const auto& [fieldName, type] = inputType.Fields[i];
            if (fieldName == name) {
                return {.Name = name, .Index = i, .Type = type};
            }
        }
        throw std::runtime_error(operatorName + " column not found: " + name);
    };

    std::vector<TKernelSortKeySpec> sortKeys;
    sortKeys.reserve(keys.size());
    std::vector<TKernelColumnRef> referenced;
    referenced.reserve(keys.size());
    for (const auto& key : keys) {
        auto ref = columnRef(key.Column);
        referenced.push_back(ref);
        sortKeys.push_back({
            .Column = std::move(ref),
            .Direction = key.Direction,
            .Nulls = key.Nulls,
        });
    }

    return TOperatorKernelSpec{
        .Kind = EOperatorKernelKind::UnaryBlocking,
        .OperatorName = std::move(operatorName),
        .InputSchemas = {std::make_shared<NQumir::NAst::TStructType>(inputType.Fields)},
        .OutputSchema = std::make_shared<NQumir::NAst::TStructType>(inputType.Fields),
        .ReferencedColumns = std::move(referenced),
        .SortKeys = std::move(sortKeys),
        .SourceModules = {"qumirdb"},
    };
}

TOperatorKernelSpec BuildJoinKernelSpec(
    const NQumir::NAst::TStructType& leftType,
    const NQumir::NAst::TStructType& rightType,
    const std::vector<TJoinKey>& keys,
    EJoinType type,
    const NQumir::NAst::TExprPtr& residualPredicate,
    const std::string& markerName)
{
    RejectRegexCall(residualPredicate, "join");
    auto columnRef = [](const NQumir::NAst::TStructType& inputType,
        const std::string& name) -> TKernelColumnRef
    {
        for (int32_t i = 0; i < static_cast<int32_t>(inputType.Fields.size()); ++i) {
            const auto& [fieldName, fieldType] = inputType.Fields[i];
            if (fieldName == name) {
                return {.Name = name, .Index = i, .Type = fieldType};
            }
        }
        throw std::runtime_error("join column not found: " + name);
    };

    std::vector<TKernelJoinKeySpec> joinKeys;
    joinKeys.reserve(keys.size());
    for (const auto& key : keys) {
        joinKeys.push_back({
            .Left = columnRef(leftType, key.Left),
            .Right = columnRef(rightType, key.Right),
            .NullsEqual = key.NullsEqual,
        });
    }

    std::vector<NQumir::NAst::TExprPtr> expressions;
    if (residualPredicate) {
        expressions.push_back(residualPredicate);
    }

    NQumir::NAst::TTypePtr outputType;
    if (type == EJoinType::LeftMark) {
        if (markerName.empty()) {
            throw std::runtime_error("join kernel spec: left_mark needs a marker column name");
        }
        // Without a nullable key column the marker is never NULL.
        const bool nullableMarker = std::ranges::any_of(joinKeys, [](const auto& key) {
            return IsNullableType(key.Left.Type) || IsNullableType(key.Right.Type);
        });
        NQumir::NAst::TTypePtr markerType = std::make_shared<NQumir::NAst::TBoolType>();
        if (nullableMarker) {
            markerType = std::make_shared<TNullable>(std::move(markerType));
        }
        auto fields = leftType.Fields;
        fields.emplace_back(markerName, std::move(markerType));
        outputType = std::make_shared<NQumir::NAst::TStructType>(std::move(fields));
    } else {
        auto output = ComputeJoinOutputType(
            std::make_shared<NQumir::NAst::TStructType>(leftType.Fields),
            std::make_shared<NQumir::NAst::TStructType>(rightType.Fields),
            type);
        if (!output) {
            throw std::runtime_error("join kernel spec: " + output.error().ToString());
        }
        outputType = std::move(*output);
    }

    return TOperatorKernelSpec{
        .Kind = EOperatorKernelKind::Binary,
        .OperatorName = "join",
        .InputSchemas = {
            std::make_shared<NQumir::NAst::TStructType>(leftType.Fields),
            std::make_shared<NQumir::NAst::TStructType>(rightType.Fields),
        },
        .OutputSchema = std::move(outputType),
        .Expressions = std::move(expressions),
        .JoinKeys = std::move(joinKeys),
        .JoinType = type,
        .Entrypoints = {
            {.Name = "jt_dispatch", .Abi = "bool(ptr HashTable, ptr HashTable, ptr TRowSet, i64, ptr PairBuffer, ptr TRowSet, ptr TRowSet, ptr i8, ptr i64, ptr i64, i64, i64)"},
            {.Name = "jt_materialize", .Abi = "i64(ptr PairBuffer, ptr TRowSet, ptr TRowSet, ptr TRowSet, ptr TRowSet, i64, i64, ptr TRowSet)"},
        },
        .SourceModules = {"qumirdb"},
    };
}

TOperatorKernelSpec BuildCrossJoinKernelSpec(
    const NQumir::NAst::TStructType& leftType,
    const NQumir::NAst::TStructType& rightType,
    EJoinType type,
    const NQumir::NAst::TExprPtr& residualPredicate)
{
    RejectRegexCall(residualPredicate, "join");
    auto outputType = ComputeJoinOutputType(
        std::make_shared<NQumir::NAst::TStructType>(leftType.Fields),
        std::make_shared<NQumir::NAst::TStructType>(rightType.Fields),
        type);
    if (!outputType) {
        throw std::runtime_error(
            "cross join kernel spec: " + outputType.error().ToString());
    }

    std::vector<NQumir::NAst::TExprPtr> expressions;
    if (residualPredicate) {
        expressions.push_back(residualPredicate);
    }

    return TOperatorKernelSpec{
        .Kind = EOperatorKernelKind::Binary,
        .OperatorName = "cross-join",
        .InputSchemas = {
            std::make_shared<NQumir::NAst::TStructType>(leftType.Fields),
            std::make_shared<NQumir::NAst::TStructType>(rightType.Fields),
        },
        .OutputSchema = std::move(*outputType),
        .Expressions = std::move(expressions),
        .JoinType = type,
        .Entrypoints = {
            {.Name = "xj_dispatch", .Abi = "bool(ptr TRowSet, i64, ptr TRowSet, i64, ptr u8, ptr PairBuffer, ptr i8, i64)"},
            {.Name = "jt_materialize", .Abi = "i64(ptr PairBuffer, ptr TRowSet, ptr TRowSet, ptr TRowSet, ptr TRowSet, i64, i64, ptr TRowSet)"},
        },
        .SourceModules = {"qumirdb"},
    };
}

void PrintKernelSpec(std::ostream& out, const TOperatorKernelSpec& spec) {
    out << "kernel-spec " << spec.OperatorName << "\n";
    out << "  kind: " << KernelKindName(spec.Kind) << "\n";

    out << "  inputs:";
    if (spec.InputSchemas.empty()) {
        out << " []\n";
    } else {
        out << "\n";
        for (size_t i = 0; i < spec.InputSchemas.size(); ++i) {
            out << "    [" << i << "] " << TypeName(spec.InputSchemas[i]) << "\n";
        }
    }

    out << "  output: " << TypeName(spec.OutputSchema) << "\n";
    if (!spec.EmptyInputGroups.empty()) {
        out << "  global-group: seeded\n";
    }

    out << "  referenced:";
    if (spec.ReferencedColumns.empty()) {
        out << " []\n";
    } else {
        out << "\n";
        for (const auto& column : spec.ReferencedColumns) {
            out << "    ";
            PrintColumn(out, column);
            out << "\n";
        }
    }

    out << "  expressions:";
    if (spec.Expressions.empty()) {
        out << " []\n";
    } else {
        out << "\n";
        for (size_t i = 0; i < spec.Expressions.size(); ++i) {
            out << "    [" << i << "] ";
            if (spec.Expressions[i]) {
                out << NQumir::NAst::NCore::PrintAst(
                    spec.Expressions[i],
                    NQumir::NAst::NCore::TPrintOptions{.Pretty = false});
            } else {
                out << "<null>";
            }
            out << "\n";
        }
    }

    out << "  keys:";
    if (spec.Keys.empty()) {
        out << " []\n";
    } else {
        out << "\n";
        for (const auto& key : spec.Keys) {
            out << "    " << key.Name << ":";
            if (key.Columns.empty()) {
                out << " []";
            } else {
                for (const auto& column : key.Columns) {
                    out << " ";
                    PrintColumn(out, column);
                }
            }
            out << "\n";
        }
    }

    out << "  aggregates:";
    if (spec.Aggregates.empty()) {
        out << " []\n";
    } else {
        out << "\n";
        for (const auto& agg : spec.Aggregates) {
            out << "    " << agg.Name << ": " << agg.Func << "(";
            if (agg.HasArg) {
                PrintColumn(out, agg.Arg);
            } else {
                out << "*";
            }
            out << ")\n";
        }
    }

    out << "  sort-keys:";
    if (spec.SortKeys.empty()) {
        out << " []\n";
    } else {
        out << "\n";
        for (const auto& key : spec.SortKeys) {
            out << "    ";
            PrintColumn(out, key.Column);
            out << " " << SortDirectionName(key.Direction)
                << " nulls " << SortNullsName(key.Nulls) << "\n";
        }
    }

    out << "  join-keys:";
    if (spec.JoinKeys.empty()) {
        out << " []\n";
    } else {
        out << "\n";
        for (const auto& key : spec.JoinKeys) {
            out << "    ";
            PrintColumn(out, key.Left);
            out << (key.NullsEqual ? " IS NOT DISTINCT FROM " : " = ");
            PrintColumn(out, key.Right);
            out << "\n";
        }
    }

    if (spec.Kind == EOperatorKernelKind::Binary && spec.OperatorName == "join") {
        out << "  join-type: " << JoinTypeName(spec.JoinType) << "\n";
    }

    out << "  entrypoints:";
    if (spec.Entrypoints.empty()) {
        out << " []\n";
    } else {
        out << "\n";
        for (const auto& entry : spec.Entrypoints) {
            out << "    " << entry.Name << ": " << entry.Abi << "\n";
        }
    }

    out << "  source-modules:";
    if (spec.SourceModules.empty()) {
        out << " []\n";
    } else {
        out << "\n";
        for (const auto& module : spec.SourceModules) {
            out << "    " << module << "\n";
        }
    }
}

} // namespace NKernel
} // namespace NQdb
