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

} // namespace NQdb
