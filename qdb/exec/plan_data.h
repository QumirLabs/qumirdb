#pragma once

#include <qdb/plan/ops/sort_key.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace NQumir::NAst {

struct TType;
using TTypePtr = std::shared_ptr<TType>;

} // namespace NQumir::NAst

namespace NQdb {

// Plain data that rebuilds operator processes from their kernel slots. The
// graph spec carries it, so this header does not include the kernel compiler.

// Types travel as qumir type text.
std::string TypeToText(const NQumir::NAst::TTypePtr& type);
NQumir::NAst::TTypePtr TypeFromText(std::string_view text);

void TypeField(auto& v, auto& type)
{
    if constexpr (std::remove_cvref_t<decltype(v)>::Reading) {
        std::string text;
        v(text);
        type = TypeFromText(text);
    } else {
        std::string text = TypeToText(type);
        v(text);
    }
}

// What a filter process needs besides its kernel, so it can be rebuilt later.
struct TFilterPlanData {
    // Index of the filter kernel in the sink (0 without a sink).
    uint32_t Kernel = 0;
    // Input columns kept in output order; nullopt keeps all of them.
    std::optional<std::vector<int32_t>> KeptInputColumns;

    void Fields(this auto& self, auto& v)
    {
        v(self.Kernel);
        v(self.KeptInputColumns);
    }
};

// One output column: either a zero-copy reference to an input column (ident
// projection) or a computed column filled by the project kernel.
struct TProjectColumn {
    bool Computed = false;
    // Ident: index into the input batch's columns.
    // Computed: index into the flat computed-buffer list.
    int32_t Index = 0;

    void Fields(this auto& self, auto& v)
    {
        v(self.Computed);
        v(self.Index);
    }
};

// What a project process needs besides its kernel, so it can be rebuilt later.
struct TProjectPlanData {
    // Index of the project kernel in the sink; none without computed columns.
    std::optional<uint32_t> Kernel;
    std::vector<TProjectColumn> Columns;
    std::vector<size_t> ComputedWidths;
    std::vector<bool> ComputedIsString;
    std::vector<bool> ComputedIsNullable;

    void Fields(this auto& self, auto& v)
    {
        v(self.Kernel);
        v(self.Columns);
        v(self.ComputedWidths);
        v(self.ComputedIsString);
        v(self.ComputedIsNullable);
    }
};

struct TSortColumnRef {
    int32_t Index = 0;
    NQumir::NAst::TTypePtr Type;

    void Fields(this auto& self, auto& v)
    {
        v(self.Index);
        TypeField(v, self.Type);
    }
};

// What a sort or top-sort needs besides its kernels, so it can be rebuilt later.
struct TSortPlanData {
    // Kernel fields are indexes in the sink (0 without a sink).
    struct TTop {
        uint32_t TopSortKernel = 0;
        uint32_t HeapKernel = 0;
        int64_t Limit = 0;

        void Fields(this auto& self, auto& v)
        {
            v(self.TopSortKernel);
            v(self.HeapKernel);
            v(self.Limit);
        }
    };

    NQumir::NAst::TTypePtr OutputType;
    std::vector<TSortKey> Keys;
    std::vector<TSortColumnRef> KeyColumns;
    uint32_t RadixKernel = 0;
    // The radix kernel is the nullable variant.
    bool RadixNullable = false;
    std::optional<TTop> Top;

    void Fields(this auto& self, auto& v)
    {
        TypeField(v, self.OutputType);
        v(self.Keys);
        v(self.KeyColumns);
        v(self.RadixKernel);
        v(self.RadixNullable);
        v(self.Top);
    }
};

// What a merge of sorted runs needs; it has no kernel.
struct TSortMergePlanData {
    NQumir::NAst::TTypePtr OutputType;
    std::vector<TSortKey> Keys;
    std::vector<TSortColumnRef> KeyColumns;

    void Fields(this auto& self, auto& v)
    {
        TypeField(v, self.OutputType);
        v(self.Keys);
        v(self.KeyColumns);
    }
};

// What a window process needs besides its kernel, so it can be rebuilt later.
struct TWindowPlanData {
    NQumir::NAst::TTypePtr OutputType;
    std::vector<TSortKey> Keys;              // partition (asc) ++ order
    std::vector<TSortColumnRef> KeyColumns;
    // Index of the window kernel in the sink (0 without a sink).
    uint32_t Kernel = 0;

    void Fields(this auto& self, auto& v)
    {
        TypeField(v, self.OutputType);
        v(self.Keys);
        v(self.KeyColumns);
        v(self.Kernel);
    }
};

enum class EAggregateOutputKeyKind {
    Fixed,
    String,
};

struct TAggregateOutputKey {
    EAggregateOutputKeyKind Kind = EAggregateOutputKeyKind::Fixed;
    bool IsNullable = false;
    size_t Size = 0;
    size_t Alignment = 0;

    void Fields(this auto& self, auto& v)
    {
        v(self.Kind);
        v(self.IsNullable);
        v(self.Size);
        v(self.Alignment);
    }
};

struct TAggregateOutputAgg {
    bool IsNullable = false;

    void Fields(this auto& self, auto& v)
    {
        v(self.IsNullable);
    }
};

// The plain data of compiled aggregate kernels (TAggregateKernels without its
// callables), so BindAggregateKernels can rebind them from their slot.
struct TAggregateKernelMeta {
    size_t NumAggs = 0;
    // The i64 state words of a group, one swiss state array each.
    size_t StateWords = 0;
    // Bytes of a group in the row layout, or 0 when the kernel has none: the
    // spec does not ask for it (TOperatorKernelSpec::AggregateRowLayout), its
    // key or a reducer holds a string, or it has groups that exist over an
    // empty input.
    int64_t RowBytes = 0;
    std::vector<int64_t> StringMetadataBuffers;
    std::vector<TAggregateOutputKey> OutputKeys;
    std::vector<TAggregateOutputAgg> OutputAggs;
    // Index of the aggregate kernel in the sink (0 without a sink).
    uint32_t Kernel = 0;

