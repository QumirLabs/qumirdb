#include <qdb/exec/planner_helpers.h>
#include <qdb/exec/runtime_context.h>
#include <qdb/exec/filter_exec.h>
#include <qdb/exec/project_exec.h>
#include <qdb/exec/sort_key_ops.h>
#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/limit.h>
#include <qdb/plan/ops/late_materialize.h>
#include <qdb/kernel/compiler.h>
#include <qdb/kernel/annotate_type.h>
#include <qdb/kernel/spec.h>
#include <qdb/plan/types/decimal.h>
#include <qdb/plan/types/nullable.h>

#include <qumir/parser/type.h>

#include <algorithm>
#include <bit>
#include <limits>
#include <ostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace NQdb {
namespace {

// Byte width of the kernel output buffer per row for a computed column.
// For TStringType (string computed columns), the JIT kernel writes one
// TStringView struct (16 bytes) per row; the executor post-converts those.
size_t ProjectColumnWidth(const NQumir::NAst::TTypePtr& type) {
    using namespace NQumir::NAst;
    if (IsDecimalType(type)) {
        return 16;
    }
    auto inner = UnwrapNamedType(UnwrapNullableType(type));
    if (auto integer = TMaybeType<TIntegerType>(inner)) {
        return static_cast<size_t>(integer.Cast()->BitWidth() / 8);
    }
    if (TMaybeType<TFloatType>(inner)) {
        return 8;
    }
    if (TMaybeType<TBoolType>(inner)) {
        return 1;
    }
    if (TMaybeType<TStringType>(inner)) {
        return 16; // sizeof(TStringView): ptr(8) + size(8)
    }
    throw std::runtime_error(
        "project: unsupported computed column type " +
        (type ? type->ToString() : std::string("<null>")));
}

// Returns the kernel-level type the project kernel should write for a computed
// column. StringView is resolved from the qumirdb source module before lowering.
NQumir::NAst::TTypePtr ProjectJitType(const NQumir::NAst::TTypePtr& outType) {
    using namespace NQumir::NAst;
    if (IsDecimalType(outType)) {
        return DecimalStorageTypeFor(outType);
    }
    if (TMaybeType<TStringType>(UnwrapNamedType(UnwrapNullableType(outType)))) {
        TTypePtr sv = std::make_shared<TNamedType>("StringView", nullptr);
        // Keep nullability so the project kernel emits the Nullable[StringView] split
        // (Value -> data, Valid -> mask) instead of casting the struct to StringView.
        return IsNullableType(outType) ? std::make_shared<TNullable>(std::move(sv)) : sv;
    }
    return outType;
}

bool IsRadixSortableType(const NQumir::NAst::TTypePtr& type) {
    using namespace NQumir::NAst;
    if (DecimalSpecOfValueType(UnwrapNullableType(type))) {
        return true; // 128-bit decimal: two-word radix (sort_binint_rowids)
    }
    auto inner = UnwrapNamedType(UnwrapNullableType(type));
    return static_cast<bool>(TMaybeType<TIntegerType>(inner)) ||
        static_cast<bool>(TMaybeType<TFloatType>(inner)) ||
        static_cast<bool>(TMaybeType<TBoolType>(inner)) ||
        static_cast<bool>(TMaybeType<TStringType>(inner));
}

bool SortNullsFirst(const NKernel::TKernelSortKeySpec& key) {
    if (key.Nulls != ESortNulls::Default) {
        return key.Nulls == ESortNulls::First;
    }
    return key.Direction == ESortDirection::Desc;
}

struct TSortKernelInputs {
    std::vector<TSortColumnRef> KeyColumns;
    std::vector<TSortRadixKeyInput> RadixKeys;
    bool AllKeysRadixSortable = true;
};

TSortKernelInputs BuildSortKernelInputs(const NKernel::TOperatorKernelSpec& spec) {
    TSortKernelInputs inputs;
    inputs.KeyColumns.reserve(spec.SortKeys.size());
    inputs.RadixKeys.reserve(spec.SortKeys.size());
    for (const auto& key : spec.SortKeys) {
        inputs.AllKeysRadixSortable =
            inputs.AllKeysRadixSortable && IsRadixSortableType(key.Column.Type);
        inputs.RadixKeys.push_back({
            .ColumnIndex = key.Column.Index,
            .Type = key.Column.Type,
            .Desc = key.Direction == ESortDirection::Desc,
            .NullsFirst = SortNullsFirst(key),
        });
        inputs.KeyColumns.push_back({
            .Index = key.Column.Index,
            .Type = key.Column.Type,
        });
    }
    return inputs;
}

void PrintKernelSpec(std::ostream* out, const NKernel::TOperatorKernelSpec& spec) {
    if (!out) {
        return;
    }
    *out << "\n========== KERNEL SPEC ==========\n";
    NKernel::PrintKernelSpec(*out, spec);
    *out << "=================================\n";
}

} // namespace

