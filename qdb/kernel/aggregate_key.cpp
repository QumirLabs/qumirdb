#include <qdb/kernel/aggregate_key.h>
#include <qdb/kernel/column_value.h>
#include <qdb/modules/qumirdb.h>
#include <qdb/plan/types/date.h>
#include <qdb/plan/types/decimal.h>
#include <qdb/plan/types/nullable.h>
#include <qdb/plan/types/type_text.h>

#include <qumir/error.h>

#include <algorithm>
#include <cctype>

namespace NQdb::NKernel {

size_t AlignUp(size_t value, size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

namespace {

using TLayout = TKeyLayout;
using TRepresentedType = TRepresentedKeyType;

TLayout TypeLayout(NQumir::NAst::TTypeFactory& types, const NQumir::NAst::TTypePtr& original) {
    using namespace NQumir::NAst;

    // Key structs follow the C ABI, which qumir and LLVM share: an i128 and the
    // BinInt struct around it are 16-aligned, so a struct holding one is too.
    if (auto decimal = DecimalSpecOf(original)) {
        return TypeLayout(types, DecimalStorageType(types, *decimal));
    }
    if (IsBinIntStorageType(original)) {
        return {16, 16};
    }
    auto type = UnwrapNamedValueType(original);
    if (auto integer = MaybeIntegerType(type)) {
        const size_t size = static_cast<size_t>(integer.Cast()->BitWidth() / 8);
        return {size, size};
    }
    if (TMaybeType<TFloatType>(type)) {
        // Fixed-width scalars are aligned to their size.
        const size_t size = static_cast<size_t>(FixedValueWidth(type));
        return {size, size};
    }
    if (TMaybeType<TBoolType>(type)) {
        return {1, 1};
    }
    if (auto structure = TMaybeType<TStructType>(type)) {
        size_t offset = 0;
        size_t maxAlignment = 1;
        for (const auto& [_, fieldType] : structure.Cast()->Fields) {
            const auto field = TypeLayout(types, fieldType);
            offset = AlignUp(offset, field.Alignment);
            offset += field.Size;
            maxAlignment = std::max(maxAlignment, field.Alignment);
        }
        return {AlignUp(offset, maxAlignment), maxAlignment};
    }
    throw NQumir::TError(
        "aggregation key type is not fixed-width and trivially copyable: " +
        SqlTypeName(original));
}

std::string PhysicalKeyComponentName(const NQumir::NAst::TTypePtr& type);

std::string PhysicalKeyInnerComponentName(const NQumir::NAst::TTypePtr& original) {
    using namespace NQumir::NAst;

    if (!original) {
        throw NQumir::TError("key type name is not supported: <null>");
    }
    if (IsBinIntStorageType(original)) {
        return "BinInt";
    }
    if (auto decimal = DecimalSpecOf(original)) {
        return DecimalStorageOf(*decimal) == EDecimalStorage::I64 ? "i64" : "BinInt";
    }
    auto type = UnwrapNamedValueType(original);
    if (auto integer = MaybeIntegerType(type)) {
        return integer.Cast()->ToString();
    }
    if (auto floating = TMaybeType<TFloatType>(type)) {
        return floating.Cast()->ToString();
    }
    if (TMaybeType<TBoolType>(type)) {
        return "bool";
    }
    if (TMaybeType<TStringType>(type)) {
        return "string";
    }
    if (auto structure = TMaybeType<TStructType>(type)) {
        std::string result = "Struct" + std::to_string(structure.Cast()->Fields.size());
        for (const auto& [_, fieldType] : structure.Cast()->Fields) {
            result += "_" + PhysicalKeyComponentName(fieldType);
        }
        return result;
    }
    throw NQumir::TError(
        "key type name is not supported: " +
        SqlTypeName(original));
}

std::string PhysicalKeyComponentName(const NQumir::NAst::TTypePtr& type) {
    if (IsNullableType(type)) {
        return "n" + PhysicalKeyInnerComponentName(UnwrapNullableType(type));
    }
    return PhysicalKeyInnerComponentName(type);
}

} // namespace

std::string PhysicalKeyTypeName(
    const std::vector<std::pair<NQumir::NAst::TTypePtr, bool>>& fields)
{
    // Name by field type + nullability + order only (not column names): the
    // compiled hash/equal address fields by offset, so structurally-identical
    // keys share one cache symbol regardless of which columns they came from.
    std::string name = "AggKey";
    for (const auto& [type, nullable] : fields) {
        name += nullable ? "_n" : "_";
        name += PhysicalKeyComponentName(type);
    }
    return name;
}

TRepresentedKeyType RepresentKeyType(NQumir::NAst::TTypeFactory& types, const NQumir::NAst::TTypePtr& original) {
    using namespace NQumir::NAst;

    const bool nullable = IsNullableType(original);
    auto inner = nullable ? UnwrapNullableType(original) : original;
    // A decimal key is its storage: a BinInt, or the plain i64 of p <= 18. A DATE key
    // is its storage integer, as kernels read it.
    if (auto decimal = DecimalSpecOf(inner)) {
        inner = DecimalStorageType(types, *decimal);
    }
    if (const auto* date = DateStorageType(inner)) {
        inner = date;
    }
    if (IsBinIntStorageType(inner)) {
        auto storage = BinIntStorageType(types);
        return {
            .Lookup = storage,
            .Stored = storage,
            .Layout = TypeLayout(types, storage),
            .Nullable = nullable,
            .Inner = storage,
        };
    }
    auto type = UnwrapNamedValueType(inner);
    if (TMaybeType<TStringType>(type)) {
        return {
            .Lookup = NQumir::NRegistry::QumirDbNamedType(types, "StringView"),
            .Stored = NQumir::NRegistry::QumirDbNamedType(types, "OwnedString"),
            .Layout = {16, 8},
            .HasString = true,
            .Nullable = nullable,
            .Inner = inner,
        };
    }
    if (MaybeIntegerType(type) || TMaybeType<TFloatType>(type) ||
        TMaybeType<TBoolType>(type)) {
        return {
            .Lookup = inner,
            .Stored = inner,
            .Layout = TypeLayout(types, inner),
            .Nullable = nullable,
            .Inner = inner,
        };
    }
    if (auto structure = TMaybeType<TStructType>(type)) {
        std::vector<std::pair<std::string, TTypePtr>> lookupFields;
        std::vector<std::pair<std::string, TTypePtr>> storedFields;
        size_t offset = 0;
        size_t maxAlignment = 1;
        bool hasString = false;

        for (const auto& [name, fieldType] : structure.Cast()->Fields) {
            auto represented = RepresentKeyType(types, fieldType);
            offset = AlignUp(offset, represented.Layout.Alignment);
            lookupFields.emplace_back(name, represented.Lookup);
            storedFields.emplace_back(name, represented.Stored);
            offset += represented.Layout.Size;
            maxAlignment = std::max(maxAlignment, represented.Layout.Alignment);
            hasString = hasString || represented.HasString;
        }
        const size_t size = AlignUp(offset, maxAlignment);
        if (!hasString) {
            return {
                .Lookup = inner,
                .Stored = inner,
                .Layout = {size, maxAlignment},
                .Nullable = nullable,
                .Inner = inner,
            };
        }
        return {
            .Lookup = types.MakeStruct(std::move(lookupFields)),
            .Stored = types.MakeStruct(std::move(storedFields)),
            .Layout = {size, maxAlignment},
            .HasString = hasString,
            .Nullable = nullable,
            .Inner = inner,
        };
    }
    throw NQumir::TError(
        "key type is not supported: " +
        SqlTypeName(original));
}

TAggregateKeyDescriptor BuildAggregateKeyDescriptor(
    NQumir::NAst::TTypeFactory& types,
    const NQumir::NAst::TStructType& inputType,
    const std::vector<std::string>& groupKeys)
{
    using namespace NQumir::NAst;

    if (groupKeys.empty()) {
        throw NQumir::TError("aggregation key descriptor requires at least one group key");
    }

    TAggregateKeyDescriptor result;
    size_t offset = 0;
    size_t maxAlignment = 1;
    std::vector<std::pair<std::string, TTypePtr>> lookupFields;
    std::vector<std::pair<std::string, TTypePtr>> storedFields;

    for (const auto& key : groupKeys) {
        int32_t columnIndex = -1;
        TTypePtr type = nullptr;
        for (int32_t i = 0; i < static_cast<int32_t>(inputType.Fields.size()); ++i) {
            const auto& name = inputType.Fields[i].first;
            const auto dot = name.rfind('.');
            const std::string bare = dot != std::string::npos ? name.substr(dot + 1) : name;
            if (name == key || bare == key) {
                columnIndex = i;
                type = inputType.Fields[i].second;
                break;
            }
        }
        if (!type) {
            throw NQumir::TError("aggregation key descriptor: unknown column '" + key + "'");
        }

        const auto represented = RepresentKeyType(types, type);
        const auto layout = represented.Layout;
        if (represented.Nullable) {
            lookupFields.emplace_back(
                "valid_" + std::to_string(result.Fields.size()),
                types.Make<TBoolType>());
            storedFields.emplace_back(
                "valid_" + std::to_string(result.Fields.size()),
                types.Make<TBoolType>());
            ++offset;
        }
        offset = AlignUp(offset, layout.Alignment);
        result.Fields.push_back({
            .ColumnName = key,
            .ColumnIndex = columnIndex,
            .Type = represented.Inner,
            .LookupType = represented.Lookup,
            .StoredType = represented.Stored,
            .IsNullable = represented.Nullable,
            .Offset = offset,
            .Size = layout.Size,
            .Alignment = layout.Alignment,
        });
        lookupFields.emplace_back(
            "key_" + std::to_string(result.Fields.size() - 1), represented.Lookup);
        storedFields.emplace_back(
            "key_" + std::to_string(result.Fields.size() - 1), represented.Stored);
        offset += layout.Size;
        maxAlignment = std::max(maxAlignment, layout.Alignment);
    }

    result.Alignment = maxAlignment;
    result.Size = AlignUp(offset, result.Alignment);
    std::vector<std::pair<TTypePtr, bool>> keyNameFields;
    keyNameFields.reserve(result.Fields.size());
    for (const auto& field : result.Fields) {
        keyNameFields.emplace_back(field.Type, field.IsNullable);
    }
    result.TypeName = PhysicalKeyTypeName(keyNameFields);
    const bool hasDistinctType = std::any_of(
        result.Fields.begin(), result.Fields.end(),
        [](const auto& field) { return field.LookupType != field.StoredType; });
    if (hasDistinctType) {
        result.LookupTypeName = result.TypeName + "_Lookup";
        result.StoredTypeName = result.TypeName + "_Stored";
        result.LookupType = types.MakeNamed(result.LookupTypeName,
            types.MakeStruct(std::move(lookupFields)));
        result.StoredType = types.MakeNamed(result.StoredTypeName,
            types.MakeStruct(std::move(storedFields)));
    } else {
        auto concrete = types.MakeNamed(result.TypeName,
            types.MakeStruct(std::move(storedFields)));
        result.LookupType = concrete;
        result.StoredType = concrete;
    }
    result.KeyType = result.StoredType;
    return result;
}

} // namespace NQdb::NKernel
