#include <qdb/kernel/aggregate_key.h>
#include <qdb/plan/types/decimal.h>
#include <qdb/plan/types/nullable.h>

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

TLayout TypeLayout(const NQumir::NAst::TTypePtr& original) {
    using namespace NQumir::NAst;

    // Key structs follow the C ABI, which qumir and LLVM share: an i128 and the
    // BinInt struct around it are 16-aligned, so a struct holding one is too.
    if (IsDecimalType(original) || IsBinIntStorageType(original)) {
        return {16, 16};
    }
    auto type = UnwrapNamedType(original);
    if (auto integer = TMaybeType<TIntegerType>(type)) {
        const size_t size = static_cast<size_t>(integer.Cast()->BitWidth() / 8);
        return {size, size};
    }
    if (TMaybeType<TFloatType>(type)) {
        return {8, 8};
    }
    if (TMaybeType<TBoolType>(type)) {
        return {1, 1};
    }
    if (auto structure = TMaybeType<TStructType>(type)) {
        size_t offset = 0;
        size_t maxAlignment = 1;
        for (const auto& [_, fieldType] : structure.Cast()->Fields) {
            const auto field = TypeLayout(fieldType);
            offset = AlignUp(offset, field.Alignment);
            offset += field.Size;
            maxAlignment = std::max(maxAlignment, field.Alignment);
        }
        return {AlignUp(offset, maxAlignment), maxAlignment};
    }
    throw NQumir::TError(
        "aggregation key type is not fixed-width and trivially copyable: " +
        (original ? original->ToString() : std::string("<null>")));
}

NQumir::NAst::TTypePtr StringHandleType(const std::string& name) {
    using namespace NQumir::NAst;
    auto u8 = std::make_shared<TIntegerType>(TIntegerType::U8);
    auto i64 = std::make_shared<TIntegerType>(TIntegerType::I64);
    auto structure = std::make_shared<TStructType>(
        std::vector<std::pair<std::string, TTypePtr>>{
            {"Data", std::make_shared<TPointerType>(u8)},
            {"Size", i64},
        });
    return std::make_shared<TNamedType>(name, std::move(structure));
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
    auto type = UnwrapNamedType(original);
    if (auto integer = TMaybeType<TIntegerType>(type)) {
        return integer.Cast()->ToString();
    }
    if (TMaybeType<TFloatType>(type)) {
        return "f64";
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
        (original ? original->ToString() : std::string("<null>")));
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
        const auto componentType = nullable
            ? std::static_pointer_cast<NQumir::NAst::TType>(
                  std::make_shared<NQdb::TNullable>(type))
            : type;
        name += "_";
        name += PhysicalKeyComponentName(componentType);
    }
    return name;
}

TRepresentedKeyType RepresentKeyType(const NQumir::NAst::TTypePtr& original) {
    using namespace NQumir::NAst;

    const bool nullable = IsNullableType(original);
    auto inner = nullable ? UnwrapNullableType(original) : original;
    if (IsDecimalType(inner) || IsBinIntStorageType(inner)) {
        auto storage = BinIntStorageType();
        return {
            .Lookup = storage,
            .Stored = storage,
            .Layout = TypeLayout(storage),
            .Nullable = nullable,
            .Inner = storage,
        };
    }
    auto type = UnwrapNamedType(inner);
    if (TMaybeType<TStringType>(type)) {
        return {
            .Lookup = StringHandleType("StringView"),
            .Stored = StringHandleType("OwnedString"),
            .Layout = {16, 8},
            .HasString = true,
            .Nullable = nullable,
            .Inner = inner,
        };
    }
    if (TMaybeType<TIntegerType>(type) || TMaybeType<TFloatType>(type) ||
        TMaybeType<TBoolType>(type)) {
        return {
            .Lookup = inner,
            .Stored = inner,
            .Layout = TypeLayout(inner),
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
            auto represented = RepresentKeyType(fieldType);
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
            .Lookup = std::make_shared<TStructType>(std::move(lookupFields)),
            .Stored = std::make_shared<TStructType>(std::move(storedFields)),
            .Layout = {size, maxAlignment},
            .HasString = hasString,
            .Nullable = nullable,
            .Inner = inner,
        };
    }
    throw NQumir::TError(
        "key type is not supported: " +
        (original ? original->ToString() : std::string("<null>")));
}

TAggregateKeyDescriptor BuildAggregateKeyDescriptor(
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
        TTypePtr type;
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

        const auto represented = RepresentKeyType(type);
        const auto layout = represented.Layout;
        if (represented.Nullable) {
            lookupFields.emplace_back(
                "valid_" + std::to_string(result.Fields.size()),
                std::make_shared<TBoolType>());
            storedFields.emplace_back(
                "valid_" + std::to_string(result.Fields.size()),
                std::make_shared<TBoolType>());
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
        result.LookupType = std::make_shared<TNamedType>(result.LookupTypeName,
            std::make_shared<TStructType>(std::move(lookupFields)));
        result.StoredType = std::make_shared<TNamedType>(result.StoredTypeName,
            std::make_shared<TStructType>(std::move(storedFields)));
    } else {
        auto concrete = std::make_shared<TNamedType>(result.TypeName,
            std::make_shared<TStructType>(std::move(storedFields)));
        result.LookupType = concrete;
        result.StoredType = concrete;
    }
    result.KeyType = result.StoredType;
    return result;
}

} // namespace NQdb::NKernel