std::optional<uint64_t> EstimateAggregateGroupCount(
    const TAggregateOperator& aggregate)
{
    const auto inputStats = aggregate.Input()->Stats_;
    if (!inputStats || aggregate.GroupKeys().empty()) {
        return std::nullopt;
    }

    uint64_t groups = 1;
    for (const auto& key : aggregate.GroupKeys()) {
        auto it = inputStats->ColumnStats.find(key);
        if (it == inputStats->ColumnStats.end() ||
            !it->second || !it->second->Ndv)
        {
            return std::nullopt;
        }
        const uint64_t ndv = *it->second->Ndv;
        if (ndv == 0) {
            return std::nullopt;
        }
        if (groups > inputStats->RowCount / ndv) {
            groups = inputStats->RowCount;
        } else {
            groups *= ndv;
        }
    }
    return std::min(groups, inputStats->RowCount);
}

std::shared_ptr<TAggregateOperator> BuildAggregateCombineOperator(
    const TAggregateOperator& aggregate,
    std::vector<std::string> groupKeys)
{
    std::vector<TAggregateSpec> combineAggs;
    combineAggs.reserve(aggregate.Aggs().size());
    for (const auto& agg : aggregate.Aggs()) {
        std::string func = agg.Func == "count" ? "sum" : agg.Func;
        combineAggs.push_back(TAggregateSpec{
            .Name = agg.Name,
            .Func = std::move(func),
            .Arg = std::make_shared<NQumir::NAst::TIdentExpr>(
                NQumir::TLocation{}, agg.Name),
        });
    }
    return std::make_shared<TAggregateOperator>(
        aggregate.Input(), std::move(groupKeys), std::move(combineAggs));
}

int64_t EstimateAggregateInitialCapacity(
    const TAggregateOperator& aggregate,
    size_t partitionCount)
{
    constexpr uint64_t DefaultCapacity = 8;
    const auto estimatedGroups = EstimateAggregateGroupCount(aggregate);
    if (!estimatedGroups || *estimatedGroups == 0) {
        return DefaultCapacity;
    }
    const uint64_t groups = *estimatedGroups;

    const uint64_t partitions = std::max<uint64_t>(partitionCount, 1);
    const uint64_t groupsPerPartition =
        groups / partitions + (groups % partitions != 0);
    // The table grows before the next insertion would cross 87.5%.
    uint64_t required = groupsPerPartition >
            (std::numeric_limits<uint64_t>::max() - 6) / 8
        ? std::numeric_limits<uint64_t>::max()
        : (groupsPerPartition * 8 + 6) / 7;

    // NDV may be a HyperLogLog estimate (TStats::NdvIsExact) and a filter
    // below the aggregate can cut the real group count by orders of magnitude
    // Cap the up-front allocation so a wrong guess costs a few doublings
    // instead of gigabytes: aht_init allocates keys and group_keys of
    // key_size each, two i64 metadata arrays and one i64 buffer per aggregate
    constexpr uint64_t ByteBudget = 256ull << 20;
    constexpr uint64_t AssumedKeyBytes = 16;
    const uint64_t slotBytes = 16 +
        8 * aggregate.Aggs().size() +
        2 * AssumedKeyBytes * aggregate.GroupKeys().size();
    required = std::min(
        required, std::max(ByteBudget / slotBytes, DefaultCapacity));

    return static_cast<int64_t>(
        std::bit_ceil(std::max(required, DefaultCapacity)));
}

