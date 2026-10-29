#include <qdb/kernel/gen.h>
#include <qdb/kernel/builder.h>
#include <qdb/kernel/column_value.h>
#include <qdb/plan/passes/unbound_vars.h>
#include <qdb/plan/types/decimal.h>
#include <qdb/plan/types/nullable.h>

#include <qumir/parser/ast.h>
#include <qumir/parser/operator.h>
#include <qumir/location.h>

#include <algorithm>
#include <functional>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string_view>

namespace NQdb {
namespace NKernel {

namespace {

NQumir::NAst::TTypePtr NamedType(const std::string& name) {
    return std::make_shared<NQumir::NAst::TNamedType>(name, nullptr);
}

NQumir::NAst::TTypePtr ColumnPointerType(
    const NQumir::NAst::TTypePtr& columnType,
    const NQumir::NAst::TTypePtr& rowSetType)
{
    using namespace NQumir::NAst;
    auto rowSetStruct = TMaybeType<TStructType>(UnwrapNamedType(rowSetType));
    if (rowSetStruct) {
        for (const auto& [name, type] : rowSetStruct.Cast()->Fields) {
            if (name == "Columns") {
                return type;
            }
        }
    }
    return std::make_shared<TPointerType>(
        columnType ? columnType : NamedType("TColumn"));
}

NQumir::NAst::TTypePtr PointerPointeeOr(
    const NQumir::NAst::TTypePtr& pointerType,
    const NQumir::NAst::TTypePtr& fallback)
{
    using namespace NQumir::NAst;
    if (auto pointer = TMaybeType<TPointerType>(pointerType)) {
        return pointer.Cast()->PointeeType;
    }
    return fallback ? fallback : NamedType("TColumn");
}

bool IsBinIntValueType(const NQumir::NAst::TTypePtr& type) {
    return IsBinIntStorageType(type) || IsDecimalType(type);
}

NQumir::NAst::TTypePtr AggregateStorageType(
    const NQumir::NAst::TTypePtr& type)
{
    if (IsBinIntValueType(type)) {
        return BinIntStorageType();
    }
    return type;
}

NQumir::NAst::TTypePtr ColumnBufferElementType(
    const NQumir::NAst::TTypePtr& valueType)
{
    using namespace NQumir::NAst;
    // Bitmap buffers are byte-addressed; scalar bool storage is not their layout.
    if (TMaybeType<TBoolType>(UnwrapNamedType(valueType))) {
        return std::make_shared<TIntegerType>(TIntegerType::U8);
    }
    return valueType;
}

int64_t AggStateByteWidth(const TAggReducerInfo& reducer) {
    return reducer.IsBinInt() ? 16 : 8;
}

bool HasStringReducer(const TAggReducerLayout& layout) {
    return std::ranges::any_of(
        layout.Reducers, [](const auto& reducer) { return reducer.IsString(); });
}

std::vector<NQumir::NAst::TExprPtr> StringCleanupCalls(
    const TAggReducerLayout& layout)
{
    using namespace NQumir::NAst;
    auto i64Type = std::make_shared<TIntegerType>();
    std::vector<TExprPtr> result;
    for (const auto& reducer : layout.Reducers) {
        if (reducer.IsString()) {
            result.push_back(NOz::Call("agg_string_cleanup_at", {
                NOz::Ident("ht"), NOz::TypedInt(reducer.ValueBufIdx, i64Type),
                NOz::TypedInt(reducer.ExtraBufIdx, i64Type)}));
        }
    }
    return result;
}

// Rewrites string-column TIdentExpr nodes to their pre-materialized StringView
// variable names (e.g. "col" -> "col_value"). String literals and operator
// comparisons are left as-is - the qumirdb module registers == / != overloads
// for (Named("StringView"), Named("StringView")) and
// (Named("StringView"), TStringType) that the Qumir type-checker resolves.
void SpecializeFilterPredicate(
    NQumir::NAst::TExprPtr& expr,
    const std::unordered_set<std::string>& stringFields,
    const std::unordered_map<std::string, std::string>& stringValues)
{
    using namespace NQumir::NAst;
    if (auto ident = TMaybeNode<TIdentExpr>(expr)) {
        if (stringFields.contains(ident.Cast()->Name)) {
            expr = std::make_shared<TIdentExpr>(
                expr->Location, stringValues.at(ident.Cast()->Name));
        }
        return;
    }
    // cast("lit", StringView) would call qdb_lit_to_sv (strlen) on every row.
    if (auto cast = TMaybeNode<TCastExpr>(expr)) {
        auto literal = TMaybeNode<TStringLiteralExpr>(cast.Cast()->Operand);
        auto target = TMaybeType<TNamedType>(cast.Cast()->Type);
        if (literal && target && target.Cast()->Name == "StringView") {
            const auto loc = expr->Location;
            auto size = std::make_shared<TNumberExpr>(
                loc, static_cast<int64_t>(literal.Cast()->Value.size()));
            size->Type = std::make_shared<TIntegerType>();
            auto data = std::make_shared<TBitcastExpr>(loc, cast.Cast()->Operand,
                std::make_shared<TPointerType>(
                    std::make_shared<TIntegerType>(TIntegerType::U8)));
            expr = std::make_shared<TCallExpr>(loc,
                std::make_shared<TIdentExpr>(loc, "qdb_sv_from_ptr"),
                std::vector<TExprPtr>{std::move(data), std::move(size)});
            return;
        }
    }
    for (auto* child : expr->MutableChildren()) {
        SpecializeFilterPredicate(*child, stringFields, stringValues);
    }
}

NQumir::NAst::TIntegerType::EKind UnsignedIntegerKind(
    NQumir::NAst::TIntegerType::EKind kind)
{
    using TIntegerType = NQumir::NAst::TIntegerType;
    switch (kind) {
        case TIntegerType::I8:
        case TIntegerType::U8:
            return TIntegerType::U8;
        case TIntegerType::I16:
        case TIntegerType::U16:
            return TIntegerType::U16;
        case TIntegerType::I32:
        case TIntegerType::U32:
            return TIntegerType::U32;
        case TIntegerType::I64:
        case TIntegerType::U64:
            return TIntegerType::U64;
        case TIntegerType::I128:
        case TIntegerType::U128:
            return TIntegerType::U128;
    }
    throw std::invalid_argument("unsupported integer kind");
}

NQumir::NAst::TExprPtr KeyValueExpr(
    const std::string& root,
    const std::vector<std::string>& path)
{
    using namespace NQumir::NAst;
    NQumir::TLocation loc{};
    TExprPtr result = std::make_shared<TIdentExpr>(loc, root);
    for (const auto& field : path) {
        result = std::make_shared<TFieldAccessExpr>(loc, std::move(result), field);
    }
    return result;
}

NQumir::NAst::TExprPtr CanonicalFloatBits(
    const std::string& root,
    const std::vector<std::string>& path)
{
    using namespace NQumir::NAst;
    NQumir::TLocation loc{};
    auto u64Type = std::make_shared<TIntegerType>(TIntegerType::U64);
    auto number = [&](int64_t value) -> TExprPtr {
        auto result = std::make_shared<TNumberExpr>(loc, value);
        result->Type = u64Type;
        return result;
    };
    auto binary = [&](const char* op, TExprPtr left, TExprPtr right) -> TExprPtr {
        return std::make_shared<TBinaryExpr>(
            loc, TOperator(op), std::move(left), std::move(right));
    };
    auto bitcast = [&](TExprPtr e, TTypePtr type) -> TExprPtr {
        return std::make_shared<TBitcastExpr>(loc,
            std::move(e),
            std::move(type));
    };
    auto masked = [&](int64_t mask) -> TExprPtr {
        return binary("&", bitcast(KeyValueExpr(root, path), u64Type), number(mask));
    };
    auto isZero = binary("==", masked(0x7fffffffffffffffLL), number(0));
    auto isNaN = binary("&&",
        binary("==", masked(0x7ff0000000000000LL),
            number(0x7ff0000000000000LL)),
        binary("!=", masked(0x000fffffffffffffLL), number(0)));
    return std::make_shared<TIfExpr>(loc, std::move(isZero), number(0),
        std::make_shared<TIfExpr>(loc, std::move(isNaN),
            number(0x7ff8000000000000LL), bitcast(KeyValueExpr(root, path), u64Type)));
}

NQumir::NAst::TExprPtr HashKeyValue(
    const NQumir::NAst::TTypePtr& originalType,
    const std::string& root,
    std::vector<std::string>& path,
    std::vector<NQumir::NAst::TExprPtr>& body,
    size_t& nextTemporary)
{
    using namespace NQumir::NAst;
    NQumir::TLocation loc{};
    auto named = TMaybeType<TNamedType>(originalType);
    if (named && (named.Cast()->Name == "StringView" ||
                  named.Cast()->Name == "OwnedString")) {
        return std::make_shared<TCastExpr>(loc,
            std::make_shared<TCallExpr>(loc,
                std::make_shared<TIdentExpr>(loc, "qdb_string_hash"),
                std::vector<TExprPtr>{KeyValueExpr(root, path)}),
            std::make_shared<TIntegerType>(TIntegerType::U64));
    }
    auto u64Type = std::make_shared<TIntegerType>(TIntegerType::U64);
    auto ident = [&](const std::string& name) -> TExprPtr {
        return std::make_shared<TIdentExpr>(loc, name);
    };
    auto number = [&](int64_t value) -> TExprPtr {
        auto result = std::make_shared<TNumberExpr>(loc, value);
        result->Type = u64Type;
        return result;
    };
    auto binary = [&](const char* op, TExprPtr left, TExprPtr right) -> TExprPtr {
        return std::make_shared<TBinaryExpr>(
            loc, TOperator(op), std::move(left), std::move(right));
    };
    auto assign = [&](const std::string& name, TExprPtr value) {
        body.push_back(std::make_shared<TAssignExpr>(
            loc, name, std::move(value)));
    };
    auto mixU64 = [&](const std::string& name) {
        assign(name, binary("^", ident(name),
            binary(">>", ident(name), number(12))));
        assign(name, binary("^", ident(name),
            binary("<<", ident(name), number(25))));
        assign(name, binary("^", ident(name),
            binary(">>", ident(name), number(27))));
        assign(name, binary("*", ident(name), number(2685821657736338717LL)));
    };

    if (IsBinIntStorageType(originalType)) {
        const std::string loName = "key_hash_" + std::to_string(nextTemporary++);
        const std::string hiName = "key_hash_" + std::to_string(nextTemporary++);
        const std::string name = "key_hash_" + std::to_string(nextTemporary++);
        body.push_back(std::make_shared<TVarStmt>(loc, loName, u64Type));
        body.push_back(std::make_shared<TVarStmt>(loc, hiName, u64Type));
        body.push_back(std::make_shared<TVarStmt>(loc, name, u64Type));
        auto i128Type = std::make_shared<TIntegerType>(TIntegerType::I128);
        auto raw = [&]() -> TExprPtr {
            return std::make_shared<TFieldAccessExpr>(
                loc, KeyValueExpr(root, path), "V");
        };
        auto shift64 = std::make_shared<TNumberExpr>(loc, int64_t{64});
        shift64->Type = i128Type;
        assign(loName, std::make_shared<TCastExpr>(loc, raw(), u64Type));
        assign(hiName, std::make_shared<TCastExpr>(loc,
            std::make_shared<TBinaryExpr>(loc, TOperator(">>"),
                raw(), std::move(shift64)),
            u64Type));
        mixU64(loName);
        mixU64(hiName);
        auto combined = binary("+", ident(hiName), number(-7046029254386353131LL));
        combined = binary("+", std::move(combined),
            binary("<<", ident(loName), number(6)));
        combined = binary("+", std::move(combined),
            binary(">>", ident(loName), number(2)));
        assign(name, binary("^", ident(loName), std::move(combined)));
        return ident(name);
    }

    const auto type = UnwrapNamedType(originalType);

    if (auto integer = TMaybeType<TIntegerType>(type)) {
        const std::string name = "key_hash_" + std::to_string(nextTemporary++);
        body.push_back(std::make_shared<TVarStmt>(loc, name, u64Type));
        auto unsignedType = std::make_shared<TIntegerType>(
            UnsignedIntegerKind(integer.Cast()->Kind));
        auto bits = std::make_shared<TCastExpr>(
            loc, KeyValueExpr(root, path), std::move(unsignedType));
        assign(name, std::make_shared<TCastExpr>(loc, std::move(bits), u64Type));
        mixU64(name);
        return ident(name);
    }

    if (TMaybeType<TBoolType>(type)) {
        const std::string name = "key_hash_" + std::to_string(nextTemporary++);
        body.push_back(std::make_shared<TVarStmt>(loc, name, u64Type));
        assign(name, std::make_shared<TCastExpr>(loc,
            KeyValueExpr(root, path), u64Type));
        assign(name, binary("*", ident(name), number(2685821657736338717LL)));
        return ident(name);
    }

    if (TMaybeType<TFloatType>(type)) {
        const std::string name = "key_hash_" + std::to_string(nextTemporary++);
        body.push_back(std::make_shared<TVarStmt>(loc, name, u64Type));
        assign(name, CanonicalFloatBits(root, path));
        mixU64(name);
        return ident(name);
    }

    if (auto structure = TMaybeType<TStructType>(type)) {
        const std::string name = "key_hash_" + std::to_string(nextTemporary++);
        body.push_back(std::make_shared<TVarStmt>(loc, name, u64Type));
        assign(name, number(0));
        for (const auto& [fieldName, fieldType] : structure.Cast()->Fields) {
            path.push_back(fieldName);
            auto fieldHash = HashKeyValue(
                fieldType, root, path, body, nextTemporary);
            path.pop_back();
            const std::string validityName = fieldName.starts_with("key_")
                ? "valid_" + fieldName.substr(std::string("key_").size())
                : std::string{};
            const bool hasValidity = !validityName.empty() && std::ranges::any_of(
                structure.Cast()->Fields,
                [&](const auto& field) { return field.first == validityName; });
            if (hasValidity) {
                auto validPath = path;
                validPath.push_back(validityName);
                fieldHash = std::make_shared<TIfExpr>(loc,
                    KeyValueExpr(root, validPath), std::move(fieldHash), number(0));
            }
            // boost-style ordered combine over already mixed field hashes.
            auto combined = binary("+", std::move(fieldHash), number(-7046029254386353131LL));
            combined = binary("+", std::move(combined),
                binary("<<", ident(name), number(6)));
            combined = binary("+", std::move(combined),
                binary(">>", ident(name), number(2)));
            assign(name, binary("^", ident(name), std::move(combined)));
        }
        return ident(name);
    }

    throw std::invalid_argument(
        "GenKeyOperationFunDecls: unsupported key leaf type " +
        (originalType ? originalType->ToString() : std::string("<null>")));
}

NQumir::NAst::TExprPtr EqualKeyValue(
    const NQumir::NAst::TTypePtr& leftType,
    const NQumir::NAst::TTypePtr& rightType,
    std::vector<std::string>& path)
{
    using namespace NQumir::NAst;
    NQumir::TLocation loc{};
    auto leftNamed = TMaybeType<TNamedType>(leftType);
    auto rightNamed = TMaybeType<TNamedType>(rightType);
    const bool leftString = leftNamed &&
        (leftNamed.Cast()->Name == "StringView" ||
         leftNamed.Cast()->Name == "OwnedString");
    const bool rightString = rightNamed &&
        (rightNamed.Cast()->Name == "StringView" ||
         rightNamed.Cast()->Name == "OwnedString");
    auto binary = [&](const char* op, TExprPtr left, TExprPtr right) -> TExprPtr {
        return std::make_shared<TBinaryExpr>(
            loc, TOperator(op), std::move(left), std::move(right));
    };
    if (leftString || rightString) {
        if (!leftString || !rightString) {
            throw std::invalid_argument(
                "GenKeyOperationFunDecls: mismatched string key leaves");
        }
        return std::make_shared<TCallExpr>(loc,
            std::make_shared<TIdentExpr>(loc, "qdb_string_equal"),
            std::vector<TExprPtr>{
                KeyValueExpr("left", path), KeyValueExpr("right", path)});
    }
    if (IsBinIntStorageType(leftType) || IsBinIntStorageType(rightType)) {
        if (!IsBinIntStorageType(leftType) || !IsBinIntStorageType(rightType)) {
            throw std::invalid_argument(
                "GenKeyOperationFunDecls: mismatched BinInt key leaves");
        }
        return binary("==", KeyValueExpr("left", path), KeyValueExpr("right", path));
    }
    const auto left = UnwrapNamedType(leftType);
    const auto right = UnwrapNamedType(rightType);
    if (TMaybeType<TIntegerType>(left) && TMaybeType<TIntegerType>(right)) {
        return binary("==", KeyValueExpr("left", path), KeyValueExpr("right", path));
    }
    if (TMaybeType<TBoolType>(left) && TMaybeType<TBoolType>(right)) {
        return binary("==", KeyValueExpr("left", path), KeyValueExpr("right", path));
    }
    if (TMaybeType<TFloatType>(left) && TMaybeType<TFloatType>(right)) {
        return binary("==",
            CanonicalFloatBits("left", path), CanonicalFloatBits("right", path));
    }
    if (auto leftStruct = TMaybeType<TStructType>(left)) {
        auto rightStruct = TMaybeType<TStructType>(right);
        if (!rightStruct) {
            throw std::invalid_argument(
                "GenKeyOperationFunDecls: mismatched struct key leaves");
        }
        TExprPtr result;
        for (const auto& [fieldName, fieldType] : leftStruct.Cast()->Fields) {
            auto rightField = std::find_if(
                rightStruct.Cast()->Fields.begin(), rightStruct.Cast()->Fields.end(),
                [&](const auto& field) { return field.first == fieldName; });
            if (rightField == rightStruct.Cast()->Fields.end()) {
                throw std::invalid_argument(
                    "GenKeyOperationFunDecls: mismatched struct key fields");
            }
            path.push_back(fieldName);
            auto fieldEqual = EqualKeyValue(fieldType, rightField->second, path);
            path.pop_back();
            const std::string validityName = fieldName.starts_with("key_")
                ? "valid_" + fieldName.substr(std::string("key_").size())
                : std::string{};
            const bool hasValidity = !validityName.empty() && std::ranges::any_of(
                leftStruct.Cast()->Fields,
                [&](const auto& field) { return field.first == validityName; });
            if (hasValidity) {
                auto validPath = path;
                validPath.push_back(validityName);
                fieldEqual = binary("||",
                    std::make_shared<TUnaryExpr>(loc, TOperator("!"),
                        KeyValueExpr("left", validPath)),
                    std::move(fieldEqual));
            }
            result = result
                ? binary("&&", std::move(result), std::move(fieldEqual))
                : std::move(fieldEqual);
        }
        if (!result) {
            throw std::invalid_argument(
                "GenKeyOperationFunDecls: empty struct keys are not supported");
        }
        return result;
    }
    throw std::invalid_argument(
        "GenKeyOperationFunDecls: unsupported equality leaf type " +
        (leftType ? leftType->ToString() : std::string("<null>")));
}

bool IsStringHandleType(const NQumir::NAst::TTypePtr& type) {
    auto named = NQumir::NAst::TMaybeType<NQumir::NAst::TNamedType>(type);
    return named && (named.Cast()->Name == "StringView" ||
                     named.Cast()->Name == "OwnedString");
}

NQumir::NAst::TTypePtr FindStringViewType(
    const NQumir::NAst::TTypePtr& originalType)
{
    using namespace NQumir::NAst;
    if (auto named = TMaybeType<TNamedType>(originalType)) {
        if (named.Cast()->Name == "StringView") {
            return named.Cast()->UnderlyingType;
        }
    }
    auto type = UnwrapNamedType(originalType);
    if (auto structure = TMaybeType<TStructType>(type)) {
        for (const auto& [_, fieldType] : structure.Cast()->Fields) {
            if (auto result = FindStringViewType(fieldType)) {
                return result;
            }
        }
    }
    return nullptr;
}

bool ContainsLogicalString(const NQumir::NAst::TTypePtr& originalType) {
    using namespace NQumir::NAst;
    auto type = UnwrapNamedType(originalType);
    if (TMaybeType<TStringType>(type)) {
        return true;
    }
    if (auto structure = TMaybeType<TStructType>(type)) {
        return std::any_of(structure.Cast()->Fields.begin(),
            structure.Cast()->Fields.end(), [](const auto& field) {
                return ContainsLogicalString(field.second);
            });
    }
    return false;
}

NQumir::NAst::TExprPtr KeyOwnedBytesExpr(
    const NQumir::NAst::TTypePtr& originalType,
    const std::string& root,
    std::vector<std::string>& path)
{
    using namespace NQumir::NAst;
    NQumir::TLocation loc{};
    auto i64Type = std::make_shared<TIntegerType>(TIntegerType::I64);
    auto zero = [&]() -> TExprPtr {
        auto value = std::make_shared<TNumberExpr>(loc, int64_t{0});
        value->Type = i64Type;
        return value;
    };
    if (IsStringHandleType(originalType)) {
        return std::make_shared<TFieldAccessExpr>(
            loc, KeyValueExpr(root, path), "Size");
    }
    if (IsBinIntStorageType(originalType)) {
        return zero();
    }
    auto type = UnwrapNamedType(originalType);
    if (TMaybeType<TIntegerType>(type) || TMaybeType<TFloatType>(type) ||
        TMaybeType<TBoolType>(type)) {
        return zero();
    }
    if (auto structure = TMaybeType<TStructType>(type)) {
        TExprPtr result = zero();
        for (const auto& [fieldName, fieldType] : structure.Cast()->Fields) {
            path.push_back(fieldName);
            auto fieldBytes = KeyOwnedBytesExpr(fieldType, root, path);
            path.pop_back();
            const std::string validityName = fieldName.starts_with("key_")
                ? "valid_" + fieldName.substr(std::string("key_").size())
                : std::string{};
            const bool hasValidity = !validityName.empty() && std::ranges::any_of(
                structure.Cast()->Fields,
                [&](const auto& field) { return field.first == validityName; });
            if (hasValidity) {
                auto validPath = path;
                validPath.push_back(validityName);
                fieldBytes = std::make_shared<TIfExpr>(loc,
                    KeyValueExpr(root, validPath), std::move(fieldBytes), zero());
            }
            result = std::make_shared<TBinaryExpr>(loc, TOperator("+"),
                std::move(result), std::move(fieldBytes));
        }
        return result;
    }
    throw std::invalid_argument(
        "GenKeyOwnershipFunDecls: unsupported key leaf type " +
        (originalType ? originalType->ToString() : std::string("<null>")));
}

NQumir::NAst::TExprPtr CloneKeyValue(
    const NQumir::NAst::TTypePtr& lookupType,
    const NQumir::NAst::TTypePtr& storedType,
    const std::string& root,
    std::vector<std::string>& path,
    std::vector<NQumir::NAst::TExprPtr>& body,
    size_t& nextTemporary)
{
    using namespace NQumir::NAst;
    NQumir::TLocation loc{};
    auto i64Type = std::make_shared<TIntegerType>(TIntegerType::I64);
    auto u8Type = std::make_shared<TIntegerType>(TIntegerType::U8);
    auto ptrU8Type = std::make_shared<TPointerType>(u8Type);
    auto ident = [&](const std::string& name) -> TExprPtr {
        return std::make_shared<TIdentExpr>(loc, name);
    };
    auto field = [&](const std::string& name) -> TExprPtr {
        return std::make_shared<TFieldAccessExpr>(
            loc, KeyValueExpr(root, path), name);
    };

    if (IsStringHandleType(lookupType) && IsStringHandleType(storedType)) {
        const std::string dataName =
            "owned_data_" + std::to_string(nextTemporary++);
        const std::string valueName =
            "owned_value_" + std::to_string(nextTemporary++);
        const std::string copyName =
            "owned_copy_" + std::to_string(nextTemporary++);
        body.push_back(std::make_shared<TVarStmt>(loc, dataName, ptrU8Type));
        auto address = std::make_shared<TBinaryExpr>(loc, TOperator("+"),
            std::make_shared<TCastExpr>(loc, ident("owned_buffer"), i64Type),
            ident("owned_offset"));
        body.push_back(std::make_shared<TAssignExpr>(loc, dataName,
            std::make_shared<TCastExpr>(loc, std::move(address), ptrU8Type)));
        body.push_back(std::make_shared<TVarStmt>(loc, copyName, i64Type));
        body.push_back(std::make_shared<TAssignExpr>(loc, copyName,
            std::make_shared<TCallExpr>(loc,
                ident("qdb_string_copy_bytes"),
                std::vector<TExprPtr>{
                    ident(dataName), field("Data"), field("Size")})));
        body.push_back(std::make_shared<TVarStmt>(loc, valueName, storedType));
        body.push_back(std::make_shared<TAssignExpr>(loc, valueName,
            std::make_shared<TStructConstructExpr>(loc, storedType,
                std::vector<TExprPtr>{ident(dataName), field("Size")})));
        body.push_back(std::make_shared<TAssignExpr>(loc, "owned_offset",
            std::make_shared<TBinaryExpr>(loc, TOperator("+"),
                ident("owned_offset"), field("Size"))));
        return ident(valueName);
    }

    auto lookup = UnwrapNamedType(lookupType);
    auto stored = UnwrapNamedType(storedType);
    if (IsBinIntStorageType(lookupType) && IsBinIntStorageType(storedType)) {
        return KeyValueExpr(root, path);
    }
    if ((TMaybeType<TIntegerType>(lookup) && TMaybeType<TIntegerType>(stored)) ||
        (TMaybeType<TFloatType>(lookup) && TMaybeType<TFloatType>(stored)) ||
        (TMaybeType<TBoolType>(lookup) && TMaybeType<TBoolType>(stored))) {
        return KeyValueExpr(root, path);
    }
    if (auto storedStruct = TMaybeType<TStructType>(stored)) {
        auto lookupStruct = TMaybeType<TStructType>(lookup);
        if (!lookupStruct) {
            throw std::invalid_argument(
                "GenKeyOwnershipFunDecls: mismatched struct key types");
        }
        std::vector<TExprPtr> fields;
        fields.reserve(storedStruct.Cast()->Fields.size());
        for (const auto& [fieldName, fieldType] : storedStruct.Cast()->Fields) {
            auto lookupField = std::find_if(
                lookupStruct.Cast()->Fields.begin(), lookupStruct.Cast()->Fields.end(),
                [&](const auto& item) { return item.first == fieldName; });
            if (lookupField == lookupStruct.Cast()->Fields.end()) {
                throw std::invalid_argument(
                    "GenKeyOwnershipFunDecls: mismatched struct key fields");
            }
            path.push_back(fieldName);
            fields.push_back(CloneKeyValue(lookupField->second, fieldType,
                root, path, body, nextTemporary));
            path.pop_back();
        }
        return std::make_shared<TStructConstructExpr>(
            loc, storedType, std::move(fields));
    }
    throw std::invalid_argument(
        "GenKeyOwnershipFunDecls: unsupported clone type " +
        (lookupType ? lookupType->ToString() : std::string("<null>")));
}

} // namespace

NQumir::NAst::TExprPtr ZeroValueExpr(
    const NQumir::NAst::TTypePtr& originalType)
{
    using namespace NQumir::NAst;
    NQumir::TLocation loc{};
    auto i64Type = std::make_shared<TIntegerType>(TIntegerType::I64);
    auto u64Type = std::make_shared<TIntegerType>(TIntegerType::U64);
    auto zero = std::make_shared<TNumberExpr>(loc, int64_t{0});
    zero->Type = i64Type;
    if (IsBinIntStorageType(originalType)) {
        auto zeroI128 = std::make_shared<TNumberExpr>(loc, int64_t{0});
        zeroI128->Type = std::make_shared<TIntegerType>(TIntegerType::I128);
        return std::make_shared<TStructConstructExpr>(loc, originalType,
            std::vector<TExprPtr>{std::move(zeroI128)},
            std::vector<std::string>{"V"});
    }
    auto type = UnwrapNamedType(originalType);
    if (TMaybeType<TIntegerType>(type)) {
        auto value = std::make_shared<TNumberExpr>(loc, int64_t{0});
        value->Type = originalType;
        return value;
    }
    if (TMaybeType<TFloatType>(type) || TMaybeType<TBoolType>(type) ||
        TMaybeType<TPointerType>(type)) {
        return std::make_shared<TCastExpr>(loc, std::move(zero), originalType);
    }
    if (auto structure = TMaybeType<TStructType>(type)) {
        std::vector<TExprPtr> fields;
        fields.reserve(structure.Cast()->Fields.size());
        for (const auto& [_, fieldType] : structure.Cast()->Fields) {
            fields.push_back(ZeroValueExpr(fieldType));
        }
        return std::make_shared<TStructConstructExpr>(
            loc, originalType, std::move(fields));
    }
    throw std::invalid_argument(
        "ZeroValueExpr: unsupported type " +
        (originalType ? originalType->ToString() : std::string("<null>")));
}

std::vector<NQumir::NAst::TExprPtr> GenKeyOperationFunDecls(
    const TAggregateKeyDescriptor& key)
{
    using namespace NQumir::NAst;
    NQumir::TLocation loc{};

    auto i64Type = std::make_shared<TIntegerType>(TIntegerType::I64);
    auto boolType = std::make_shared<TBoolType>();

    auto makeHash = [&](const TTypePtr& type) -> TExprPtr {
        std::vector<TExprPtr> body;
        size_t nextTemporary = 0;
        std::vector<std::string> path;
        auto value = HashKeyValue(type, "key", path, body, nextTemporary);
        body.push_back(std::make_shared<TReturnExpr>(loc,
            std::make_shared<TCastExpr>(loc, std::move(value), i64Type)));
        std::vector<TParam> params = {
            std::make_shared<TVarStmt>(loc, "key", type),
        };
        auto function = std::make_shared<TFunDecl>(loc, "rh_hash", std::vector<TGenericParam>{}, std::move(params),
            std::make_shared<TBlockExpr>(loc, std::move(body)), i64Type);
        function->Type = std::make_shared<TFunctionType>(
            std::vector<TTypePtr>{type}, i64Type);
        function->Cacheable = true;
        return function;
    };
    auto makeEqual = [&](const TTypePtr& leftType,
                         const TTypePtr& rightType) -> TExprPtr {
        std::vector<TParam> params = {
            std::make_shared<TVarStmt>(loc, "left", leftType),
            std::make_shared<TVarStmt>(loc, "right", rightType),
        };
        std::vector<std::string> path;
        std::vector<TExprPtr> body = {
            std::make_shared<TReturnExpr>(
                loc, EqualKeyValue(leftType, rightType, path)),
        };
        auto function = std::make_shared<TFunDecl>(loc, "rh_key_equal",
            std::vector<TGenericParam>{}, std::move(params), std::make_shared<TBlockExpr>(loc, std::move(body)),
            boolType);
        function->Type = std::make_shared<TFunctionType>(
            std::vector<TTypePtr>{leftType, rightType}, boolType);
        function->Cacheable = true;
        return function;
    };

    std::vector<TExprPtr> result;
    result.push_back(makeHash(key.LookupType));
    if (key.HasDistinctLookupType()) {
        result.push_back(makeHash(key.StoredType));
        result.push_back(makeEqual(key.StoredType, key.LookupType));
        // The row join table stores lookup keys (StringView handles into the
        // retained batch) and compares them with probe lookup keys.
        result.push_back(makeEqual(key.LookupType, key.LookupType));
    }
    result.push_back(makeEqual(key.StoredType, key.StoredType));
    return result;
}

std::vector<NQumir::NAst::TExprPtr> GenKeyOwnershipFunDecls(
    const TAggregateKeyDescriptor& key)
{
    using namespace NQumir::NAst;
    NQumir::TLocation loc{};
    auto i64Type = std::make_shared<TIntegerType>(TIntegerType::I64);
    auto u8Type = std::make_shared<TIntegerType>(TIntegerType::U8);
    auto ptrU8Type = std::make_shared<TPointerType>(u8Type);

    std::vector<std::string> path;
    auto bytesExpr = KeyOwnedBytesExpr(key.LookupType, "key", path);
    std::vector<TParam> bytesParams = {
        std::make_shared<TVarStmt>(loc, "key", key.LookupType),
    };
    auto bytes = std::make_shared<TFunDecl>(loc, "key_owned_bytes",
        std::vector<TGenericParam>{},
        std::move(bytesParams), std::make_shared<TBlockExpr>(loc,
            std::vector<TExprPtr>{
                std::make_shared<TReturnExpr>(loc, std::move(bytesExpr))}),
        i64Type);
    bytes->Type = std::make_shared<TFunctionType>(
        std::vector<TTypePtr>{key.LookupType}, i64Type);
    bytes->Cacheable = true;

    std::vector<TExprPtr> cloneBody;
    TExprPtr cloneValue;
    if (key.HasDistinctLookupType()) {
        auto offset = std::make_shared<TVarStmt>(loc, "owned_offset", i64Type);
        offset->Init = std::make_shared<TNumberExpr>(loc, int64_t{0});
        cloneBody.push_back(std::move(offset));
        size_t nextTemporary = 0;
        path.clear();
        cloneValue = CloneKeyValue(key.LookupType, key.StoredType,
            "key", path, cloneBody, nextTemporary);
    } else {
        cloneValue = std::make_shared<TIdentExpr>(loc, "key");
    }
    cloneBody.push_back(
        std::make_shared<TReturnExpr>(loc, std::move(cloneValue)));
    std::vector<TParam> cloneParams = {
        std::make_shared<TVarStmt>(loc, "key", key.LookupType),
        std::make_shared<TVarStmt>(loc, "owned_buffer", ptrU8Type),
    };
    auto clone = std::make_shared<TFunDecl>(loc, "key_clone_owned",
        std::vector<TGenericParam>{},
        std::move(cloneParams),
        std::make_shared<TBlockExpr>(loc, std::move(cloneBody)), key.StoredType);
    clone->Type = std::make_shared<TFunctionType>(
        std::vector<TTypePtr>{key.LookupType, ptrU8Type}, key.StoredType);
    clone->Cacheable = true;

    return {std::move(bytes), std::move(clone)};
}

void SubstFieldsInPlace(
    NQumir::NAst::TExprPtr& expr,
    const std::unordered_map<std::string, std::string>& fieldValues)
{
    if (!expr) {
        return;
    }
    if (auto node = NQumir::NAst::TMaybeNode<NQumir::NAst::TIdentExpr>(expr)) {
        if (auto it = fieldValues.find(node.Cast()->Name);
            it != fieldValues.end()) {
            expr = std::make_shared<NQumir::NAst::TIdentExpr>(
                expr->Location, it->second);
            return;
        }
        return;
    }
    for (auto* child : expr->MutableChildren()) {
        SubstFieldsInPlace(*child, fieldValues);
    }
}

bool UsesNullableValue(
    const NQumir::NAst::TExprPtr& expr,
    const std::unordered_map<std::string, std::string>& validityNames)
{
    using namespace NQumir::NAst;
    if (!expr) {
        return false;
    }
    if (auto ident = TMaybeNode<TIdentExpr>(expr)) {
        return validityNames.contains(ident.Cast()->Name);
    }
    return std::ranges::any_of(expr->Children(), [&](const auto& child) {
        return UsesNullableValue(child, validityNames);
    });
}

bool IsNullableBoolType(const NQumir::NAst::TTypePtr& type) {
    using namespace NQumir::NAst;
    auto named = TMaybeType<TNamedType>(type);
    if (!named || named.Cast()->Name != "Nullable" ||
        named.Cast()->TypeArgs.size() != 1)
    {
        return false;
    }
    const auto& arg = named.Cast()->TypeArgs.front();
    return arg.Kind == TGenericArg::EKind::Type &&
        static_cast<bool>(TMaybeType<TBoolType>(arg.Type));
}

bool HasNullableBoolCast(const NQumir::NAst::TExprPtr& expr) {
    using namespace NQumir::NAst;
    if (!expr) {
        return false;
    }
    if (TMaybeNode<TCastExpr>(expr) && IsNullableBoolType(expr->Type)) {
        return true;
    }
    return std::ranges::any_of(expr->Children(), [](const auto& child) {
        return HasNullableBoolCast(child);
    });
}

// qumirdb.oz's Nullable[T] = <struct (Value T) (Valid bool)>. Wrap a materialized
// value + validity into that struct so the .oz nullable operators apply.
NQumir::NAst::TTypePtr NullableNamedType(const NQumir::NAst::TTypePtr& valueType) {
    using namespace NQumir::NAst;
    auto structType = std::make_shared<TStructType>(
        std::vector<std::pair<std::string, TTypePtr>>{
            {"Value", valueType}, {"Valid", std::make_shared<TBoolType>()}});
    return std::make_shared<TNamedType>("Nullable", structType,
        std::vector<TGenericArg>{TGenericArg::TypeArg(valueType)});
}

NQumir::NAst::TExprPtr BuildNullableStruct(
    NQumir::NAst::TExprPtr value,
    NQumir::NAst::TExprPtr valid,
    const NQumir::NAst::TTypePtr& valueType)
{
    using namespace NQumir::NAst;
    NQumir::TLocation loc{};
    // Mirror the .oz operators: (cast (struct ((Value v) (Valid b))) <named Nullable[T]>).
    // A cast constructs the named struct; a bare `(:)` annotation is weaker and leaves
    // operator resolution unable to pick the nullable overloads.
    auto anonStruct = std::make_shared<TStructType>(
        std::vector<std::pair<std::string, TTypePtr>>{
            {"Value", valueType}, {"Valid", std::make_shared<TBoolType>()}});
    auto structLit = std::make_shared<TStructConstructExpr>(loc, anonStruct,
        std::vector<TExprPtr>{std::move(value), std::move(valid)},
        std::vector<std::string>{"Value", "Valid"});
    return std::make_shared<TCastExpr>(loc, structLit, NullableNamedType(valueType));
}

// Kernel takes (ref TRowSet) directly. Column data pointers are extracted via
// TRowSet.Columns[colIdx].Data with a two-step cast: <ptr i8> -> i64 -> <ptr T>.
NQumir::NAst::TExprPtr GenFilterKernelAst(
    NQumir::NAst::TExprPtr predicate,
    const NQumir::NAst::TStructType& inputType,
    const std::unordered_map<std::string, int32_t>& fieldIndices,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr stringViewType,
    std::vector<std::shared_ptr<std::string>>& /*literalStorage*/)
{
    using namespace NQumir::NAst;
    NQumir::TLocation loc{};

    // Only columns the predicate actually reads need materializing. This mirrors
    // TFilterOperator::ComputeReferencedColumns (FindUnboundVars over the
    // predicate). Skipping unread columns avoids wasted work and, for string
    // columns, a dead StringView build the wasm -O3 backend miscompiles.
    const auto referenced = FindUnboundVars(predicate);

    std::unordered_map<std::string, std::string> fixedValues;
    std::unordered_set<std::string> stringFields;
    std::unordered_map<std::string, std::string> stringValues;
    for (const auto& [name, type] : inputType.Fields) {
        if (TMaybeType<TStringType>(
                UnwrapNamedType(UnwrapNullableType(type)))) {
            stringFields.insert(name);
            stringValues.emplace(name, name + "_value");
        } else {
            fixedValues.emplace(name, name + "_value");
        }
    }

    SpecializeFilterPredicate(
        predicate, stringFields, stringValues);
    SubstFieldsInPlace(predicate, fixedValues);

    // Single param: (var rowSet <ref TRowSet>) - raw struct type, no TNamedType wrapper
    auto rowSetRefType = std::make_shared<TReferenceType>(rowSetType);

    std::vector<TExprPtr> bodyStmts;
    auto identRowSet = NOz::Ident("rowSet");

    auto fieldOf = [&](const std::string& name) {
        return std::make_shared<TFieldAccessExpr>(loc, identRowSet, name);
    };

    // Extract n, selection, cols from rowSet
    bodyStmts.push_back(NOz::Var("n", std::make_shared<TIntegerType>()));
    bodyStmts.push_back(NOz::Assign("n", fieldOf("RowCount")));

    auto ptrU8Type = std::make_shared<TPointerType>(
        std::make_shared<TIntegerType>(TIntegerType::U8));
    bodyStmts.push_back(NOz::Var("selection", ptrU8Type));
    bodyStmts.push_back(NOz::Assign("selection", fieldOf("Selection")));

    auto ptrColumnType = ColumnPointerType(columnType, rowSetType);
    auto columnValueType = PointerPointeeOr(ptrColumnType, columnType);
    bodyStmts.push_back(NOz::Var("cols", ptrColumnType));
    bodyStmts.push_back(NOz::Assign("cols", fieldOf("Columns")));

    // Bind every input field through the common nullable column materializer.
    std::vector<TExprPtr> loopSetup;
    std::unordered_map<std::string, std::string> validityNames;
    for (const auto& [name, type] : inputType.Fields) {
        if (!referenced.contains(name)) {
            continue;
        }
        int32_t idx = fieldIndices.at(name);
        auto colElem = std::make_shared<TIndexExpr>(loc,
            std::make_shared<TIdentExpr>(loc, "cols"),
            std::make_shared<TNumberExpr>(loc, int64_t(idx)));
        bodyStmts.push_back(NOz::Var(name, columnValueType));
        bodyStmts.push_back(NOz::Assign(name, colElem));
        const std::string prefix = name + "_filter";
        auto materialized = BuildColumnValueAst(
            name, "i", prefix, type, stringViewType);
        loopSetup.insert(loopSetup.end(),
            std::make_move_iterator(materialized.Setup.begin()),
            std::make_move_iterator(materialized.Setup.end()));
        const std::string valueName = TMaybeType<TStringType>(
            UnwrapNamedType(UnwrapNullableType(type)))
            ? stringValues.at(name)
            : fixedValues.at(name);
        if (IsNullableType(type)) {
            // Bind as Nullable[T] so predicate operators resolve to the .oz nullable
            // overloads (comparisons -> Nullable[bool], qdb_is_null -> bool).
            validityNames.emplace(valueName, prefix + "_valid");
            auto nt = NullableNamedType(materialized.ValueType);
            loopSetup.push_back(NOz::Var(valueName, nt));
            loopSetup.push_back(NOz::Assign(valueName, BuildNullableStruct(
                std::move(materialized.Value), std::move(materialized.IsValid),
                materialized.ValueType)));
        } else {
            loopSetup.push_back(NOz::Var(valueName, materialized.ValueType));
            loopSetup.push_back(NOz::Assign(valueName, std::move(materialized.Value)));
        }
    }

    // Loop
    auto varI = NOz::Var("i", std::make_shared<TIntegerType>());
    auto initI = NOz::Assign("i",
        std::make_shared<TNumberExpr>(loc, int64_t(0)));
    auto cond = std::make_shared<TBinaryExpr>(loc, TOperator("<"),
        std::make_shared<TIdentExpr>(loc, "i"),
        std::make_shared<TIdentExpr>(loc, "n"));
    TExprPtr selected;
    if (!HasNullableBoolCast(predicate) &&
        (validityNames.empty() || !UsesNullableValue(predicate, validityNames)))
    {
        // No nullable column referenced: ordinary bool predicate (unchanged path).
        selected = std::move(predicate);
    } else {
        // The predicate evaluates through the .oz nullable operators to Nullable[bool]
        // (or plain bool); qdb_is_true selects iff it is TRUE (NULL is not TRUE).
        selected = std::make_shared<TCallExpr>(loc,
            std::make_shared<TIdentExpr>(loc, "qdb_is_true"),
            std::vector<TExprPtr>{std::move(predicate)});
    }
    auto castedPred = std::make_shared<TCastExpr>(loc, std::move(selected),
        std::make_shared<TIntegerType>(TIntegerType::U8));
    auto writeSel = std::make_shared<TArrayAssignExpr>(loc, "selection",
        std::vector<TExprPtr>{std::make_shared<TIdentExpr>(loc, "i")},
        castedPred);
    auto incrI = NOz::Assign("i",
        std::make_shared<TBinaryExpr>(loc, TOperator("+"),
            std::make_shared<TIdentExpr>(loc, "i"),
            std::make_shared<TNumberExpr>(loc, int64_t(1))));

    bodyStmts.push_back(varI);
    bodyStmts.push_back(initI);
    loopSetup.push_back(std::move(writeSel));

    // A row an earlier operator rejected is not evaluated: the predicate may
    // be expensive, and may fail on a value that was filtered out on purpose.
    auto u8Zero = [&]() {
        auto zero = std::make_shared<TNumberExpr>(loc, int64_t(0));
        zero->Type = std::make_shared<TIntegerType>(TIntegerType::U8);
        return zero;
    };
    bodyStmts.push_back(NOz::Var("input_selection_is_null",
        std::make_shared<TBoolType>()));
    bodyStmts.push_back(NOz::Assign("input_selection_is_null",
        std::make_shared<TBinaryExpr>(loc, TOperator("=="),
            std::make_shared<TCastExpr>(loc,
                std::make_shared<TIdentExpr>(loc, "__input_selection__"),
                std::make_shared<TIntegerType>()),
            std::make_shared<TNumberExpr>(loc, int64_t(0)))));
    auto inputSelected = std::make_shared<TBinaryExpr>(loc, TOperator("||"),
        std::make_shared<TIdentExpr>(loc, "input_selection_is_null"),
        std::make_shared<TBinaryExpr>(loc, TOperator("!="),
            std::make_shared<TIndexExpr>(loc,
                std::make_shared<TIdentExpr>(loc, "__input_selection__"),
                std::make_shared<TIdentExpr>(loc, "i")),
            u8Zero()));
    auto writeRejected = std::make_shared<TArrayAssignExpr>(loc, "selection",
        std::vector<TExprPtr>{std::make_shared<TIdentExpr>(loc, "i")},
        u8Zero());
    std::vector<TExprPtr> loopBody;
    loopBody.push_back(std::make_shared<TIfExpr>(loc, std::move(inputSelected),
        std::make_shared<TBlockExpr>(loc, std::move(loopSetup)),
        std::make_shared<TBlockExpr>(loc,
            std::vector<TExprPtr>{std::move(writeRejected)})));
    loopBody.push_back(std::move(incrI));
    bodyStmts.push_back(std::make_shared<TWhileStmtExpr>(loc, cond,
        std::make_shared<TBlockExpr>(loc, std::move(loopBody))));

    auto ptrI8Type = std::make_shared<TPointerType>(
        std::make_shared<TIntegerType>(TIntegerType::I8));
    auto ptrPtrI8Type = std::make_shared<TPointerType>(ptrI8Type);
    auto builder = NOz::TFunBuilder("<kernel>")
        .Param("rowSet", rowSetRefType)
        .Param("__arena__", ptrI8Type)
        .Param("__regexes__", ptrPtrI8Type)
        .Param("__input_selection__", ptrU8Type)
        .Return(std::make_shared<TVoidType>());
    for (auto& stmt : bodyStmts) {
        builder.Stmt(std::move(stmt));
    }
    auto funDecl = std::move(builder).Build();

    return std::make_shared<TBlockExpr>(loc, std::vector<TExprPtr>{funDecl});
}

namespace {

// Recursively collects all identifier names referenced in an expression.
void CollectIdentNames(
    const NQumir::NAst::TExprPtr& expr,
    std::unordered_set<std::string>& out)
{
    using namespace NQumir::NAst;
    if (!expr) {
        return;
    }
    if (auto ident = TMaybeNode<TIdentExpr>(expr)) {
        out.insert(ident.Cast()->Name);
        return;
    }
    for (const auto& child : expr->Children()) {
        CollectIdentNames(child, out);
    }
}

// Deep-copies a filter predicate AST so the in-place rewrites
// (SpecializeFilterPredicate / SubstFieldsInPlace) don't mutate a predicate
// shared across multiple compilations (CompileJoin compiles each entry from a
// fresh program). Covers the node kinds that appear in filter predicates.
NQumir::NAst::TExprPtr CloneFilterExpr(const NQumir::NAst::TExprPtr& expr) {
    using namespace NQumir::NAst;
    if (!expr) {
        return nullptr;
    }
    const NQumir::TLocation loc = expr->Location;
    TExprPtr out;
    if (auto n = TMaybeNode<TIdentExpr>(expr)) {
        out = std::make_shared<TIdentExpr>(loc, n.Cast()->Name);
    } else if (auto n = TMaybeNode<TNumberExpr>(expr)) {
        out = n.Cast()->IsFloat()
            ? std::make_shared<TNumberExpr>(loc, n.Cast()->FloatValue)
            : std::make_shared<TNumberExpr>(loc, n.Cast()->IntValue);
    } else if (auto n = TMaybeNode<TStringLiteralExpr>(expr)) {
        out = std::make_shared<TStringLiteralExpr>(loc, n.Cast()->Value);
    } else if (auto n = TMaybeNode<TUnaryExpr>(expr)) {
        out = std::make_shared<TUnaryExpr>(loc, n.Cast()->Operator,
            CloneFilterExpr(n.Cast()->Operand));
    } else if (auto n = TMaybeNode<TBinaryExpr>(expr)) {
        out = std::make_shared<TBinaryExpr>(loc, n.Cast()->Operator,
            CloneFilterExpr(n.Cast()->Left), CloneFilterExpr(n.Cast()->Right));
    } else if (auto n = TMaybeNode<TCallExpr>(expr)) {
        std::vector<TExprPtr> args;
        args.reserve(n.Cast()->Args.size());
        for (const auto& a : n.Cast()->Args) {
            args.push_back(CloneFilterExpr(a));
        }
        out = std::make_shared<TCallExpr>(loc,
            CloneFilterExpr(n.Cast()->Callee), std::move(args));
    } else if (auto n = TMaybeNode<TCastExpr>(expr)) {
        out = std::make_shared<TCastExpr>(loc,
            CloneFilterExpr(n.Cast()->Operand), n.Cast()->Type);
    } else if (auto n = TMaybeNode<TIndexExpr>(expr)) {
        out = std::make_shared<TIndexExpr>(loc,
            CloneFilterExpr(n.Cast()->Collection), CloneFilterExpr(n.Cast()->Index));
    } else if (auto n = TMaybeNode<TIfExpr>(expr)) {
        auto ifExpr = n.Cast();
        out = std::make_shared<TIfExpr>(loc,
            CloneFilterExpr(ifExpr->Cond), CloneFilterExpr(ifExpr->Then),
            ifExpr->Else ? CloneFilterExpr(ifExpr->Else) : nullptr);
    } else if (auto n = TMaybeNode<TFieldAccessExpr>(expr)) {
        out = std::make_shared<TFieldAccessExpr>(loc,
            CloneFilterExpr(n.Cast()->Object), n.Cast()->FieldName);
    } else if (auto n = TMaybeNode<TBlockExpr>(expr)) {
        std::vector<TExprPtr> stmts;
        stmts.reserve(n.Cast()->Stmts.size());
        for (const auto& s : n.Cast()->Stmts) stmts.push_back(CloneFilterExpr(s));
        out = std::make_shared<TBlockExpr>(loc, std::move(stmts));
    } else if (auto n = TMaybeNode<TVarStmt>(expr)) {
        auto v = std::make_shared<TVarStmt>(loc, n.Cast()->Name, n.Cast()->Type);
        if (n.Cast()->Init) v->Init = CloneFilterExpr(n.Cast()->Init);
        out = std::move(v);
    } else {
        throw std::runtime_error(
            "residual filter: cannot clone predicate node '" +
            std::string(expr->NodeName()) + "'");
    }
    out->Type = expr->Type;
    return out;
}

} // namespace

// Residual join filter: evaluates `predicate` on a single (left_row, right_row)
// pair. Columns are read directly from the two row stores (contiguous TRowSet
// arrays) by decoding the packed row IDs. Mirrors GenFilterKernelAst's column
// binding, but with no loop and per-side store/row selection: inner-schema
// fields with index < leftFieldCount come from left_store at left_row, the rest
// from right_store at right_row.
NQumir::NAst::TExprPtr GenJoinResidualFilterAst(
    NQumir::NAst::TExprPtr predicate,
    const NQumir::NAst::TStructType& innerType,
    size_t leftFieldCount,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr stringViewType)
{
    using namespace NQumir::NAst;
    NQumir::TLocation loc{};

    auto i64Type = std::make_shared<TIntegerType>();
    auto boolType = std::make_shared<TBoolType>();

    // Work on a private copy - CompileJoin compiles each entry from a fresh
    // program, so the shared predicate must not be mutated in place.
    predicate = CloneFilterExpr(predicate);

    // Per-field value-variable names (matching GenFilterKernelAst's scheme).
    std::unordered_map<std::string, std::string> fixedValues;
    std::unordered_set<std::string> stringFields;
    std::unordered_map<std::string, std::string> stringValues;
    for (const auto& [name, type] : innerType.Fields) {
        if (TMaybeType<TStringType>(UnwrapNamedType(UnwrapNullableType(type)))) {
            stringFields.insert(name);
            stringValues.emplace(name, name + "_value");
        } else {
            fixedValues.emplace(name, name + "_value");
        }
    }

    // Which columns the predicate actually touches (collect before renaming).
    std::unordered_set<std::string> referenced;
    CollectIdentNames(predicate, referenced);

    SpecializeFilterPredicate(predicate, stringFields, stringValues);
    SubstFieldsInPlace(predicate, fixedValues);

    auto rowSetPtrType = std::make_shared<TPointerType>(
        AsNamed("TRowSet", rowSetType));
    auto rowSetRefType = std::make_shared<TReferenceType>(
        AsNamed("TRowSet", rowSetType));
    auto ptrI8Type = std::make_shared<TPointerType>(
        std::make_shared<TIntegerType>(TIntegerType::I8));
    std::vector<TParam> params = {
        std::make_shared<TVarStmt>(loc, "left_store", rowSetPtrType),
        std::make_shared<TVarStmt>(loc, "right_store", rowSetPtrType),
        std::make_shared<TVarStmt>(loc, "__arena__", ptrI8Type),
        std::make_shared<TVarStmt>(loc, "stream_left_batch", rowSetRefType),
        std::make_shared<TVarStmt>(loc, "stream_right_batch", rowSetRefType),
        std::make_shared<TVarStmt>(loc, "left_row_id", i64Type),
        std::make_shared<TVarStmt>(loc, "right_row_id", i64Type),
    };

    auto ident = [&](const std::string& name) -> TExprPtr {
        return std::make_shared<TIdentExpr>(loc, name);
    };
    auto numI64 = [&](int64_t value) -> TExprPtr {
        auto result = std::make_shared<TNumberExpr>(loc, value);
        result->Type = i64Type;
        return result;
    };
    auto binary = [&](const char* op, TExprPtr l, TExprPtr r) -> TExprPtr {
        return std::make_shared<TBinaryExpr>(loc, TOperator(op),
            std::move(l), std::move(r));
    };
    auto var = [&](const std::string& name, TTypePtr type) -> TExprPtr {
        return std::make_shared<TVarStmt>(loc, name, std::move(type));
    };
    auto assign = [&](const std::string& name, TExprPtr value) -> TExprPtr {
        return std::make_shared<TAssignExpr>(loc, name, std::move(value));
    };
    auto block = [&](std::vector<TExprPtr> stmts) -> TExprPtr {
        return std::make_shared<TBlockExpr>(loc, std::move(stmts));
    };

    auto ptrColumnType = ColumnPointerType(columnType, rowSetType);
    auto columnValueType = PointerPointeeOr(ptrColumnType, columnType);

    std::vector<TExprPtr> body;
    // Decode packed row IDs: batch = id >> 32, row = id & 0xffffffff.
    body.push_back(var("left_batch", i64Type));
    body.push_back(assign("left_batch", binary(">>", ident("left_row_id"), numI64(32))));
    body.push_back(var("left_row", i64Type));
    body.push_back(assign("left_row", binary("&", ident("left_row_id"), numI64(0xffffffff))));
    body.push_back(var("right_batch", i64Type));
    body.push_back(assign("right_batch", binary(">>", ident("right_row_id"), numI64(32))));
    body.push_back(var("right_row", i64Type));
    body.push_back(assign("right_row", binary("&", ident("right_row_id"), numI64(0xffffffff))));

    // Per-side Columns pointers. Batch index -1 means the side is the current
    // stream batch and is not present in the row store.
    auto storeColumns = [&](const char* store, const char* batchVar) -> TExprPtr {
        auto rs = std::make_shared<TIndexExpr>(loc, ident(store), ident(batchVar));
        return std::make_shared<TFieldAccessExpr>(loc, rs, "Columns");
    };
    auto streamColumns = [&](const char* streamBatch) -> TExprPtr {
        return std::make_shared<TFieldAccessExpr>(loc, ident(streamBatch), "Columns");
    };
    body.push_back(var("left_cols", ptrColumnType));
    body.push_back(std::make_shared<TIfExpr>(loc,
        binary("==", ident("left_batch"), numI64(-1)),
        block({assign("left_cols", streamColumns("stream_left_batch"))}),
        block({assign("left_cols", storeColumns("left_store", "left_batch"))})));
    body.push_back(var("right_cols", ptrColumnType));
    body.push_back(std::make_shared<TIfExpr>(loc,
        binary("==", ident("right_batch"), numI64(-1)),
        block({assign("right_cols", streamColumns("stream_right_batch"))}),
        block({assign("right_cols", storeColumns("right_store", "right_batch"))})));

    // Bind only the referenced columns through the shared materializer.
    std::unordered_map<std::string, std::string> validityNames;
    int32_t fieldIndex = 0;
    for (const auto& [name, type] : innerType.Fields) {
        const int32_t idx = fieldIndex++;
        if (!referenced.contains(name)) {
            continue;
        }
        const bool isLeft = static_cast<size_t>(idx) < leftFieldCount;
        const char* colsVar = isLeft ? "left_cols" : "right_cols";
        const char* rowVar = isLeft ? "left_row" : "right_row";
        const int32_t colIdx = isLeft ? idx
            : idx - static_cast<int32_t>(leftFieldCount);

        auto colElem = std::make_shared<TIndexExpr>(loc,
            ident(colsVar), numI64(colIdx));
        body.push_back(var(name, columnValueType));
        body.push_back(assign(name, colElem));

        const std::string prefix = name + "_residual";
        auto materialized = BuildColumnValueAst(name, rowVar, prefix, type, stringViewType);
        body.insert(body.end(),
            std::make_move_iterator(materialized.Setup.begin()),
            std::make_move_iterator(materialized.Setup.end()));
        const std::string valueName = TMaybeType<TStringType>(
            UnwrapNamedType(UnwrapNullableType(type)))
            ? stringValues.at(name)
            : fixedValues.at(name);
        if (IsNullableType(type)) {
            validityNames.emplace(valueName, prefix + "_valid");
            auto nt = NullableNamedType(materialized.ValueType);
            body.push_back(var(valueName, nt));
            body.push_back(assign(valueName, BuildNullableStruct(
                std::move(materialized.Value), std::move(materialized.IsValid),
                materialized.ValueType)));
        } else {
            body.push_back(var(valueName, materialized.ValueType));
            body.push_back(assign(valueName, std::move(materialized.Value)));
        }
    }

    // Evaluate the predicate (with three-valued-logic truth when nullable).
    TExprPtr result;
    if (!HasNullableBoolCast(predicate) &&
        (validityNames.empty() || !UsesNullableValue(predicate, validityNames)))
    {
        result = std::move(predicate);
    } else {
        result = std::make_shared<TCallExpr>(loc,
            std::make_shared<TIdentExpr>(loc, "qdb_is_true"),
            std::vector<TExprPtr>{std::move(predicate)});
    }
    TExprPtr castedResult = std::make_shared<TCastExpr>(loc, std::move(result), boolType);
    if (referenced.contains("__arena__")) {
        // The strings of one pair are dead once its truth is known. Releasing
        // them keeps the scratch of a kernel call at one pair, however many
        // pairs the call evaluates.
        body.push_back(var("pair_matched", boolType));
        body.push_back(assign("pair_matched", std::move(castedResult)));
        body.push_back(std::make_shared<TCallExpr>(loc,
            ident("qdb_string_arena_reset"), std::vector<TExprPtr>{ident("__arena__")}));
        castedResult = ident("pair_matched");
    }
    body.push_back(std::make_shared<TReturnExpr>(loc, std::move(castedResult)));

    return std::make_shared<TFunDecl>(loc, "jt_residual_filter",
        std::vector<TGenericParam>{},
        std::move(params), std::make_shared<TBlockExpr>(loc, std::move(body)),
        boolType);
}

// Project kernel for COMPUTED columns. Mirrors GenFilterKernelAst's column
// binding/materialization (column refs in the exprs are rewritten to {name}_value
// temps), but instead of writing a selection mask it writes computed values to
// output buffers. A struct expression is evaluated once into a temporary and
// its fields are scattered into adjacent scalar buffers.
NQumir::NAst::TExprPtr GenProjectKernelAst(
    std::vector<NQumir::NAst::TExprPtr> computedExprs,
    const std::vector<NQumir::NAst::TTypePtr>& computedTypes,
    const NQumir::NAst::TStructType& inputType,
    const std::unordered_map<std::string, int32_t>& fieldIndices,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr stringViewType,
    std::vector<std::shared_ptr<std::string>>& literalStorage)
{
    using namespace NQumir::NAst;
    NQumir::TLocation loc{};

    auto ident = [&](const std::string& name) -> TExprPtr {
        return std::make_shared<TIdentExpr>(loc, name);
    };
    auto numI64 = [&](int64_t v) -> TExprPtr {
        auto r = std::make_shared<TNumberExpr>(loc, v);
        r->Type = std::make_shared<TIntegerType>();
        return r;
    };
    auto var = [&](const std::string& name, TTypePtr type) -> TExprPtr {
        return std::make_shared<TVarStmt>(loc, name, std::move(type));
    };
    auto assign = [&](const std::string& name, TExprPtr value) -> TExprPtr {
        return std::make_shared<TAssignExpr>(loc, name, std::move(value));
    };
    auto cast = [&](TExprPtr e, TTypePtr t) -> TExprPtr {
        return std::make_shared<TCastExpr>(loc, std::move(e), std::move(t));
    };

    auto i64Type = std::make_shared<TIntegerType>();
    auto u8Type = std::make_shared<TIntegerType>(TIntegerType::U8);
    auto ptrU8Type = std::make_shared<TPointerType>(u8Type);
    auto ptrPtrU8Type = std::make_shared<TPointerType>(ptrU8Type);
    auto boolType = std::make_shared<TBoolType>();

    // qumirdb.oz's Nullable[T] = <struct (Value T) (Valid bool)>. Kernels carry
    // nullable values as this struct so the .oz operators apply; on output the
    // struct is split back into a data buffer (Value) and a validity mask (Valid).
    auto nullableType = [&](const TTypePtr& valueType) -> TTypePtr {
        auto structType = std::make_shared<TStructType>(
            std::vector<std::pair<std::string, TTypePtr>>{
                {"Value", valueType}, {"Valid", boolType}});
        return std::make_shared<TNamedType>("Nullable", structType,
            std::vector<TGenericArg>{TGenericArg::TypeArg(valueType)});
    };
    auto field = [&](const std::string& name, const std::string& f) -> TExprPtr {
        return std::make_shared<TFieldAccessExpr>(loc, ident(name), f);
    };

    // Collect column names referenced in computed expressions before substitution.
    std::unordered_set<std::string> referencedCols;
    {
        std::function<void(const TExprPtr&)> collect = [&](const TExprPtr& e) {
            if (!e) return;
            if (auto id = TMaybeNode<TIdentExpr>(e)) {
                referencedCols.insert(id.Cast()->Name);
                return;
            }
            for (const auto& child : e->Children()) collect(child);
        };
        for (const auto& expr : computedExprs) collect(expr);
    }

    // Value-variable naming, identical to the filter kernel.
    std::unordered_map<std::string, std::string> fixedValues;
    std::unordered_set<std::string> stringFields;
    std::unordered_map<std::string, std::string> stringValues;
    for (const auto& [name, type] : inputType.Fields) {
        if (TMaybeType<TStringType>(UnwrapNamedType(UnwrapNullableType(type)))) {
            stringFields.insert(name);
            stringValues.emplace(name, name + "_value");
        } else {
            fixedValues.emplace(name, name + "_value");
        }
    }
    for (auto& expr : computedExprs) {
        SpecializeFilterPredicate(expr, stringFields, stringValues);
        SubstFieldsInPlace(expr, fixedValues);
    }

    auto rowSetRefType = std::make_shared<TReferenceType>(rowSetType);
    // `__arena__` is the string-concat scratch arena (opaque TStringArena*). It is
    // always passed even when unused; qdb_string_concat(__arena__, ...) references it.
    auto ptrI8Type = std::make_shared<TPointerType>(
        std::make_shared<TIntegerType>(TIntegerType::I8));
    auto ptrPtrI8Type = std::make_shared<TPointerType>(ptrI8Type);
    std::vector<TParam> params = {
        std::make_shared<TVarStmt>(loc, "rowSet", rowSetRefType),
        std::make_shared<TVarStmt>(loc, "out", ptrPtrU8Type),
        std::make_shared<TVarStmt>(loc, "__arena__", ptrI8Type),
        std::make_shared<TVarStmt>(loc, "__regexes__", ptrPtrI8Type),
    };

    std::vector<TExprPtr> bodyStmts;
    auto identRowSet = ident("rowSet");
    auto fieldOf = [&](const std::string& name) {
        return std::make_shared<TFieldAccessExpr>(loc, identRowSet, name);
    };
    bodyStmts.push_back(var("n", i64Type));
    bodyStmts.push_back(assign("n", fieldOf("RowCount")));

    auto ptrColumnType = ColumnPointerType(columnType, rowSetType);
    auto columnValueType = PointerPointeeOr(ptrColumnType, columnType);
    bodyStmts.push_back(var("cols", ptrColumnType));
    bodyStmts.push_back(assign("cols", fieldOf("Columns")));

    // Bind + materialize only columns referenced in computed expressions.
    std::vector<TExprPtr> loopSetup;
    for (const auto& [name, type] : inputType.Fields) {
        if (!referencedCols.contains(name)) {
            continue;
        }
        const int32_t idx = fieldIndices.at(name);
        auto colElem = std::make_shared<TIndexExpr>(loc, ident("cols"), numI64(idx));
        bodyStmts.push_back(var(name, columnValueType));
        bodyStmts.push_back(assign(name, colElem));
        auto materialized = BuildColumnValueAst(name, "i", name + "_proj", type, stringViewType);
        loopSetup.insert(loopSetup.end(),
            std::make_move_iterator(materialized.Setup.begin()),
            std::make_move_iterator(materialized.Setup.end()));
        const std::string valueName = stringFields.contains(name)
            ? stringValues.at(name) : fixedValues.at(name);
        if (IsNullableType(type)) {
            // Bind the column as a Nullable[T] struct so nullable operators apply.
            loopSetup.push_back(var(valueName, nullableType(materialized.ValueType)));
            loopSetup.push_back(assign(valueName, BuildNullableStruct(
                std::move(materialized.Value), std::move(materialized.IsValid),
                materialized.ValueType)));
        } else {
            loopSetup.push_back(var(valueName, materialized.ValueType));
            loopSetup.push_back(assign(valueName, std::move(materialized.Value)));
        }
    }

    // A struct-valued expression still occupies one entry in computedExprs, but
    // each field has its own physical output buffer.
    std::vector<TTypePtr> outputTypes;
    for (const auto& type : computedTypes) {
        auto structure = TMaybeType<TStructType>(
            !IsNullableType(type) ? UnwrapNamedType(type) : TTypePtr{});
        if (structure) {
            const auto structType = structure.Cast();
            for (const auto& [_, fieldType] : structType->Fields) {
                outputTypes.push_back(fieldType);
            }
        } else {
            outputTypes.push_back(type);
        }
    }

    // Data buffers live at out[k]; validity masks for nullable columns at
    // out[numComputed + k]. out_k = (<ptr Value_k>) out[k].
    const size_t numComputed = outputTypes.size();
    for (size_t k = 0; k < numComputed; ++k) {
        const bool nullable = IsNullableType(outputTypes[k]);
        auto valueType = nullable ? UnwrapNullableType(outputTypes[k]) : outputTypes[k];
        auto ptrTk = std::make_shared<TPointerType>(ColumnBufferElementType(valueType));
        auto outK = std::make_shared<TIndexExpr>(loc, ident("out"), numI64(int64_t(k)));
        bodyStmts.push_back(var("out_" + std::to_string(k), ptrTk));
        bodyStmts.push_back(assign("out_" + std::to_string(k),
            cast(cast(outK, i64Type), ptrTk)));
        if (nullable) {
            auto outMaskK = std::make_shared<TIndexExpr>(loc, ident("out"),
                numI64(int64_t(numComputed + k)));
            bodyStmts.push_back(var("out_mask_" + std::to_string(k), ptrU8Type));
            bodyStmts.push_back(assign("out_mask_" + std::to_string(k),
                cast(cast(outMaskK, i64Type), ptrU8Type)));
        }
    }

    bodyStmts.push_back(var("i", i64Type));
    bodyStmts.push_back(assign("i", numI64(0)));
    size_t outputIndex = 0;
    auto emitOutput = [&](TExprPtr value, const TTypePtr& type) {
        const size_t k = outputIndex++;
        auto emitValue = [&](TExprPtr result) {
            const auto valueType = UnwrapNullableType(type);
            if (TMaybeType<TBoolType>(UnwrapNamedType(valueType))) {
                loopSetup.push_back(std::make_shared<TCallExpr>(loc,
                    ident("qdb_bitmap_set_valid"),
                    std::vector<TExprPtr>{ident("out_" + std::to_string(k)),
                        ident("i"), cast(std::move(result), valueType)}));
            } else {
                loopSetup.push_back(std::make_shared<TArrayAssignExpr>(loc,
                    "out_" + std::to_string(k), std::vector<TExprPtr>{ident("i")},
                    cast(std::move(result), valueType)));
            }
        };
        if (IsNullableType(type)) {
            // Split the Nullable[T] result: Value -> data buffer, Valid -> mask.
            auto nt = nullableType(UnwrapNullableType(type));
            const std::string rName = "r_" + std::to_string(k);
            loopSetup.push_back(var(rName, nt));
            loopSetup.push_back(assign(rName, std::move(value)));
            emitValue(field(rName, "Value"));
            loopSetup.push_back(std::make_shared<TCallExpr>(loc,
                ident("qdb_bitmap_set_valid"),
                std::vector<TExprPtr>{ident("out_mask_" + std::to_string(k)),
                    ident("i"), field(rName, "Valid")}));
        } else {
            emitValue(std::move(value));
        }
    };
    for (size_t k = 0; k < computedExprs.size(); ++k) {
        auto structure = TMaybeType<TStructType>(
            !IsNullableType(computedTypes[k])
                ? UnwrapNamedType(computedTypes[k])
                : TTypePtr{});
        if (!structure) {
            emitOutput(std::move(computedExprs[k]), computedTypes[k]);
            continue;
        }

        const std::string resultName = "project_result_" + std::to_string(k);
        loopSetup.push_back(var(resultName, computedTypes[k]));
        loopSetup.push_back(assign(resultName, std::move(computedExprs[k])));
        const auto structType = structure.Cast();
        for (const auto& [fieldName, fieldType] : structType->Fields) {
            emitOutput(field(resultName, fieldName), fieldType);
        }
    }
    loopSetup.push_back(assign("i",
        std::make_shared<TBinaryExpr>(loc, TOperator("+"), ident("i"), numI64(1))));
    auto cond = std::make_shared<TBinaryExpr>(loc, TOperator("<"), ident("i"), ident("n"));
    bodyStmts.push_back(std::make_shared<TWhileStmtExpr>(loc, cond,
        std::make_shared<TBlockExpr>(loc, std::move(loopSetup))));

    auto funBody = std::make_shared<TBlockExpr>(loc, std::move(bodyStmts));
    auto funDecl = std::make_shared<TFunDecl>(loc, "<project>",
        std::vector<TGenericParam>{},
        std::move(params), funBody, std::make_shared<TVoidType>());
    return std::make_shared<TBlockExpr>(loc, std::vector<TExprPtr>{funDecl});
}

TAggReducerLayout BuildAggReducerLayout(
    const std::vector<std::string>& funcs,
    const std::vector<TAggArg>& args)
{
    TAggReducerLayout layout;
    layout.Reducers.reserve(funcs.size());
    int nextBufIdx = 0;
    for (size_t i = 0; i < funcs.size(); ++i) {
        TAggReducerInfo info;
        info.Func = funcs[i];
        const TAggArg& arg = args[i];
        info.HasArg = arg.ColumnIndex >= 0;
        info.ArgColumnIndex = arg.ColumnIndex;
        info.NeedsValidity = arg.IsNullable && info.HasArg;
        const bool isAggFunc =
            info.Func == "sum" || info.Func == "min" || info.Func == "max";
        info.ValueKind = info.HasArg && isAggFunc
            ? arg.ValueKind
            : EAggValueKind::Int64;
        info.ValueBufIdx = nextBufIdx++;
        if (info.IsBinInt() || info.IsString()) {
            info.ExtraBufIdx = nextBufIdx++;
        }
        info.IsNullableOutput = info.NeedsValidity && isAggFunc;
        if (info.IsNullableOutput) {
            info.ValidBufIdx = nextBufIdx++;
        }
        layout.Reducers.push_back(std::move(info));
    }
    layout.NumAggBuffers = static_cast<size_t>(nextBufIdx);
    return layout;
}

std::optional<TAggRowLayout> BuildAggRowLayout(
    const TAggregateKeyDescriptor& key,
    const TAggReducerLayout& layout,
    bool hasEmptyInputGroups)
{
    // A string key is a fixed-width handle in the row, with its bytes in the
    // owned arena; a string reducer's state is not.
    if (HasStringReducer(layout) || hasEmptyInputGroups) {
        return std::nullopt;
    }
    const bool wideKey = std::ranges::any_of(key.Fields, [](const auto& field) {
        return IsBinIntStorageType(field.Type);
    });
    TAggRowLayout rows;
    rows.KeyAlignment = wideKey ? 16 : 8;
    const auto alignment = static_cast<size_t>(rows.KeyAlignment);
    rows.KeyOffset = static_cast<int64_t>(AlignUp(8, alignment));
    rows.KeyStride = static_cast<int64_t>(AlignUp(key.Size, alignment));
    rows.StateOffset = rows.KeyOffset + rows.KeyStride;
    rows.RowBytes = static_cast<int64_t>(AlignUp(
        static_cast<size_t>(rows.StateOffset) + 8 * layout.NumAggBuffers, alignment));
    return rows;
}

namespace {

// Rows of a batch that one find-or-insert of the row layout takes at a time.
// A chunk bounds the lookup scratch, and the groups it touches are still in
// the cache when its last pass applies the values to them.
constexpr int64_t AggRowChunkRows = 2048;
// Rows per arena block of the row layout, as in the row join table.
constexpr int64_t AggRowBlockRows = 16384;

// The per-row parts of the aggregate update, shared by the swiss loop of
// agg_dispatch and the passes of agg_rows_update. Each call builds new AST
// nodes, so a part can be placed in both functions.
class TAggregateUpdateAst {
public:
    TAggregateUpdateAst(
        const NQumir::NAst::TStructType& inputType,
        const TAggregateKeyDescriptor& key,
        const TAggReducerLayout& layout,
        const NQumir::NAst::TTypePtr& columnType,
        const NQumir::NAst::TTypePtr& rowSetType,
        bool hasPrecomputedHash)
        : Key_(key)
        , Layout_(layout)
        , PtrColumnType_(ColumnPointerType(columnType, rowSetType))
        , ColumnValueType_(PointerPointeeOr(PtrColumnType_, columnType))
        , StringViewType_(FindStringViewType(key.LookupType))
        , HasPrecomputedHash_(hasPrecomputedHash)
    {
        using namespace NQumir::NAst;
        if (!StringViewType_ && HasStringReducer(layout)) {
            StringViewType_ = std::make_shared<TNamedType>("StringView", nullptr);
        }
        // Each argument column is read at most once per row. Count-only
        // arguments need no value; nullable ones read only their validity.
        for (const auto& r : layout.Reducers) {
            if (r.ArgColumnIndex < 0 || ArgColumns_.contains(r.ArgColumnIndex)) {
                continue;
            }
            const int32_t idx = r.ArgColumnIndex;
            TArgColumn ac;
            ac.Type = inputType.Fields[idx].second;
            ac.NeedValue = std::ranges::any_of(layout.Reducers,
                [&](const auto& candidate) {
                    return candidate.ArgColumnIndex == idx &&
                        candidate.Func != "count";
                });
            const auto valueType = UnwrapNullableType(ac.Type);
            if (IsBinIntValueType(valueType)) {
                ac.ValueKind = EAggValueKind::BinInt;
            } else if (TMaybeType<TFloatType>(UnwrapNamedType(valueType))) {
                ac.ValueKind = EAggValueKind::Float64;
            } else if (TMaybeType<TStringType>(UnwrapNamedType(valueType))) {
                ac.ValueKind = EAggValueKind::String;
            } else if (TMaybeType<TBoolType>(UnwrapNamedType(valueType))) {
                ac.ValueKind = EAggValueKind::Bool;
            }
            ac.Nullable = IsNullableType(ac.Type);
            // A string is variable-width and a bool bit-packed: neither is indexed.
            const bool packed = ac.ValueKind == EAggValueKind::String ||
                ac.ValueKind == EAggValueKind::Bool;
            ac.Materialized = ac.Nullable || (ac.NeedValue && packed);
            ArgColumns_.emplace(idx, std::move(ac));
        }
    }

