#include "row_layouts.h"

#include <qdb/kernel/column_value.h>
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
        auto value = field;
        while (true) {
            const auto inner = UnwrapNamedType(UnwrapNullableType(value));
            if (inner == value) {
                break;
            }
            value = inner;
        }
        if (TMaybeType<TStringType>(value)) {
            layouts.push_back({});
        } else if (TMaybeType<TBoolType>(value)) {
            layouts.push_back({.Bitmap = true});
        } else if (const auto width = NKernel::FixedValueWidth(field)) {
            layouts.push_back({.Width = static_cast<int32_t>(width)});
        } else {
            throw std::runtime_error("unsupported row column: " + field->ToString());
        }
    }
    return layouts;
}

} // namespace NQdb