NQumir::NAst::TTypePtr BuildSourceRuntimeType(TSourceOperator& src)
{
    if (auto requiredColumns = src.RequiredColumns()) {
        std::unordered_set<std::string> cols;
        for (auto& [name, _] : requiredColumns->Fields) {
            auto dot = name.rfind('.');
            cols.insert(dot != std::string::npos ? name.substr(dot + 1) : name);
        }
        src.GetSource().RestrictColumns(cols);
    }

    auto&& qualSt = src.OutputColumns();
    std::unordered_map<std::string, std::pair<std::string, NQumir::NAst::TTypePtr>> bareToQual;
    if (qualSt) {
        for (const auto& [qname, ftype] : qualSt->Fields) {
            auto dot = qname.rfind('.');
            auto bare = dot != std::string::npos ? qname.substr(dot + 1) : qname;
            bareToQual.try_emplace(bare, qname, ftype);
        }
    }

    std::vector<std::pair<std::string, NQumir::NAst::TTypePtr>> fields;
    for (const auto& col : src.GetSource().Schema().Columns) {
        auto bare = std::string(col.Name);
        auto it = bareToQual.find(bare);
        if (it != bareToQual.end()) {
            fields.emplace_back(it->second.first, it->second.second);
        } else {
            fields.emplace_back(bare, col.Type);
        }
    }
    return std::make_shared<NQumir::NAst::TStructType>(std::move(fields));
}

EJoinBuildSide ChooseJoinBuildSide(const TJoinOperator& join) {
    // Publishing requires the build side to finish before probing.
    if (const auto& runtimeFilter = join.RuntimeFilter()) {
        return runtimeFilter->BuildSide == EJoinFilterSide::Left
            ? EJoinBuildSide::Left
            : EJoinBuildSide::Right;
    }
    const auto type = join.JoinType();
    // A residual predicate needs right rows, not just key membership.
    const bool semiAnti =
        type == EJoinType::LeftSemi || type == EJoinType::LeftAnti;
    const bool streamable = semiAnti && !join.Filter();
    if ((type != EJoinType::Inner && !streamable) || join.Keys().empty()) {
        return EJoinBuildSide::Auto;
    }
    auto leftStats = join.Left()->Stats_;
    auto rightStats = join.Right()->Stats_;
    if (!leftStats || !rightStats
        || leftStats->RowCount == 0 || rightStats->RowCount == 0) {
        return EJoinBuildSide::Auto;
    }
    const double l = static_cast<double>(leftStats->RowCount);
    const double r = static_cast<double>(rightStats->RowCount);
    if (r * JoinAsymmetryRatio <= l) {
        return EJoinBuildSide::Right;
    }
    if (!semiAnti && l * JoinAsymmetryRatio <= r) {
        return EJoinBuildSide::Left;
    }
    return EJoinBuildSide::Auto;
}

std::string_view JoinBuildSideName(EJoinBuildSide side) {
    switch (side) {
        case EJoinBuildSide::Left: return "left";
        case EJoinBuildSide::Right: return "right";
        case EJoinBuildSide::Auto: return "auto";
    }
    return "auto";
}

TUnaryRuntimeProcess BuildFilterRuntimeProcess(
    TFilterOperator& filter,
    const NQumir::NAst::TTypePtr& inputType,
    TKernelCompilerOptions options)
{
    if (!options.RuntimeContext) {
        options.RuntimeContext = std::make_shared<TRuntimeContext>();
    }
    auto* inputStruct = static_cast<NQumir::NAst::TStructType*>(inputType.get());
    if (!inputStruct) {
        throw std::runtime_error("filter input must have TStructType");
    }

    auto spec = NKernel::BuildFilterKernelSpec(
        *inputStruct, filter.Predicate(), "<kernel>", options.ExternalCatalog.get(),
        options.RuntimeContext.get());
    TKernelCompiler compiler(std::move(options));
    auto dispatch = compiler.CompileFilter(spec);

    auto columnPlan = BuildFilterColumnPlan(filter, inputType);
    if (columnPlan.KeptInputColumns) {
        return {
            .Process = MakeFilterSelectProcess(
                std::move(dispatch), std::move(*columnPlan.KeptInputColumns)),
            .OutputType = std::move(columnPlan.OutputType),
        };
    }
    return {
        .Process = MakeFilterProcess(std::move(dispatch)),
        .OutputType = std::move(columnPlan.OutputType),
    };
}