    // The batch row count, selection and the columns the update reads.
    std::vector<NQumir::NAst::TExprPtr> Prologue() const {
        using namespace NQumir::NAst;
        auto u8Type = std::make_shared<TIntegerType>(TIntegerType::U8);
        auto u64Type = std::make_shared<TIntegerType>(TIntegerType::U64);
        std::vector<TExprPtr> stmts;
        auto bind = [&](const std::string& name, TTypePtr type, TExprPtr value) {
            stmts.push_back(NOz::Var(name, std::move(type)));
            stmts.push_back(NOz::Assign(name, std::move(value)));
        };
        bind("n", I64(), NOz::Field("batch", "RowCount"));
        bind("selection", std::make_shared<TPointerType>(u8Type),
            NOz::Field("batch", "Selection"));
        bind("cols", PtrColumnType_, NOz::Field("batch", "Columns"));
        if (HasPrecomputedHash_) {
            bind("hash_ptr", std::make_shared<TPointerType>(u64Type),
                NOz::Field("batch", "Hash"));
        }
        for (size_t fieldIndex = 0; fieldIndex < Key_.Fields.size(); ++fieldIndex) {
            bind("key_column_" + std::to_string(fieldIndex), ColumnValueType_,
                ColumnAt(Key_.Fields[fieldIndex].ColumnIndex));
        }
        for (const auto& [idx, ac] : ArgColumns_) {
            if (ac.Materialized) {
                bind("arg_column_" + std::to_string(idx), ColumnValueType_, ColumnAt(idx));
            } else if (ac.NeedValue) {
                auto ptrValType = std::make_shared<TPointerType>(
                    AggregateStorageType(UnwrapNullableType(ac.Type)));
                bind("values_" + std::to_string(idx), ptrValType, NOz::Cast(
                    NOz::Cast(NOz::Field(ColumnAt(idx), "Data"), I64()), ptrValType));
            }
        }
        bind("selection_is_null", std::make_shared<TBoolType>(),
            NOz::Bin(TOperator("=="), NOz::Cast(NOz::Ident("selection"), I64()),
                NOz::TypedInt(0, I64())));
        return stmts;
    }

