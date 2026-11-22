#pragma once

#include <qdb/kernel/aggregate_key.h>

#include <qumir/parser/type.h>
#include <qumir/parser/type_factory.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace NQdb::NKernel {

// One component of a join equi-key. Unlike the aggregation key field, it tracks
// the column on BOTH sides (different names/positions, identical represented
// type), since left and right rows must hash/compare into one shared Key.
struct TJoinKeyField {
    std::string LeftColumnName;
    std::string RightColumnName;
    int32_t LeftColumnIndex = -1;
    int32_t RightColumnIndex = -1;
    NQumir::NAst::TTypePtr Type = nullptr;       // inner (Nullable stripped), shared by both sides
    NQumir::NAst::TTypePtr LookupType = nullptr;
    NQumir::NAst::TTypePtr StoredType = nullptr;
    bool IsNullable = false;           // left.Nullable || right.Nullable (unified)
    bool NullsEqual = false;
    size_t Offset = 0;
    size_t Size = 0;
    size_t Alignment = 0;
};

// Mirrors TAggregateKeyDescriptor, but the Key type is column-name-independent
// (named by component types only) so both sides instantiate the same
// rh_hash/rh_key_equal overloads.
struct TJoinKeyDescriptor {
    std::string TypeName;
    std::string LookupTypeName;
    std::string StoredTypeName;
    NQumir::NAst::TTypePtr KeyType = nullptr; // == StoredType
    NQumir::NAst::TTypePtr LookupType = nullptr;
    NQumir::NAst::TTypePtr StoredType = nullptr;
    std::vector<TJoinKeyField> Fields;
    size_t Size = 0;
    size_t Alignment = 0;

    bool IsScalar() const { return Fields.size() == 1; }
    bool HasDistinctLookupType() const { return LookupType != StoredType; }
};

// keys: parallel (leftColumn, rightColumn) pairs. Reuses RepresentKeyType for
// the per-component representation; throws NQumir::TError if a column is missing
// or the two sides of a key pair have incompatible types.
TJoinKeyDescriptor BuildJoinKeyDescriptor(
    NQumir::NAst::TTypeFactory& types,
    const NQumir::NAst::TStructType& leftType,
    const NQumir::NAst::TStructType& rightType,
    const std::vector<std::pair<std::string, std::string>>& keys);

} // namespace NQdb::NKernel
