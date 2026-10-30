#include <qdb/kernel/join_key.h>

#include <qumir/error.h>

#include <algorithm>

namespace NQdb::NKernel {

using namespace NQumir::NAst;

namespace {

int32_t FindColumn(const TStructType& schema, const std::string& name, TTypePtr& outType) {
    for (int32_t i = 0; i < static_cast<int32_t>(schema.Fields.size()); ++i) {
        if (schema.Fields[i].first == name) {
            outType = schema.Fields[i].second;
            return i;
        }
    }
    return -1;
}

} // namespace

TJoinKeyDescriptor BuildJoinKeyDescriptor(
    const TStructType& leftType, const TStructType& rightType,
    const std::vector<std::pair<std::string, std::string>>& keys)
{
    if (keys.empty()) {
        throw NQumir::TError("join key descriptor requires at least one key pair");
    }

    TJoinKeyDescriptor result;
    size_t offset = 0;
    size_t maxAlignment = 1;
    std::vector<std::pair<std::string, TTypePtr>> lookupFields;
    std::vector<std::pair<std::string, TTypePtr>> storedFields;

    for (const auto& [leftName, rightName] : keys) {
        TTypePtr leftFieldType;
        TTypePtr rightFieldType;
        const int32_t leftIndex = FindColumn(leftType, leftName, leftFieldType);
        const int32_t rightIndex = FindColumn(rightType, rightName, rightFieldType);
        if (leftIndex < 0) {
            throw NQumir::TError("join key descriptor: unknown left column '" + leftName + "'");
        }
        if (rightIndex < 0) {
            throw NQumir::TError("join key descriptor: unknown right column '" + rightName + "'");
        }

        const auto leftRep = RepresentKeyType(leftFieldType);
        const auto rightRep = RepresentKeyType(rightFieldType);
        // Both sides must produce the same Key bytes to collide in one table.
        if (leftRep.Inner->ToString() != rightRep.Inner->ToString()) {
            throw NQumir::TError(
                "join key types incompatible: left '" + leftName + "' is " +
                leftRep.Inner->ToString() + ", right '" + rightName + "' is " +
                rightRep.Inner->ToString());
        }

        const bool nullable = leftRep.Nullable || rightRep.Nullable;
        const auto layout = leftRep.Layout;
        if (nullable) {
            lookupFields.emplace_back(
                "valid_" + std::to_string(result.Fields.size()), std::make_shared<TBoolType>());
            storedFields.emplace_back(
                "valid_" + std::to_string(result.Fields.size()), std::make_shared<TBoolType>());
            ++offset;
        }
        offset = AlignUp(offset, layout.Alignment);
        result.Fields.push_back({
            .LeftColumnName = leftName,
            .RightColumnName = rightName,
            .LeftColumnIndex = leftIndex,
            .RightColumnIndex = rightIndex,
            .Type = leftRep.Inner,
            .LookupType = leftRep.Lookup,
            .StoredType = leftRep.Stored,
            .IsNullable = nullable,
            .Offset = offset,
            .Size = layout.Size,
            .Alignment = layout.Alignment,
        });
        lookupFields.emplace_back(
            "key_" + std::to_string(result.Fields.size() - 1), leftRep.Lookup);
        storedFields.emplace_back(
            "key_" + std::to_string(result.Fields.size() - 1), leftRep.Stored);
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
