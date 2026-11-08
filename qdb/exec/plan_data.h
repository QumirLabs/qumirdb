#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace NQdb {

// Plain data that rebuilds operator processes from their kernel slots. The
// graph spec carries it, so this header does not include the kernel compiler.

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

} // namespace NQdb