    NQumir::NAst::TExprPtr Selected(const std::string& row) const {
        using namespace NQumir::NAst;
        auto u8Type = std::make_shared<TIntegerType>(TIntegerType::U8);
        return NOz::Bin(TOperator("||"), NOz::Ident("selection_is_null"),
            NOz::Bin(TOperator("!="), NOz::Index("selection", NOz::Ident(row)),
                NOz::TypedInt(0, u8Type)));
    }

    // key_value and hash_value of batch row `row`.
    std::vector<NQumir::NAst::TExprPtr> Key(const std::string& row) const {
        using namespace NQumir::NAst;
        NQumir::TLocation loc{};
        auto u64Type = std::make_shared<TIntegerType>(TIntegerType::U64);
        std::vector<TExprPtr> stmts;
        std::vector<TColumnValueAst> keyFields;
        keyFields.reserve(Key_.Fields.size());
        for (size_t fieldIndex = 0; fieldIndex < Key_.Fields.size(); ++fieldIndex) {
            auto logicalType = Key_.Fields[fieldIndex].IsNullable
                ? std::make_shared<TNullable>(Key_.Fields[fieldIndex].Type)
                : Key_.Fields[fieldIndex].Type;
            keyFields.push_back(BuildColumnValueAst(
                "key_column_" + std::to_string(fieldIndex), row,
                "key_value_" + std::to_string(fieldIndex),
                std::move(logicalType), StringViewType_));
            stmts.insert(stmts.end(),
                std::make_move_iterator(keyFields.back().Setup.begin()),
                std::make_move_iterator(keyFields.back().Setup.end()));
        }
        auto namedKey = TMaybeType<TNamedType>(Key_.LookupType);
        auto keyStruct = namedKey
            ? TMaybeType<TStructType>(namedKey.Cast()->UnderlyingType)
            : TMaybeType<TStructType>(Key_.LookupType);
        if (!keyStruct) {
            throw std::invalid_argument(
                "GenGenericAggregateDispatchAst: key must be a struct");
        }
        std::vector<TExprPtr> fields;
        fields.reserve(keyStruct.Cast()->Fields.size());
        for (const auto& [fieldName, fieldType] : keyStruct.Cast()->Fields) {
            const bool validity = fieldName.starts_with("valid_");
            const std::string_view prefix = validity ? "valid_" : "key_";
            if (!fieldName.starts_with(prefix)) {
                throw std::invalid_argument(
                    "GenGenericAggregateDispatchAst: unexpected key field '" +
                    fieldName + "'");
            }
            const size_t fieldIndex = std::stoull(fieldName.substr(prefix.size()));
            if (fieldIndex >= Key_.Fields.size()) {
                throw std::invalid_argument(
                    "GenGenericAggregateDispatchAst: invalid key field '" +
                    fieldName + "'");
            }
            fields.push_back(validity
                ? keyFields[fieldIndex].IsValid
                : keyFields[fieldIndex].Value);
        }
        stmts.push_back(NOz::Var("key_value", Key_.LookupType));
        stmts.push_back(NOz::Assign("key_value", std::make_shared<TStructConstructExpr>(
            loc, Key_.LookupType, std::move(fields))));
        stmts.push_back(NOz::Var("hash_value", u64Type));
        auto computed = NOz::Cast(NOz::Call("rh_hash", {NOz::Ident("key_value")}), u64Type);
        if (HasPrecomputedHash_) {
            // hash_ptr is only guaranteed non-null when the shuffle upstream
            // produced a single-batch (view) output; the rarer multi-batch
            // gather path leaves it null, so fall back to rh_hash per row.
            stmts.push_back(NOz::If(
                NOz::Bin(TOperator("!="), NOz::Cast(NOz::Ident("hash_ptr"), I64()),
                    NOz::TypedInt(0, I64())),
                NOz::Block({NOz::Assign("hash_value",
                    NOz::Index("hash_ptr", NOz::Ident(row)))}),
                NOz::Block({NOz::Assign("hash_value", std::move(computed))})));
        } else {
            stmts.push_back(NOz::Assign("hash_value", std::move(computed)));
        }
        return stmts;
    }