TFilterColumnPlan BuildFilterColumnPlan(
    const TFilterOperator& filter,
    const NQumir::NAst::TTypePtr& inputType)
{
    auto* inputStruct = static_cast<NQumir::NAst::TStructType*>(
        inputType.get());
    if (!inputStruct) {
        throw std::runtime_error("filter input must have TStructType");
    }

    // Emit exactly the pruned output columns; a schema identical to the input
    // (by name and order) keeps the zero-copy fast path.
    NQumir::NAst::TTypePtr outputType;
    const NQumir::NAst::TStructType* outputStruct = nullptr;
    if (auto* fun = static_cast<NQumir::NAst::TFunctionType*>(filter.Type.get())) {
        outputType = fun->ReturnType;
        outputStruct = static_cast<NQumir::NAst::TStructType*>(outputType.get());
    }
    // A stale/decorrelated ReturnType can list columns absent from the actual
    // input; only narrow when it is a proper subset, else pass through in full.
    if (outputStruct && !outputStruct->Fields.empty()) {
        std::vector<int32_t> keptIndices;
        keptIndices.reserve(outputStruct->Fields.size());
        bool subsetOfInput = true;
        bool identity =
            outputStruct->Fields.size() == inputStruct->Fields.size();
        for (size_t o = 0; o < outputStruct->Fields.size(); ++o) {
            const auto& name = outputStruct->Fields[o].first;
            int32_t index = -1;
            for (size_t i = 0; i < inputStruct->Fields.size(); ++i) {
                if (inputStruct->Fields[i].first == name) {
                    index = static_cast<int32_t>(i);
                    break;
                }
            }
            if (index < 0) {
                subsetOfInput = false;
                break;
            }
            if (static_cast<size_t>(index) != o) {
                identity = false;
            }
            keptIndices.push_back(index);
        }
        if (subsetOfInput && !identity) {
            return {
                .OutputType = std::move(outputType),
                .KeptInputColumns = std::move(keptIndices),
            };
        }
    }

    return {
        .OutputType = inputType,
    };
}

TProjectColumnPlan BuildProjectColumnPlan(
    TProjectOperator& project,
    const NQumir::NAst::TStructType& inputStruct)
{
    TProjectColumnPlan plan;
    std::vector<std::pair<std::string, NQumir::NAst::TTypePtr>> outFields;
    auto&& annotatedOutput = project.OutputColumns();
    size_t annotatedFieldIndex = 0;
    for (size_t projectionIndex = 0;
         projectionIndex < project.Projections().size();
         ++projectionIndex)
    {
        const auto& projection = project.Projections()[projectionIndex];
        if (auto identNode = NQumir::NAst::TMaybeNode<NQumir::NAst::TIdentExpr>(
                projection.Expression)) {
            const std::string& exprName = identNode.Cast()->Name;
            auto it = std::find_if(
                inputStruct.Fields.begin(), inputStruct.Fields.end(),
                [&](const auto& field) { return field.first == exprName; });
            if (it == inputStruct.Fields.end()) {
                throw std::runtime_error("project column not found: " + exprName);
            }
            plan.Columns.push_back({
                .Computed = false,
                .Index = static_cast<int32_t>(
                    std::distance(inputStruct.Fields.begin(), it)),
            });
            const std::string& outputName = annotatedOutput
                    && annotatedFieldIndex < annotatedOutput->Fields.size()
                ? annotatedOutput->Fields[annotatedFieldIndex].first
                : projection.Name;
            outFields.emplace_back(outputName, it->second);
            ++annotatedFieldIndex;
        } else {
            NQumir::NAst::TTypePtr outType;
            if (projection.Expression->Type
                && !IsNullableType(projection.Expression->Type)
                && NQumir::NAst::TMaybeType<NQumir::NAst::TStructType>(
                    projection.Expression->Type))
            {
                outType = projection.Expression->Type;
            } else if (annotatedOutput
                && annotatedFieldIndex < annotatedOutput->Fields.size())
            {
                outType = annotatedOutput->Fields[annotatedFieldIndex].second;
            }
            if (!outType) {
                outType = NKernel::AnnotateExprType(
                    projection.Expression, inputStruct);
            }
            using namespace NQumir::NAst;
            auto structType = TMaybeType<TStructType>(
                !IsNullableType(outType)
                    ? UnwrapNamedType(outType)
                    : TTypePtr{});
            if (!structType && IsNullableType(outType)
                && TMaybeType<TStructType>(
                    UnwrapNamedType(UnwrapNullableType(outType))))
            {
                throw NQumir::TError(
                    "nullable struct-return projections are not supported");
            }
            plan.ComputedExprs.push_back(projection.Expression);
            plan.ComputedJitTypes.push_back(ProjectJitType(outType));

            auto addComputedColumn = [&](const std::string& fallbackName,
                                         const TTypePtr& fieldType) {
                const std::string& outputName = annotatedOutput
                        && annotatedFieldIndex < annotatedOutput->Fields.size()
                    ? annotatedOutput->Fields[annotatedFieldIndex].first
                    : fallbackName;
                const bool isStr = static_cast<bool>(TMaybeType<TStringType>(
                    UnwrapNamedType(UnwrapNullableType(fieldType))));
                plan.Columns.push_back({
                    .Computed = true,
                    .Index = static_cast<int32_t>(plan.ComputedWidths.size()),
                });
                plan.ComputedWidths.push_back(ProjectColumnWidth(fieldType));
                plan.ComputedIsString.push_back(isStr);
                plan.ComputedIsNullable.push_back(IsNullableType(fieldType));
                outFields.emplace_back(outputName, fieldType);
                ++annotatedFieldIndex;
            };

            if (structType) {
                const auto structure = structType.Cast();
                for (const auto& [fieldName, fieldType] : structure->Fields) {
                    addComputedColumn(fieldName, fieldType);
                }
                continue;
            }

            addComputedColumn(projection.Name, outType);
        }
    }
    plan.OutputType = std::make_shared<NQumir::NAst::TStructType>(
        std::move(outFields));
    return plan;
}

