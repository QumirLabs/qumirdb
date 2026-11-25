#pragma once

#include <qdb/plan/types/date.h>
#include <qdb/plan/types/nullable.h>

#include <qumir/parser/type.h>
#include <qumir/parser/type_factory.h>

#include <charconv>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace NQdb {

struct TDecimalSpec {
    int32_t Precision = 18;
    int32_t Scale = 0;
};

inline constexpr int32_t MaxDecimalPrecision = 38;
inline constexpr int32_t DefaultDecimalPrecision = 18;
inline constexpr int32_t DefaultDecimalScale = 0;

inline void ValidateDecimalSpec(TDecimalSpec spec) {
    if (spec.Precision <= 0 || spec.Precision > MaxDecimalPrecision) {
        throw std::invalid_argument("DECIMAL precision must be in [1, 38]");
    }
    if (spec.Scale < 0 || spec.Scale > spec.Precision) {
        throw std::invalid_argument("DECIMAL scale must be in [0, precision]");
    }
}

inline std::optional<int32_t> ParseDecimalArg(std::string_view value) {
    int32_t parsed = 0;
    auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (ec != std::errc{} || ptr != value.data() + value.size()) {
        return std::nullopt;
    }
    return parsed;
}

inline std::optional<TDecimalSpec> DecimalSpecOfValueType(
    const NQumir::NAst::TTypePtr& type)
{
    auto named = NQumir::NAst::TMaybeType<NQumir::NAst::TNamedType>(type);
    if (!named) {
        return std::nullopt;
    }
    auto decimal = named.Cast();
    const auto& args = decimal->TypeArgs;
    if (decimal->Name != "DECIMAL" || args.size() != 2 ||
        args[0].Kind != NQumir::NAst::TGenericArg::EKind::Value ||
        args[1].Kind != NQumir::NAst::TGenericArg::EKind::Value)
    {
        return std::nullopt;
    }
    auto precision = ParseDecimalArg(args[0].Value);
    auto scale = ParseDecimalArg(args[1].Value);
    if (!precision || !scale) {
        return std::nullopt;
    }
    TDecimalSpec spec{.Precision = *precision, .Scale = *scale};
    ValidateDecimalSpec(spec);
    return spec;
}

inline std::optional<TDecimalSpec> DecimalSpecOf(
    const NQumir::NAst::TTypePtr& type)
{
    return DecimalSpecOfValueType(UnwrapNullableType(type));
}

inline bool IsDecimalType(const NQumir::NAst::TTypePtr& type) {
    return DecimalSpecOf(type).has_value();
}

// The widest precision whose unscaled value fits a signed 64-bit integer.
inline constexpr int32_t MaxDecimal64Precision = 18;

// The signed integer that holds the unscaled value of a DECIMAL.
enum class EDecimalStorage {
    I64,
    I128,
};

inline EDecimalStorage DecimalStorageOf(TDecimalSpec spec) {
    return spec.Precision <= MaxDecimal64Precision
        ? EDecimalStorage::I64
        : EDecimalStorage::I128;
}

inline int32_t DecimalStorageBytes(TDecimalSpec spec) {
    return DecimalStorageOf(spec) == EDecimalStorage::I64
        ? 8
        : 16;
}

// `<named DECIMAL [p s]>`. The only constructor of a DECIMAL type: the arguments are
// interned as text, so they are always explicit and printed one way.
inline NQumir::NAst::TTypePtr MakeDecimalType(NQumir::NAst::TTypeFactory& types, TDecimalSpec spec) {
    ValidateDecimalSpec(spec);
    return types.MakeNamed("DECIMAL", nullptr, {
        NQumir::NAst::TGenericArg::ValueArg(std::to_string(spec.Precision)),
        NQumir::NAst::TGenericArg::ValueArg(std::to_string(spec.Scale)),
    });
}

inline bool IsBinIntStorageType(const NQumir::NAst::TTypePtr& type) {
    auto named = NQumir::NAst::TMaybeType<NQumir::NAst::TNamedType>(
        UnwrapNullableType(type));
    return named && named.Cast()->Name == "BinInt";
}

// A signed 128-bit integer has the same two-word layout as DECIMAL storage.
// U128 is not listed: SQL cannot produce it yet.
inline bool IsInt128Type(const NQumir::NAst::TTypePtr& type) {
    auto integer = NQumir::NAst::TMaybeType<NQumir::NAst::TIntegerType>(
        UnwrapNullableType(type));
    return integer && integer.Cast()->Width == NQumir::NAst::TIntegerType::I128;
}

inline bool IsDecimal128Type(const NQumir::NAst::TTypePtr& type) {
    auto decimal = DecimalSpecOf(type);
    return decimal && DecimalStorageOf(*decimal) == EDecimalStorage::I128;
}

inline bool HasBinIntLayout(const NQumir::NAst::TTypePtr& type) {
    return IsDecimal128Type(type) || IsBinIntStorageType(type) || IsInt128Type(type);
}

// qumirdb.oz's `(type BinInt <struct (V i128)>)`, built complete.
inline NQumir::NAst::TTypePtr BinIntStorageType(NQumir::NAst::TTypeFactory& types) {
    auto layout = types.MakeStruct({{"V", types.MakeInteger(NQumir::NAst::TIntegerType::I128)}});
    return types.MakeNamed("BinInt", layout);
}

// The kernel type of the unscaled value: a plain i64, or an i128 inside the BinInt
// struct that keeps decimal operators away from other integers.
inline NQumir::NAst::TTypePtr DecimalStorageType(NQumir::NAst::TTypeFactory& types, TDecimalSpec spec) {
    if (DecimalStorageOf(spec) == EDecimalStorage::I64) {
        return types.MakeInteger(NQumir::NAst::TIntegerType::I64);
    }
    return BinIntStorageType(types);
}

// The value type a physical path reads: a decimal's storage, else the type without
// nullability and named wrappers.
inline NQumir::NAst::TTypePtr PhysicalValueType(
    NQumir::NAst::TTypeFactory& types, const NQumir::NAst::TTypePtr& type)
{
    if (auto decimal = DecimalSpecOf(type)) {
        return DecimalStorageType(types, *decimal);
    }
    return UnwrapNamedValueType(UnwrapNullableType(type));
}

inline NQumir::NAst::TTypePtr DecimalStorageTypeFor(
    NQumir::NAst::TTypeFactory& types, const NQumir::NAst::TTypePtr& type)
{
    auto storage = DecimalStorageType(types, *DecimalSpecOf(type));
    return IsNullableType(type)
        ? MakeNullableType(types, std::move(storage))
        : storage;
}

} // namespace NQdb