    // arg_val_<c> and arg_valid_<c> of batch row `row`. Scalar integers and
    // floats are carried as i64 bits, booleans as 0/1; BinInt stays typed.
    std::vector<NQumir::NAst::TExprPtr> Arguments(const std::string& row) const {
        using namespace NQumir::NAst;
        NQumir::TLocation loc{};
        auto boolType = std::make_shared<TBoolType>();
        std::vector<TExprPtr> stmts;
        for (const auto& [idx, ac] : ArgColumns_) {
            std::optional<TColumnValueAst> mat;
            if (ac.Materialized) {
                mat = BuildColumnValueAst(
                    "arg_column_" + std::to_string(idx), row,
                    "arg_value_" + std::to_string(idx), ac.Type,
                    StringViewType_, ac.NeedValue);
                stmts.insert(stmts.end(),
                    std::make_move_iterator(mat->Setup.begin()),
                    std::make_move_iterator(mat->Setup.end()));
            }
            const std::string validName = "arg_valid_" + std::to_string(idx);
            if (!ac.NeedValue) {
                if (ac.Nullable) {
                    stmts.push_back(NOz::Var(validName, boolType));
                    stmts.push_back(NOz::Assign(validName, mat->IsValid));
                }
                continue;
            }
            TExprPtr valExpr;
            if (mat) {
                if (ac.ValueKind == EAggValueKind::BinInt ||
                    ac.ValueKind == EAggValueKind::String) {
                    valExpr = mat->Value;
                } else if (ac.ValueKind == EAggValueKind::Float64) {
                    valExpr = std::make_shared<TBitcastExpr>(loc, mat->Value, I64());
                } else if (ac.ValueKind == EAggValueKind::Bool) {
                    // Not a cast: an integer cast sign-extends TRUE to -1.
                    valExpr = NOz::If(mat->Value, NOz::TypedInt(1, I64()), NOz::TypedInt(0, I64()));
                } else {
                    valExpr = NOz::Cast(mat->Value, I64());
                }
            } else {
                auto cell = NOz::Index("values_" + std::to_string(idx), NOz::Ident(row));
                if (ac.ValueKind == EAggValueKind::BinInt) {
                    valExpr = std::move(cell);
                } else if (ac.ValueKind == EAggValueKind::Float64) {
                    valExpr = std::make_shared<TBitcastExpr>(loc, std::move(cell), I64());
                } else {
                    valExpr = NOz::Cast(std::move(cell), I64());
                }
            }
            auto argType = ac.ValueKind == EAggValueKind::BinInt
                ? BinIntStorageType()
                : (ac.ValueKind == EAggValueKind::String
                    ? AsNamed("StringView", StringViewType_)
                    : I64());
            const std::string vname = "arg_val_" + std::to_string(idx);
            stmts.push_back(NOz::Var(vname, argType));
            stmts.push_back(NOz::Assign(vname, std::move(valExpr)));
            if (ac.Nullable) {
                stmts.push_back(NOz::Var(validName, boolType));
                stmts.push_back(NOz::Assign(validName, mat->IsValid));
            }
        }
        return stmts;
    }