TUnaryRuntimeProcess BuildProjectRuntimeProcess(
    TProjectOperator& project,
    const NQumir::NAst::TTypePtr& inputType,
    TKernelCompilerOptions options)
{
    if (!options.RuntimeContext) {
        options.RuntimeContext = std::make_shared<TRuntimeContext>();
    }
    auto* inputStruct = static_cast<NQumir::NAst::TStructType*>(inputType.get());
    if (!inputStruct) {
        throw std::runtime_error("project input must have TStructType");
    }

    auto plan = BuildProjectColumnPlan(project, *inputStruct);

    TKernelCompiler::TProjectDispatch dispatch;
    if (!plan.ComputedExprs.empty()) {
        auto spec = NKernel::BuildProjectKernelSpec(
            *inputStruct,
            plan.ComputedExprs,
            plan.ComputedJitTypes,
            "<project>",
            options.ExternalCatalog.get(),
            options.RuntimeContext.get());
        TKernelCompiler compiler(std::move(options));
        dispatch = compiler.CompileProject(spec);
    }

    auto process = MakeProjectProcess(
        std::move(plan.Columns),
        std::move(dispatch),
        std::move(plan.ComputedWidths),
        std::move(plan.ComputedIsString),
        std::move(plan.ComputedIsNullable));

    return {
        .Process = std::move(process),
        .OutputType = std::move(plan.OutputType),
    };
}

// Radix key byte width: integer width, or 8 for f64.
int32_t RadixKeyWidthBytes(const NQumir::NAst::TTypePtr& type) {
    using namespace NQumir::NAst;
    if (DecimalSpecOfValueType(UnwrapNullableType(type))) {
        return 16; // qdb_bin_int (128-bit)
    }
    auto inner = UnwrapNamedType(UnwrapNullableType(type));
    if (auto integer = TMaybeType<TIntegerType>(inner)) {
        return static_cast<int32_t>(integer.Cast()->BitWidth() / 8);
    }
    if (TMaybeType<TFloatType>(inner)) {
        return 8;
    }
    if (TMaybeType<TBoolType>(inner)) {
        return 1;
    }
    return 0;
}

bool IsStringRadixKey(const NQumir::NAst::TTypePtr& type) {
    using namespace NQumir::NAst;
    return static_cast<bool>(TMaybeType<TStringType>(
        UnwrapNamedType(UnwrapNullableType(type))));
}

