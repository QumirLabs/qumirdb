#pragma once

#include <cstdint>

namespace NQdb {

enum class EValueKind : uint8_t {
    Signed,
    Unsigned,
    Float,
    // Bit-packed.
    Bool,
    String,
    // One char per row.
    Symbol,
    // A day count since 1970-01-01.
    Date,
    // An unscaled signed integer.
    Decimal,
};

// How host code on a task reads the values of a column. It is computed from
// the column type before execution, so tasks never read AST types.
struct TValueType {
    EValueKind Kind = EValueKind::Signed;
    // Bytes of one value; 0 for Bool and String.
    int32_t Width = 0;
    // Digits after the point of a Decimal.
    int32_t Scale = 0;
    bool Nullable = false;

    void Fields(this auto& self, auto& v)
    {
        v(self.Kind);
        v(self.Width);
        v(self.Scale);
        v(self.Nullable);
    }
};

} // namespace NQdb