    // Applies the arguments to the states at state_slot of the state buffers,
    // `buffer(i)` being AggBuffers slot i; is_new is non-zero for a new group.
    // A failing string reducer returns -1 from the enclosing function.
    std::vector<NQumir::NAst::TExprPtr> Reducers(
        const std::function<NQumir::NAst::TExprPtr(int)>& buffer) const
    {
        using namespace NQumir::NAst;
        auto ptrI64Type = std::make_shared<TPointerType>(I64());
        auto numI64 = [&](int64_t value) { return NOz::TypedInt(value, I64()); };
        auto binary = [&](const char* op, TExprPtr left, TExprPtr right) {
            return NOz::Bin(TOperator(op), std::move(left), std::move(right));
        };
        auto slotIndex = [&](const std::string& buf) {
            return NOz::Index(buf, NOz::Ident("state_slot"));
        };
        auto isNew = [&]() {
            return binary("!=", NOz::Ident("is_new"), numI64(0));
        };
        auto failure = [&]() {
            return NOz::Block({NOz::Return(numI64(-1))});
        };
        std::vector<TExprPtr> stmts;
        auto bindBuffer = [&](const std::string& name, int index) {
            stmts.push_back(NOz::Var(name, ptrI64Type));
            stmts.push_back(NOz::Assign(name, buffer(index)));
        };
        for (size_t ri = 0; ri < Layout_.Reducers.size(); ++ri) {
            const auto& info = Layout_.Reducers[ri];
            const std::string bufName = "buf_" + std::to_string(ri);
            const std::string reduceName = "reduce_" + std::to_string(ri);
            const std::string validBufName = "validbuf_" + std::to_string(ri);
            bindBuffer(bufName, info.ValueBufIdx);
            std::string hiBufName;
            if (info.IsBinInt() || info.IsString()) {
                hiBufName = "hibuf_" + std::to_string(ri);
                bindBuffer(hiBufName, info.ExtraBufIdx);
            }
            auto valueI = [&]() -> TExprPtr {
                if (info.Func == "count") {
                    return numI64(0);
                }
                return info.HasArg ? NOz::Ident("arg_val_" + std::to_string(info.ArgColumnIndex))
                                   : numI64(0);
            };
            auto validI = [&]() -> TExprPtr {
                return info.NeedsValidity && ArgColumns_.at(info.ArgColumnIndex).Nullable
                    ? NOz::Ident("arg_valid_" + std::to_string(info.ArgColumnIndex))
                    : NOz::Bool(true);
            };
            if (info.IsString()) {
                auto invoke = [&](TExprPtr seed) -> TExprPtr {
                    return NOz::Call("agg_string_reduce", {
                        NOz::Ident(bufName), NOz::Ident(hiBufName), NOz::Ident("state_slot"),
                        valueI(), NOz::Bool(info.Func == "min"), std::move(seed)});
                };
                if (!info.NeedsValidity) {
                    stmts.push_back(NOz::If(
                        NOz::Unary(TOperator("!"), invoke(isNew())), failure()));
                    continue;
                }
                bindBuffer(validBufName, info.ValidBufIdx);
                stmts.push_back(NOz::If(validI(), NOz::Block({
                    NOz::If(NOz::Unary(TOperator("!"),
                            invoke(binary("==", slotIndex(validBufName), numI64(0)))),
                        failure()),
                    NOz::ArrayAssign(validBufName, NOz::Ident("state_slot"),
                        binary("+", slotIndex(validBufName), numI64(1))),
                })));
                continue;
            }
            if (info.IsBinInt()) {
                if (!info.NeedsValidity) {
                    stmts.push_back(NOz::Call(reduceName, {
                        NOz::Ident(bufName), NOz::Ident(hiBufName), NOz::Ident("state_slot"),
                        valueI(), isNew()}));
                    continue;
                }
                bindBuffer(validBufName, info.ValidBufIdx);
                stmts.push_back(NOz::Call(reduceName, {
                    NOz::Ident(bufName), NOz::Ident(hiBufName), NOz::Ident("state_slot"),
                    NOz::Ident(validBufName), valueI(), validI()}));
                continue;
            }
            if (!info.NeedsValidity) {
                stmts.push_back(NOz::ArrayAssign(bufName, NOz::Ident("state_slot"),
                    NOz::Call(reduceName, {slotIndex(bufName), valueI(), isNew()})));
            } else if (info.Func == "count") {
                stmts.push_back(NOz::Call(reduceName,
                    {NOz::Ident(bufName), NOz::Ident("state_slot"), validI()}));
            } else {
                bindBuffer(validBufName, info.ValidBufIdx);
                stmts.push_back(NOz::Call(reduceName, {NOz::Ident(bufName),
                    NOz::Ident(validBufName), NOz::Ident("state_slot"), valueI(), validI()}));
            }
        }
        return stmts;
    }

private:
    struct TArgColumn {
        NQumir::NAst::TTypePtr Type;
        EAggValueKind ValueKind = EAggValueKind::Int64;
        bool Nullable = false;
        bool NeedValue = false;
        // Read through BuildColumnValueAst: a nullable or packed column.
        bool Materialized = false;
    };

    static NQumir::NAst::TTypePtr I64() {
        return std::make_shared<NQumir::NAst::TIntegerType>();
    }

    static NQumir::NAst::TExprPtr ColumnAt(int32_t index) {
        return NOz::Index("cols", NOz::TypedInt(index, I64()));
    }

