#pragma once

#include <qumir/parser/type.h>
#include <qumir/parser/type_factory.h>

namespace NQdb {

// `<named Nullable [T]>`: a T value with a validity flag. It is built complete with
// qumirdb.oz's struct `<struct (Value T) (Valid bool)>`, which kernels carry.
inline NQumir::NAst::TTypePtr MakeNullableType(
    NQumir::NAst::TTypeFactory& types,
    const NQumir::NAst::TTypePtr& value)
{
    auto layout = types.MakeStruct({{"Value", value}, {"Valid", types.Make<NQumir::NAst::TBoolType>()}});
    return types.MakeNamed("Nullable", layout, {NQumir::NAst::TGenericArg::TypeArg(value)});
}

inline bool IsNullableType(const NQumir::NAst::TTypePtr& type) {
    auto named = NQumir::NAst::TMaybeType<NQumir::NAst::TNamedType>(type);
    if (!named) {
        return false;
    }
    auto nullable = named.Cast();
    return nullable->Name == "Nullable" && nullable->TypeArgs.size() == 1
        && nullable->TypeArgs[0].Kind == NQumir::NAst::TGenericArg::EKind::Type;
}

inline NQumir::NAst::TTypePtr UnwrapNullableType(const NQumir::NAst::TTypePtr& type) {
    if (IsNullableType(type)) {
        return static_cast<const NQumir::NAst::TNamedType*>(type)->TypeArgs[0].Type;
    }
    return type;
}

} // namespace NQdb