TSortRuntimeProcess BuildSortRuntimeProcess(
    const NQumir::NAst::TStructType& inputType,
    const std::vector<TSortKey>& keys,
    std::string_view kernelName,
    TKernelCompilerOptions options)
{
    auto spec = NKernel::BuildSortKernelSpec(inputType, keys, std::string(kernelName));
    PrintKernelSpec(options.Diagnostics, spec);
    auto sortInputs = BuildSortKernelInputs(spec);
    TSortRadixKernel radixKernel;
    if (!sortInputs.AllKeysRadixSortable || sortInputs.RadixKeys.empty()) {
        throw std::runtime_error("sort kernel is unavailable for requested keys");
    }
    {
        auto* sink = options.Sink;
        const size_t sinkBefore = sink ? sink->size() : 0;
        // The nullable variant is only reachable when a key column can carry a
        // null mask; with all keys non-nullable by schema it is dead code, so
        // skip generating it.
        bool anyNullableKey = false;
        for (const auto& key : sortInputs.RadixKeys) {
            anyNullableKey = anyNullableKey || IsNullableType(key.Type);
        }
        auto radixOptions = options;
        if (kernelName == "top-sort") {
            radixOptions.Diagnostics = nullptr;
        }
        TKernelCompiler compiler(std::move(radixOptions));
        radixKernel = {
            .Enabled = true,
            .Dispatch = anyNullableKey
                ? TKernelCompiler::TSortRadixCompositeDispatch{}
                : compiler.CompileRadixSortComposite(sortInputs.RadixKeys, &inputType),
            .NullableDispatch = anyNullableKey
                ? compiler.CompileRadixSortCompositeNullable(sortInputs.RadixKeys, &inputType)
                : TKernelCompiler::TSortRadixCompositeNullableDispatch{},
        };
        if (kernelName == "top-sort") {
            if (sink) {
                for (size_t i = sinkBefore; i < sink->size(); ++i) {
                    (*sink)[i].ExportArtifacts = false;
                }
            }
            TKernelCompiler topSortCompiler(std::move(options));
            radixKernel.TopSortDispatch =
                topSortCompiler.CompileTopSort(sortInputs.RadixKeys, &inputType);
            radixKernel.HeapTopKDispatch =
                topSortCompiler.CompileHeapTopK(sortInputs.RadixKeys);
        }
        if (sink) {
            // Resolved key metadata for the exec exporter, on all emitted sort
            // kernels.
            std::vector<TGeneratedKernel::TSortKeyMeta> meta;
            meta.reserve(keys.size());
            for (size_t k = 0; k < sortInputs.KeyColumns.size(); ++k) {
                meta.push_back({
                    .Index = sortInputs.KeyColumns[k].Index,
                    .WidthBytes = RadixKeyWidthBytes(sortInputs.KeyColumns[k].Type),
                    .IsString = IsStringRadixKey(sortInputs.KeyColumns[k].Type),
                    .Desc = keys[k].Direction == ESortDirection::Desc,
                });
            }
            for (size_t i = sinkBefore; i < sink->size(); ++i) {
                (*sink)[i].SortKeys = meta;
            }
        }
    }
    return {
        .KeyColumns = std::move(sortInputs.KeyColumns),
        .RadixKernel = std::move(radixKernel),
    };
}

