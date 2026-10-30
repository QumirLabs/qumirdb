#pragma once

#include <qdb/plan/types/nullable.h>

#include <qumir/parser/type.h>

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

// Keep DECIMAL printable by the existing qumir core printer: it is a Named type
// syntactically, with qdb-only precision/scale semantics.
struct TDecimal : NQumir::NAst::TNamedType {
    TDecimalSpec Spec;

    explicit TDecimal(
        int32_t precision = DefaultDecimalPrecision,
        int32_t scale = DefaultDecimalScale)
        : TNamedType("DECIMAL", nullptr, {
              NQumir::NAst::TGenericArg::ValueArg(std::to_string(precision)),
              NQumir::NAst::TGenericArg::ValueArg(std::to_string(scale)),
          })
        , Spec{.Precision = precision, .Scale = scale}
    {
        ValidateDecimalSpec(Spec);
    }

    std::string ToString() const override {
        return "DECIMAL(" + std::to_string(Spec.Precision) + "," +
            std::to_string(Spec.Scale) + ")";
    }
};

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
    if (!type) {
        return std::nullopt;
    }
    if (auto decimal = std::dynamic_pointer_cast<TDecimal>(type)) {
        return decimal->Spec;
    }
    auto named = NQumir::NAst::TMaybeType<NQumir::NAst::TNamedType>(type);
    if (!named || named.Cast()->Name != "DECIMAL") {
        return std::nullopt;
    }
    const auto& args = named.Cast()->TypeArgs;
    if (args.empty()) {
        return TDecimalSpec{
            .Precision = DefaultDecimalPrecision,
            .Scale = DefaultDecimalScale,
        };
    }
    if (args.size() != 2 ||
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

inline EDecimalStorage DecimalStorageOf(TDecimalSpec) {
    // TODO: I64 up to MaxDecimal64Precision once kernels and readers take it (#426).
    return EDecimalStorage::I128;
}

inline int32_t DecimalStorageBytes(TDecimalSpec spec) {
    return DecimalStorageOf(spec) == EDecimalStorage::I64
        ? 8
        : 16;
}

inline NQumir::NAst::TTypePtr MakeDecimalType(TDecimalSpec spec) {
    ValidateDecimalSpec(spec);
    return std::make_shared<TDecimal>(spec.Precision, spec.Scale);
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
    return integer && integer.Cast()->Kind == NQumir::NAst::TIntegerType::I128;
}

inline bool HasBinIntLayout(const NQumir::NAst::TTypePtr& type) {
    auto decimal = DecimalSpecOf(type);
    return (decimal && DecimalStorageOf(*decimal) == EDecimalStorage::I128) ||
        IsBinIntStorageType(type) || IsInt128Type(type);
}

inline NQumir::NAst::TTypePtr BinIntStorageType() {
    return std::make_shared<NQumir::NAst::TNamedType>("BinInt", nullptr);
}

inline NQumir::NAst::TTypePtr DecimalStorageType() {
    return BinIntStorageType();
}

inline NQumir::NAst::TTypePtr DecimalStorageTypeFor(
    const NQumir::NAst::TTypePtr& type)
{
    auto storage = DecimalStorageType();
    return IsNullableType(type)
        ? std::static_pointer_cast<NQumir::NAst::TType>(
              std::make_shared<TNullable>(std::move(storage)))
        : storage;
}

} // namespace NQdb
