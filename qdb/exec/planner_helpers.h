#pragma once

#include <qdb/exec/join_exec.h>
#include <qdb/exec/project_exec.h>
#include <qdb/exec/sort_exec.h>
#include <qdb/exec/unary_process.h>
#include <qdb/kernel/compiler.h>
#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/ops/sort.h>
#include <qdb/plan/ops/window.h>
#include <qdb/scheduler/settings.h>

#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace NQdb {

struct TUnaryRuntimeProcess {
    TUnaryStreamProcess Process;
    NQumir::NAst::TTypePtr OutputType;
};

struct TFilterColumnPlan {
    NQumir::NAst::TTypePtr OutputType;
    std::optional<std::vector<int32_t>> KeptInputColumns;
};

// Compilation-free filter output layout shared by native execution and the
// typed exec-plan builder.
TFilterColumnPlan BuildFilterColumnPlan(
    const TFilterOperator& filter,
    const NQumir::NAst::TTypePtr& inputType);

// Estimate a power-of-two Robin Hood table capacity from group-key NDVs.
// partitionCount is the number of disjoint tables that will share the groups;
// use one for a semantic/browser aggregate that owns the complete input.
int64_t EstimateAggregateInitialCapacity(
    const TAggregateOperator& aggregate,
    size_t partitionCount = 1);

// Estimate the number of output groups from input row count and group-key
// NDVs. Returns nullopt when any key lacks usable statistics.
std::optional<uint64_t> EstimateAggregateGroupCount(
    const TAggregateOperator& aggregate);

// Build the final-phase operator that merges rows emitted by a partial
// aggregate. This is scheduler-neutral physical planning shared by local and
// future distributed lowerers.
std::shared_ptr<TAggregateOperator> BuildAggregateCombineOperator(
    const TAggregateOperator& aggregate,
    std::vector<std::string> groupKeys);

// Compilation-free projection layout: column plan, computed-expression types
// and the output struct. Shared by the runtime process builder and the plan
// exporter so output typing has one source.
struct TProjectColumnPlan {
    std::vector<TProjectColumn> Columns;
    // Grouped one-to-one: one expression may have a struct JIT type.
    std::vector<NQumir::NAst::TExprPtr> ComputedExprs;
    std::vector<NQumir::NAst::TTypePtr> ComputedJitTypes;
    // Flat physical outputs: struct fields occupy separate entries.
    std::vector<size_t> ComputedWidths;
    std::vector<bool> ComputedIsString;
    std::vector<bool> ComputedIsNullable;
    NQumir::NAst::TTypePtr OutputType;
};

TProjectColumnPlan BuildProjectColumnPlan(
    TProjectOperator& project,
    const NQumir::NAst::TStructType& inputStruct);

struct TSortRuntimeProcess {
    std::vector<TSortColumnRef> KeyColumns;
    TSortRadixKernel RadixKernel;
};

// Print a human-readable runtime-plan tree for diagnostics (--verbose).
void PrintRuntimePlan(
    std::ostream& out,
    const TOperatorPtr& root,
    NScheduler::EJoinTableKind table);

NQumir::NAst::TTypePtr BuildSourceRuntimeType(TSourceOperator& src);

// Build-side selection for a join. Shared by native lowering and the browser
// plan export so the JoinAsymmetryRatio threshold has one home (C++). An
// inner join on the symmetric table, which the browser runtime drives, fixes
// its build side only at that ratio; an inner equi-join on the row join table
// builds the smaller side whenever both inputs have estimates.
EJoinBuildSide ChooseJoinBuildSide(
    const TJoinOperator& join,
    NScheduler::EJoinTableKind table);

// Whether the row join table (kernel/join/join_rows.oz) serves a join under
// this table setting and build side: an equi-join without a residual that is
// inner, or outer with a null-supplying build side (a preserved one needs
// matched marks). The table has no adaptive mode, so the build side is fixed.
bool RowJoinTableServes(
    const TJoinOperator& join,
    NScheduler::EJoinTableKind table,
    EJoinBuildSide buildSide);
std::string_view JoinBuildSideName(EJoinBuildSide side);

TUnaryRuntimeProcess BuildFilterRuntimeProcess(
    TFilterOperator& filter,
    const NQumir::NAst::TTypePtr& inputType,
    TKernelCompilerOptions options);

TUnaryRuntimeProcess BuildProjectRuntimeProcess(
    TProjectOperator& project,
    const NQumir::NAst::TTypePtr& inputType,
    TKernelCompilerOptions options);

TSortRuntimeProcess BuildSortRuntimeProcess(
    const NQumir::NAst::TStructType& inputType,
    const std::vector<TSortKey>& keys,
    std::string_view kernelName,
    TKernelCompilerOptions options);

struct TWindowRuntimeProcess {
    NQumir::NAst::TTypePtr OutputType;
    std::vector<TSortKey> Keys;              // partition (asc) ++ order
    std::vector<TSortColumnRef> KeyColumns;
    TSortRadixKernel Kernel;                 // Kernel.Dispatch = window kernel
};

TWindowRuntimeProcess BuildWindowRuntimeProcess(
    TWindowOperator& window,
    const NQumir::NAst::TTypePtr& inputType,
    TKernelCompilerOptions options);

} // namespace NQdb
