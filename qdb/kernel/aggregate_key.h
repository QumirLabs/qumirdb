#pragma once

#include <qumir/parser/type.h>
#include <qumir/parser/type_factory.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace NQdb::NKernel {

// Byte layout of a key (component) type.
struct TKeyLayout {
    size_t Size = 0;
    size_t Alignment = 0;
};

// Lookup/stored representations of a single key column type, shared by the
// aggregation and join key descriptors. For strings the lookup type is a
// non-owning StringView and the stored type an OwnedString; for fixed-width
// and string-free composites Lookup == Stored == Inner.
struct TRepresentedKeyType {
    NQumir::NAst::TTypePtr Lookup = nullptr;
    NQumir::NAst::TTypePtr Stored = nullptr;
    TKeyLayout Layout;
    bool HasString = false;
    bool Nullable = false;
    NQumir::NAst::TTypePtr Inner = nullptr; // type with the Nullable wrapper stripped
};

size_t AlignUp(size_t value, size_t alignment);

// Computes the lookup/stored representation + byte layout of one key column
// type (integer/f64/bool/string/composite). Throws NQumir::TError for
// unsupported types.
TRepresentedKeyType RepresentKeyType(NQumir::NAst::TTypeFactory& types, const NQumir::NAst::TTypePtr& original);

// Shared physical key type name for aggregation and join descriptors. The name
// is structural: component type, nullability, and order only.
std::string PhysicalKeyTypeName(
    const std::vector<std::pair<NQumir::NAst::TTypePtr, bool>>& fields);

struct TAggregateKeyField {
    std::string ColumnName;
    int32_t ColumnIndex = -1;
    NQumir::NAst::TTypePtr Type = nullptr;
    NQumir::NAst::TTypePtr LookupType = nullptr;
    NQumir::NAst::TTypePtr StoredType = nullptr;
    bool IsNullable = false;
    size_t Offset = 0;
    size_t Size = 0;
    size_t Alignment = 0;
};

struct TAggregateKeyDescriptor {
    std::string TypeName;
    std::string LookupTypeName;
    std::string StoredTypeName;
    NQumir::NAst::TTypePtr KeyType = nullptr;
    NQumir::NAst::TTypePtr LookupType = nullptr;
    NQumir::NAst::TTypePtr StoredType = nullptr;
    std::vector<TAggregateKeyField> Fields;
    size_t Size = 0;
    size_t Alignment = 0;

    bool IsScalar() const {
        return Fields.size() == 1;
    }

    bool HasDistinctLookupType() const {
        return LookupType != StoredType;
    }
};

// Builds lookup and stored key representations for generated aggregation kernels.
TAggregateKeyDescriptor BuildAggregateKeyDescriptor(
    NQumir::NAst::TTypeFactory& types,
    const NQumir::NAst::TStructType& inputType,
    const std::vector<std::string>& groupKeys);

} // namespace NQdb::NKernel