    void Fields(this auto& self, auto& v)
    {
        v(self.NumAggs);
        v(self.StateWords);
        v(self.RowBytes);
        v(self.StringMetadataBuffers);
        v(self.OutputKeys);
        v(self.OutputAggs);
        v(self.Kernel);
    }
};

// The layouts of the aggregate hash table: the swiss table, with one array per
// state word, or the row layout (kernel/aggregation/aggregation_rows.oz),
// which keeps the key and the states of a group in one arena row.
enum class EAggregateTableLayout {
    Swiss,
    Row,
};

// How the table of one aggregate task starts and when it changes layout;
// PlanAggregateTable (planner_helpers.h) chooses it.
struct TAggregateTablePlan {
    EAggregateTableLayout Layout = EAggregateTableLayout::Swiss;
    // Swiss slots, rounded up to a power of two of at least 8.
    int64_t InitialCapacity = 8;
    // The groups a table that starts in the row layout has entries for.
    int64_t InitialGroups = 0;
    // A swiss table that must grow while it holds this many groups or more
    // moves to the row layout instead; 0 keeps it swiss.
    int64_t RowSwitchGroups = 0;
    // The groups that move sizes the row entries for, when that is more than
    // twice the groups it moves; 0 sizes them for twice the groups.
    int64_t RowExpectedGroups = 0;

    void Fields(this auto& self, auto& v)
    {
        v(self.Layout);
        v(self.InitialCapacity);
        v(self.InitialGroups);
        v(self.RowSwitchGroups);
        v(self.RowExpectedGroups);
    }
};

// When a partial aggregate stops hashing and when it empties its table.
struct TPartialAggregatePolicy {
    // Input rows the table absorbs before its reduction is judged.
    uint64_t SampleRows = 128 * 1024;
    // Rows per group below which hashing does not pay for itself: the
    // remaining rows pass through as single-row partial states.
    uint64_t MinReduction = 8;
    // Groups at which the table emits its groups and restarts empty.
    int64_t MaxGroups = 1 << 20;

    void Fields(this auto& self, auto& v)
    {
        v(self.SampleRows);
        v(self.MinReduction);
        v(self.MaxGroups);
    }
};

// What an aggregate-hash dispatch needs besides its kernel slot.
struct TAggregateHashPlanData {
    // Index of the hash kernel in the sink (0 without a sink).
    uint32_t Kernel = 0;
    std::vector<int64_t> KeyColumns;

    void Fields(this auto& self, auto& v)
    {
        v(self.Kernel);
        v(self.KeyColumns);
    }
};

// One side of a join hash kernel: entry 0 hashes the left keys, entry 1 the
// right ones.
struct TJoinHashPlanData {
    // Index of the join hash kernel in the sink (0 without a sink).
    uint32_t Kernel = 0;
    uint32_t Entry = 0;
    std::vector<int64_t> KeyColumns;

    void Fields(this auto& self, auto& v)
    {
        v(self.Kernel);
        v(self.Entry);
        v(self.KeyColumns);
    }
};

// The input a join builds its table on; Auto builds both adaptively.
enum class EJoinBuildSide { Auto, Left, Right };

// The plain data of compiled hash join kernels (TJoinKernels without its
// callables), so BindJoinKernels can rebind them from their slot.
struct TJoinKernelMeta {
    std::vector<int64_t> LeftKeyColumns;
    std::vector<int64_t> RightKeyColumns;
    // Index of the join kernel in the sink (0 without a sink).
    uint32_t Kernel = 0;

    void Fields(this auto& self, auto& v)
    {
        v(self.LeftKeyColumns);
        v(self.RightKeyColumns);
        v(self.Kernel);
    }
};

// The plain data of compiled row join kernels (TRowJoinKernels without its
// callables), so BindRowJoinKernels can rebind them from their slots.
struct TRowJoinKernelMeta {
    // String payload rows point into the build batches, which the caller then
    // keeps alive until the processor is released.
    bool RetainsBuildBatches = false;
    // The build side is the left side of a SEMI/ANTI join: the probe marks its
    // rows, and EmitByMark emits the output once the probe side has ended.
    bool EmitsByMark = false;
    // An ANTI join keeps its NULL-key build rows in a second arena
    // (AppendNullKeys), whose rows EmitByMark emits as well.
    bool KeepsNullKeyRows = false;
    std::vector<int64_t> BuildKeyColumns;
    std::vector<int64_t> ProbeKeyColumns;
    // Index of the row join kernel in the sink (0 without a sink).
    uint32_t Kernel = 0;
    // The instrumentation measure kernel over the probe schema and the column
    // widths it sizes (TRowJoinKernels::ProbeMeasureSlot).
    uint32_t ProbeMeasureKernel = 0;
    std::vector<int64_t> ProbeMeasureWidths;

    void Fields(this auto& self, auto& v)
    {
        v(self.RetainsBuildBatches);
        v(self.EmitsByMark);
        v(self.KeepsNullKeyRows);
        v(self.BuildKeyColumns);
        v(self.ProbeKeyColumns);
        v(self.Kernel);
        v(self.ProbeMeasureKernel);
        v(self.ProbeMeasureWidths);
    }
};

} // namespace NQdb