TWindowRuntimeProcess BuildWindowRuntimeProcess(
    TWindowOperator& window,
    const NQumir::NAst::TTypePtr& inputType,
    TKernelCompilerOptions options)
{
    using namespace NQumir::NAst;
    auto* inputStruct = static_cast<TStructType*>(inputType.get());
    if (!inputStruct) {
        throw std::runtime_error("window input must have TStructType");
    }

    // Resolve a key/arg name the same way the hash-shuffle does (MakeGroupKeyHash
    // in plan_lowerer.cpp): bare-suffix OR exact match, first hit. Keeping the two
    // in lockstep guarantees the shuffle and the per-task boundary detection pick
    // the same column even when a bare suffix collides (e.g. `a.k` and `k`).
    auto fieldIndex = [&](const std::string& name) -> int32_t {
        for (size_t i = 0; i < inputStruct->Fields.size(); ++i) {
            const std::string& field = inputStruct->Fields[i].first;
            auto dot = field.rfind('.');
            const std::string bare =
                dot != std::string::npos ? field.substr(dot + 1) : field;
            if (bare == name || field == name) {
                return static_cast<int32_t>(i);
            }
        }
        throw std::runtime_error("window: column not found: " + name);
    };

    // Combined sort keys: partition (asc) ++ order. Clusters partitions and
    // orders within each; the segmented scan runs over this order.
    std::vector<TSortKey> keys;
    for (const auto& partition : window.PartitionKeys()) {
        keys.push_back(TSortKey{.Column = partition});
    }
    for (const auto& order : window.OrderKeys()) {
        keys.push_back(order);
    }
    if (keys.empty()) {
        throw std::runtime_error(
            "window without partition/order is not supported in exec yet");
    }

    std::vector<TSortColumnRef> keyColumns;
    std::vector<TSortRadixKeyInput> radixKeys;
    std::vector<TSortRadixKeyInput> partitionRadixKeys;
    std::vector<TSortRadixKeyInput> orderRadixKeys;
    for (const auto& key : keys) {
        const int32_t idx = fieldIndex(key.Column);
        const auto& type = inputStruct->Fields[idx].second;
        TSortRadixKeyInput radixKey{
            .ColumnIndex = idx,
            .Type = type,
            .Desc = key.Direction == ESortDirection::Desc,
            .NullsFirst = SortNullsFirst(key),
        };
        keyColumns.push_back({.Index = idx, .Type = type});
        if (partitionRadixKeys.size() < window.PartitionKeys().size()) {
            partitionRadixKeys.push_back(radixKey);
        } else {
            orderRadixKeys.push_back(radixKey);
        }
        radixKeys.push_back(std::move(radixKey));
    }

    // Running (ordered) sum/max honor the frame: only UNBOUNDED PRECEDING ..
    // CURRENT ROW is supported; RANGE mode makes CURRENT ROW peer-inclusive (all
    // rows equal on the order key share the value through the last peer), ROWS is
    // strict row-by-row. Ordered windows always carry a parser-materialized frame.
    auto framePeerInclusive = [&]() -> bool {
        const auto& frame = window.Frame();
        if (!frame) {
            return true; // default RANGE UNBOUNDED PRECEDING .. CURRENT ROW
        }
        const bool fullStart = frame->Start.Kind == EFrameBoundKind::UnboundedPreceding;
        const bool currentEnd = frame->End.Kind == EFrameBoundKind::CurrentRow;
        if (!fullStart || !currentEnd) {
            throw std::runtime_error(
                "window exec supports only UNBOUNDED PRECEDING .. CURRENT ROW "
                "running frames yet");
        }
        return frame->Mode == EWindowFrameMode::Range;
    };

    // Each window function becomes an appended output column. sum is either a
    // full-partition broadcast (no ORDER BY) or a running prefix; avg is
    // currently full-partition only; max is currently ordered-prefix only.
    std::vector<TWindowFuncInput> funcs;
    for (const auto& fn : window.Functions()) {
        if (fn.Func != "sum" && fn.Func != "rank" && fn.Func != "avg" &&
            fn.Func != "max") {
            throw std::runtime_error(
                "window function not supported in exec yet: " + fn.Func);
        }
        if (fn.Func == "rank") {
            if (fn.Arg) {
                throw std::runtime_error("window rank does not accept an argument");
            }
            if (orderRadixKeys.empty()) {
                throw std::runtime_error("window rank requires an order key yet");
            }
            funcs.push_back({
                .Func = fn.Func,
                .ResultType = std::make_shared<TIntegerType>(),
            });
            continue;
        }
        auto ident = TMaybeNode<TIdentExpr>(fn.Arg);
        if (!ident) {
            throw std::runtime_error("window " + fn.Func + " requires a column argument");
        }
        const int32_t idx = fieldIndex(ident.Cast()->Name);
        const auto& argType = inputStruct->Fields[idx].second;
        auto valueType = UnwrapNullableType(argType);
        const bool isBinInt =
            IsDecimalType(valueType) || IsBinIntStorageType(valueType);
        auto inner = UnwrapNamedType(valueType);
        auto integer = TMaybeType<TIntegerType>(inner);
        const bool isInteger = static_cast<bool>(integer);
        const bool isFloat = static_cast<bool>(TMaybeType<TFloatType>(inner));
        auto wrapNullable = [&](TTypePtr type) -> TTypePtr {
            return IsNullableType(argType)
                ? std::make_shared<TNullable>(std::move(type))
                : type;
        };
        if (fn.Func == "avg") {
            if (!window.OrderKeys().empty()) {
                throw std::runtime_error(
                    "window avg supports only full-partition frames yet");
            }
            if (!isBinInt && !isFloat && !isInteger) {
                throw std::runtime_error(
                    "window exec supports only integer/f64/decimal arguments for avg yet");
            }
            funcs.push_back({
                .Func = "avg_partition",
                .ArgColumn = idx,
                .ResultType = ComputeWindowAvgResultType(argType),
            });
            continue;
        }
        if (fn.Func == "sum") {
            if (!isBinInt && !isFloat && !isInteger) {
                throw std::runtime_error(
                    "window exec supports only integer/f64/decimal arguments for sum yet");
            }
            const bool ordered = !orderRadixKeys.empty();
            funcs.push_back({
                .Func = ordered ? "sum_prefix" : "sum_partition",
                .ArgColumn = idx,
                .ResultType = isInteger
                    ? wrapNullable(std::make_shared<TIntegerType>())
                    : inputStruct->Fields[idx].second,
                .PeerInclusive = ordered && framePeerInclusive(),
            });
            continue;
        }
        if (fn.Func == "max") {
            if (orderRadixKeys.empty()) {
                throw std::runtime_error(
                    "window max supports only ordered running frames yet");
            }
            if (!isBinInt && !TMaybeType<TFloatType>(inner) &&
                (!integer || integer.Cast()->BitWidth() != 64)) {
                throw std::runtime_error(
                    "window exec supports only i64/f64/decimal arguments for max yet");
            }
            funcs.push_back({
                .Func = "max_prefix",
                .ArgColumn = idx,
                .ResultType = inputStruct->Fields[idx].second,
                .PeerInclusive = framePeerInclusive(),
            });
            continue;
        }
        throw std::runtime_error(
            "window function not supported in exec yet: " + fn.Func);
    }

    auto* sink = options.Sink;
    const size_t sinkBefore = sink ? sink->size() : 0;
    TKernelCompiler compiler(std::move(options));
    TSortRadixKernel kernel;
    kernel.Enabled = true;
    kernel.Dispatch = compiler.CompileWindow(
        radixKeys, *inputStruct, partitionRadixKeys, orderRadixKeys, funcs);
    if (sink) {
        std::vector<TGeneratedKernel::TSortKeyMeta> meta;
        meta.reserve(keyColumns.size());
        for (size_t k = 0; k < keyColumns.size(); ++k) {
            meta.push_back({
                .Index = keyColumns[k].Index,
                .WidthBytes = RadixKeyWidthBytes(keyColumns[k].Type),
                .IsString = IsStringRadixKey(keyColumns[k].Type),
                .Desc = keys[k].Direction == ESortDirection::Desc,
            });
        }
        for (size_t i = sinkBefore; i < sink->size(); ++i) {
            (*sink)[i].SortKeys = meta;
        }
    }

    return {
        .OutputType = ComputeWindowOutputType(inputType, window.Functions()),
        .Keys = std::move(keys),
        .KeyColumns = std::move(keyColumns),
        .Kernel = std::move(kernel),
    };
}

