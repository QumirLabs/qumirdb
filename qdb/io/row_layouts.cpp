#include "row_layouts.h"

#include <qdb/plan/types/date.h>
#include <qdb/plan/types/decimal.h>
#include <qdb/plan/types/nullable.h>

#include <stdexcept>

namespace NQdb {

std::vector<TValueLayout> RowLayouts(const NQumir::NAst::TTypePtr& type) {
    using namespace NQumir::NAst;
    auto structure = TMaybeType<TStructType>(UnwrapNamedType(type));
    if (!structure) {
        throw std::runtime_error("row type must be a struct");
    }
    std::vector<TValueLayout> layouts;
    for (const auto& [name, field] : structure.Cast()->Fields) {
        const auto value = ValueTypeOf(field);
        if (!value) {
            throw std::runtime_error("unsupported row column: " + field->ToString());
        }
        layouts.push_back({.Width = value->Width, .Bitmap = value->Kind == EValueKind::Bool});
    }
    return layouts;
}

std::optional<TValueType> ValueTypeOf(const NQumir::NAst::TTypePtr& type) {
    using namespace NQumir::NAst;
    const bool nullable = IsNullableType(type);
    const auto value = UnwrapNullableType(type);
    if (const auto decimal = DecimalSpecOfValueType(value)) {
        return TValueType{
            .Kind = EValueKind::Decimal,
            .Width = DecimalStorageBytes(*decimal),
            .Scale = decimal->Scale,
            .Nullable = nullable,
        };
    }
    if (const auto date = std::dynamic_pointer_cast<TDateType>(value)) {
        return TValueType{.Kind = EValueKind::Date, .Width = date->BitWidth() / 8, .Nullable = nullable};
    }
    if (IsBinIntStorageType(value)) {
        return TValueType{.Kind = EValueKind::Signed, .Width = 16, .Nullable = nullable};
    }
    // Aliases and Nullable may nest around the other types.
    auto inner = value;
    while (true) {
        const auto next = UnwrapNamedType(UnwrapNullableType(inner));
        if (next == inner) {
            break;
        }
        inner = next;
    }
    if (auto maybeInteger = TMaybeType<TIntegerType>(inner)) {
        const auto integer = maybeInteger.Cast();
        return TValueType{
            .Kind = integer->IsSigned() ? EValueKind::Signed : EValueKind::Unsigned,
            .Width = integer->BitWidth() / 8,
            .Nullable = nullable,
        };
    }
    if (auto maybeFloat = TMaybeType<TFloatType>(inner)) {
        return TValueType{.Kind = EValueKind::Float, .Width = maybeFloat.Cast()->BitWidth() / 8, .Nullable = nullable};
    }
    if (TMaybeType<TBoolType>(inner)) {
        return TValueType{.Kind = EValueKind::Bool, .Nullable = nullable};
    }
    if (TMaybeType<TStringType>(inner)) {
        return TValueType{.Kind = EValueKind::String, .Nullable = nullable};
    }
    return std::nullopt;
}

} // namespace NQdb