    const TAggregateKeyDescriptor& Key_;
    const TAggReducerLayout& Layout_;
    NQumir::NAst::TTypePtr PtrColumnType_;
    NQumir::NAst::TTypePtr ColumnValueType_;
    NQumir::NAst::TTypePtr StringViewType_;
    bool HasPrecomputedHash_ = false;
    std::map<int32_t, TArgColumn> ArgColumns_;
};

// A pointer to state word `word` of the group row `rowVar` (tagged as in
// agr_find_or_insert: the caller masks bit 0 off first).
NQumir::NAst::TExprPtr RowStateWord(
    const std::string& rowVar, const TAggRowLayout& rows, int word)
{
    using namespace NQumir::NAst;
    auto i64Type = std::make_shared<TIntegerType>();
    return NOz::Cast(
        NOz::Add(NOz::Cast(NOz::Ident(rowVar), i64Type),
            NOz::TypedInt(rows.StateOffset + 8 * static_cast<int64_t>(word), i64Type)),
        std::make_shared<TPointerType>(i64Type));
}

} // namespace

NQumir::NAst::TExprPtr GenGenericAggregateDispatchAst(
    const NQumir::NAst::TStructType& inputType,
    const TAggregateKeyDescriptor& key,
    const TAggReducerLayout& layout,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr hashTableType,
    bool hasPrecomputedHash,
    const std::vector<int64_t>& emptyInputGroups,
    const std::optional<TAggRowLayout>& rows)
{
    using namespace NQumir::NAst;
    NQumir::TLocation loc{};

    auto i64Type = std::make_shared<TIntegerType>();
    auto u64Type = std::make_shared<TIntegerType>(TIntegerType::U64);
    auto ptrI64Type = std::make_shared<TPointerType>(i64Type);
    auto ptrStoredKeyType = std::make_shared<TPointerType>(key.StoredType);

    auto ident = [&](const std::string& name) -> TExprPtr {
        return std::make_shared<TIdentExpr>(loc, name);
    };
    auto number = [&](int64_t value, TTypePtr type) -> TExprPtr {
        auto result = std::make_shared<TNumberExpr>(loc, value);
        result->Type = std::move(type);
        return result;
    };
    auto numI64 = [&](int64_t value) { return number(value, i64Type); };
    auto binary = [&](const char* op, TExprPtr left, TExprPtr right) -> TExprPtr {
        return std::make_shared<TBinaryExpr>(
            loc, TOperator(op), std::move(left), std::move(right));
    };
    auto call = [&](const std::string& name, std::vector<TExprPtr> args) -> TExprPtr {
        return std::make_shared<TCallExpr>(loc, ident(name), std::move(args));
    };
    auto cast = [&](TExprPtr expr, TTypePtr type) -> TExprPtr {
        return std::make_shared<TCastExpr>(loc, std::move(expr), std::move(type));
    };
    auto assign = [&](const std::string& name, TExprPtr value) -> TExprPtr {
        return std::make_shared<TAssignExpr>(loc, name, std::move(value));
    };
    auto var = [&](const std::string& name, TTypePtr type) -> TExprPtr {
        return std::make_shared<TVarStmt>(loc, name, std::move(type));
    };
    auto block = [&](std::vector<TExprPtr> stmts) -> TExprPtr {
        return std::make_shared<TBlockExpr>(loc, std::move(stmts));
    };
    auto field = [&](const std::string& object, const std::string& name) -> TExprPtr {
        return std::make_shared<TFieldAccessExpr>(loc, ident(object), name);
    };

    auto hashTableRefType = std::make_shared<TReferenceType>(
        AsNamed("HashTable", hashTableType));
    auto rowSetRefType = std::make_shared<TReferenceType>(
        AsNamed("TRowSet", rowSetType));
    std::vector<TParam> params = {
        std::make_shared<TVarStmt>(loc, "ht", hashTableRefType),
        std::make_shared<TVarStmt>(loc, "batch", rowSetRefType),
        std::make_shared<TVarStmt>(loc, "arg", i64Type),
        std::make_shared<TVarStmt>(loc, "op", i64Type),
    };

    const TAggregateUpdateAst row(
        inputType, key, layout, columnType, rowSetType, hasPrecomputedHash);
    std::vector<TExprPtr> update;
    if (rows) {
        update.push_back(NOz::If(call("agr_active", {ident("ht")}), block({
            NOz::Return(call("agg_rows_update", {ident("ht"), ident("batch"), numI64(0)})),
        })));
    }
    for (auto& stmt : row.Prologue()) {
        update.push_back(std::move(stmt));
    }
    update.push_back(var("state_slot", i64Type));
    update.push_back(assign("state_slot", numI64(-1)));
    update.push_back(var("is_new", i64Type));
    update.push_back(assign("is_new", numI64(0)));
    update.push_back(var("stored_witness", key.StoredType));
    update.push_back(assign("stored_witness", ZeroValueExpr(key.StoredType)));
    update.push_back(var("i", i64Type));
    update.push_back(assign("i", numI64(0)));

    // Scalar states live at the probe slot; string reducers retain their dense
    // ownership buffers. This policy must match aggregate finalization.
    const std::string upsertName = HasStringReducer(layout)
        ? "aht_upsert_dual"
        : "aht_upsert_aggregate";
    std::vector<TExprPtr> process = row.Key("i");
    for (auto& stmt : row.Arguments("i")) {
        process.push_back(std::move(stmt));
    }
    std::vector<TExprPtr> validBody = {
        assign("state_slot", call(upsertName, {
            ident("ht"),
            ident("key_value"),
            ident("stored_witness"),
            ident("is_new"),
            ident("hash_value"),
        })),
    };
    if (rows) {
        // The table moves to the row layout instead of growing; the rest of
        // the batch, this row included, goes there.
        validBody.push_back(NOz::If(binary("==", ident("state_slot"), numI64(-2)), block({
            NOz::If(NOz::Unary(TOperator("!"), call("agr_from_swiss", {
                    ident("ht"),
                    numI64(rows->KeyOffset),
                    numI64(rows->StateOffset),
                    numI64(rows->RowBytes),
                    numI64(AggRowBlockRows),
                    cast(numI64(0), ptrStoredKeyType),
                })),
                block({NOz::Return(numI64(-1))})),
            NOz::Return(call("agg_rows_update", {ident("ht"), ident("batch"), ident("i")})),
        })));
    }
    validBody.push_back(std::make_shared<TIfExpr>(loc,
        binary("<", ident("state_slot"), numI64(0)),
        block({std::make_shared<TReturnExpr>(loc, numI64(-1))}), nullptr));
    auto ptrPtrI64Type = std::make_shared<TPointerType>(ptrI64Type);
    validBody.push_back(var("agg_buffers", ptrPtrI64Type));
    validBody.push_back(assign("agg_buffers", field("ht", "AggBuffers")));
    for (auto& stmt : row.Reducers([&](int index) {
             return std::make_shared<TIndexExpr>(loc, ident("agg_buffers"), numI64(index));
         }))
    {
        validBody.push_back(std::move(stmt));
    }
    process.push_back(block(std::move(validBody)));
    auto loop = block({
        std::make_shared<TIfExpr>(loc, row.Selected("i"), block(std::move(process)), nullptr),
        assign("i", binary("+", ident("i"), numI64(1))),
    });
    update.push_back(std::make_shared<TWhileStmtExpr>(
        loc, binary("<", ident("i"), ident("n")), std::move(loop)));
    update.push_back(numI64(0));

    TExprPtr init = cast(call("aht_init", {
        ident("ht"), ident("arg"),
        numI64(static_cast<int64_t>(layout.NumAggBuffers)),
        numI64(static_cast<int64_t>(key.Size)),
    }), i64Type);
    if (!emptyInputGroups.empty()) {
        // Seed the groups that exist over an empty input, without pretending an input
        // row arrived. The leading key column holds the group's id and every other key
        // column is NULL, which is how the update loop encodes a NULL key. Reducer
        // buffers remain at their identity (COUNT 0, validity 0).
        if (key.Fields.empty() || key.Fields[0].IsNullable ||
            !TMaybeType<TIntegerType>(UnwrapNamedType(key.Fields[0].Type)))
        {
            throw std::invalid_argument(
                "an aggregate with empty-input groups requires a leading integer key");
        }
        auto namedSeedKey = TMaybeType<TNamedType>(key.LookupType);
        auto seedKeyStruct = namedSeedKey
            ? TMaybeType<TStructType>(namedSeedKey.Cast()->UnderlyingType)
            : TMaybeType<TStructType>(key.LookupType);
        if (!seedKeyStruct) {
            throw std::invalid_argument("GenGenericAggregateDispatchAst: key must be a struct");
        }
        std::vector<TExprPtr> seed = {
            var("initialized", i64Type),
            assign("initialized", std::move(init)),
            std::make_shared<TIfExpr>(loc,
                binary("==", ident("initialized"), numI64(0)),
                block({std::make_shared<TReturnExpr>(loc, numI64(0))}), nullptr),
            var("seed_key", key.LookupType),
            var("stored_witness", key.StoredType),
            assign("stored_witness", ZeroValueExpr(key.StoredType)),
            var("is_new", i64Type),
            assign("is_new", numI64(0)),
            var("state_slot", i64Type),
        };
        for (const int64_t id : emptyInputGroups) {
            std::vector<TExprPtr> fields;
            for (const auto& [fieldName, fieldType] : seedKeyStruct.Cast()->Fields) {
                fields.push_back(fieldName == "key_0"
                    ? number(id, key.Fields[0].Type)
                    : ZeroValueExpr(fieldType));
            }
            seed.push_back(assign("seed_key", std::make_shared<TStructConstructExpr>(
                loc, key.LookupType, std::move(fields))));
            seed.push_back(assign("state_slot", call(upsertName, {
                ident("ht"), ident("seed_key"), ident("stored_witness"),
                ident("is_new"), cast(call("rh_hash", {ident("seed_key")}), u64Type),
            })));
            seed.push_back(std::make_shared<TIfExpr>(loc,
                binary("<", ident("state_slot"), numI64(0)),
                block({call("aht_destroy", {ident("ht")}),
                    std::make_shared<TReturnExpr>(loc, numI64(0))}), nullptr));
        }
        seed.push_back(numI64(1));
        init = block(std::move(seed));
    }
    auto destroyStmts = StringCleanupCalls(layout);
    // agr_destroy frees a table in either layout.
    destroyStmts.push_back(call(rows ? "agr_destroy" : "aht_destroy", {ident("ht")}));
    destroyStmts.push_back(numI64(1));

    // The operations of EAggregateKernelOp (compiler.h); any other returns 0.
    auto opIs = [&](int64_t op) {
        return binary("==", ident("op"), numI64(op));
    };
    TExprPtr dispatch = numI64(0);
    if (rows) {
        dispatch = NOz::If(opIs(5),
            block({NOz::FieldAssign(ident("ht"), "RowExpectedGroups", ident("arg")), numI64(1)}),
            std::move(dispatch));
        dispatch = NOz::If(opIs(4),
            block({NOz::FieldAssign(ident("ht"), "RowSwitchGroups", ident("arg")), numI64(1)}),
            std::move(dispatch));
        dispatch = NOz::If(opIs(3),
            cast(call("agr_init", {
                ident("ht"), ident("arg"),
                numI64(static_cast<int64_t>(layout.NumAggBuffers)),
                numI64(static_cast<int64_t>(key.Size)),
                numI64(rows->RowBytes),
                numI64(AggRowBlockRows),
            }), i64Type),
            std::move(dispatch));
    }
    dispatch = NOz::If(opIs(2), block(std::move(destroyStmts)), std::move(dispatch));
    dispatch = NOz::If(opIs(1), block(std::move(update)), std::move(dispatch));
    dispatch = NOz::If(opIs(0), std::move(init), std::move(dispatch));
    auto body = std::make_shared<TBlockExpr>(loc,
        std::vector<TExprPtr>{std::make_shared<TReturnExpr>(loc, dispatch)});
    auto function = std::make_shared<TFunDecl>(
        loc, "agg_dispatch", std::vector<TGenericParam>{}, std::move(params), std::move(body), i64Type);
    return std::make_shared<TBlockExpr>(loc, std::vector<TExprPtr>{function});
}

NQumir::NAst::TExprPtr GenGenericAggregateRowsUpdateAst(
    const NQumir::NAst::TStructType& inputType,
    const TAggregateKeyDescriptor& key,
    const TAggReducerLayout& layout,
    const TAggRowLayout& rows,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr hashTableType,
    bool hasPrecomputedHash)
{
    using namespace NQumir::NAst;
    auto i64Type = std::make_shared<TIntegerType>();
    auto u64Type = std::make_shared<TIntegerType>(TIntegerType::U64);
    auto u8Type = std::make_shared<TIntegerType>(TIntegerType::U8);
    auto ptrKeyType = std::make_shared<TPointerType>(key.LookupType);
    auto ptrStoredKeyType = std::make_shared<TPointerType>(key.StoredType);
    auto numI64 = [&](int64_t value) { return NOz::TypedInt(value, i64Type); };
    auto binary = [&](const char* op, TExprPtr left, TExprPtr right) {
        return NOz::Bin(TOperator(op), std::move(left), std::move(right));
    };
    auto scratch = [&](const std::string& name) {
        return NOz::Field(NOz::Field("ht", "RowScratch"), name);
    };
    auto failure = [&]() {
        return NOz::Block({NOz::Return(numI64(-1))});
    };

    const TAggregateUpdateAst row(
        inputType, key, layout, columnType, rowSetType, hasPrecomputedHash);
    NOz::TFunBuilder builder("agg_rows_update");
    builder.Param("ht", std::make_shared<TReferenceType>(AsNamed("HashTable", hashTableType)))
        .Param("batch", std::make_shared<TReferenceType>(AsNamed("TRowSet", rowSetType)))
        .Param("start", i64Type)
        .Return(i64Type);
    for (auto& stmt : row.Prologue()) {
        builder.Stmt(std::move(stmt));
    }
    builder
        .Stmt(NOz::If(NOz::Unary(TOperator("!"), NOz::Call("rt_scratch_reserve", {
                NOz::Field("ht", "RowScratch"),
                numI64(AggRowChunkRows),
                numI64(rows.KeyStride),
            })),
            failure()))
        .Var("hashes", std::make_shared<TPointerType>(u64Type))
        .Assign("hashes", scratch("Hashes"))
        .Var("rows", std::make_shared<TPointerType>(i64Type))
        .Assign("rows", scratch("Rows"))
        .Var("keys", std::make_shared<TPointerType>(u8Type))
        .Assign("keys", scratch("Keys"))
        .Var("groups", std::make_shared<TPointerType>(u64Type))
        .Assign("groups", scratch("Entries"))
        .Var("i", i64Type)
        .Assign("i", NOz::Ident("start"));

    // Pass 1, the only one that reads the keys: the key and hash of every
    // selected row of the chunk.
    std::vector<TExprPtr> collect = row.Key("i");
    collect.push_back(NOz::ArrayAssign("rows", NOz::Ident("cnt"), NOz::Ident("i")));
    collect.push_back(NOz::ArrayAssign("hashes", NOz::Ident("cnt"), NOz::Ident("hash_value")));
    // The scratch keys keep the row layout's key alignment (KeyStride).
    collect.push_back(NOz::Var("key_slot", ptrKeyType));
    collect.push_back(NOz::Assign("key_slot", NOz::Cast(
        NOz::Add(NOz::Cast(NOz::Ident("keys"), i64Type),
            NOz::Mul(NOz::Ident("cnt"), numI64(rows.KeyStride))),
        ptrKeyType)));
    collect.push_back(NOz::ArrayAssign("key_slot", numI64(0), NOz::Ident("key_value")));
    collect.push_back(NOz::Assign("cnt", NOz::Add(NOz::Ident("cnt"), numI64(1))));

    // The last pass applies every row to its group: the row's states are the
    // buffers, at slot 0.
    std::vector<TExprPtr> apply = {
        NOz::Var("r", i64Type),
        NOz::Assign("r", NOz::Index("rows", NOz::Ident("j"))),
        NOz::Var("group", u64Type),
        NOz::Assign("group", NOz::Index("groups", NOz::Ident("j"))),
        NOz::Var("is_new", i64Type),
        NOz::Assign("is_new", NOz::Cast(
            binary("&", NOz::Ident("group"), NOz::TypedInt(1, u64Type)), i64Type)),
        NOz::Var("group_row", std::make_shared<TPointerType>(u8Type)),
        NOz::Assign("group_row", NOz::Cast(NOz::Cast(
            binary("&", NOz::Ident("group"), NOz::TypedInt(-2, u64Type)), i64Type),
            std::make_shared<TPointerType>(u8Type))),
        NOz::Var("state_slot", i64Type),
        NOz::Assign("state_slot", numI64(0)),
    };
    for (auto& stmt : row.Arguments("r")) {
        apply.push_back(std::move(stmt));
    }
    for (auto& stmt : row.Reducers([&](int word) {
             return RowStateWord("group_row", rows, word);
         }))
    {
        apply.push_back(std::move(stmt));
    }
    apply.push_back(NOz::Assign("j", NOz::Add(NOz::Ident("j"), numI64(1))));

    auto chunk = NOz::Block({
        NOz::Var("cnt", i64Type),
        NOz::Assign("cnt", numI64(0)),
        NOz::While(
            binary("&&", binary("<", NOz::Ident("i"), NOz::Ident("n")),
                binary("<", NOz::Ident("cnt"), numI64(AggRowChunkRows))),
            NOz::Block({
                NOz::If(row.Selected("i"), NOz::Block(std::move(collect))),
                NOz::Assign("i", NOz::Add(NOz::Ident("i"), numI64(1))),
            })),
        // Passes 2 and 3: agr_find_or_insert in aggregation_rows.oz.
        NOz::If(NOz::Unary(TOperator("!"), NOz::Call("agr_find_or_insert", {
                NOz::Ident("ht"),
                NOz::Ident("cnt"),
                numI64(rows.KeyOffset),
                numI64(rows.KeyStride),
                numI64(rows.StateOffset),
                NOz::Cast(numI64(0), ptrKeyType),
                NOz::Cast(numI64(0), ptrStoredKeyType),
            })),
            failure()),
        NOz::Var("j", i64Type),
        NOz::Assign("j", numI64(0)),
        NOz::While(binary("<", NOz::Ident("j"), NOz::Ident("cnt")), NOz::Block(std::move(apply))),
    });
    builder
        .Stmt(NOz::While(binary("<", NOz::Ident("i"), NOz::Ident("n")), std::move(chunk)))
        .Stmt(NOz::Return(numI64(0)));
    return NOz::Block({std::move(builder).Build()});
}

NQumir::NAst::TExprPtr GenGenericAggregateFinalizeAst(
    const TAggregateKeyDescriptor& key,
    const TAggReducerLayout& layout,
    NQumir::NAst::TTypePtr hashTableType,
    NQumir::NAst::TTypePtr columnType,
    const std::optional<TAggRowLayout>& rows)
{
    using namespace NQumir::NAst;
    NQumir::TLocation loc{};

    auto i64Type = std::make_shared<TIntegerType>();
    auto u8Type = std::make_shared<TIntegerType>(TIntegerType::U8);
    auto ptrKeyType = std::make_shared<TPointerType>(key.KeyType);
    auto ptrU8Type = std::make_shared<TPointerType>(u8Type);
    auto ptrPtrU8Type = std::make_shared<TPointerType>(ptrU8Type);

    auto numI64 = [&](int64_t value) -> TExprPtr {
        auto expr = std::make_shared<TNumberExpr>(loc, value);
        expr->Type = i64Type;
        return expr;
    };
    auto ptrI64Type = std::make_shared<TPointerType>(i64Type);
    auto ptrPtrI64Type = std::make_shared<TPointerType>(ptrI64Type);
    auto ptrColumnType = columnType
        ? std::make_shared<TPointerType>(
            AsNamed("TColumn", columnType))
        : nullptr;
    auto hashTableRefType = std::make_shared<TReferenceType>(
        AsNamed("HashTable", std::move(hashTableType)));
    auto ident = [&](const std::string& name) -> TExprPtr {
        return std::make_shared<TIdentExpr>(loc, name);
    };
    auto binary = [&](const char* op, TExprPtr left, TExprPtr right) -> TExprPtr {
        return std::make_shared<TBinaryExpr>(
            loc, TOperator(op), std::move(left), std::move(right));
    };
    auto index = [&](TExprPtr object, TExprPtr slot) -> TExprPtr {
        return std::make_shared<TIndexExpr>(loc, std::move(object), std::move(slot));
    };

    std::vector<TParam> params = {
        std::make_shared<TVarStmt>(loc, "ht", hashTableRefType),
        std::make_shared<TVarStmt>(loc, "columns", ptrColumnType),
        std::make_shared<TVarStmt>(loc, "output_buffers", ptrPtrI64Type),
        std::make_shared<TVarStmt>(loc, "output_agg_masks", ptrPtrU8Type),
        std::make_shared<TVarStmt>(loc, "output_capacity", i64Type),
    };

    std::vector<TExprPtr> bodyStmts;
    bodyStmts.push_back(std::make_shared<TVarStmt>(loc, "result", i64Type));
    bodyStmts.push_back(std::make_shared<TAssignExpr>(loc, "result",
        std::make_shared<TFieldAccessExpr>(loc, ident("ht"), "Size")));
    bodyStmts.push_back(std::make_shared<TIfExpr>(loc,
        binary("<", ident("output_capacity"), ident("result")),
        std::make_shared<TBlockExpr>(loc, std::vector<TExprPtr>{
            std::make_shared<TAssignExpr>(loc, "result", numI64(-1)),
        }),
        nullptr));

    // Fixed-width keys with physical states are read in slot order: a Ctrl
    // scan replaces the per-group state gather through SlotId.
    const bool slotOrder = !HasStringReducer(layout) &&
        std::ranges::none_of(key.Fields, [](const auto& field) {
            return static_cast<bool>(
                TMaybeType<TStringType>(UnwrapNamedType(field.Type)));
        });
    const char* keyIndex = slotOrder ? "phys" : "slot";
    // The output columns, which both layouts fill.
    std::vector<TExprPtr> project;
    // The swiss table's keys and states; with a row layout, they are read
    // only while the table is swiss.
    std::vector<TExprPtr> swissOnly;
    auto& swiss = rows ? swissOnly : project;
    swiss.push_back(std::make_shared<TVarStmt>(loc, "group_keys", ptrKeyType));
    swiss.push_back(std::make_shared<TAssignExpr>(loc, "group_keys",
        std::make_shared<TCastExpr>(loc,
            std::make_shared<TFieldAccessExpr>(
                loc, ident("ht"), slotOrder ? "Keys" : "GroupKeys"),
            ptrKeyType)));
    if (slotOrder) {
        swiss.push_back(std::make_shared<TVarStmt>(loc, "ctrl", ptrU8Type));
        swiss.push_back(std::make_shared<TAssignExpr>(loc, "ctrl",
            std::make_shared<TFieldAccessExpr>(loc, ident("ht"), "Ctrl")));
        swiss.push_back(std::make_shared<TVarStmt>(loc, "phys", i64Type));
        swiss.push_back(std::make_shared<TAssignExpr>(loc, "phys", numI64(0)));
    }
    if (!ptrColumnType) {
        throw std::invalid_argument(
            "GenGenericAggregateFinalizeAst: missing TColumn type");
    }
    for (size_t fieldIndex = 0; fieldIndex < key.Fields.size(); ++fieldIndex) {
        const bool isString = TMaybeType<TStringType>(
            UnwrapNamedType(key.Fields[fieldIndex].Type));
        const std::string maskName =
            "output_mask_" + std::to_string(fieldIndex);
        // Index() on `columns` gives this field's address directly, scaled
        // by qumir's real TColumn size. No pointer-array/cast step needed
        // here (see column() in agg_finish_rowset, which fills this same
        // `columns` array).
        auto fieldIndexLit = [&]() {
            return std::make_shared<TNumberExpr>(loc, static_cast<int64_t>(fieldIndex));
        };
        auto column = std::make_shared<TIndexExpr>(loc, ident("columns"), fieldIndexLit());
        if (key.Fields[fieldIndex].IsNullable) {
            project.push_back(std::make_shared<TVarStmt>(loc, maskName, ptrU8Type));
            project.push_back(std::make_shared<TAssignExpr>(loc, maskName,
                std::make_shared<TFieldAccessExpr>(loc, column, "Mask")));
        }
        if (isString) {
            const std::string dataName =
                "output_data_" + std::to_string(fieldIndex);
            const std::string offsetsName =
                "output_offsets_" + std::to_string(fieldIndex);
            const std::string copyResultName =
                "output_copy_result_" + std::to_string(fieldIndex);
            column = std::make_shared<TIndexExpr>(loc, ident("columns"), fieldIndexLit());
            project.push_back(std::make_shared<TVarStmt>(loc, dataName, ptrU8Type));
            project.push_back(std::make_shared<TAssignExpr>(loc, dataName,
                std::make_shared<TCastExpr>(loc,
                    std::make_shared<TCastExpr>(loc,
                        std::make_shared<TFieldAccessExpr>(
                            loc, column, "Data"), i64Type),
                    ptrU8Type)));
            column = std::make_shared<TIndexExpr>(loc, ident("columns"), fieldIndexLit());
            project.push_back(std::make_shared<TVarStmt>(
                loc, offsetsName, ptrI64Type));
            project.push_back(std::make_shared<TAssignExpr>(loc, offsetsName,
                std::make_shared<TCastExpr>(loc,
                    std::make_shared<TCastExpr>(loc,
                        std::make_shared<TFieldAccessExpr>(
                            loc, column, "Offsets"), i64Type),
                    ptrI64Type)));
            project.push_back(std::make_shared<TArrayAssignExpr>(loc,
                offsetsName,
                std::vector<TExprPtr>{
                    std::make_shared<TNumberExpr>(loc, int64_t{0})},
                std::make_shared<TNumberExpr>(loc, int64_t{0})));
            project.push_back(std::make_shared<TVarStmt>(
                loc, copyResultName, i64Type));
            project.push_back(std::make_shared<TAssignExpr>(
                loc, copyResultName,
                std::make_shared<TNumberExpr>(loc, int64_t{0})));
            continue;
        }
        const std::string name = "output_key_" + std::to_string(fieldIndex);
        auto ptrFieldType = std::make_shared<TPointerType>(
            ColumnBufferElementType(key.Fields[fieldIndex].Type));
        project.push_back(std::make_shared<TVarStmt>(loc, name, ptrFieldType));
        column = std::make_shared<TIndexExpr>(loc, ident("columns"), fieldIndexLit());
        project.push_back(std::make_shared<TAssignExpr>(loc, name,
            std::make_shared<TCastExpr>(loc,
                std::make_shared<TCastExpr>(loc,
                    std::make_shared<TFieldAccessExpr>(
                        loc, column, "Data"), i64Type),
                ptrFieldType)));
    }
    if (!layout.Reducers.empty()) {
        swiss.push_back(std::make_shared<TVarStmt>(loc, "agg_buffers", ptrPtrI64Type));
        swiss.push_back(std::make_shared<TAssignExpr>(loc, "agg_buffers",
            std::make_shared<TFieldAccessExpr>(loc, ident("ht"), "AggBuffers")));
        for (size_t i = 0; i < layout.Reducers.size(); ++i) {
            const auto& r = layout.Reducers[i];
            const std::string srcName = "agg_src_" + std::to_string(i);
            const std::string dstName = "output_agg_" + std::to_string(i);
            if (r.IsString()) {
                project.push_back(std::make_shared<TCallExpr>(loc,
                    ident("agg_string_finalize_at"), std::vector<TExprPtr>{
                        ident("ht"), numI64(r.ValueBufIdx),
                        numI64(r.ExtraBufIdx),
                        std::make_shared<TCastExpr>(loc,
                            index(ident("output_buffers"), numI64(i)), i64Type),
                        ident("result")}));
            } else {
                swiss.push_back(std::make_shared<TVarStmt>(loc, srcName, ptrI64Type));
                swiss.push_back(std::make_shared<TAssignExpr>(loc, srcName,
                    index(ident("agg_buffers"),
                        numI64(static_cast<int64_t>(r.ValueBufIdx)))));
                auto output = index(ident("output_buffers"), numI64(static_cast<int64_t>(i)));
                if (r.IsBool()) {
                    // A boolean output column is a bitmap, not one word per row.
                    project.push_back(std::make_shared<TVarStmt>(loc, dstName, ptrU8Type));
                    project.push_back(std::make_shared<TAssignExpr>(loc, dstName,
                        std::make_shared<TCastExpr>(loc,
                            std::make_shared<TCastExpr>(loc, std::move(output), i64Type),
                            ptrU8Type)));
                } else {
                    project.push_back(std::make_shared<TVarStmt>(loc, dstName, ptrI64Type));
                    project.push_back(std::make_shared<TAssignExpr>(
                        loc, dstName, std::move(output)));
                }
                if (r.IsBinInt()) {
                    const std::string hiName = "agg_hi_" + std::to_string(i);
                    swiss.push_back(std::make_shared<TVarStmt>(loc, hiName, ptrI64Type));
                    swiss.push_back(std::make_shared<TAssignExpr>(loc, hiName,
                        index(ident("agg_buffers"),
                            numI64(static_cast<int64_t>(r.ExtraBufIdx)))));
                }
            }
            if (!r.IsNullableOutput) {
                continue;
            }
            // Bind the internal valid-count buffer so the slot loop can derive a
            // nullable aggregate's output mask (zero valid args => NULL result).
            const std::string validName = "validbuf_" + std::to_string(i);
            swiss.push_back(std::make_shared<TVarStmt>(loc, validName, ptrI64Type));
            swiss.push_back(std::make_shared<TAssignExpr>(loc, validName,
                index(ident("agg_buffers"), numI64(static_cast<int64_t>(r.ValidBufIdx)))));
        }
    }
    project.push_back(std::make_shared<TVarStmt>(loc, "slot", i64Type));
    project.push_back(std::make_shared<TAssignExpr>(
        loc, "slot", std::make_shared<TNumberExpr>(loc, int64_t{0})));

    // Dense output order is independent of the physical aggregate-state slot.
    if (!HasStringReducer(layout) && !slotOrder) {
        swiss.push_back(std::make_shared<TVarStmt>(loc, "state_slots", ptrI64Type));
        swiss.push_back(std::make_shared<TAssignExpr>(loc, "state_slots",
            std::make_shared<TFieldAccessExpr>(loc, ident("ht"), "SlotId")));
    }
    auto stateSlot = [&]() -> TExprPtr {
        if (slotOrder) {
            return ident("phys");
        }
        if (HasStringReducer(layout)) {
            return ident("slot");
        }
        return index(ident("state_slots"), ident("slot"));
    };
    // The statements that project one group into output row `slot`: keyAt()
    // reads its key, stateAt(buffer, i) the state word of AggBuffers slot i,
    // `buffer` naming the swiss array bound for it.
    auto groupStatements = [&](const std::function<TExprPtr()>& keyAt,
                               const std::function<TExprPtr(const std::string&, int)>& stateAt)
    {
        std::vector<TExprPtr> loopStmts;
        for (size_t fieldIndex = 0; fieldIndex < key.Fields.size(); ++fieldIndex) {
            TExprPtr value = std::make_shared<TFieldAccessExpr>(
                loc, keyAt(), "key_" + std::to_string(fieldIndex));
            if (key.Fields[fieldIndex].IsNullable) {
                auto valid = std::make_shared<TFieldAccessExpr>(
                    loc, keyAt(), "valid_" + std::to_string(fieldIndex));
                loopStmts.push_back(std::make_shared<TCallExpr>(loc,
                    ident("qdb_bitmap_set_valid"),
                    std::vector<TExprPtr>{
                        ident("output_mask_" + std::to_string(fieldIndex)),
                        ident("slot"), std::move(valid)}));
            }
            const bool isString = TMaybeType<TStringType>(
                UnwrapNamedType(key.Fields[fieldIndex].Type));
            if (TMaybeType<TBoolType>(UnwrapNamedType(key.Fields[fieldIndex].Type))) {
                loopStmts.push_back(std::make_shared<TCallExpr>(loc,
                    ident("qdb_bitmap_set_valid"),
                    std::vector<TExprPtr>{
                        ident("output_key_" + std::to_string(fieldIndex)),
                        ident("slot"), std::move(value)}));
                continue;
            }
            if (!isString) {
                loopStmts.push_back(std::make_shared<TArrayAssignExpr>(
                    loc, "output_key_" + std::to_string(fieldIndex),
                    std::vector<TExprPtr>{ident("slot")}, std::move(value)));
                continue;
            }
            const std::string dataName =
                "output_data_" + std::to_string(fieldIndex);
            const std::string offsetsName =
                "output_offsets_" + std::to_string(fieldIndex);
            const std::string copyResultName =
                "output_copy_result_" + std::to_string(fieldIndex);
            auto offset = std::make_shared<TIndexExpr>(
                loc, ident(offsetsName), ident("slot"));
            auto destination = std::make_shared<TCastExpr>(loc,
                std::make_shared<TBinaryExpr>(loc, TOperator("+"),
                    std::make_shared<TCastExpr>(loc, ident(dataName), i64Type),
                    offset), ptrU8Type);
            auto source = std::make_shared<TFieldAccessExpr>(
                loc, value, "Data");
            auto byteSize = std::make_shared<TFieldAccessExpr>(
                loc, value, "Size");
            loopStmts.push_back(std::make_shared<TAssignExpr>(loc, copyResultName,
                std::make_shared<TCallExpr>(loc,
                    ident("qdb_string_copy_bytes"),
                    std::vector<TExprPtr>{
                        std::move(destination), std::move(source), byteSize})));
            loopStmts.push_back(std::make_shared<TArrayAssignExpr>(loc,
                offsetsName,
                std::vector<TExprPtr>{std::make_shared<TBinaryExpr>(loc,
                    TOperator("+"), ident("slot"),
                    std::make_shared<TNumberExpr>(loc, int64_t{1}))},
                std::make_shared<TBinaryExpr>(loc, TOperator("+"),
                    std::make_shared<TIndexExpr>(
                        loc, ident(offsetsName), ident("slot")),
                    std::make_shared<TFieldAccessExpr>(loc, value, "Size"))));
        }
        for (size_t i = 0; i < layout.Reducers.size(); ++i) {
            const auto& r = layout.Reducers[i];
            const std::string srcName = "agg_src_" + std::to_string(i);
            const std::string dstName = "output_agg_" + std::to_string(i);
            if (!r.IsString()) {
                if (r.IsBinInt()) {
                    const std::string hiName = "agg_hi_" + std::to_string(i);
                    auto outLoSlot = binary("*", ident("slot"), numI64(2));
                    auto outHiSlot = binary("+",
                        binary("*", ident("slot"), numI64(2)), numI64(1));
                    loopStmts.push_back(std::make_shared<TArrayAssignExpr>(loc, dstName,
                        std::vector<TExprPtr>{std::move(outLoSlot)},
                        stateAt(srcName, r.ValueBufIdx)));
                    loopStmts.push_back(std::make_shared<TArrayAssignExpr>(loc, dstName,
                        std::vector<TExprPtr>{std::move(outHiSlot)},
                        stateAt(hiName, r.ExtraBufIdx)));
                } else if (r.IsBool()) {
                    loopStmts.push_back(std::make_shared<TCallExpr>(loc,
                        ident("qdb_bitmap_set_valid"),
                        std::vector<TExprPtr>{
                            ident(dstName), ident("slot"),
                            binary("!=", stateAt(srcName, r.ValueBufIdx), numI64(0))}));
                } else {
                    loopStmts.push_back(std::make_shared<TArrayAssignExpr>(loc, dstName,
                        std::vector<TExprPtr>{ident("slot")},
                        stateAt(srcName, r.ValueBufIdx)));
                }
            }
            if (!r.IsNullableOutput) {
                continue;
            }
            const std::string validName = "validbuf_" + std::to_string(i);
            loopStmts.push_back(std::make_shared<TCallExpr>(loc,
                ident("qdb_bitmap_set_valid"),
                std::vector<TExprPtr>{
                    std::make_shared<TIndexExpr>(loc, ident("output_agg_masks"),
                        numI64(static_cast<int64_t>(i))),
                    ident("slot"),
                    std::make_shared<TBinaryExpr>(loc, TOperator(">"),
                        stateAt(validName, r.ValidBufIdx), numI64(0))}));
        }
        loopStmts.push_back(std::make_shared<TAssignExpr>(loc, "slot",
            std::make_shared<TBinaryExpr>(loc, TOperator("+"), ident("slot"),
                std::make_shared<TNumberExpr>(loc, int64_t{1}))));
        return loopStmts;
    };

    auto loopStmts = groupStatements(
        [&] {
            return std::make_shared<TIndexExpr>(loc, ident("group_keys"), ident(keyIndex));
        },
        [&](const std::string& buffer, int) {
            return index(ident(buffer), stateSlot());
        });
    if (slotOrder) {
        // Empty Ctrl bytes are 0x80; the scan stops at the last group.
        auto empty = std::make_shared<TNumberExpr>(loc, int64_t{128});
        empty->Type = u8Type;
        auto occupied = binary("<", index(ident("ctrl"), ident("phys")), empty);
        auto emit = std::make_shared<TIfExpr>(loc, std::move(occupied),
            std::make_shared<TBlockExpr>(loc, std::move(loopStmts)), nullptr);
        loopStmts = {
            std::move(emit),
            std::make_shared<TAssignExpr>(loc, "phys",
                binary("+", ident("phys"), numI64(1))),
        };
    }
    swiss.push_back(std::make_shared<TWhileStmtExpr>(loc,
        std::make_shared<TBinaryExpr>(loc, TOperator("<"),
            ident("slot"), ident("result")),
        std::make_shared<TBlockExpr>(loc, std::move(loopStmts))));

    if (rows) {
        // A table in the row layout: a scan of the arena, in insertion order.
        auto rowStmts = groupStatements(
            [&] { return NOz::Index("row_key", numI64(0)); },
            [&](const std::string&, int word) {
                return NOz::Index("row_states", numI64(word));
            });
        rowStmts.insert(rowStmts.begin(), {
            NOz::Var("row_key", ptrKeyType),
            NOz::Assign("row_key", NOz::Cast(NOz::Add(
                NOz::Cast(NOz::Ident("row"), i64Type), numI64(rows->KeyOffset)), ptrKeyType)),
            NOz::Var("row_states", ptrI64Type),
            NOz::Assign("row_states", NOz::Cast(NOz::Add(
                NOz::Cast(NOz::Ident("row"), i64Type), numI64(rows->StateOffset)), ptrI64Type)),
        });
        rowStmts.push_back(NOz::Assign("row", NOz::Call("ra_next_row",
            {NOz::Field("ht", "RowArena"), NOz::Ident("b"), NOz::Ident("end"), NOz::Ident("row")})));
        project.push_back(NOz::If(NOz::Call("agr_active", {NOz::Ident("ht")}), NOz::Block({
            NOz::Var("b", i64Type),
            NOz::Var("end", i64Type),
            NOz::Var("row", ptrU8Type),
            NOz::Assign("row", NOz::Call("ra_first_row",
                {NOz::Field("ht", "RowArena"), NOz::Ident("b"), NOz::Ident("end")})),
            NOz::While(binary("!=", NOz::Cast(NOz::Ident("row"), i64Type), numI64(0)),
                NOz::Block(std::move(rowStmts))),
        }), NOz::Block(std::move(swissOnly))));
    }

    bodyStmts.push_back(std::make_shared<TIfExpr>(loc,
        std::make_shared<TBinaryExpr>(loc, TOperator(">="), ident("result"),
            std::make_shared<TNumberExpr>(loc, int64_t{0})),
        std::make_shared<TBlockExpr>(loc, std::move(project)), nullptr));
    bodyStmts.push_back(std::make_shared<TReturnExpr>(loc, ident("result")));
    auto body = std::make_shared<TBlockExpr>(loc, std::move(bodyStmts));
    auto function = std::make_shared<TFunDecl>(
        loc, "agg_finalize", std::vector<TGenericParam>{}, std::move(params), std::move(body), i64Type);
    return std::make_shared<TBlockExpr>(loc, std::vector<TExprPtr>{function});
}

NQumir::NAst::TExprPtr GenGenericAggregateFinishRowSetAst(
    const TAggregateKeyDescriptor& key,
    const TAggReducerLayout& layout,
    NQumir::NAst::TTypePtr hashTableType,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType,
    bool rowLayout)
{
    using namespace NQumir::NAst;
    namespace Oz = NKernel::NOz;
    // agr_destroy frees a table in either layout.
    const char* destroy = rowLayout ? "agr_destroy" : "aht_destroy";

    // Conservative (native/wasm64) sizes, used only for qdb_alloc calls.
    // Real strides can be smaller under wasm32 (24 and 4 bytes), never
    // larger, so this never under-allocates. Never use these for address
    // math - use column() below, which qumir scales to the real target size.
    constexpr int64_t ColumnSize = 48;
    constexpr int64_t PtrSize = 8;

    auto i64Type = std::make_shared<TIntegerType>(TIntegerType::I64);
    auto i32Type = std::make_shared<TIntegerType>(TIntegerType::I32);
    auto i8Type = std::make_shared<TIntegerType>(TIntegerType::I8);
    auto u8Type = std::make_shared<TIntegerType>(TIntegerType::U8);
    auto ptrI8Type = std::make_shared<TPointerType>(i8Type);
    auto ptrU8Type = std::make_shared<TPointerType>(u8Type);
    auto ptrI64Type = std::make_shared<TPointerType>(i64Type);
    auto ptrPtrU8Type = std::make_shared<TPointerType>(ptrU8Type);
    auto ptrPtrI64Type = std::make_shared<TPointerType>(ptrI64Type);
    auto ptrColumnType = std::make_shared<TPointerType>(
        AsNamed("TColumn", columnType));
    auto rowSetRefType = std::make_shared<TReferenceType>(
        AsNamed("TRowSet", rowSetType));
    auto hashTableRefType = std::make_shared<TReferenceType>(
        AsNamed("HashTable", hashTableType));

    auto number = [&](int64_t value) -> TExprPtr {
        return Oz::TypedInt(value, i64Type);
    };
    auto numI32 = [&](int64_t value) -> TExprPtr {
        return Oz::TypedInt(value, i32Type);
    };
    auto numU8 = [&](int64_t value) -> TExprPtr {
        return Oz::TypedInt(value, u8Type);
    };
    auto allocAs = [&](TTypePtr type, TExprPtr byteSize) -> TExprPtr {
        return Oz::Cast(
            Oz::Call("qdb_alloc", {std::move(byteSize)}),
            std::move(type));
    };
    // Index() on a ptr TColumn array gives the element's address, scaled by
    // the real TColumn size. Use this for FieldAssign/FieldAccess only: its
    // result cannot be cast to a raw pointer, since qumir has no address-of
    // operator and a struct value cannot become a Ptr.
    auto column = [&](size_t idx) -> TExprPtr {
        return Oz::Index("columns", number(static_cast<int64_t>(idx)));
    };
    // TODO(wasm32): the string-reducer branch below still needs a raw
    // address for agg_string_finalize_at, which column() cannot give (see
    // above). It falls back to a fixed ColumnSize stride - correct for
    // wasm64, wrong for wasm32 the same way the key-field bug was. No
    // current query hits this (string MIN/MAX plus wasm32), so it stays
    // open. Fix it like the key-field path: pass `columns` and index it
    // inside the callee.
    auto columnPtr = [&](size_t idx) -> TExprPtr {
        return Oz::Cast(Oz::Add(
            Oz::Cast(Oz::Ident("columns"), i64Type),
            number(static_cast<int64_t>(idx) * ColumnSize)),
            ptrColumnType);
    };

    const int64_t keyCount = static_cast<int64_t>(key.Fields.size());
    const int64_t aggCount = static_cast<int64_t>(layout.Reducers.size());
    const int64_t columnCount = keyCount + aggCount;
    int64_t ownedPtrCount = 5; // columns + byte sizes + pointer tables.
    for (const auto& field : key.Fields) {
        ++ownedPtrCount; // Data
        if (field.IsNullable) {
            ++ownedPtrCount; // Mask
        }
        if (TMaybeType<TStringType>(UnwrapNamedType(field.Type))) {
            ++ownedPtrCount; // Offsets
        }
    }
    for (const auto& reducer : layout.Reducers) {
        ++ownedPtrCount; // Data
        if (reducer.IsString()) {
            ++ownedPtrCount; // Offsets
        }
        if (reducer.IsNullableOutput) {
            ++ownedPtrCount; // Mask
        }
    }

    Oz::TFunBuilder builder("agg_finish_rowset");
    builder
        .Param("ht", hashTableRefType)
        .Param("out_rowset", rowSetRefType)
        .Return(i64Type)
        .Var("size", i64Type)
        .Assign("size", Oz::Field("ht", "Size"))
        .Var("mask_bytes", i64Type)
        .Assign("mask_bytes",
            Oz::Bin(TOperator(">>"),
                Oz::Add(Oz::Ident("size"), number(7)),
                number(3)))
        .Var("owners", ptrI64Type)
        .Assign("owners", allocAs(ptrI64Type,
            number((ownedPtrCount + 1) * PtrSize)))
        .Stmt(Oz::ArrayAssign("owners", number(0), number(ownedPtrCount)))
        .Var("owner_idx", i64Type)
        .Assign("owner_idx", number(1));

    auto remember = [&](const std::string& ptrName) {
        builder
            .Stmt(Oz::ArrayAssign("owners", Oz::Ident("owner_idx"),
                Oz::Cast(Oz::Ident(ptrName), i64Type)))
            .Assign("owner_idx",
                Oz::Add(Oz::Ident("owner_idx"), number(1)));
    };

    builder
        .Var("columns", ptrColumnType)
        .Assign("columns", allocAs(ptrColumnType,
            number(std::max<int64_t>(columnCount, 1) * ColumnSize)));
    remember("columns");

    builder
        .Var("key_bytes", ptrI64Type)
        .Assign("key_bytes", allocAs(ptrI64Type,
            number(std::max<int64_t>(keyCount, 1) * PtrSize)));
    remember("key_bytes");

    builder
        .Var("agg_bytes", ptrI64Type)
        .Assign("agg_bytes", allocAs(ptrI64Type,
            number(std::max<int64_t>(aggCount, 1) * PtrSize)));
    remember("agg_bytes");

    builder
        .Var("agg_buffers", ptrPtrI64Type)
        .Assign("agg_buffers", allocAs(ptrPtrI64Type,
            number(std::max<int64_t>(aggCount, 1) * PtrSize)));
    remember("agg_buffers");

    builder
        .Var("agg_masks", ptrPtrU8Type)
        .Assign("agg_masks", allocAs(ptrPtrU8Type,
            number(std::max<int64_t>(aggCount, 1) * PtrSize)));
    remember("agg_masks");

    builder
        .Var("measured", i64Type)
        .Assign("measured", Oz::Call("agg_measure_outputs", {
            Oz::Ident("ht"), Oz::Ident("key_bytes"), Oz::Ident("agg_bytes"),
            Oz::Ident("size")}));
    auto measureFailure = StringCleanupCalls(layout);
    measureFailure.push_back(Oz::Call(destroy, {Oz::Ident("ht")}));
    measureFailure.push_back(Oz::Return(number(-1)));
    builder.Stmt(Oz::If(
        Oz::Bin(TOperator("!="), Oz::Ident("measured"), Oz::Ident("size")),
        Oz::Block(std::move(measureFailure))));

    for (size_t i = 0; i < key.Fields.size(); ++i) {
        const auto& fieldInfo = key.Fields[i];
        const bool isString = TMaybeType<TStringType>(
            UnwrapNamedType(fieldInfo.Type));
        const auto idx = number(static_cast<int64_t>(i));
        const std::string dataName = "key_data_" + std::to_string(i);
        const std::string maskName = "key_mask_" + std::to_string(i);
        const std::string offsetsName = "key_offsets_" + std::to_string(i);

        builder
            .Var(dataName, ptrU8Type)
            .Assign(dataName, allocAs(ptrU8Type, Oz::Index("key_bytes", idx)));
        remember(dataName);
        builder
            .Stmt(Oz::FieldAssign(
                column(i), "Data", Oz::Cast(Oz::Ident(dataName), ptrI8Type)))
            .Stmt(Oz::FieldAssign(column(i), "DataBitOffset", numI32(0)));

        if (fieldInfo.IsNullable) {
            builder
                .Var(maskName, ptrU8Type)
                .Assign(maskName, allocAs(ptrU8Type, Oz::Ident("mask_bytes")));
            remember(maskName);
            builder.Stmt(Oz::FieldAssign(column(i), "Mask", Oz::Ident(maskName)));
        } else {
            builder.Stmt(Oz::FieldAssign(column(i), "Mask", Oz::NullPtr(ptrU8Type)));
        }
        builder.Stmt(Oz::FieldAssign(column(i), "MaskBitOffset", numI32(0)));

        if (isString) {
            builder
                .Var(offsetsName, ptrI64Type)
                .Assign(offsetsName, allocAs(ptrI64Type,
                    Oz::Mul(Oz::Add(Oz::Ident("size"), number(1)), number(8))));
            remember(offsetsName);
            builder
                .Stmt(Oz::FieldAssign(column(i), "Offsets", Oz::Ident(offsetsName)))
                .Stmt(Oz::FieldAssign(column(i), "OffsetWidth", numU8(8)));
        } else {
            builder
                .Stmt(Oz::FieldAssign(column(i), "Offsets", Oz::NullPtr(ptrI64Type)))
                .Stmt(Oz::FieldAssign(column(i), "OffsetWidth", numU8(0)));
        }
    }

    for (size_t i = 0; i < layout.Reducers.size(); ++i) {
        const auto& reducer = layout.Reducers[i];
        const size_t columnIdx = key.Fields.size() + i;
        const auto idx = number(static_cast<int64_t>(i));
        const std::string dataName = "agg_data_" + std::to_string(i);
        const std::string maskName = "agg_mask_" + std::to_string(i);
        const std::string offsetsName = "agg_offsets_" + std::to_string(i);

        if (reducer.IsString()) {
            builder
                .Var(dataName, ptrU8Type)
                .Assign(dataName, allocAs(ptrU8Type, Oz::Index("agg_bytes", idx)));
            remember(dataName);
            builder
                .Var(offsetsName, ptrI64Type)
                .Assign(offsetsName, allocAs(ptrI64Type,
                    Oz::Mul(Oz::Add(Oz::Ident("size"), number(1)), number(8))));
            remember(offsetsName);
            builder
                .Stmt(Oz::ArrayAssign("agg_buffers", idx,
                    Oz::Cast(columnPtr(columnIdx), ptrI64Type)))
                .Stmt(Oz::FieldAssign(column(columnIdx), "Data",
                    Oz::Cast(Oz::Ident(dataName), ptrI8Type)))
                .Stmt(Oz::FieldAssign(column(columnIdx), "DataBitOffset", numI32(0)))
                .Stmt(Oz::FieldAssign(column(columnIdx), "Offsets",
                    Oz::Ident(offsetsName)))
                .Stmt(Oz::FieldAssign(column(columnIdx), "OffsetWidth", numU8(8)));
        } else {
            builder
                .Var(dataName, ptrI64Type)
                .Assign(dataName, allocAs(ptrI64Type, Oz::Index("agg_bytes", idx)));
            remember(dataName);
            builder
                .Stmt(Oz::ArrayAssign("agg_buffers", idx, Oz::Ident(dataName)))
                .Stmt(Oz::FieldAssign(column(columnIdx), "Data",
                    Oz::Cast(Oz::Ident(dataName), ptrI8Type)))
                .Stmt(Oz::FieldAssign(column(columnIdx), "DataBitOffset", numI32(0)))
                .Stmt(Oz::FieldAssign(column(columnIdx), "Offsets",
                    Oz::NullPtr(ptrI64Type)))
                .Stmt(Oz::FieldAssign(column(columnIdx), "OffsetWidth", numU8(0)));
        }

        if (reducer.IsNullableOutput) {
            builder
                .Var(maskName, ptrU8Type)
                .Assign(maskName, allocAs(ptrU8Type, Oz::Ident("mask_bytes")));
            remember(maskName);
            builder
                .Stmt(Oz::ArrayAssign("agg_masks", idx, Oz::Ident(maskName)))
                .Stmt(Oz::FieldAssign(column(columnIdx), "Mask", Oz::Ident(maskName)));
        } else {
            builder
                .Stmt(Oz::ArrayAssign("agg_masks", idx, Oz::NullPtr(ptrU8Type)))
                .Stmt(Oz::FieldAssign(column(columnIdx), "Mask", Oz::NullPtr(ptrU8Type)));
        }
        builder.Stmt(Oz::FieldAssign(
            column(columnIdx), "MaskBitOffset", numI32(0)));
    }

    builder
        .Var("finalized", i64Type)
        .Assign("finalized", Oz::Call("agg_finalize", {
            Oz::Ident("ht"),
            Oz::Ident("columns"),
            Oz::Ident("agg_buffers"),
            Oz::Ident("agg_masks"),
            Oz::Ident("size"),
        }));
    for (auto& cleanup : StringCleanupCalls(layout)) {
        builder.Stmt(std::move(cleanup));
    }
    builder
        .Stmt(Oz::Call(destroy, {Oz::Ident("ht")}))
        .Stmt(Oz::If(
            Oz::Bin(TOperator("!="), Oz::Ident("finalized"), Oz::Ident("size")),
            Oz::Block({Oz::Return(number(-1))})))
        .Stmt(Oz::FieldAssign(Oz::Ident("out_rowset"), "Columns", Oz::Ident("columns")))
        .Stmt(Oz::FieldAssign(Oz::Ident("out_rowset"), "ColumnCount", number(columnCount)))
        .Stmt(Oz::FieldAssign(Oz::Ident("out_rowset"), "RowCount", Oz::Ident("size")))
        .Stmt(Oz::FieldAssign(Oz::Ident("out_rowset"), "Selection", Oz::NullPtr(ptrU8Type)))
        .Stmt(Oz::FieldAssign(Oz::Ident("out_rowset"), "Destroy", Oz::NullPtr(ptrI64Type)))
        .Stmt(Oz::FieldAssign(Oz::Ident("out_rowset"), "Private", Oz::Ident("owners")))
        .Stmt(Oz::FieldAssign(Oz::Ident("out_rowset"), "RefCount", number(1)))
        .Stmt(Oz::Return(Oz::Ident("size")));

    return Oz::Block({std::move(builder).Build()});
}

NQumir::NAst::TExprPtr GenGenericAggregateMeasureAst(
    const TAggregateKeyDescriptor& key,
    const TAggReducerLayout& layout,
    NQumir::NAst::TTypePtr hashTableType,
    const std::optional<TAggRowLayout>& rows)
{
    using namespace NQumir::NAst;
    NQumir::TLocation loc{};
    auto i64Type = std::make_shared<TIntegerType>(TIntegerType::I64);
    auto ptrI64Type = std::make_shared<TPointerType>(i64Type);
    auto ptrPtrI64Type = std::make_shared<TPointerType>(ptrI64Type);
    auto ptrKeyType = std::make_shared<TPointerType>(key.StoredType);
    auto ptrU8Type = std::make_shared<TPointerType>(
        std::make_shared<TIntegerType>(TIntegerType::U8));
    auto hashTableRefType = std::make_shared<TReferenceType>(
        AsNamed("HashTable", std::move(hashTableType)));
    auto ident = [&](const std::string& name) -> TExprPtr {
        return std::make_shared<TIdentExpr>(loc, name);
    };
    auto number = [&](int64_t value) -> TExprPtr {
        auto result = std::make_shared<TNumberExpr>(loc, value);
        result->Type = i64Type;
        return result;
    };
    auto binary = [&](const char* op, TExprPtr left, TExprPtr right) -> TExprPtr {
        return std::make_shared<TBinaryExpr>(
            loc, TOperator(op), std::move(left), std::move(right));
    };
    auto outputAt = [&](size_t fieldIndex) -> TExprPtr {
        return std::make_shared<TIndexExpr>(loc, ident("output_key_bytes"),
            number(static_cast<int64_t>(fieldIndex)));
    };

    std::vector<TParam> params = {
        std::make_shared<TVarStmt>(loc, "ht", hashTableRefType),
        std::make_shared<TVarStmt>(loc, "output_key_bytes", ptrI64Type),
        std::make_shared<TVarStmt>(loc, "output_agg_bytes", ptrI64Type),
        std::make_shared<TVarStmt>(loc, "output_capacity", i64Type),
    };
    std::vector<TExprPtr> body;
    body.push_back(std::make_shared<TVarStmt>(loc, "size", i64Type));
    body.push_back(std::make_shared<TAssignExpr>(loc, "size",
        std::make_shared<TFieldAccessExpr>(loc, ident("ht"), "Size")));
    body.push_back(std::make_shared<TIfExpr>(loc,
        binary("<", ident("output_capacity"), ident("size")),
        std::make_shared<TBlockExpr>(loc, std::vector<TExprPtr>{
            std::make_shared<TReturnExpr>(loc, number(-1))}), nullptr));
    body.push_back(std::make_shared<TVarStmt>(loc, "group_keys", ptrKeyType));
    body.push_back(std::make_shared<TAssignExpr>(loc, "group_keys",
        std::make_shared<TCastExpr>(loc,
            std::make_shared<TFieldAccessExpr>(loc, ident("ht"), "GroupKeys"),
            ptrKeyType)));

    for (size_t fieldIndex = 0; fieldIndex < key.Fields.size(); ++fieldIndex) {
        const auto logical = UnwrapNamedType(key.Fields[fieldIndex].Type);
        if (ContainsLogicalString(key.Fields[fieldIndex].Type) &&
            !TMaybeType<TStringType>(logical)) {
            throw std::invalid_argument(
                "GenGenericAggregateMeasureAst: nested variable-width output "
                "is not supported");
        }
        if (TMaybeType<TStringType>(logical)) {
            body.push_back(std::make_shared<TArrayAssignExpr>(loc,
                "output_key_bytes", std::vector<TExprPtr>{
                    number(static_cast<int64_t>(fieldIndex))}, number(0)));
        } else if (TMaybeType<TBoolType>(logical)) {
            body.push_back(std::make_shared<TArrayAssignExpr>(loc,
                "output_key_bytes",
                std::vector<TExprPtr>{number(static_cast<int64_t>(fieldIndex))},
                binary(">>", binary("+", ident("size"), number(7)), number(3))));
        } else {
            body.push_back(std::make_shared<TArrayAssignExpr>(loc,
                "output_key_bytes",
                std::vector<TExprPtr>{number(static_cast<int64_t>(fieldIndex))},
                binary("*", ident("size"),
                    number(static_cast<int64_t>(key.Fields[fieldIndex].Size)))));
        }
    }

    if (HasStringReducer(layout)) {
        body.push_back(std::make_shared<TVarStmt>(loc, "agg_buffers", ptrPtrI64Type));
        body.push_back(std::make_shared<TAssignExpr>(loc, "agg_buffers",
            std::make_shared<TFieldAccessExpr>(loc, ident("ht"), "AggBuffers")));
    }
    for (size_t i = 0; i < layout.Reducers.size(); ++i) {
        const auto& reducer = layout.Reducers[i];
        TExprPtr bytes;
        if (reducer.IsString()) {
            bytes = std::make_shared<TCallExpr>(loc, ident("agg_string_measure"),
                std::vector<TExprPtr>{
                    std::make_shared<TIndexExpr>(loc, ident("agg_buffers"),
                        number(static_cast<int64_t>(reducer.ExtraBufIdx))),
                    ident("size")});
        } else if (reducer.IsBool()) {
            bytes = binary(">>", binary("+", ident("size"), number(7)), number(3));
        } else {
            bytes = binary("*", ident("size"), number(AggStateByteWidth(reducer)));
        }
        body.push_back(std::make_shared<TArrayAssignExpr>(loc,
            "output_agg_bytes", std::vector<TExprPtr>{number(static_cast<int64_t>(i))},
            std::move(bytes)));
    }

    // The bytes of the string keys, summed over the groups: the swiss table's
    // dense keys, or the rows of a table in the row layout.
    const bool stringKey = std::ranges::any_of(key.Fields, [](const auto& field) {
        return static_cast<bool>(TMaybeType<TStringType>(UnwrapNamedType(field.Type)));
    });
    auto addKeyBytes = [&](const std::function<TExprPtr()>& keyAt) {
        std::vector<TExprPtr> stmts;
        for (size_t fieldIndex = 0; fieldIndex < key.Fields.size(); ++fieldIndex) {
            if (!TMaybeType<TStringType>(UnwrapNamedType(key.Fields[fieldIndex].Type))) {
                continue;
            }
            TExprPtr stored = std::make_shared<TFieldAccessExpr>(
                loc, keyAt(), "key_" + std::to_string(fieldIndex));
            auto size = std::make_shared<TFieldAccessExpr>(
                loc, std::move(stored), "Size");
            stmts.push_back(std::make_shared<TArrayAssignExpr>(loc,
                "output_key_bytes",
                std::vector<TExprPtr>{number(static_cast<int64_t>(fieldIndex))},
                binary("+", outputAt(fieldIndex), std::move(size))));
        }
        return stmts;
    };
    std::vector<TExprPtr> swiss;
    swiss.push_back(std::make_shared<TVarStmt>(loc, "slot", i64Type));
    swiss.push_back(std::make_shared<TAssignExpr>(loc, "slot", number(0)));
    auto loop = addKeyBytes([&] {
        return std::make_shared<TIndexExpr>(loc, ident("group_keys"), ident("slot"));
    });
    loop.push_back(std::make_shared<TAssignExpr>(loc, "slot",
        binary("+", ident("slot"), number(1))));
    swiss.push_back(std::make_shared<TWhileStmtExpr>(loc,
        binary("<", ident("slot"), ident("size")),
        std::make_shared<TBlockExpr>(loc, std::move(loop))));
    if (rows && stringKey) {
        auto rowStmts = addKeyBytes([&] { return NOz::Index("row_key", number(0)); });
        rowStmts.insert(rowStmts.begin(), {
            NOz::Var("row_key", ptrKeyType),
            NOz::Assign("row_key", NOz::Cast(NOz::Add(
                NOz::Cast(NOz::Ident("row"), i64Type), number(rows->KeyOffset)), ptrKeyType)),
        });
        rowStmts.push_back(NOz::Assign("row", NOz::Call("ra_next_row",
            {NOz::Field("ht", "RowArena"), NOz::Ident("b"), NOz::Ident("end"), NOz::Ident("row")})));
        body.push_back(NOz::If(NOz::Call("agr_active", {NOz::Ident("ht")}), NOz::Block({
            NOz::Var("b", i64Type),
            NOz::Var("end", i64Type),
            NOz::Var("row", ptrU8Type),
            NOz::Assign("row", NOz::Call("ra_first_row",
                {NOz::Field("ht", "RowArena"), NOz::Ident("b"), NOz::Ident("end")})),
            NOz::While(binary("!=", NOz::Cast(NOz::Ident("row"), i64Type), number(0)),
                NOz::Block(std::move(rowStmts))),
        }), NOz::Block(std::move(swiss))));
    } else {
        body.insert(body.end(), swiss.begin(), swiss.end());
    }
    body.push_back(std::make_shared<TReturnExpr>(loc, ident("size")));

    auto function = std::make_shared<TFunDecl>(loc, "agg_measure_outputs",
        std::vector<TGenericParam>{},
        std::move(params), std::make_shared<TBlockExpr>(loc, std::move(body)),
        i64Type);
    return std::make_shared<TBlockExpr>(
        loc, std::vector<TExprPtr>{std::move(function)});
}

std::vector<NQumir::NAst::TExprPtr> GenReducerFunDecls(
    const TAggReducerLayout& layout)
{
    using namespace NQumir::NAst;
    NQumir::TLocation loc{};

    auto i64Type = std::make_shared<TIntegerType>();
    auto u64Type = std::make_shared<TIntegerType>(TIntegerType::U64);
    auto f64Type = std::make_shared<TFloatType>();
    auto boolType = std::make_shared<TBoolType>();
    auto voidType = std::make_shared<TVoidType>();
    auto ptrI64Type = std::make_shared<TPointerType>(i64Type);
    auto binIntType = BinIntStorageType();

    auto ident = [&](const std::string& name) {
        return std::make_shared<TIdentExpr>(loc, name);
    };
    auto numI64 = [&](int64_t value) -> TExprPtr {
        auto expr = std::make_shared<TNumberExpr>(loc, value);
        expr->Type = i64Type;
        return expr;
    };
    auto binary = [&](const char* op, TExprPtr l, TExprPtr r) -> TExprPtr {
        return std::make_shared<TBinaryExpr>(loc, TOperator(op), std::move(l), std::move(r));
    };
    auto block = [&](std::vector<TExprPtr> stmts) -> std::shared_ptr<TBlockExpr> {
        return std::make_shared<TBlockExpr>(loc, std::move(stmts));
    };
    // buf[dense_slot]
    auto slotOf = [&](const std::string& buf) -> TExprPtr {
        return std::make_shared<TIndexExpr>(loc, ident(buf), ident("dense_slot"));
    };
    // buf[dense_slot] = value
    auto storeSlot = [&](const std::string& buf, TExprPtr value) -> TExprPtr {
        return std::make_shared<TArrayAssignExpr>(loc, buf,
            std::vector<TExprPtr>{ident("dense_slot")}, std::move(value));
    };
    auto bitcast = [&](TExprPtr e, TTypePtr type) -> TExprPtr {
        return std::make_shared<TBitcastExpr>(loc,
            std::move(e),
            type);
    };
    auto cast = [&](TExprPtr e, TTypePtr type) -> TExprPtr {
        return std::make_shared<TCastExpr>(loc, std::move(e), std::move(type));
    };
    auto field = [&](TExprPtr object, const std::string& name) -> TExprPtr {
        return std::make_shared<TFieldAccessExpr>(loc, std::move(object), name);
    };
    // A BinInt is an i128; aggregate state still lives in two i64 slot words,
    // so it is split and reassembled here rather than read as struct fields.
    auto i128Type = std::make_shared<TIntegerType>(TIntegerType::I128);
    auto shift64 = [&]() -> TExprPtr {
        auto value = std::make_shared<TNumberExpr>(loc, int64_t{64});
        value->Type = i128Type;
        return value;
    };
    auto binInt = [&](TExprPtr lo, TExprPtr hi) -> TExprPtr {
        // The low half must not sign-extend, so it goes through u64 first.
        auto loWide = cast(cast(std::move(lo), u64Type), i128Type);
        auto hiWide = binary("<<", cast(std::move(hi), i128Type), shift64());
        return std::make_shared<TStructConstructExpr>(loc, binIntType,
            std::vector<TExprPtr>{binary("|", std::move(hiWide), std::move(loWide))},
            std::vector<std::string>{"V"});
    };
    auto storeBinInt = [&](const std::string& loBuf,
                           const std::string& hiBuf) -> std::vector<TExprPtr> {
        auto raw = [&]() -> TExprPtr { return field(ident("next"), "V"); };
        return {
            storeSlot(loBuf, cast(raw(), i64Type)),
            storeSlot(hiBuf, cast(binary(">>", raw(), shift64()), i64Type)),
        };
    };
    auto binIntAccumulate = [&](const std::string& func,
                                TExprPtr seed) -> TExprPtr {
        if (func == "sum") {
            return binary("+", ident("prev"), ident("value"));
        }
        if (func == "min") {
            return std::make_shared<TIfExpr>(loc, std::move(seed),
                ident("value"),
                std::make_shared<TIfExpr>(loc,
                    binary("<", ident("value"), ident("prev")),
                    ident("value"), ident("prev")));
        }
        if (func == "max") {
            return std::make_shared<TIfExpr>(loc, std::move(seed),
                ident("value"),
                std::make_shared<TIfExpr>(loc,
                    binary(">", ident("value"), ident("prev")),
                    ident("value"), ident("prev")));
        }
        throw std::invalid_argument(
            "GenReducerFunDecls: unsupported BinInt aggregate: " + func);
    };

    std::vector<TExprPtr> result;
    result.reserve(layout.Reducers.size());
    for (size_t i = 0; i < layout.Reducers.size(); ++i) {
        const auto& r = layout.Reducers[i];
        const std::string& func = r.Func;
        const std::string name = "reduce_" + std::to_string(i);

        if (r.IsString()) {
            continue;
        }

        if (r.IsBinInt()) {
            const std::string loBuf = "lo_buf";
            const std::string hiBuf = "hi_buf";
            std::vector<TParam> params = {
                std::make_shared<TVarStmt>(loc, loBuf, ptrI64Type),
                std::make_shared<TVarStmt>(loc, hiBuf, ptrI64Type),
                std::make_shared<TVarStmt>(loc, "dense_slot", i64Type),
            };
            if (r.NeedsValidity) {
                params.push_back(std::make_shared<TVarStmt>(loc, "valid_buf", ptrI64Type));
            }
            params.push_back(std::make_shared<TVarStmt>(loc, "value", binIntType));
            if (r.NeedsValidity) {
                params.push_back(std::make_shared<TVarStmt>(loc, "value_is_valid", boolType));
            } else {
                params.push_back(std::make_shared<TVarStmt>(loc, "is_new", boolType));
            }

            std::vector<TExprPtr> stmts = {
                std::make_shared<TVarStmt>(loc, "prev", binIntType),
                std::make_shared<TAssignExpr>(loc, "prev",
                    binInt(slotOf(loBuf), slotOf(hiBuf))),
                std::make_shared<TVarStmt>(loc, "next", binIntType),
            };
            if (r.NeedsValidity) {
                auto stores = storeBinInt(loBuf, hiBuf);
                std::vector<TExprPtr> inner = {
                    std::make_shared<TAssignExpr>(loc, "next",
                        binIntAccumulate(func,
                            binary("==", slotOf("valid_buf"), numI64(0)))),
                };
                inner.insert(inner.end(),
                    std::make_move_iterator(stores.begin()),
                    std::make_move_iterator(stores.end()));
                inner.push_back(storeSlot("valid_buf",
                    binary("+", slotOf("valid_buf"), numI64(1))));
                stmts.push_back(std::make_shared<TIfExpr>(loc, ident("value_is_valid"),
                    block(std::move(inner)), nullptr));
            } else {
                stmts.push_back(std::make_shared<TAssignExpr>(loc, "next",
                    binIntAccumulate(func, ident("is_new"))));
                auto stores = storeBinInt(loBuf, hiBuf);
                stmts.insert(stmts.end(),
                    std::make_move_iterator(stores.begin()),
                    std::make_move_iterator(stores.end()));
            }
            result.push_back(std::make_shared<TFunDecl>(loc, name,
                std::vector<TGenericParam>{},
                std::move(params), block(std::move(stmts)), voidType));
            continue;
        }

        // Shape A: scalar accumulator over a single i64 buffer slot. Used for
        // the whole non-nullable-argument path and for count(*). Byte-identical
        // to the historical reducer contract.
        if (!r.NeedsValidity) {
            std::vector<TParam> params = {
                std::make_shared<TVarStmt>(loc, "prev", i64Type),
                std::make_shared<TVarStmt>(loc, "value", i64Type),
                std::make_shared<TVarStmt>(loc, "is_new", boolType),
            };
            TExprPtr resultExpr;
            if (r.IsFloat()) {
                // f64 reducer: states/values are carried as i64 bits, so we
                // bitcast to f64, run f64 arithmetic, and bitcast back to i64.
                auto prevF = bitcast(ident("prev"), f64Type);
                auto valueF = bitcast(ident("value"), f64Type);
                if (func == "sum") {
                    resultExpr = bitcast(binary("+", prevF, valueF), i64Type);
                } else if (func == "min") {
                    resultExpr = bitcast(std::make_shared<TIfExpr>(loc, ident("is_new"),
                        valueF,
                        std::make_shared<TIfExpr>(loc, binary("<", valueF, prevF),
                            valueF, prevF)), i64Type);
                } else if (func == "max") {
                    resultExpr = bitcast(std::make_shared<TIfExpr>(loc, ident("is_new"),
                        valueF,
                        std::make_shared<TIfExpr>(loc, binary(">", valueF, prevF),
                            valueF, prevF)), i64Type);
                } else {
                    throw std::invalid_argument(
                        "GenReducerFunDecls: unsupported float aggregate: " + func);
                }
            } else if (func == "count") {
                resultExpr = binary("+", ident("prev"), numI64(1));
            } else if (func == "sum") {
                resultExpr = binary("+", ident("prev"), ident("value"));
            } else if (func == "min") {
                resultExpr = std::make_shared<TIfExpr>(loc, ident("is_new"), ident("value"),
                    std::make_shared<TIfExpr>(loc, binary("<", ident("value"), ident("prev")),
                        ident("value"), ident("prev")));
            } else if (func == "max") {
                resultExpr = std::make_shared<TIfExpr>(loc, ident("is_new"), ident("value"),
                    std::make_shared<TIfExpr>(loc, binary(">", ident("value"), ident("prev")),
                        ident("value"), ident("prev")));
            } else {
                throw std::invalid_argument(
                    "GenReducerFunDecls: unknown aggregate function: " + func);
            }
            auto body = block({std::make_shared<TReturnExpr>(loc, std::move(resultExpr))});
            result.push_back(std::make_shared<TFunDecl>(loc, name,
                std::vector<TGenericParam>{},
                std::move(params), std::move(body), i64Type));
            continue;
        }

        // Shape B-count: count(arg) with a nullable argument. Only counts rows
        // whose argument is non-null; count(*) never reaches here (HasArg false).
        if (func == "count") {
            std::vector<TParam> params = {
                std::make_shared<TVarStmt>(loc, "buf", ptrI64Type),
                std::make_shared<TVarStmt>(loc, "dense_slot", i64Type),
                std::make_shared<TVarStmt>(loc, "value_is_valid", boolType),
            };
            auto body = block({
                std::make_shared<TIfExpr>(loc, ident("value_is_valid"),
                    block({storeSlot("buf", binary("+", slotOf("buf"), numI64(1)))}),
                    nullptr),
            });
            result.push_back(std::make_shared<TFunDecl>(loc, name,
                std::vector<TGenericParam>{},
                std::move(params), std::move(body), voidType));
            continue;
        }

        // Shape B-agg: sum/min/max with a nullable argument. valid_buf tracks the
        // number of non-null contributions seen so far; the first valid value
        // seeds buf, and only later values accumulate/compare (so min/max never
        // fold against the zero-initialized buffer).
        std::vector<TParam> params = {
            std::make_shared<TVarStmt>(loc, "buf", ptrI64Type),
            std::make_shared<TVarStmt>(loc, "valid_buf", ptrI64Type),
            std::make_shared<TVarStmt>(loc, "dense_slot", i64Type),
            std::make_shared<TVarStmt>(loc, "value", i64Type),
            std::make_shared<TVarStmt>(loc, "value_is_valid", boolType),
        };
        TExprPtr accumulate;
        if (r.IsFloat()) {
            // States/values carried as i64 bits: bitcast to f64, op, bitcast back.
            auto prevF = bitcast(slotOf("buf"), f64Type);
            auto valueF = bitcast(ident("value"), f64Type);
            if (func == "sum") {
                accumulate = storeSlot("buf", bitcast(binary("+", prevF, valueF), i64Type));
            } else if (func == "min") {
                accumulate = std::make_shared<TIfExpr>(loc, binary("<", valueF, prevF),
                    block({storeSlot("buf", ident("value"))}), nullptr);
            } else if (func == "max") {
                accumulate = std::make_shared<TIfExpr>(loc, binary(">", valueF, prevF),
                    block({storeSlot("buf", ident("value"))}), nullptr);
            } else {
                throw std::invalid_argument(
                    "GenReducerFunDecls: unsupported float aggregate: " + func);
            }
        } else if (func == "sum") {
            accumulate = storeSlot("buf", binary("+", slotOf("buf"), ident("value")));
        } else if (func == "min") {
            accumulate = std::make_shared<TIfExpr>(loc,
                binary("<", ident("value"), slotOf("buf")),
                block({storeSlot("buf", ident("value"))}), nullptr);
        } else if (func == "max") {
            accumulate = std::make_shared<TIfExpr>(loc,
                binary(">", ident("value"), slotOf("buf")),
                block({storeSlot("buf", ident("value"))}), nullptr);
        } else {
            throw std::invalid_argument(
                "GenReducerFunDecls: unknown aggregate function: " + func);
        }
        auto seedOrAccumulate = std::make_shared<TIfExpr>(loc,
            binary("==", slotOf("valid_buf"), numI64(0)),
            block({storeSlot("buf", ident("value"))}),
            block({std::move(accumulate)}));
        auto inner = block({
            std::move(seedOrAccumulate),
            storeSlot("valid_buf", binary("+", slotOf("valid_buf"), numI64(1))),
        });
        auto body = block({
            std::make_shared<TIfExpr>(loc, ident("value_is_valid"),
                std::move(inner), nullptr),
        });
        result.push_back(std::make_shared<TFunDecl>(loc, name,
            std::vector<TGenericParam>{},
            std::move(params), std::move(body), voidType));
    }
    return result;
}

NQumir::NAst::TExprPtr GenApplyReducersFunDecl(const TAggReducerLayout& layout)
{
    using namespace NQumir::NAst;
    NQumir::TLocation loc{};

    auto i64Type = std::make_shared<TIntegerType>();
    auto boolType = std::make_shared<TBoolType>();
    auto ptrI64Type = std::make_shared<TPointerType>(i64Type);
    auto ptrPtrI64Type = std::make_shared<TPointerType>(ptrI64Type);

    auto ident = [&](const std::string& name) {
        return std::make_shared<TIdentExpr>(loc, name);
    };
    auto numI64 = [&](int64_t value) -> TExprPtr {
        auto expr = std::make_shared<TNumberExpr>(loc, value);
        expr->Type = i64Type;
        return expr;
    };
    auto bindBuffer = [&](const std::string& name, int index, std::vector<TExprPtr>& out) {
        out.push_back(std::make_shared<TVarStmt>(loc, name, ptrI64Type));
        out.push_back(std::make_shared<TAssignExpr>(loc, name,
            std::make_shared<TIndexExpr>(loc, ident("agg_buffers"),
                numI64(static_cast<int64_t>(index)))));
    };

    std::vector<TParam> params = {
        std::make_shared<TVarStmt>(loc, "agg_buffers", ptrPtrI64Type),
        std::make_shared<TVarStmt>(loc, "dense_slot", i64Type),
        std::make_shared<TVarStmt>(loc, "value", i64Type),
        std::make_shared<TVarStmt>(loc, "value_is_valid", boolType),
        std::make_shared<TVarStmt>(loc, "is_new", boolType),
    };

    std::vector<TExprPtr> stmts;
    for (size_t i = 0; i < layout.Reducers.size(); ++i) {
        const auto& r = layout.Reducers[i];
        const std::string bufName = "buf_" + std::to_string(i);
        const std::string reduceName = "reduce_" + std::to_string(i);
        bindBuffer(bufName, r.ValueBufIdx, stmts);
        if (r.IsBinInt() || r.IsString()) {
            // Legacy aht_update/agg_apply_reducers carries a single i64 value.
            // Wide/string aggregation is handled by the fused dispatch path, which
            // materializes typed per-column values before calling reducers.
            continue;
        }

        if (!r.NeedsValidity) {
            // Shape A: out-of-line scalar reducer; capture and store the result.
            auto prev = std::make_shared<TIndexExpr>(loc, ident(bufName), ident("dense_slot"));
            auto call = std::make_shared<TCallExpr>(loc, ident(reduceName),
                std::vector<TExprPtr>{prev, ident("value"), ident("is_new")});
            stmts.push_back(std::make_shared<TArrayAssignExpr>(loc, bufName,
                std::vector<TExprPtr>{ident("dense_slot")}, call));
            continue;
        }

        if (r.Func == "count") {
            // Shape B-count: in-place reducer, no return value.
            stmts.push_back(std::make_shared<TCallExpr>(loc, ident(reduceName),
                std::vector<TExprPtr>{
                    ident(bufName), ident("dense_slot"), ident("value_is_valid")}));
            continue;
        }

        // Shape B-agg: in-place reducer over value buffer + valid-count buffer.
        const std::string validName = "validbuf_" + std::to_string(i);
        bindBuffer(validName, r.ValidBufIdx, stmts);
        stmts.push_back(std::make_shared<TCallExpr>(loc, ident(reduceName),
            std::vector<TExprPtr>{
                ident(bufName), ident(validName), ident("dense_slot"),
                ident("value"), ident("value_is_valid")}));
    }

    auto body = std::make_shared<TBlockExpr>(loc, std::move(stmts));
    return std::make_shared<TFunDecl>(loc, "agg_apply_reducers",
        std::vector<TGenericParam>{},
        std::move(params), std::move(body), std::make_shared<TVoidType>());
}

} // namespace NKernel
} // namespace NQdb