namespace {

void PrintRuntimePlanNode(
    std::ostream& out, const TOperatorPtr& root, int depth)
{
    const std::string indent(static_cast<size_t>(depth) * 2, ' ');
    out << indent;
    if (auto node = TMaybeOp<TSourceOperator>(root)) {
        out << "source " << node.Cast()->SourcePath() << "\n";
        return;
    }
    if (auto node = TMaybeOp<TFilterOperator>(root)) {
        out << "filter [JIT: AST -> IR -> LLVM]\n";
        PrintRuntimePlanNode(out, node.Cast()->Input(), depth + 1);
        return;
    }
    if (auto node = TMaybeOp<TProjectOperator>(root)) {
        out << "project [column mapping]\n";
        PrintRuntimePlanNode(out, node.Cast()->Input(), depth + 1);
        return;
    }
    if (auto node = TMaybeOp<TAggregateOperator>(root)) {
        out << "aggregate [JIT: update + finalize]\n";
        PrintRuntimePlanNode(out, node.Cast()->Input(), depth + 1);
        return;
    }
    if (auto node = TMaybeOp<TJoinOperator>(root)) {
        out << "join [symmetric hash, JIT probe+insert]\n";
        PrintRuntimePlanNode(out, node.Cast()->Left(), depth + 1);
        PrintRuntimePlanNode(out, node.Cast()->Right(), depth + 1);
        return;
    }
    if (auto node = TMaybeOp<TSortOperator>(root)) {
        out << "sort [stable indices]\n";
        PrintRuntimePlanNode(out, node.Cast()->Input(), depth + 1);
        return;
    }
    if (auto node = TMaybeOp<TTopSortOperator>(root)) {
        out << "top-sort [bounded stable state]\n";
        PrintRuntimePlanNode(out, node.Cast()->Input(), depth + 1);
        return;
    }
    if (auto node = TMaybeOp<TLimitOperator>(root)) {
        out << "limit [" << node.Cast()->Limit()
            << ", offset " << node.Cast()->Offset() << "]\n";
        PrintRuntimePlanNode(out, node.Cast()->Input(), depth + 1);
        return;
    }
    if (auto node = TMaybeOp<TLateMaterializeOperator>(root)) {
        out << "late-materialize [" << node.Cast()->Columns().size()
            << " columns]\n";
        PrintRuntimePlanNode(out, node.Cast()->Input(), depth + 1);
        return;
    }
    out << "unknown\n";
}

} // namespace

void PrintRuntimePlan(std::ostream& out, const TOperatorPtr& root) {
    out << "\n========== RUNTIME PLAN ==========\n";
    PrintRuntimePlanNode(out, root, 0);
    out << "==================================\n";
}

} // namespace NQdb
