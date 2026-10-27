#include <qdb/kernel/join_gen.h>

#include <qdb/kernel/aggregate_key.h>
#include <qdb/kernel/builder.h>
#include <qdb/kernel/column_value.h>
#include <qdb/kernel/gen.h>
#include <qdb/plan/types/decimal.h>
#include <qdb/plan/types/nullable.h>

#include <qumir/error.h>

#include <algorithm>

namespace NQdb::NKernel {

using namespace NQumir::NAst;

namespace {

TTypePtr NamedType(const std::string& name) {
    return std::make_shared<TNamedType>(name, nullptr);
}

TTypePtr ColumnPointerType(const TTypePtr& columnType, const TTypePtr& rowSetType) {
    auto rowSetStruct = TMaybeType<TStructType>(UnwrapNamedType(rowSetType));
    if (rowSetStruct) {
        for (const auto& [name, type] : rowSetStruct.Cast()->Fields) {
            if (name == "Columns") {
                return type;
            }
        }
    }
    return std::make_shared<TPointerType>(columnType ? columnType : NamedType("TColumn"));
}

TTypePtr PointerPointeeOr(const TTypePtr& pointerType, const TTypePtr& fallback) {
    if (auto pointer = TMaybeType<TPointerType>(pointerType)) {
        return pointer.Cast()->PointeeType;
    }
    return fallback
        ? fallback
        : NamedType("TColumn");
}

TAggregateKeyDescriptor JoinKeyAggregateShim(const TJoinKeyDescriptor& key) {
    TAggregateKeyDescriptor shim;
    shim.TypeName = key.TypeName;
    shim.LookupTypeName = key.LookupTypeName;
    shim.StoredTypeName = key.StoredTypeName;
    shim.KeyType = key.KeyType;
    shim.LookupType = key.LookupType;
    shim.StoredType = key.StoredType;
    shim.Size = key.Size;
    shim.Alignment = key.Alignment;
    shim.Fields.reserve(key.Fields.size());
    for (const auto& field : key.Fields) {
        shim.Fields.push_back(
            TAggregateKeyField{
                .ColumnName = field.LeftColumnName,
                .ColumnIndex = field.LeftColumnIndex,
                .Type = field.Type,
                .LookupType = field.LookupType,
                .StoredType = field.StoredType,
                .IsNullable = field.IsNullable,
                .Offset = field.Offset,
                .Size = field.Size,
                .Alignment = field.Alignment,
            });
    }
    return shim;
}


// A join key read from batch row `rowVar` of the key_column_N columns: Setup
// declares the per-field temporaries (prefix + N), Value builds the
// <named LookupKey> struct in the order the struct declares its fields
// (key_N <- value, valid_N <- validity). TKeyDescriptor is TJoinKeyDescriptor
// or its TAggregateKeyDescriptor shim.
struct TKeyReadAst {
    std::vector<TExprPtr> Setup;
    TExprPtr Value;
};

template <typename TKeyDescriptor>
TKeyReadAst GenKeyReadAst(
    const TKeyDescriptor& key,
    const std::string& rowVar,
    const std::string& temporaryPrefix,
    const TTypePtr& stringViewType)
{
    NQumir::TLocation loc{};
    std::vector<TColumnValueAst> keyFields;
    keyFields.reserve(key.Fields.size());
    for (size_t fieldIndex = 0; fieldIndex < key.Fields.size(); ++fieldIndex) {
        auto logicalType = key.Fields[fieldIndex].IsNullable
            ? std::make_shared<TNullable>(key.Fields[fieldIndex].Type)
            : key.Fields[fieldIndex].Type;
        keyFields.push_back(BuildColumnValueAst(
            "key_column_" + std::to_string(fieldIndex),
            rowVar,
            temporaryPrefix + std::to_string(fieldIndex),
            std::move(logicalType),
            stringViewType));
    }
    auto namedKey = TMaybeType<TNamedType>(key.LookupType);
    auto keyStruct = namedKey
        ? TMaybeType<TStructType>(namedKey.Cast()->UnderlyingType)
        : TMaybeType<TStructType>(key.LookupType);
    if (!keyStruct) {
        throw NQumir::TError("join key must be a struct");
    }
    std::vector<TExprPtr> structFields;
    structFields.reserve(keyStruct.Cast()->Fields.size());
    for (const auto& [fieldName, fieldType] : keyStruct.Cast()->Fields) {
        const bool validity = fieldName.starts_with("valid_");
        const std::string_view prefix = validity
            ? "valid_"
            : "key_";
        const size_t fieldIndex = std::stoull(fieldName.substr(prefix.size()));
        structFields.push_back(
            validity ? keyFields[fieldIndex].IsValid : keyFields[fieldIndex].Value);
    }
    TKeyReadAst result;
    for (auto& keyField : keyFields) {
        result.Setup.insert(
            result.Setup.end(),
            std::make_move_iterator(keyField.Setup.begin()),
            std::make_move_iterator(keyField.Setup.end()));
    }
    result.Value =
        std::make_shared<TStructConstructExpr>(loc, key.LookupType, std::move(structFields));
    return result;
}

} // namespace

std::vector<NQumir::NAst::TExprPtr> GenKeyTypeDecls(const TAggregateKeyDescriptor& key) {
    NQumir::TLocation loc{};
    std::vector<NQumir::NAst::TExprPtr> stmts;
    if (!key.LookupTypeName.empty()) {
        stmts.push_back(std::make_shared<TTypeDeclStmt>(loc, key.LookupType));
    }
    if (!key.StoredTypeName.empty()) {
        stmts.push_back(std::make_shared<TTypeDeclStmt>(loc, key.StoredType));
    } else if (TMaybeType<TNamedType>(key.KeyType)) {
        stmts.push_back(std::make_shared<TTypeDeclStmt>(loc, key.KeyType));
    }
    return stmts;
}

std::vector<NQumir::NAst::TExprPtr> GenJoinKeyTypeDecls(const TJoinKeyDescriptor& key) {
    return GenKeyTypeDecls(JoinKeyAggregateShim(key));
}

std::vector<NQumir::NAst::TExprPtr> GenJoinKeyOpsFunDecls(const TJoinKeyDescriptor& key) {
    auto functions = GenKeyOperationFunDecls(JoinKeyAggregateShim(key));
    auto addValidity = [&](const TTypePtr& type) {
        namespace Oz = NOz;
        // Hash table equality groups NULL keys together. Reject NULL components
        // for ordinary equality, while allowing NULL-equal keys for set membership.
        TExprPtr valid = Oz::Bool(true);
        for (size_t i = 0; i < key.Fields.size(); ++i) {
            if (key.Fields[i].IsNullable && !key.Fields[i].NullsEqual) {
                valid = Oz::Bin(
                    TOperator("&&"),
                    std::move(valid),
                    Oz::Field("key", "valid_" + std::to_string(i)));
            }
        }
        Oz::TFunBuilder builder("jt_key_valid");
        builder.Param("key", type)
            .Return(std::make_shared<TBoolType>())
            .Stmt(Oz::Return(std::move(valid)));
        functions.push_back(std::move(builder).Build());
    };
    addValidity(key.StoredType);
    if (key.HasDistinctLookupType()) {
        addValidity(key.LookupType);
    }
    return functions;
}

std::vector<NQumir::NAst::TExprPtr> GenJoinKeyOwnershipFunDecls(const TJoinKeyDescriptor& key) {
    return GenKeyOwnershipFunDecls(JoinKeyAggregateShim(key));
}

NQumir::NAst::TExprPtr GenJoinInsertKeyOnlyAst(
    const TJoinKeyDescriptor& key,
    const std::string& funcName,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr hashTableType,
    NQumir::NAst::TTypePtr pairBufferType,
    NQumir::NAst::TTypePtr stringViewType)
{
    NQumir::TLocation loc{};

    auto i64Type = std::make_shared<TIntegerType>();
    auto u8Type = std::make_shared<TIntegerType>(TIntegerType::U8);
    auto boolType = std::make_shared<TBoolType>();
    auto ptrU8Type = std::make_shared<TPointerType>(u8Type);
    auto ptrI64Type = std::make_shared<TPointerType>(i64Type);

    auto ident = [&](const std::string& name) -> TExprPtr {
        return std::make_shared<TIdentExpr>(loc, name);
    };
    auto number = [&](int64_t value, TTypePtr type) -> TExprPtr {
        auto result = std::make_shared<TNumberExpr>(loc, value);
        result->Type = std::move(type);
        return result;
    };
    auto numI64 = [&](int64_t value) {
        return number(value, i64Type);
    };
    auto binary = [&](const char* op, TExprPtr left, TExprPtr right) -> TExprPtr {
        return std::make_shared<TBinaryExpr>(loc, TOperator(op), std::move(left), std::move(right));
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

    auto hashTableRefType = std::make_shared<TReferenceType>(AsNamed("HashTable", hashTableType));
    auto rowSetRefType = std::make_shared<TReferenceType>(AsNamed("TRowSet", rowSetType));
    auto pairBufferRefType =
        std::make_shared<TReferenceType>(AsNamed("PairBuffer", pairBufferType));

    // Same ABI prefix as GenJoinProcessAst.
    std::vector<TParam> params = {
        std::make_shared<TVarStmt>(loc, "own", hashTableRefType),
        std::make_shared<TVarStmt>(loc, "opp", hashTableRefType),
        std::make_shared<TVarStmt>(loc, "batch", rowSetRefType),
        std::make_shared<TVarStmt>(loc, "key_columns", ptrI64Type),
        std::make_shared<TVarStmt>(loc, "batch_idx", i64Type),
        std::make_shared<TVarStmt>(loc, "pairs", pairBufferRefType),
    };

    auto ptrColumnType = ColumnPointerType(columnType, rowSetType);
    auto columnValueType = PointerPointeeOr(ptrColumnType, columnType);
    auto columnAt = [&](size_t fieldIndex) -> TExprPtr {
        return std::make_shared<TIndexExpr>(
            loc,
            ident("cols"),
            std::make_shared<TIndexExpr>(
                loc,
                ident("key_columns"),
                numI64(static_cast<int64_t>(fieldIndex))));
    };

    std::vector<TExprPtr> body;
    body.push_back(var("n", i64Type));
    body.push_back(assign("n", field("batch", "RowCount")));
    body.push_back(var("selection", ptrU8Type));
    body.push_back(assign("selection", field("batch", "Selection")));
    body.push_back(var("cols", ptrColumnType));
    body.push_back(assign("cols", field("batch", "Columns")));
    for (size_t fi = 0; fi < key.Fields.size(); ++fi) {
        const std::string name = "key_column_" + std::to_string(fi);
        body.push_back(var(name, columnValueType));
        body.push_back(assign(name, columnAt(fi)));
    }
    body.push_back(var("selection_is_null", boolType));
    body.push_back(
        assign("selection_is_null", binary("==", cast(ident("selection"), i64Type), numI64(0))));
    body.push_back(var("i", i64Type));
    body.push_back(assign("i", numI64(0)));

    auto keyRead = GenKeyReadAst(key, "i", "key_value_", stringViewType);

    body.push_back(var("stored_witness", key.StoredType));
    body.push_back(assign("stored_witness", ZeroValueExpr(key.StoredType)));

    auto u64Type = std::make_shared<TIntegerType>(TIntegerType::U64);
    auto insertCall = call(
        "jt_insert_slot_only",
        {
            ident("own"),
            ident("key_value"),
            ident("stored_witness"),
            cast(call("rh_hash", {ident("key_value")}), u64Type),
        });

    std::vector<TExprPtr> process;
    process.insert(
        process.end(),
        std::make_move_iterator(keyRead.Setup.begin()),
        std::make_move_iterator(keyRead.Setup.end()));
    process.push_back(var("key_value", key.LookupType));
    process.push_back(assign("key_value", std::move(keyRead.Value)));
    process.push_back(
        std::make_shared<TIfExpr>(
            loc,
            std::make_shared<TUnaryExpr>(loc, TOperator("!"), std::move(insertCall)),
            block({std::make_shared<TReturnExpr>(loc, number(0, boolType))}),
            nullptr));

    auto selected = binary(
        "||",
        ident("selection_is_null"),
        binary(
            "!=",
            std::make_shared<TIndexExpr>(loc, ident("selection"), ident("i")),
            number(0, u8Type)));
    auto loop = block({
        std::make_shared<TIfExpr>(loc, std::move(selected), block(std::move(process)), nullptr),
        assign("i", binary("+", ident("i"), numI64(1))),
    });
    body.push_back(
        std::make_shared<TWhileStmtExpr>(
            loc,
            binary("<", ident("i"), ident("n")),
            std::move(loop)));
    body.push_back(std::make_shared<TReturnExpr>(loc, number(1, boolType)));

    return std::make_shared<TFunDecl>(
        loc,
        funcName,
        std::vector<TGenericParam>{},
        std::move(params),
        std::make_shared<TBlockExpr>(loc, std::move(body)),
        boolType);
}

NQumir::NAst::TExprPtr GenJoinFinalizeSemiAntiAst(
    const TJoinKeyDescriptor& key,
    bool isAnti,
    const std::string& funcName,
    NQumir::NAst::TTypePtr hashTableType,
    NQumir::NAst::TTypePtr pairBufferType)
{
    NQumir::TLocation loc{};

    auto i64Type = std::make_shared<TIntegerType>();
    auto boolType = std::make_shared<TBoolType>();
    auto ptrI64Type = std::make_shared<TPointerType>(i64Type);

    auto ident = [&](const std::string& name) -> TExprPtr {
        return std::make_shared<TIdentExpr>(loc, name);
    };
    auto number = [&](int64_t value, TTypePtr type) -> TExprPtr {
        auto result = std::make_shared<TNumberExpr>(loc, value);
        result->Type = std::move(type);
        return result;
    };
    auto numI64 = [&](int64_t value) {
        return number(value, i64Type);
    };
    auto binary = [&](const char* op, TExprPtr left, TExprPtr right) -> TExprPtr {
        return std::make_shared<TBinaryExpr>(loc, TOperator(op), std::move(left), std::move(right));
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
    auto index = [&](TExprPtr arr, TExprPtr idx) -> TExprPtr {
        return std::make_shared<TIndexExpr>(loc, std::move(arr), std::move(idx));
    };

    auto hashTableRefType = std::make_shared<TReferenceType>(AsNamed("HashTable", hashTableType));
    auto pairBufferRefType =
        std::make_shared<TReferenceType>(AsNamed("PairBuffer", pairBufferType));

    // Key pointer types for GroupKeys (own) and Keys (opp)
    auto ptrKeyType = std::make_shared<TPointerType>(key.KeyType);

    std::vector<TParam> params = {
        std::make_shared<TVarStmt>(loc, "own", hashTableRefType),
        std::make_shared<TVarStmt>(loc, "opp", hashTableRefType),
        std::make_shared<TVarStmt>(loc, "pairs", pairBufferRefType),
    };

    std::vector<TExprPtr> body;
    // own_gkeys = cast(own.GroupKeys) <ptr Key>
    body.push_back(var("own_gkeys", ptrKeyType));
    body.push_back(assign("own_gkeys", cast(field("own", "GroupKeys"), ptrKeyType)));
    // own_aggs = own.AggBuffers
    auto ptrPtrI64Type = std::make_shared<TPointerType>(ptrI64Type);
    body.push_back(var("own_aggs", ptrPtrI64Type));
    body.push_back(assign("own_aggs", field("own", "AggBuffers")));
    // own_counts = own_aggs[0], own_datas = own_aggs[2]
    body.push_back(var("own_counts", ptrI64Type));
    body.push_back(assign("own_counts", index(ident("own_aggs"), numI64(0))));
    body.push_back(var("own_datas", ptrI64Type));
    body.push_back(assign("own_datas", index(ident("own_aggs"), numI64(2))));
    // opp_keys = cast(opp.Keys) <ptr Key>
    body.push_back(var("opp_keys", ptrKeyType));
    body.push_back(assign("opp_keys", cast(field("opp", "Keys"), ptrKeyType)));
    // slot = 0
    body.push_back(var("slot", i64Type));
    body.push_back(assign("slot", numI64(0)));

    // Inner bucket drain loop body (built once, reused by reference in while)
    // while k < bcount: if !pb_push(pairs, bdata[k], -1): return #f; k++
    std::vector<TExprPtr> drainBody;
    {
        auto pushCall = call(
            "pb_push",
            {
                ident("pairs"),
                index(ident("bdata"), ident("k")),
                numI64(-1),
            });
        drainBody.push_back(
            std::make_shared<TIfExpr>(
                loc,
                std::make_shared<TUnaryExpr>(loc, TOperator("!"), std::move(pushCall)),
                block({std::make_shared<TReturnExpr>(loc, number(0, boolType))}),
                nullptr));
        drainBody.push_back(assign("k", binary("+", ident("k"), numI64(1))));
    }

    // Outer slot loop body
    std::vector<TExprPtr> slotBody;
    {
        // key = own_gkeys[slot]
        slotBody.push_back(var("key", key.KeyType));
        slotBody.push_back(assign("key", index(ident("own_gkeys"), ident("slot"))));
        // opp_slot = rh_lookup_slot(opp_keys, opp.Ctrl, opp.SlotId, opp.Capacity,
        // key)
        slotBody.push_back(var("opp_slot", i64Type));
        slotBody.push_back(assign(
            "opp_slot",
            call(
                "rh_lookup_slot",
                {
                    ident("opp_keys"),
                    field("opp", "Ctrl"),
                    field("opp", "SlotId"),
                    field("opp", "Capacity"),
                    ident("key"),
                })));
        // A NULL component in ordinary equality never matches another NULL.
        slotBody.push_back(var("found", boolType));
        slotBody.push_back(assign(
            "found",
            binary(
                "&&",
                call("jt_key_valid", {ident("key")}),
                binary("!=", ident("opp_slot"), numI64(-1)))));
        // emit_condition: SEMI -> found; ANTI -> !found
        TExprPtr emitCond = isAnti
            ? std::static_pointer_cast<TExpr>(
                std::make_shared<TUnaryExpr>(loc, TOperator("!"), ident("found")))
            : ident("found");
        // if emit_condition: drain bucket
        std::vector<TExprPtr> emitBody;
        {
            emitBody.push_back(var("bcount", i64Type));
            emitBody.push_back(assign("bcount", index(ident("own_counts"), ident("slot"))));
            emitBody.push_back(var("bdata", ptrI64Type));
            emitBody.push_back(
                assign("bdata", cast(index(ident("own_datas"), ident("slot")), ptrI64Type)));
            emitBody.push_back(var("k", i64Type));
            emitBody.push_back(assign("k", numI64(0)));
            emitBody.push_back(
                std::make_shared<TWhileStmtExpr>(
                    loc,
                    binary("<", ident("k"), ident("bcount")),
                    block(std::vector<TExprPtr>(drainBody))));
        }
        slotBody.push_back(
            std::make_shared<TIfExpr>(
                loc,
                std::move(emitCond),
                block(std::move(emitBody)),
                nullptr));
        slotBody.push_back(assign("slot", binary("+", ident("slot"), numI64(1))));
    }

    body.push_back(
        std::make_shared<TWhileStmtExpr>(
            loc,
            binary("<", ident("slot"), field("own", "Size")),
            block(std::move(slotBody))));
    body.push_back(std::make_shared<TReturnExpr>(loc, number(1, boolType)));

    return std::make_shared<TFunDecl>(
        loc,
        funcName,
        std::vector<TGenericParam>{},
        std::move(params),
        std::make_shared<TBlockExpr>(loc, std::move(body)),
        boolType);
}

namespace {

enum class EJoinBatchMode {
    ProbeInsert,
    ProbeOnly,
    InsertOnly,
    ProbeMark,
    SemiProbe,
    MarkJoinProbe,
};

NQumir::NAst::TExprPtr GenJoinBatchAst(
    const TJoinKeyDescriptor& key,
    bool isLeft,
    const std::string& funcName,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr hashTableType,
    NQumir::NAst::TTypePtr pairBufferType,
    NQumir::NAst::TTypePtr stringViewType,
    EJoinBatchMode mode,
    bool hasPrecomputedHash,
    bool isAnti = false,
    bool markMatched = false,
    bool keyDecides = false)
{
    NQumir::TLocation loc{};

    auto i64Type = std::make_shared<TIntegerType>();
    auto u8Type = std::make_shared<TIntegerType>(TIntegerType::U8);
    auto u64Type = std::make_shared<TIntegerType>(TIntegerType::U64);
    auto boolType = std::make_shared<TBoolType>();
    auto ptrU8Type = std::make_shared<TPointerType>(u8Type);
    auto ptrU64Type = std::make_shared<TPointerType>(u64Type);
    auto ptrI64Type = std::make_shared<TPointerType>(i64Type);
    auto ptrI8Type = std::make_shared<TPointerType>(std::make_shared<TIntegerType>(TIntegerType::I8));

    auto ident = [&](const std::string& name) -> TExprPtr {
        return std::make_shared<TIdentExpr>(loc, name);
    };
    auto number = [&](int64_t value, TTypePtr type) -> TExprPtr {
        auto result = std::make_shared<TNumberExpr>(loc, value);
        result->Type = std::move(type);
        return result;
    };
    auto numI64 = [&](int64_t value) {
        return number(value, i64Type);
    };
    auto binary = [&](const char* op, TExprPtr left, TExprPtr right) -> TExprPtr {
        return std::make_shared<TBinaryExpr>(loc, TOperator(op), std::move(left), std::move(right));
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

    auto hashTableRefType = std::make_shared<TReferenceType>(AsNamed("HashTable", hashTableType));
    auto rowSetRefType = std::make_shared<TReferenceType>(AsNamed("TRowSet", rowSetType));
    auto rowSetPtrType = std::make_shared<TPointerType>(AsNamed("TRowSet", rowSetType));
    auto pairBufferRefType =
        std::make_shared<TReferenceType>(AsNamed("PairBuffer", pairBufferType));
    std::vector<TParam> params;
    if (mode == EJoinBatchMode::ProbeInsert || mode == EJoinBatchMode::InsertOnly) {
        params.push_back(std::make_shared<TVarStmt>(loc, "own", hashTableRefType));
        params.push_back(std::make_shared<TVarStmt>(loc, "opp", hashTableRefType));
    } else {
        params.push_back(std::make_shared<TVarStmt>(loc, "build", hashTableRefType));
    }
    params.push_back(std::make_shared<TVarStmt>(loc, "batch", rowSetRefType));
    params.push_back(std::make_shared<TVarStmt>(loc, "key_columns", ptrI64Type));
    params.push_back(std::make_shared<TVarStmt>(loc, "batch_idx", i64Type));
    params.push_back(std::make_shared<TVarStmt>(loc, "pairs", pairBufferRefType));
    params.push_back(std::make_shared<TVarStmt>(loc, "left_store", rowSetPtrType));
    params.push_back(std::make_shared<TVarStmt>(loc, "right_store", rowSetPtrType));
    params.push_back(std::make_shared<TVarStmt>(loc, "arena", ptrI8Type));

    auto ptrColumnType = ColumnPointerType(columnType, rowSetType);
    auto columnValueType = PointerPointeeOr(ptrColumnType, columnType);
    auto columnAt = [&](size_t fieldIndex) -> TExprPtr {
        return std::make_shared<TIndexExpr>(
            loc,
            ident("cols"),
            std::make_shared<TIndexExpr>(
                loc,
                ident("key_columns"),
                numI64(static_cast<int64_t>(fieldIndex))));
    };

    std::vector<TExprPtr> body;
    body.push_back(var("n", i64Type));
    body.push_back(assign("n", field("batch", "RowCount")));
    body.push_back(var("selection", ptrU8Type));
    body.push_back(assign("selection", field("batch", "Selection")));
    body.push_back(var("cols", ptrColumnType));
    body.push_back(assign("cols", field("batch", "Columns")));
    for (size_t fieldIndex = 0; fieldIndex < key.Fields.size(); ++fieldIndex) {
        const std::string name = "key_column_" + std::to_string(fieldIndex);
        body.push_back(var(name, columnValueType));
        body.push_back(assign(name, columnAt(fieldIndex)));
    }
    body.push_back(var("selection_is_null", boolType));
    body.push_back(
        assign("selection_is_null", binary("==", cast(ident("selection"), i64Type), numI64(0))));
    // The stored_witness value only binds the StoredKey template type at the
    // generic dual-key call sites; a zero value works for every mode.
    body.push_back(var("stored_witness", key.StoredType));
    body.push_back(assign("stored_witness", ZeroValueExpr(key.StoredType)));
    if (mode == EJoinBatchMode::MarkJoinProbe) {
        // The marker of every unmatched row depends on whether the complete right
        // set contains NULL, so that is looked up once per batch. The zero
        // StoredKey is the NULL key (mj_set_has_null).
        body.push_back(var("build_has_null", boolType));
        body.push_back(assign(
            "build_has_null",
            call("mj_set_has_null", {ident("build"), ZeroValueExpr(key.StoredType)})));
    }
    if (hasPrecomputedHash) {
        body.push_back(var("hash_ptr", ptrU64Type));
        body.push_back(assign("hash_ptr", field("batch", "Hash")));
    }
    body.push_back(var("i", i64Type));
    body.push_back(assign("i", numI64(0)));

    auto keyRead = GenKeyReadAst(key, "i", "key_value_", stringViewType);

    // own_row_id = (batch_idx << 32) | (i & 0xffffffff)
    auto ownRowId = binary(
        "+",
        binary("<<", ident("batch_idx"), numI64(32)),
        binary("&", ident("i"), numI64(0xffffffff)));

    std::vector<TExprPtr> keyValueSetup = {
        var("key_value", key.LookupType),
        assign("key_value", std::move(keyRead.Value)),
        var("hash_value", u64Type),
    };
    if (hasPrecomputedHash) {
        // hash_ptr is only guaranteed non-null when the shuffle upstream produced
        // a single-batch (view) output; the rarer multi-batch gather path leaves
        // it null, so fall back to rh_hash per row.
        keyValueSetup.push_back(
            std::make_shared<TIfExpr>(
                loc,
                binary("!=", cast(ident("hash_ptr"), i64Type), numI64(0)),
                block({
                    assign(
                        "hash_value",
                        std::make_shared<TIndexExpr>(loc, ident("hash_ptr"), ident("i"))),
                }),
                block(
                    {assign("hash_value", cast(call("rh_hash", {ident("key_value")}), u64Type))})));
    } else {
        keyValueSetup.push_back(
            assign("hash_value", cast(call("rh_hash", {ident("key_value")}), u64Type)));
    }

    TExprPtr emitCall;
    if (mode == EJoinBatchMode::ProbeInsert) {
        emitCall = call(
            "jt_emit_and_insert",
            {
                ident("own"),
                ident("opp"),
                ident("key_value"),
                ident("stored_witness"),
                std::move(ownRowId),
                numI64(isLeft ? 1 : 0),
                numI64(markMatched ? 1 : 0),
                ident("pairs"),
                ident("left_store"),
                ident("right_store"),
                ident("arena"),
                ident("hash_value"),
            });
    } else if (mode == EJoinBatchMode::ProbeOnly) {
        emitCall = call(
            "jt_probe_and_emit",
            {
                ident("build"),
                ident("key_value"),
                ident("stored_witness"),
                std::move(ownRowId),
                numI64(isLeft ? 1 : 0),
                numI64(markMatched ? 1 : 0),
                ident("pairs"),
                ident("left_store"),
                ident("right_store"),
                ident("arena"),
                ident("batch"),
                ident("batch"),
                ident("hash_value"),
            });
    } else if (mode == EJoinBatchMode::InsertOnly) {
        emitCall = call(
            "jt_insert_row_only",
            {
                ident("own"),
                ident("key_value"),
                ident("stored_witness"),
                std::move(ownRowId),
                ident("hash_value"),
            });
    } else if (mode == EJoinBatchMode::SemiProbe) {
        emitCall = call(
            "jt_probe_semi",
            {
                ident("build"),
                ident("key_value"),
                ident("stored_witness"),
                std::move(ownRowId),
                numI64(isAnti ? 1 : 0),
                ident("pairs"),
                ident("hash_value"),
            });
    } else if (mode == EJoinBatchMode::MarkJoinProbe) {
        emitCall = call(
            "pb_push",
            {
                ident("pairs"),
                std::move(ownRowId),
                call(
                    "mj_marker",
                    {
                        ident("build"),
                        ident("key_value"),
                        ident("stored_witness"),
                        ident("build_has_null"),
                        ident("hash_value"),
                    }),
            });
    } else {
        emitCall = call(
            "jt_probe_and_mark",
            {
                ident("build"),
                ident("key_value"),
                ident("stored_witness"),
                std::move(ownRowId),
                numI64(keyDecides ? 1 : 0),
                ident("left_store"),
                ident("right_store"),
                ident("arena"),
                ident("batch"),
                ident("hash_value"),
            });
    }

    std::vector<TExprPtr> process;
    process.insert(
        process.end(),
        std::make_move_iterator(keyRead.Setup.begin()),
        std::make_move_iterator(keyRead.Setup.end()));
    process.insert(
        process.end(),
        std::make_move_iterator(keyValueSetup.begin()),
        std::make_move_iterator(keyValueSetup.end()));
    // if (!jt_emit_and_insert(...)) return false;
    process.push_back(
        std::make_shared<TIfExpr>(
            loc,
            std::make_shared<TUnaryExpr>(loc, TOperator("!"), std::move(emitCall)),
            block({std::make_shared<TReturnExpr>(loc, number(0, boolType))}),
            nullptr));

    auto selected = binary(
        "||",
        ident("selection_is_null"),
        binary(
            "!=",
            std::make_shared<TIndexExpr>(loc, ident("selection"), ident("i")),
            number(0, u8Type)));
    auto loop = block({
        std::make_shared<TIfExpr>(loc, std::move(selected), block(std::move(process)), nullptr),
        assign("i", binary("+", ident("i"), numI64(1))),
    });
    body.push_back(
        std::make_shared<TWhileStmtExpr>(
            loc,
            binary("<", ident("i"), ident("n")),
            std::move(loop)));
    body.push_back(std::make_shared<TReturnExpr>(loc, number(1, boolType)));

    auto function = std::make_shared<TFunDecl>(
        loc,
        funcName,
        std::vector<TGenericParam>{},
        std::move(params),
        std::make_shared<TBlockExpr>(loc, std::move(body)),
        boolType);
    return function;
}

} // namespace

NQumir::NAst::TExprPtr GenJoinProcessAst(
    const TJoinKeyDescriptor& key,
    bool isLeft,
    const std::string& funcName,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr hashTableType,
    NQumir::NAst::TTypePtr pairBufferType,
    NQumir::NAst::TTypePtr stringViewType,
    bool hasPrecomputedHash,
    bool markMatched)
{
    return GenJoinBatchAst(
        key,
        isLeft,
        funcName,
        std::move(columnType),
        std::move(rowSetType),
        std::move(hashTableType),
        std::move(pairBufferType),
        std::move(stringViewType),
        EJoinBatchMode::ProbeInsert,
        hasPrecomputedHash,
        /*isAnti=*/false,
        markMatched);
}

NQumir::NAst::TExprPtr GenJoinInsertRowsOnlyAst(
    const TJoinKeyDescriptor& key,
    bool isLeft,
    const std::string& funcName,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr hashTableType,
    NQumir::NAst::TTypePtr pairBufferType,
    NQumir::NAst::TTypePtr stringViewType,
    bool hasPrecomputedHash)
{
    return GenJoinBatchAst(
        key,
        isLeft,
        funcName,
        std::move(columnType),
        std::move(rowSetType),
        std::move(hashTableType),
        std::move(pairBufferType),
        std::move(stringViewType),
        EJoinBatchMode::InsertOnly,
        hasPrecomputedHash);
}

NQumir::NAst::TExprPtr GenJoinProbeAst(
    const TJoinKeyDescriptor& key,
    bool isLeft,
    const std::string& funcName,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr hashTableType,
    NQumir::NAst::TTypePtr pairBufferType,
    NQumir::NAst::TTypePtr stringViewType,
    bool hasPrecomputedHash,
    bool markMatched)
{
    return GenJoinBatchAst(
        key,
        isLeft,
        funcName,
        std::move(columnType),
        std::move(rowSetType),
        std::move(hashTableType),
        std::move(pairBufferType),
        std::move(stringViewType),
        EJoinBatchMode::ProbeOnly,
        hasPrecomputedHash,
        /*isAnti=*/false,
        markMatched);
}

NQumir::NAst::TExprPtr GenJoinProbeSemiAst(
    const TJoinKeyDescriptor& key,
    bool isAnti,
    const std::string& funcName,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr hashTableType,
    NQumir::NAst::TTypePtr pairBufferType,
    NQumir::NAst::TTypePtr stringViewType,
    bool hasPrecomputedHash)
{
    return GenJoinBatchAst(
        key,
        /*isLeft=*/true,
        funcName,
        std::move(columnType),
        std::move(rowSetType),
        std::move(hashTableType),
        std::move(pairBufferType),
        std::move(stringViewType),
        EJoinBatchMode::SemiProbe,
        hasPrecomputedHash,
        isAnti);
}

NQumir::NAst::TExprPtr GenMarkJoinProbeAst(
    const TJoinKeyDescriptor& key,
    const std::string& funcName,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr hashTableType,
    NQumir::NAst::TTypePtr pairBufferType,
    NQumir::NAst::TTypePtr stringViewType,
    bool hasPrecomputedHash)
{
    return GenJoinBatchAst(
        key,
        /*isLeft=*/true,
        funcName,
        std::move(columnType),
        std::move(rowSetType),
        std::move(hashTableType),
        std::move(pairBufferType),
        std::move(stringViewType),
        EJoinBatchMode::MarkJoinProbe,
        hasPrecomputedHash);
}

NQumir::NAst::TExprPtr GenJoinProbeMarkAst(
    const TJoinKeyDescriptor& key,
    bool keyDecides,
    const std::string& funcName,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr hashTableType,
    NQumir::NAst::TTypePtr pairBufferType,
    NQumir::NAst::TTypePtr stringViewType,
    bool hasPrecomputedHash)
{
    return GenJoinBatchAst(
        key,
        /*isLeft=*/false,
        funcName,
        std::move(columnType),
        std::move(rowSetType),
        std::move(hashTableType),
        std::move(pairBufferType),
        std::move(stringViewType),
        EJoinBatchMode::ProbeMark,
        hasPrecomputedHash,
        /*isAnti=*/false,
        /*markMatched=*/false,
        keyDecides);
}

NQumir::NAst::TExprPtr GenJoinDispatchAst(
    const TJoinKeyDescriptor& key,
    EJoinType type,
    bool hasResidual,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr hashTableType,
    NQumir::NAst::TTypePtr pairBufferType)
{
    namespace Oz = NKernel::NOz;

    auto i64Type = std::make_shared<TIntegerType>(TIntegerType::I64);
    auto boolType = std::make_shared<TBoolType>();
    auto ptrI64Type = std::make_shared<TPointerType>(i64Type);
    auto ptrI8Type = std::make_shared<TPointerType>(std::make_shared<TIntegerType>(TIntegerType::I8));
    auto hashTableRefType = std::make_shared<TReferenceType>(AsNamed("HashTable", hashTableType));
    auto rowSetRefType = std::make_shared<TReferenceType>(AsNamed("TRowSet", rowSetType));
    auto rowSetPtrType = std::make_shared<TPointerType>(AsNamed("TRowSet", rowSetType));
    auto pairBufferRefType =
        std::make_shared<TReferenceType>(AsNamed("PairBuffer", pairBufferType));

    auto number = [&](int64_t value) -> TExprPtr {
        return Oz::TypedInt(value, i64Type);
    };
    auto opIs = [&](int64_t op) -> TExprPtr {
        return Oz::Bin(TOperator("=="), Oz::Ident("op"), number(op));
    };
    auto boolReturn = [&](bool value) -> TExprPtr {
        return Oz::Return(Oz::Bool(value));
    };
    const int64_t keySize = static_cast<int64_t>(key.Size);
    const bool isSemiAnti = type == EJoinType::LeftSemi || type == EJoinType::LeftAnti;
    const bool isMark = type == EJoinType::LeftMark;

    // The left rows of a SEMI/ANTI or mark join are stored before any right
    // row is, so storing them probes nothing.
    auto processLeft = [&]() {
        if (isSemiAnti || isMark) {
            return Oz::Call(
                "jt_insert_left_only",
                {
                    Oz::Ident("left"),
                    Oz::Ident("right"),
                    Oz::Ident("batch"),
                    Oz::Ident("left_key_columns"),
                    Oz::Ident("batch_idx"),
                    Oz::Ident("pairs"),
                    Oz::Ident("left_store"),
                    Oz::Ident("right_store"),
                    Oz::Ident("arena"),
                });
        }
        return Oz::Call(
            "jt_process_left",
            {
                Oz::Ident("left"),
                Oz::Ident("right"),
                Oz::Ident("batch"),
                Oz::Ident("left_key_columns"),
                Oz::Ident("batch_idx"),
                Oz::Ident("pairs"),
                Oz::Ident("left_store"),
                Oz::Ident("right_store"),
                Oz::Ident("arena"),
            });
    };
    auto processRight = [&]() {
        return Oz::Call(
            "jt_process_right",
            {
                Oz::Ident("right"),
                Oz::Ident("left"),
                Oz::Ident("batch"),
                Oz::Ident("right_key_columns"),
                Oz::Ident("batch_idx"),
                Oz::Ident("pairs"),
                Oz::Ident("left_store"),
                Oz::Ident("right_store"),
                Oz::Ident("arena"),
            });
    };
    auto streamLeft = [&]() {
        if (isMark) {
            return Oz::Call(
                "mj_probe_left",
                {
                    Oz::Ident("right"),
                    Oz::Ident("batch"),
                    Oz::Ident("left_key_columns"),
                    Oz::Ident("batch_idx"),
                    Oz::Ident("pairs"),
                    Oz::Ident("left_store"),
                    Oz::Ident("right_store"),
                    Oz::Ident("arena"),
                });
        }
        // A residual needs right rows, which the key-only right table lacks.
        if (isSemiAnti && hasResidual) {
            return Oz::Bool(false);
        }
        if (isSemiAnti) {
            return Oz::Call(
                "jt_probe_left_semi",
                {
                    Oz::Ident("right"),
                    Oz::Ident("batch"),
                    Oz::Ident("left_key_columns"),
                    Oz::Ident("batch_idx"),
                    Oz::Ident("pairs"),
                    Oz::Ident("left_store"),
                    Oz::Ident("right_store"),
                    Oz::Ident("arena"),
                });
        }
        return Oz::Call(
            "jt_probe_left_stream",
            {
                Oz::Ident("right"),
                Oz::Ident("batch"),
                Oz::Ident("left_key_columns"),
                Oz::Ident("batch_idx"),
                Oz::Ident("pairs"),
                Oz::Ident("left_store"),
                Oz::Ident("right_store"),
                Oz::Ident("arena"),
            });
    };
    auto streamRight = [&]() {
        if (isMark) {
            return Oz::Bool(false);
        }
        // A SEMI/ANTI join that built its left side marks the left rows the
        // right stream matches.
        if (isSemiAnti) {
            return Oz::Call(
                "jt_probe_right_mark",
                {
                    Oz::Ident("left"),
                    Oz::Ident("batch"),
                    Oz::Ident("right_key_columns"),
                    Oz::Ident("batch_idx"),
                    Oz::Ident("pairs"),
                    Oz::Ident("left_store"),
                    Oz::Ident("right_store"),
                    Oz::Ident("arena"),
                });
        }
        return Oz::Call(
            "jt_probe_right_stream",
            {
                Oz::Ident("left"),
                Oz::Ident("batch"),
                Oz::Ident("right_key_columns"),
                Oz::Ident("batch_idx"),
                Oz::Ident("pairs"),
                Oz::Ident("left_store"),
                Oz::Ident("right_store"),
                Oz::Ident("arena"),
            });
    };
    auto insertRightKeyOnly = [&]() {
        return Oz::Call(
            "jt_insert_key_only",
            {
                Oz::Ident("right"),
                Oz::Ident("left"),
                Oz::Ident("batch"),
                Oz::Ident("right_key_columns"),
                Oz::Ident("batch_idx"),
                Oz::Ident("pairs"),
            });
    };

    // The symmetric SEMI/ANTI path stores the distinct right keys; a residual
    // needs the right rows, so it streams them against the left side instead.
    auto updateRight = [&]() -> TExprPtr {
        if (isSemiAnti && hasResidual) {
            return Oz::Bool(false);
        }
        if (isSemiAnti || isMark) {
            return insertRightKeyOnly();
        }
        return processRight();
    };

    // ok = jt_finalize_outer(own, opp, pairs); if !ok return #f;
    // Emits `own`'s rows that had no `opp` match as pairs (own_rowid, -1).
    auto callFinalizeOuter = [&](const char* own, const char* opp) -> std::vector<TExprPtr> {
        return {
            Oz::Assign(
                "ok",
                Oz::Call(
                    "jt_finalize_outer",
                    {
                        Oz::Ident(own),
                        Oz::Ident(opp),
                        Oz::Ident("pairs"),
                    })),
            Oz::If(Oz::Unary(TOperator("!"), Oz::Ident("ok")), Oz::Block({boolReturn(false)})),
        };
    };

    // Swap the two row-ids of every pair in [start, pairs.Count), turning
    // right-unmatched pairs (right_rowid, -1) into canonical (-1, right_rowid)
    // = (left=null, right) order the materializer expects.
    auto swapPairsFrom = [&](TExprPtr start) -> std::vector<TExprPtr> {
        return {
            Oz::Assign("data", Oz::Field("pairs", "Data")),
            Oz::Assign("i", std::move(start)),
            Oz::While(
                Oz::Bin(TOperator("<"), Oz::Ident("i"), Oz::Field("pairs", "Count")),
                Oz::Block({
                    Oz::Assign("tmp", Oz::Index("data", Oz::Mul(Oz::Ident("i"), number(2)))),
                    Oz::ArrayAssign(
                        "data",
                        Oz::Mul(Oz::Ident("i"), number(2)),
                        Oz::Index("data", Oz::Add(Oz::Mul(Oz::Ident("i"), number(2)), number(1)))),
                    Oz::ArrayAssign(
                        "data",
                        Oz::Add(Oz::Mul(Oz::Ident("i"), number(2)), number(1)),
                        Oz::Ident("tmp")),
                    Oz::Assign("i", Oz::Add(Oz::Ident("i"), number(1))),
                })),
        };
    };

    // Local scratch declarations shared by the outer-finalize blocks below.
    auto outerFinalizeVars = [&]() -> std::vector<TExprPtr> {
        return {
            Oz::Var("ok", boolType),
            Oz::Var("data", ptrI64Type),
            Oz::Var("i", i64Type),
            Oz::Var("tmp", i64Type),
        };
    };

    auto rightOuterFinalize = [&]() -> TExprPtr {
        std::vector<TExprPtr> stmts = outerFinalizeVars();
        auto fin = callFinalizeOuter("right", "left");
        stmts.insert(stmts.end(), fin.begin(), fin.end());
        auto swap = swapPairsFrom(number(0));
        stmts.insert(stmts.end(), swap.begin(), swap.end());
        stmts.push_back(boolReturn(true));
        return Oz::Block(std::move(stmts));
    };

    // FULL OUTER: emit left-unmatched (already in (left, -1) order) then
    // right-unmatched (appended as (right, -1)); swap only the appended tail
    // [n, Count) so it reads (-1, right).
    auto fullOuterFinalize = [&]() -> TExprPtr {
        std::vector<TExprPtr> stmts = outerFinalizeVars();
        stmts.push_back(Oz::Var("n", i64Type));
        auto finLeft = callFinalizeOuter("left", "right");
        stmts.insert(stmts.end(), finLeft.begin(), finLeft.end());
        stmts.push_back(Oz::Assign("n", Oz::Field("pairs", "Count")));
        auto finRight = callFinalizeOuter("right", "left");
        stmts.insert(stmts.end(), finRight.begin(), finRight.end());
        auto swap = swapPairsFrom(Oz::Ident("n"));
        stmts.insert(stmts.end(), swap.begin(), swap.end());
        stmts.push_back(boolReturn(true));
        return Oz::Block(std::move(stmts));
    };

    auto finalize = [&]() -> TExprPtr {
        // Key existence cannot identify unmatched rows after a residual rejected
        // all candidate pairs. LEFT/RIGHT and SEMI/ANTI joins must use marked
        // streaming finalization; FULL marks the matched rows of both tables.
        if (hasResidual
            && (type == EJoinType::Left || type == EJoinType::Right || isSemiAnti))
        {
            return boolReturn(false);
        }
        if (hasResidual && type == EJoinType::Full) {
            return Oz::Return(
                Oz::Bin(
                    TOperator("&&"),
                    Oz::Call(
                        "jt_emit_by_mark",
                        {Oz::Ident("left"), number(1), Oz::Bool(false), Oz::Ident("pairs")}),
                    Oz::Call(
                        "jt_emit_by_mark",
                        {Oz::Ident("right"), number(0), Oz::Bool(false), Oz::Ident("pairs")})));
        }
        if (isSemiAnti) {
            return Oz::Return(
                Oz::Call(
                    "jt_finalize_semi_anti",
                    {
                        Oz::Ident("left"),
                        Oz::Ident("right"),
                        Oz::Ident("pairs"),
                    }));
        }
        if (type == EJoinType::Left) {
            return Oz::Return(
                Oz::Call(
                    "jt_finalize_outer",
                    {
                        Oz::Ident("left"),
                        Oz::Ident("right"),
                        Oz::Ident("pairs"),
                    }));
        }
        if (type == EJoinType::Right) {
            return rightOuterFinalize();
        }
        if (type == EJoinType::Full) {
            return fullOuterFinalize();
        }
        if (isMark) {
            // The zero StoredKey is the NULL key (mj_set_has_null).
            return Oz::Return(
                Oz::Call(
                    "mj_finalize",
                    {
                        Oz::Ident("left"),
                        Oz::Ident("right"),
                        ZeroValueExpr(key.StoredType),
                        Oz::Ident("pairs"),
                    }));
        }
        return boolReturn(true);
    };

    // A streaming LEFT/RIGHT join builds its preserved side, a streaming
    // SEMI/ANTI join its left side; the stream probe marks the build rows it
    // matches. This emits the unmarked rows, or the marked ones of a SEMI join.
    auto finalizeStream = [&]() -> TExprPtr {
        if (type != EJoinType::Left && type != EJoinType::Right && !isSemiAnti) {
            return boolReturn(false);
        }
        const bool buildIsLeft = type != EJoinType::Right;
        return Oz::Return(
            Oz::Call(
                "jt_emit_by_mark",
                {
                    Oz::Ident(buildIsLeft ? "left" : "right"),
                    number(buildIsLeft ? 1 : 0),
                    Oz::Bool(type == EJoinType::LeftSemi),
                    Oz::Ident("pairs"),
                }));
    };

    Oz::TFunBuilder builder("jt_dispatch");
    TExprPtr initBody = Oz::Return(
        Oz::Bin(
            TOperator("&&"),
            Oz::Call("jt_init", {Oz::Ident("left"), Oz::Ident("arg"), number(keySize)}),
            Oz::Call("jt_init", {Oz::Ident("right"), Oz::Ident("arg"), number(keySize)})));
    std::vector<TExprPtr> destroyBody;
    destroyBody.push_back(Oz::Call("jt_destroy", {Oz::Ident("left")}));
    destroyBody.push_back(Oz::Call("jt_destroy", {Oz::Ident("right")}));
    destroyBody.push_back(Oz::Call("pb_destroy", {Oz::Ident("pairs")}));
    destroyBody.push_back(boolReturn(true));
    builder.Param("left", hashTableRefType)
        .Param("right", hashTableRefType)
        .Param("batch", rowSetRefType)
        .Param("batch_idx", i64Type)
        .Param("pairs", pairBufferRefType)
        .Param("left_store", rowSetPtrType)
        .Param("right_store", rowSetPtrType)
        .Param("arena", ptrI8Type)
        .Param("left_key_columns", ptrI64Type)
        .Param("right_key_columns", ptrI64Type)
        .Param("arg", i64Type)
        .Param("op", i64Type)
        .Return(boolType)
        .Stmt(Oz::If(opIs(0), Oz::Block({std::move(initBody)})))
        .Stmt(Oz::If(opIs(1), Oz::Block({Oz::Return(processLeft())})))
        .Stmt(Oz::If(opIs(2), Oz::Block({Oz::Return(updateRight())})))
        .Stmt(Oz::If(opIs(3), Oz::Block({Oz::Return(streamLeft())})))
        .Stmt(Oz::If(opIs(4), Oz::Block({Oz::Return(streamRight())})))
        .Stmt(Oz::If(opIs(5), finalize()))
        .Stmt(Oz::If(opIs(6), Oz::Block(std::move(destroyBody))))
        .Stmt(Oz::If(opIs(7), finalizeStream()))
        .Stmt(boolReturn(false));

    return std::move(builder).Build();
}

NQumir::NAst::TExprPtr GenCrossJoinDispatchAst(
    EJoinType type,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr pairBufferType)
{
    namespace Oz = NKernel::NOz;

    auto i64Type = std::make_shared<TIntegerType>(TIntegerType::I64);
    auto boolType = std::make_shared<TBoolType>();
    auto u8PtrType =
        std::make_shared<TPointerType>(std::make_shared<TIntegerType>(TIntegerType::U8));
    auto ptrI8Type = std::make_shared<TPointerType>(std::make_shared<TIntegerType>(TIntegerType::I8));
    auto rowSetPtrType = std::make_shared<TPointerType>(AsNamed("TRowSet", rowSetType));
    auto pairBufferRefType =
        std::make_shared<TReferenceType>(AsNamed("PairBuffer", pairBufferType));

    const bool keepsUnmatchedLeft =
        type == EJoinType::Left || type == EJoinType::Full || type == EJoinType::LeftAnti;
    const bool keepsUnmatchedRight = type == EJoinType::Right || type == EJoinType::Full;
    const bool emitsPairs =
        type == EJoinType::Inner || type == EJoinType::Left || keepsUnmatchedRight;
    if (!emitsPairs && type != EJoinType::LeftSemi && type != EJoinType::LeftAnti) {
        throw NQumir::TError(
            "cross join: unsupported join type " + std::string(JoinTypeName(type)));
    }

    auto opIs = [&](int64_t op) -> TExprPtr {
        return Oz::Bin(TOperator("=="), Oz::Ident("op"), Oz::TypedInt(op, i64Type));
    };
    TExprPtr emitUnmatchedRight = keepsUnmatchedRight
        ? Oz::Call(
            "xj_emit_unmatched_right",
            {
                Oz::Ident("right_store"),
                Oz::Ident("right_batch_count"),
                Oz::Ident("right_matched"),
                Oz::Ident("pairs"),
            })
        : Oz::Bool(true);

    Oz::TFunBuilder builder("xj_dispatch");
    builder.Param("left_store", rowSetPtrType)
        .Param("batch_idx", i64Type)
        .Param("right_store", rowSetPtrType)
        .Param("right_batch_count", i64Type)
        .Param("right_matched", u8PtrType)
        .Param("pairs", pairBufferRefType)
        .Param("arena", ptrI8Type)
        .Param("op", i64Type)
        .Return(boolType)
        .Stmt(
            Oz::If(
                opIs(0),
                Oz::Block({
                    Oz::Return(
                        Oz::Call(
                            "xj_emit",
                            {
                                Oz::Ident("left_store"),
                                Oz::Ident("batch_idx"),
                                Oz::Ident("right_store"),
                                Oz::Ident("right_batch_count"),
                                Oz::Ident("right_matched"),
                                Oz::Ident("pairs"),
                                Oz::Ident("arena"),
                                Oz::Bool(emitsPairs),
                                Oz::Bool(type == EJoinType::LeftSemi),
                                Oz::Bool(keepsUnmatchedLeft),
                                Oz::Bool(keepsUnmatchedRight),
                            })),
                })))
        .Stmt(
            Oz::If(
                opIs(1),
                Oz::Block({
                    Oz::Call("pb_destroy", {Oz::Ident("pairs")}),
                    Oz::Return(Oz::Bool(true)),
                })))
        .Stmt(Oz::If(opIs(2), Oz::Block({Oz::Return(std::move(emitUnmatchedRight))})))
        .Stmt(Oz::Return(Oz::Bool(false)));

    return std::move(builder).Build();
}

NQumir::NAst::TExprPtr GenKeyHashBatchAst(
    const TAggregateKeyDescriptor& key,
    const std::string& funcName,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr stringViewType)
{
    NQumir::TLocation loc{};

    auto i64Type = std::make_shared<TIntegerType>();
    auto u8Type = std::make_shared<TIntegerType>(TIntegerType::U8);
    auto u64Type = std::make_shared<TIntegerType>(TIntegerType::U64);
    auto boolType = std::make_shared<TBoolType>();
    auto ptrU8Type = std::make_shared<TPointerType>(u8Type);
    auto ptrU64Type = std::make_shared<TPointerType>(u64Type);
    auto ptrI64Type = std::make_shared<TPointerType>(i64Type);
    auto ptrLookupKeyType = std::make_shared<TPointerType>(key.LookupType);

    auto ident = [&](const std::string& name) -> TExprPtr {
        return std::make_shared<TIdentExpr>(loc, name);
    };
    auto number = [&](int64_t value, TTypePtr type) -> TExprPtr {
        auto result = std::make_shared<TNumberExpr>(loc, value);
        result->Type = std::move(type);
        return result;
    };
    auto numI64 = [&](int64_t value) {
        return number(value, i64Type);
    };
    auto binary = [&](const char* op, TExprPtr left, TExprPtr right) -> TExprPtr {
        return std::make_shared<TBinaryExpr>(loc, TOperator(op), std::move(left), std::move(right));
    };
    auto cast = [&](TExprPtr expr, TTypePtr type) -> TExprPtr {
        return std::make_shared<TCastExpr>(loc, std::move(expr), std::move(type));
    };
    auto call = [&](const std::string& name, std::vector<TExprPtr> args) -> TExprPtr {
        return std::make_shared<TCallExpr>(loc, ident(name), std::move(args));
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

    auto rowSetRefType = std::make_shared<TReferenceType>(AsNamed("TRowSet", rowSetType));
    std::vector<TParam> params{
        std::make_shared<TVarStmt>(loc, "batch", rowSetRefType),
        std::make_shared<TVarStmt>(loc, "hashes", ptrU64Type),
        std::make_shared<TVarStmt>(loc, "key_columns", ptrI64Type),
        std::make_shared<TVarStmt>(loc, "key_type_witness", ptrLookupKeyType),
    };

    auto ptrColumnType = ColumnPointerType(columnType, rowSetType);
    auto columnValueType = PointerPointeeOr(ptrColumnType, columnType);
    auto columnAt = [&](size_t fieldIndex) -> TExprPtr {
        return std::make_shared<TIndexExpr>(
            loc,
            ident("cols"),
            std::make_shared<TIndexExpr>(
                loc,
                ident("key_columns"),
                numI64(static_cast<int64_t>(fieldIndex))));
    };

    std::vector<TExprPtr> body;
    body.push_back(var("n", i64Type));
    body.push_back(assign("n", field("batch", "RowCount")));
    body.push_back(var("selection", ptrU8Type));
    body.push_back(assign("selection", field("batch", "Selection")));
    body.push_back(var("cols", ptrColumnType));
    body.push_back(assign("cols", field("batch", "Columns")));
    for (size_t fieldIndex = 0; fieldIndex < key.Fields.size(); ++fieldIndex) {
        const std::string name = "key_column_" + std::to_string(fieldIndex);
        body.push_back(var(name, columnValueType));
        body.push_back(assign(name, columnAt(fieldIndex)));
    }
    body.push_back(var("selection_is_null", boolType));
    body.push_back(
        assign("selection_is_null", binary("==", cast(ident("selection"), i64Type), numI64(0))));
    body.push_back(var("i", i64Type));
    body.push_back(assign("i", numI64(0)));

    auto keyRead = GenKeyReadAst(key, "i", "hash_key_value_", stringViewType);

    std::vector<TExprPtr> process;
    process.insert(
        process.end(),
        std::make_move_iterator(keyRead.Setup.begin()),
        std::make_move_iterator(keyRead.Setup.end()));
    process.push_back(
        std::make_shared<TArrayAssignExpr>(
            loc,
            "hashes",
            std::vector<TExprPtr>{ident("i")},
            cast(call("rh_hash", {std::move(keyRead.Value)}), u64Type)));

    // A rejected row gets no hash: its slot keeps what the caller left there.
    auto selected = binary(
        "||",
        ident("selection_is_null"),
        binary(
            "!=",
            std::make_shared<TIndexExpr>(loc, ident("selection"), ident("i")),
            number(0, u8Type)));
    auto loop = block({
        std::make_shared<TIfExpr>(loc, std::move(selected), block(std::move(process)), nullptr),
        assign("i", binary("+", ident("i"), numI64(1))),
    });
    body.push_back(
        std::make_shared<TWhileStmtExpr>(
            loc,
            binary("<", ident("i"), ident("n")),
            std::move(loop)));
    body.push_back(std::make_shared<TReturnExpr>(loc, number(1, boolType)));

    auto function = std::make_shared<TFunDecl>(
        loc,
        funcName,
        std::vector<TGenericParam>{},
        std::move(params),
        std::make_shared<TBlockExpr>(loc, std::move(body)),
        boolType);
    function->Type = std::make_shared<TFunctionType>(
        std::vector<TTypePtr>{rowSetRefType, ptrU64Type, ptrI64Type, ptrLookupKeyType},
        boolType);
    function->Cacheable = true;
    return function;
}

NQumir::NAst::TExprPtr GenJoinHashBatchAst(
    const TJoinKeyDescriptor& key,
    const std::string& funcName,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr stringViewType)
{
    return GenKeyHashBatchAst(
        JoinKeyAggregateShim(key),
        funcName,
        columnType,
        rowSetType,
        stringViewType);
}

NQumir::NAst::TExprPtr GenKeyHashEntrypointAst(
    const TAggregateKeyDescriptor& key,
    const std::string& funcName,
    const std::string& batchFuncName,
    NQumir::NAst::TTypePtr rowSetType)
{
    NQumir::TLocation loc{};
    auto i64Type = std::make_shared<TIntegerType>();
    auto u64Type = std::make_shared<TIntegerType>(TIntegerType::U64);
    auto boolType = std::make_shared<TBoolType>();
    auto ptrU64Type = std::make_shared<TPointerType>(u64Type);
    auto ptrI64Type = std::make_shared<TPointerType>(i64Type);
    auto ptrLookupKeyType = std::make_shared<TPointerType>(key.LookupType);
    auto rowSetRefType = std::make_shared<TReferenceType>(AsNamed("TRowSet", rowSetType));

    std::vector<TParam> params{
        std::make_shared<TVarStmt>(loc, "batch", rowSetRefType),
        std::make_shared<TVarStmt>(loc, "hashes", ptrU64Type),
        std::make_shared<TVarStmt>(loc, "key_columns", ptrI64Type),
    };
    auto zero = std::make_shared<TNumberExpr>(loc, int64_t{0});
    zero->Type = i64Type;
    auto witness = std::make_shared<TCastExpr>(loc, std::move(zero), ptrLookupKeyType);
    auto call = std::make_shared<TCallExpr>(
        loc,
        std::make_shared<TIdentExpr>(loc, batchFuncName),
        std::vector<TExprPtr>{
            std::make_shared<TIdentExpr>(loc, "batch"),
            std::make_shared<TIdentExpr>(loc, "hashes"),
            std::make_shared<TIdentExpr>(loc, "key_columns"),
            std::move(witness),
        });
    auto body = std::make_shared<TBlockExpr>(
        loc,
        std::vector<TExprPtr>{
            std::make_shared<TReturnExpr>(loc, std::move(call)),
        });
    auto function = std::make_shared<TFunDecl>(
        loc,
        funcName,
        std::vector<TGenericParam>{},
        std::move(params),
        std::move(body),
        boolType);
    function->Type = std::make_shared<TFunctionType>(
        std::vector<TTypePtr>{rowSetRefType, ptrU64Type, ptrI64Type},
        boolType);
    return function;
}

NQumir::NAst::TExprPtr GenJoinHashEntrypointAst(
    const TJoinKeyDescriptor& key,
    const std::string& funcName,
    NQumir::NAst::TTypePtr rowSetType)
{
    return GenKeyHashEntrypointAst(
        JoinKeyAggregateShim(key),
        funcName,
        "jt_hash_batch",
        std::move(rowSetType));
}

namespace {

// One output column of the generated materializer: which side it reads,
// the source column index on that side, and its logical type.
struct TJoinOutputColumn {
    enum class EKind {
        Fixed,
        Bool,
        BinInt,
        String,
        Marker,
    };

    bool IsLeft = true;
    int32_t SrcColIdx = 0;
    NQumir::NAst::TTypePtr Type;
    NQumir::NAst::TTypePtr StorageType;
    EKind Kind = EKind::Fixed;
    int64_t FixedWidth = 0;
};

std::vector<TJoinOutputColumn> BuildJoinOutputColumns(
    const TStructType& leftType,
    const TStructType& rightType,
    EJoinOutputColumns output)
{
    auto classify = [](const TTypePtr& type, TJoinOutputColumn& col) {
        if (DecimalSpecOfValueType(UnwrapNullableType(type))) {
            col.Kind = TJoinOutputColumn::EKind::BinInt;
            return;
        }
        auto inner = UnwrapNamedType(UnwrapNullableType(type));
        if (auto integer = TMaybeType<TIntegerType>(inner)) {
            col.StorageType = inner;
            col.FixedWidth = integer.Cast()->BitWidth() / 8;
        } else if (TMaybeType<TFloatType>(inner)) {
            col.StorageType = inner;
            col.FixedWidth = 8;
        } else if (TMaybeType<TBoolType>(inner)) {
            col.Kind = TJoinOutputColumn::EKind::Bool;
        } else if (TMaybeType<TStringType>(inner)) {
            col.Kind = TJoinOutputColumn::EKind::String;
        } else {
            throw NQumir::TError(
                "GenJoinMaterializeAst: cannot materialize column of type " +
                (type ? type->ToString() : std::string("<null>")));
        }
    };

    std::vector<TJoinOutputColumn> columns;
    for (int32_t i = 0; i < static_cast<int32_t>(leftType.Fields.size()); ++i) {
        TJoinOutputColumn col{.IsLeft = true, .SrcColIdx = i, .Type = leftType.Fields[i].second};
        classify(col.Type, col);
        columns.push_back(std::move(col));
    }
    if (output == EJoinOutputColumns::LeftRight) {
        for (int32_t i = 0; i < static_cast<int32_t>(rightType.Fields.size()); ++i) {
            TJoinOutputColumn col{
                .IsLeft = false,
                .SrcColIdx = i,
                .Type = rightType.Fields[i].second,
            };
            classify(col.Type, col);
            columns.push_back(std::move(col));
        }
    }
    if (output == EJoinOutputColumns::LeftMarker) {
        columns.push_back({.Kind = TJoinOutputColumn::EKind::Marker});
    }
    return columns;
}

} // namespace

NQumir::NAst::TExprPtr GenJoinMaterializeAst(
    const NQumir::NAst::TStructType& leftType,
    const NQumir::NAst::TStructType& rightType,
    EJoinOutputColumns output,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr pairBufferType,
    NQumir::NAst::TTypePtr stringViewType)
{
    namespace Oz = NKernel::NOz;

    (void)stringViewType;

    const auto columns = BuildJoinOutputColumns(leftType, rightType, output);
    const int64_t columnCount = static_cast<int64_t>(columns.size());
    int64_t ownedPtrCount = 1; // columns, recorded by jm_begin
    for (const auto& column : columns) {
        ownedPtrCount += column.Kind == TJoinOutputColumn::EKind::String
            ? 3
            : 2;
    }

    auto i64Type = std::make_shared<TIntegerType>(TIntegerType::I64);
    auto ptrColumnType = ColumnPointerType(columnType, rowSetType);
    auto rowSetRefType = std::make_shared<TReferenceType>(AsNamed("TRowSet", rowSetType));
    auto rowSetPtrType = std::make_shared<TPointerType>(AsNamed("TRowSet", rowSetType));
    auto pairBufferRefType =
        std::make_shared<TReferenceType>(AsNamed("PairBuffer", pairBufferType));

    auto number = [&](int64_t value) -> TExprPtr {
        return Oz::TypedInt(value, i64Type);
    };
    auto outputColumn = [&](size_t index) -> TExprPtr {
        return Oz::Index("columns", number(static_cast<int64_t>(index)));
    };
    auto workerArgs = [&](const TJoinOutputColumn& column, size_t index) {
        const bool isLeft = column.IsLeft;
        return std::vector<TExprPtr>{
            Oz::Ident("pairs"),
            Oz::Ident(isLeft ? "left_store" : "right_store"),
            Oz::Ident(isLeft ? "stream_left" : "stream_right"),
            Oz::Ident("start"),
            Oz::Ident("n"),
            number(isLeft ? 0 : 1),
            number(column.SrcColIdx),
            outputColumn(index),
            Oz::Ident("out"),
        };
    };

    Oz::TFunBuilder builder("jt_materialize");
    builder.Param("pairs", pairBufferRefType)
        .Param("left_store", rowSetPtrType)
        .Param("right_store", rowSetPtrType)
        .Param("stream_left", rowSetRefType)
        .Param("stream_right", rowSetRefType)
        .Param("start", i64Type)
        .Param("limit", i64Type)
        .Param("out", rowSetRefType)
        .Return(i64Type)
        .Var("n", i64Type)
        .Assign("n", Oz::Sub(Oz::Field("pairs", "Count"), Oz::Ident("start")))
        .Stmt(
            Oz::If(
                Oz::Bin(TOperator("<"), Oz::Ident("limit"), Oz::Ident("n")),
                Oz::Block({Oz::Assign("n", Oz::Ident("limit"))})))
        .Stmt(
            Oz::If(
                Oz::Bin(TOperator("<="), Oz::Ident("n"), number(0)),
                Oz::Block({Oz::Return(number(0))})))
        .Var("columns", ptrColumnType)
        .Assign(
            "columns",
            Oz::Call(
                "jm_begin",
                {
                    number(columnCount),
                    number(ownedPtrCount),
                    Oz::Ident("n"),
                    Oz::Ident("out"),
                }));

    int64_t ownerIndex = 2;
    for (size_t index = 0; index < columns.size(); ++index) {
        const auto& column = columns[index];
        auto args = workerArgs(column, index);
        switch (column.Kind) {
        case TJoinOutputColumn::EKind::Fixed:
            args.push_back(number(ownerIndex++));
            args.push_back(number(ownerIndex++));
            args.push_back(number(column.FixedWidth));
            args.push_back(Oz::Cast(number(0), std::make_shared<TPointerType>(column.StorageType)));
            builder.Stmt(Oz::Call("jt_materialize_fixed_column", std::move(args)));
            break;
        case TJoinOutputColumn::EKind::Bool:
            args.push_back(number(ownerIndex++));
            args.push_back(number(ownerIndex++));
            builder.Stmt(Oz::Call("jt_materialize_bool_column", std::move(args)));
            break;
        case TJoinOutputColumn::EKind::BinInt:
            args.push_back(number(ownerIndex++));
            args.push_back(number(ownerIndex++));
            builder.Stmt(Oz::Call("jt_materialize_binint_column", std::move(args)));
            break;
        case TJoinOutputColumn::EKind::String:
            args.push_back(number(ownerIndex++));
            args.push_back(number(ownerIndex++));
            args.push_back(number(ownerIndex++));
            builder.Stmt(Oz::Call("jt_materialize_string_column", std::move(args)));
            break;
        case TJoinOutputColumn::EKind::Marker:
            builder.Stmt(
                Oz::Call(
                    "mj_materialize_marker_column",
                    {
                        Oz::Ident("pairs"),
                        Oz::Ident("start"),
                        Oz::Ident("n"),
                        outputColumn(index),
                        Oz::Ident("out"),
                        number(ownerIndex),
                        number(ownerIndex + 1),
                    }));
            ownerIndex += 2;
            break;
        }
    }

    builder.Stmt(Oz::Return(Oz::Ident("n")));
    return std::move(builder).Build();
}

// ---------------------------------------------------------------------------
// Row join table (kernel/join/join_rows.oz)
// ---------------------------------------------------------------------------

namespace Oz = NOz;

namespace {

// Byte size and alignment of one payload column in a build row.
void ClassifyRowJoinColumn(
    const TTypePtr& type,
    TRowJoinColumn& col,
    int64_t& size,
    int64_t& alignment)
{
    if (DecimalSpecOfValueType(UnwrapNullableType(type))) {
        col.Kind = TRowJoinColumn::EKind::BinInt;
        size = 16;
        alignment = 16;
        return;
    }
    auto inner = UnwrapNamedType(UnwrapNullableType(type));
    if (auto integer = TMaybeType<TIntegerType>(inner)) {
        col.Kind = TRowJoinColumn::EKind::Fixed;
        col.StorageType = inner;
        col.FixedWidth = integer.Cast()->BitWidth() / 8;
        size = col.FixedWidth;
        alignment = col.FixedWidth;
    } else if (TMaybeType<TFloatType>(inner)) {
        col.Kind = TRowJoinColumn::EKind::Fixed;
        col.StorageType = inner;
        col.FixedWidth = 8;
        size = 8;
        alignment = 8;
    } else if (TMaybeType<TBoolType>(inner)) {
        col.Kind = TRowJoinColumn::EKind::Bool;
        size = 1;
        alignment = 1;
    } else if (TMaybeType<TStringType>(inner)) {
        col.Kind = TRowJoinColumn::EKind::String;
        size = 16;
        alignment = 8;
    } else {
        throw NQumir::TError(
            "BuildRowJoinLayout: cannot store column of type " +
            (type ? type->ToString() : std::string("<null>")));
    }
}

int64_t AlignUpI64(int64_t value, int64_t alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

// Batch prologue shared by append and probe: n, selection, cols, the key
// columns (cols[key_columns[k]]) and, when present, the precomputed hashes.
void GenBatchPrologue(
    Oz::TFunBuilder& builder,
    const TJoinKeyDescriptor& key,
    const TRowJoinGenTypes& types,
    bool hasPrecomputedHash)
{
    auto i64Type = std::make_shared<TIntegerType>(TIntegerType::I64);
    auto u8Type = std::make_shared<TIntegerType>(TIntegerType::U8);
    auto u64Type = std::make_shared<TIntegerType>(TIntegerType::U64);
    auto boolType = std::make_shared<TBoolType>();
    auto ptrColumnType = ColumnPointerType(types.Column, types.RowSet);
    auto columnValueType = PointerPointeeOr(ptrColumnType, types.Column);
    builder.Var("n", i64Type)
        .Assign("n", Oz::Field("batch", "RowCount"))
        .Var("selection", std::make_shared<TPointerType>(u8Type))
        .Assign("selection", Oz::Field("batch", "Selection"))
        .Var("cols", ptrColumnType)
        .Assign("cols", Oz::Field("batch", "Columns"))
        .Var("selection_is_null", boolType)
        .Assign(
            "selection_is_null",
            Oz::Bin(
                TOperator("=="),
                Oz::Cast(Oz::Ident("selection"), i64Type),
                Oz::TypedInt(0, i64Type)));
    for (size_t fieldIndex = 0; fieldIndex < key.Fields.size(); ++fieldIndex) {
        const std::string name = "key_column_" + std::to_string(fieldIndex);
        builder.Var(name, columnValueType)
            .Assign(
                name,
                Oz::Index(
                    "cols",
                    Oz::Index(
                        "key_columns",
                        Oz::TypedInt(static_cast<int64_t>(fieldIndex), i64Type))));
    }
    if (hasPrecomputedHash) {
        builder.Var("hash_ptr", std::make_shared<TPointerType>(u64Type))
            .Assign("hash_ptr", Oz::Field("batch", "Hash"));
    }
}

// hash_value = hash_ptr ? hash_ptr[row] : rh_hash(key_value), as u64.
std::vector<TExprPtr>
GenHashValue(const std::string& rowVar, const std::string& keyVar, bool hasPrecomputedHash) {
    auto i64Type = std::make_shared<TIntegerType>(TIntegerType::I64);
    auto u64Type = std::make_shared<TIntegerType>(TIntegerType::U64);
    std::vector<TExprPtr> stmts;
    stmts.push_back(Oz::Var("hash_value", u64Type));
    auto computed = Oz::Cast(Oz::Call("rh_hash", {Oz::Ident(keyVar)}), u64Type);
    if (hasPrecomputedHash) {
        // The shuffle leaves Hash null on its multi-batch gather path.
        stmts.push_back(
            Oz::If(
                Oz::Bin(
                    TOperator("!="),
                    Oz::Cast(Oz::Ident("hash_ptr"), i64Type),
                    Oz::TypedInt(0, i64Type)),
                Oz::Block({Oz::Assign("hash_value", Oz::Index("hash_ptr", Oz::Ident(rowVar)))}),
                Oz::Block({Oz::Assign("hash_value", std::move(computed))})));
    } else {
        stmts.push_back(Oz::Assign("hash_value", std::move(computed)));
    }
    return stmts;
}

TExprPtr RowSelected(const std::string& rowVar) {
    auto u8Type = std::make_shared<TIntegerType>(TIntegerType::U8);
    return Oz::Bin(
        TOperator("||"),
        Oz::Ident("selection_is_null"),
        Oz::Bin(
            TOperator("!="),
            Oz::Index("selection", Oz::Ident(rowVar)),
            Oz::TypedInt(0, u8Type)));
}

// (cast (+ (cast row i64) offset) <ptr T>)
TExprPtr RowFieldPointer(const std::string& rowVar, int64_t offset, const TTypePtr& pointeeType) {
    auto i64Type = std::make_shared<TIntegerType>(TIntegerType::I64);
    return Oz::Cast(
        Oz::Add(Oz::Cast(Oz::Ident(rowVar), i64Type), Oz::TypedInt(offset, i64Type)),
        std::make_shared<TPointerType>(pointeeType));
}

TTypePtr RefType(const char* name, const TTypePtr& type) {
    return std::make_shared<TReferenceType>(AsNamed(name, type));
}

} // namespace

TRowJoinLayout BuildRowJoinLayout(
    const TStructType& buildType,
    const TJoinKeyDescriptor& key,
    bool marked)
{
    TRowJoinLayout layout;
    bool hasBinInt = false;
    int64_t cursor = 0;
    std::vector<std::pair<int64_t, int64_t>> sizes;
    for (int32_t i = 0; i < static_cast<int32_t>(buildType.Fields.size()); ++i) {
        TRowJoinColumn col{.SrcColIdx = i};
        int64_t size = 0;
        int64_t alignment = 1;
        ClassifyRowJoinColumn(buildType.Fields[i].second, col, size, alignment);
        hasBinInt = hasBinInt || col.Kind == TRowJoinColumn::EKind::BinInt;
        layout.HasString = layout.HasString || col.Kind == TRowJoinColumn::EKind::String;
        layout.Columns.push_back(std::move(col));
        sizes.emplace_back(size, alignment);
    }
    const bool keyHasBinInt = std::ranges::any_of(key.Fields, [](const auto& field) {
        return IsBinIntStorageType(field.Type);
    });
    const int64_t rowAlignment = (hasBinInt || keyHasBinInt)
        ? 16
        : 8;
    // The hash/next word, then the key struct at the row alignment.
    layout.KeyOffset = AlignUpI64(8, rowAlignment);
    cursor = layout.KeyOffset + static_cast<int64_t>(key.Size);
    for (size_t i = 0; i < layout.Columns.size(); ++i) {
        if (IsNullableType(buildType.Fields[i].second)) {
            layout.Columns[i].ValidOffset = cursor++;
        }
    }
    if (marked) {
        layout.MarkOffset = cursor++;
    }
    for (size_t i = 0; i < layout.Columns.size(); ++i) {
        cursor = AlignUpI64(cursor, sizes[i].second);
        layout.Columns[i].Offset = cursor;
        cursor += sizes[i].first;
    }
    layout.RowBytes = AlignUpI64(cursor, rowAlignment);
    if (key.IsScalar()) {
        const auto& field = key.Fields[0];
        auto integer = TMaybeType<TIntegerType>(UnwrapNamedType(field.Type));
        if (integer && !(field.IsNullable && field.NullsEqual)) {
            layout.DirectKeyOffset = layout.KeyOffset + static_cast<int64_t>(field.Offset);
            layout.DirectKeyType = integer.Cast();
        }
    }
    return layout;
}

NQumir::NAst::TExprPtr GenRowJoinAppendAst(
    const TJoinKeyDescriptor& key,
    const TRowJoinLayout& layout,
    const std::string& funcName,
    const TRowJoinGenTypes& types,
    bool hasPrecomputedHash,
    bool nullKeys)
{
    auto i64Type = std::make_shared<TIntegerType>(TIntegerType::I64);
    auto u8Type = std::make_shared<TIntegerType>(TIntegerType::U8);
    auto u64Type = std::make_shared<TIntegerType>(TIntegerType::U64);
    auto boolType = std::make_shared<TBoolType>();
    auto number = [&](int64_t value) {
        return Oz::TypedInt(value, i64Type);
    };

    Oz::TFunBuilder builder(funcName);
    builder.Param("arena", RefType("RowArena", types.Arena))
        .Param("batch", RefType("TRowSet", types.RowSet))
        .Param("key_columns", std::make_shared<TPointerType>(i64Type))
        .Return(boolType);
    GenBatchPrologue(builder, key, types, hasPrecomputedHash);

    auto keyRead = GenKeyReadAst(key, "i", "key_value_", types.StringView);
    std::vector<TExprPtr> store = std::move(keyRead.Setup);
    store.push_back(Oz::Var("key_value", key.LookupType));
    store.push_back(Oz::Assign("key_value", std::move(keyRead.Value)));

    // A row that no table indexes needs no hash.
    std::vector<TExprPtr> valid;
    if (nullKeys) {
        valid.push_back(Oz::Var("hash_value", u64Type));
        valid.push_back(Oz::Assign("hash_value", Oz::TypedInt(0, u64Type)));
    } else {
        valid = GenHashValue("i", "key_value", hasPrecomputedHash);
    }
    valid.push_back(Oz::Var("row", std::make_shared<TPointerType>(u8Type)));
    valid.push_back(Oz::Assign("row", Oz::Call("ra_new_row", {Oz::Ident("arena")})));
    valid.push_back(
        Oz::If(
            Oz::Bin(TOperator("=="), Oz::Cast(Oz::Ident("row"), i64Type), number(0)),
            Oz::Block({Oz::Return(Oz::Bool(false))})));
    valid.push_back(Oz::Var("words", std::make_shared<TPointerType>(u64Type)));
    valid.push_back(
        Oz::Assign("words", Oz::Cast(Oz::Ident("row"), std::make_shared<TPointerType>(u64Type))));
    valid.push_back(Oz::ArrayAssign("words", number(0), Oz::Ident("hash_value")));
    valid.push_back(Oz::Var("key_slot", std::make_shared<TPointerType>(key.LookupType)));
    valid.push_back(
        Oz::Assign("key_slot", RowFieldPointer("row", layout.KeyOffset, key.LookupType)));
    valid.push_back(Oz::ArrayAssign("key_slot", number(0), Oz::Ident("key_value")));
    if (layout.MarkOffset >= 0) {
        valid.push_back(
            Oz::ArrayAssign("row", number(layout.MarkOffset), Oz::TypedInt(0, u8Type)));
    }
    for (const auto& col : layout.Columns) {
        std::vector<TExprPtr> args = {
            Oz::Ident("row"),
            number(col.Offset),
            number(col.ValidOffset),
            Oz::Index("cols", number(col.SrcColIdx)),
            Oz::Ident("i"),
        };
        switch (col.Kind) {
        case TRowJoinColumn::EKind::Fixed:
            args.push_back(Oz::Cast(number(0), std::make_shared<TPointerType>(col.StorageType)));
            valid.push_back(Oz::Call("jr_store_fixed", std::move(args)));
            break;
        case TRowJoinColumn::EKind::Bool:
            valid.push_back(Oz::Call("jr_store_bool", std::move(args)));
            break;
        case TRowJoinColumn::EKind::BinInt:
            valid.push_back(Oz::Call("jr_store_binint", std::move(args)));
            break;
        case TRowJoinColumn::EKind::String:
            valid.push_back(Oz::Call("jr_store_string", std::move(args)));
            break;
        }
    }
    auto keyValid = Oz::Call("jt_key_valid", {Oz::Ident("key_value")});
    store.push_back(
        Oz::If(
            nullKeys ? Oz::Unary(TOperator("!"), std::move(keyValid)) : std::move(keyValid),
            Oz::Block(std::move(valid))));

    builder.Var("i", i64Type)
        .Assign("i", number(0))
        .Stmt(
            Oz::While(
                Oz::Bin(TOperator("<"), Oz::Ident("i"), Oz::Ident("n")),
                Oz::Block({
                    Oz::If(RowSelected("i"), Oz::Block(std::move(store))),
                    Oz::Assign("i", Oz::Add(Oz::Ident("i"), number(1))),
                })))
        .Stmt(Oz::Return(Oz::Bool(true)));
    return std::move(builder).Build();
}

NQumir::NAst::TExprPtr GenRowJoinProbeAst(
    const TJoinKeyDescriptor& key,
    const TRowJoinLayout& layout,
    bool probeIsLeft,
    bool emitUnmatched,
    const std::string& funcName,
    const TRowJoinGenTypes& types,
    bool hasPrecomputedHash)
{
    auto i64Type = std::make_shared<TIntegerType>(TIntegerType::I64);
    auto u8Type = std::make_shared<TIntegerType>(TIntegerType::U8);
    auto u64Type = std::make_shared<TIntegerType>(TIntegerType::U64);
    auto boolType = std::make_shared<TBoolType>();
    auto ptrKeyType = std::make_shared<TPointerType>(key.LookupType);
    auto number = [&](int64_t value) {
        return Oz::TypedInt(value, i64Type);
    };

    Oz::TFunBuilder builder(funcName);
    builder.Param("table", RefType("RowTable", types.Table))
        .Param("scratch", RefType("RowScratch", types.Scratch))
        .Param("batch", RefType("TRowSet", types.RowSet))
        .Param("key_columns", std::make_shared<TPointerType>(i64Type))
        .Param("pairs", RefType("PairBuffer", types.PairBuffer))
        .Return(boolType);
    GenBatchPrologue(builder, key, types, hasPrecomputedHash);
    // A NULL key matches nothing, so the preserved row of an outer join is
    // padded at once.
    auto padNullKey = [&](const std::string& row) -> TExprPtr {
        if (!emitUnmatched) {
            return nullptr;
        }
        auto padded = Oz::Call(
            "jr_push_pair",
            {
                Oz::Ident("pairs"),
                Oz::Ident(row),
                Oz::NullPtr(std::make_shared<TPointerType>(u8Type)),
                Oz::Bool(probeIsLeft),
            });
        return Oz::Block({
            Oz::If(
                Oz::Unary(TOperator("!"), std::move(padded)),
                Oz::Block({Oz::Return(Oz::Bool(false))})),
        });
    };
    if (layout.DirectKeyOffset >= 0) {
        // A direct index (jr_build_direct) needs neither the hash nor the
        // scratch: every key is looked up as it is read.
        auto directRead = GenKeyReadAst(key, "d", "direct_key_", types.StringView);
        std::vector<TExprPtr> lookup = std::move(directRead.Setup);
        lookup.push_back(Oz::Var("direct_key", key.LookupType));
        lookup.push_back(Oz::Assign("direct_key", std::move(directRead.Value)));
        lookup.push_back(
            Oz::If(
                Oz::Call("jt_key_valid", {Oz::Ident("direct_key")}),
                Oz::Block({
                    Oz::If(
                        Oz::Unary(
                            TOperator("!"),
                            Oz::Call(
                                "jr_direct_probe",
                                {
                                    Oz::Ident("table"),
                                    Oz::Ident("pairs"),
                                    Oz::Cast(Oz::Field("direct_key", "key_0"), i64Type),
                                    Oz::Ident("d"),
                                    Oz::Bool(probeIsLeft),
                                    Oz::Bool(emitUnmatched),
                                    number(layout.MarkOffset),
                                })),
                        Oz::Block({Oz::Return(Oz::Bool(false))})),
                }),
                padNullKey("d")));
        builder.Stmt(
            Oz::If(
                Oz::Bin(
                    TOperator("!="),
                    Oz::Cast(Oz::Field("table", "Direct"), i64Type),
                    number(0)),
                Oz::Block({
                    Oz::Var("d", i64Type),
                    Oz::Assign("d", number(0)),
                    Oz::While(
                        Oz::Bin(TOperator("<"), Oz::Ident("d"), Oz::Ident("n")),
                        Oz::Block({
                            Oz::If(RowSelected("d"), Oz::Block(std::move(lookup))),
                            Oz::Assign("d", Oz::Add(Oz::Ident("d"), number(1))),
                        })),
                    Oz::Return(Oz::Bool(true)),
                })));
    }
    builder
        .Stmt(
            Oz::If(
                Oz::Unary(
                    TOperator("!"),
                    Oz::Call(
                        "rt_scratch_reserve",
                        {
                            Oz::Ident("scratch"),
                            Oz::Ident("n"),
                            number(static_cast<int64_t>(key.Size)),
                        })),
                Oz::Block({Oz::Return(Oz::Bool(false))})))
        .Var("hashes", std::make_shared<TPointerType>(u64Type))
        .Assign("hashes", Oz::Field("scratch", "Hashes"))
        .Var("rows", std::make_shared<TPointerType>(i64Type))
        .Assign("rows", Oz::Field("scratch", "Rows"))
        .Var("keys", ptrKeyType)
        .Assign("keys", Oz::Cast(Oz::Field("scratch", "Keys"), ptrKeyType))
        .Var("cnt", i64Type)
        .Assign("cnt", number(0));

    // Pass 1, the only one that reads the batch: the key and hash of every
    // selected row with a valid key. The pointer and key passes are
    // rt_seek_all of the row table and jr_probe_candidates of join_rows.oz.
    auto keyRead = GenKeyReadAst(key, "i", "key_value_", types.StringView);
    std::vector<TExprPtr> collect = std::move(keyRead.Setup);
    collect.push_back(Oz::Var("key_value", key.LookupType));
    collect.push_back(Oz::Assign("key_value", std::move(keyRead.Value)));
    std::vector<TExprPtr> valid = GenHashValue("i", "key_value", hasPrecomputedHash);
    valid.push_back(Oz::ArrayAssign("rows", Oz::Ident("cnt"), Oz::Ident("i")));
    valid.push_back(Oz::ArrayAssign("hashes", Oz::Ident("cnt"), Oz::Ident("hash_value")));
    valid.push_back(Oz::ArrayAssign("keys", Oz::Ident("cnt"), Oz::Ident("key_value")));
    valid.push_back(Oz::Assign("cnt", Oz::Add(Oz::Ident("cnt"), number(1))));
    collect.push_back(
        Oz::If(
            Oz::Call("jt_key_valid", {Oz::Ident("key_value")}),
            Oz::Block(std::move(valid)),
            padNullKey("i")));
    builder.Var("i", i64Type)
        .Assign("i", number(0))
        .Stmt(
            Oz::While(
                Oz::Bin(TOperator("<"), Oz::Ident("i"), Oz::Ident("n")),
                Oz::Block({
                    Oz::If(RowSelected("i"), Oz::Block(std::move(collect))),
                    Oz::Assign("i", Oz::Add(Oz::Ident("i"), number(1))),
                })))
        .Stmt(Oz::Call("rt_seek_all", {Oz::Ident("table"), Oz::Ident("scratch"), Oz::Ident("cnt")}))
        .Stmt(
            Oz::Return(
                Oz::Call(
                    "jr_probe_candidates",
                    {
                        Oz::Ident("table"),
                        Oz::Ident("scratch"),
                        Oz::Ident("pairs"),
                        Oz::Ident("cnt"),
                        number(layout.KeyOffset),
                        Oz::Bool(probeIsLeft),
                        Oz::Bool(emitUnmatched),
                        number(layout.MarkOffset),
                        Oz::Cast(number(0), ptrKeyType),
                    })));
    return std::move(builder).Build();
}

NQumir::NAst::TExprPtr GenRowJoinDispatchAst(
    const TJoinKeyDescriptor& key,
    const TRowJoinLayout& layout,
    const TRowJoinGenTypes& types,
    EJoinType type)
{
    auto i64Type = std::make_shared<TIntegerType>(TIntegerType::I64);
    auto boolType = std::make_shared<TBoolType>();
    auto number = [&](int64_t value) {
        return Oz::TypedInt(value, i64Type);
    };
    auto opIs = [&](int64_t op) {
        return Oz::Bin(TOperator("=="), Oz::Ident("op"), number(op));
    };
    // Rows per arena block: a few hundred KiB per block for typical rows.
    constexpr int64_t blockRows = 16384;

    // Build: the direct index when the key and the build keys allow one,
    // otherwise the salted table.
    std::vector<TExprPtr> build;
    if (layout.DirectKeyOffset >= 0) {
        build.push_back(
            Oz::If(
                Oz::Call(
                    "jr_build_direct",
                    {
                        Oz::Ident("table"),
                        Oz::Ident("arena"),
                        number(layout.DirectKeyOffset),
                        Oz::Cast(number(0), std::make_shared<TPointerType>(layout.DirectKeyType)),
                    }),
                Oz::Block({Oz::Return(Oz::Bool(true))})));
    }
    build.push_back(
        Oz::Return(
            Oz::Call(
                "jr_build_rows",
                {
                    Oz::Ident("table"),
                    Oz::Ident("arena"),
                    number(layout.KeyOffset),
                    Oz::Cast(number(0), std::make_shared<TPointerType>(key.LookupType)),
                })));

    Oz::TFunBuilder builder("jr_dispatch");
    builder.Param("arena", RefType("RowArena", types.Arena))
        .Param("table", RefType("RowTable", types.Table))
        .Param("scratch", RefType("RowScratch", types.Scratch))
        .Param("pairs", RefType("PairBuffer", types.PairBuffer))
        .Param("batch", RefType("TRowSet", types.RowSet))
        .Param("key_columns", std::make_shared<TPointerType>(i64Type))
        .Param("op", i64Type)
        .Return(boolType)
        .Stmt(
            Oz::If(
                opIs(0),
                Oz::Block({
                    Oz::Call(
                        "ra_init",
                        {Oz::Ident("arena"), number(layout.RowBytes), number(blockRows)}),
                    Oz::Return(Oz::Bool(true)),
                })))
        .Stmt(
            Oz::If(
                opIs(1),
                Oz::Block({
                    Oz::Return(
                        Oz::Call(
                            "jr_append_batch",
                            {Oz::Ident("arena"), Oz::Ident("batch"), Oz::Ident("key_columns")})),
                })))
        .Stmt(Oz::If(opIs(2), Oz::Block(std::move(build))))
        .Stmt(
            Oz::If(
                opIs(3),
                Oz::Block({
                    Oz::Return(
                        Oz::Call(
                            "jr_probe_batch",
                            {
                                Oz::Ident("table"),
                                Oz::Ident("scratch"),
                                Oz::Ident("batch"),
                                Oz::Ident("key_columns"),
                                Oz::Ident("pairs"),
                            })),
                })))
        .Stmt(
            Oz::If(
                opIs(4),
                Oz::Block({
                    Oz::Call("ra_destroy", {Oz::Ident("arena")}),
                    Oz::Call("rt_destroy", {Oz::Ident("table")}),
                    Oz::Call("rt_scratch_destroy", {Oz::Ident("scratch")}),
                    Oz::Call("pb_destroy", {Oz::Ident("pairs")}),
                    Oz::Return(Oz::Bool(true)),
                })));
    if (layout.MarkOffset >= 0) {
        if (type == EJoinType::LeftAnti) {
            builder.Stmt(
                Oz::If(
                    opIs(5),
                    Oz::Block({
                        Oz::Return(
                            Oz::Call(
                                "jr_append_null_keys",
                                {Oz::Ident("arena"), Oz::Ident("batch"), Oz::Ident("key_columns")})),
                    })));
        }
        builder.Stmt(
            Oz::If(
                opIs(6),
                Oz::Block({
                    Oz::Return(
                        Oz::Call(
                            "jr_emit_by_mark",
                            {
                                Oz::Ident("arena"),
                                Oz::Ident("pairs"),
                                number(layout.MarkOffset),
                                Oz::Bool(type == EJoinType::LeftSemi),
                            })),
                })));
    }
    builder.Stmt(Oz::Return(Oz::Bool(false)));
    return std::move(builder).Build();
}

NQumir::NAst::TExprPtr GenRowJoinMaterializeAst(
    const TStructType& leftType,
    const TStructType& rightType,
    bool buildIsLeft,
    EJoinOutputColumns output,
    const TRowJoinLayout& layout,
    const TRowJoinGenTypes& types)
{
    auto i64Type = std::make_shared<TIntegerType>(TIntegerType::I64);
    auto number = [&](int64_t value) {
        return Oz::TypedInt(value, i64Type);
    };
    auto ptrColumnType = ColumnPointerType(types.Column, types.RowSet);
    auto rowSetPtrType = std::make_shared<TPointerType>(AsNamed("TRowSet", types.RowSet));

    const auto& probeType = buildIsLeft
        ? rightType
        : leftType;
    const auto probeColumns = output == EJoinOutputColumns::LeftRight
        ? BuildJoinOutputColumns(probeType, probeType, EJoinOutputColumns::Left)
        : std::vector<TJoinOutputColumn>{};
    const int64_t probePair = buildIsLeft
        ? 1
        : 0;
    const int64_t buildPair = buildIsLeft
        ? 0
        : 1;
    const int64_t columnCount = static_cast<int64_t>(leftType.Fields.size())
        + (output == EJoinOutputColumns::LeftRight
            ? static_cast<int64_t>(rightType.Fields.size())
            : 0);
    int64_t ownedPtrCount = 1; // columns, recorded by jm_begin
    for (const auto& column : probeColumns) {
        ownedPtrCount += column.Kind == TJoinOutputColumn::EKind::String
            ? 3
            : 2;
    }
    for (const auto& column : layout.Columns) {
        ownedPtrCount += column.Kind == TRowJoinColumn::EKind::String
            ? 3
            : 2;
    }

    Oz::TFunBuilder builder("jr_materialize");
    builder.Param("pairs", RefType("PairBuffer", types.PairBuffer))
        .Param("stream", RefType("TRowSet", types.RowSet))
        .Param("start", i64Type)
        .Param("limit", i64Type)
        .Param("out", RefType("TRowSet", types.RowSet))
        .Return(i64Type)
        .Var("n", i64Type)
        .Assign("n", Oz::Sub(Oz::Field("pairs", "Count"), Oz::Ident("start")))
        .Stmt(
            Oz::If(
                Oz::Bin(TOperator("<"), Oz::Ident("limit"), Oz::Ident("n")),
                Oz::Block({Oz::Assign("n", Oz::Ident("limit"))})))
        .Stmt(
            Oz::If(
                Oz::Bin(TOperator("<="), Oz::Ident("n"), number(0)),
                Oz::Block({Oz::Return(number(0))})))
        .Var("columns", ptrColumnType)
        .Assign(
            "columns",
            Oz::Call(
                "jm_begin",
                {number(columnCount), number(ownedPtrCount), Oz::Ident("n"), Oz::Ident("out")}));

    int64_t ownerIndex = 2;
    auto emitProbeColumn = [&](const TJoinOutputColumn& column, int64_t outIndex) {
        std::vector<TExprPtr> args = {
            Oz::Ident("pairs"),
            Oz::NullPtr(rowSetPtrType),
            Oz::Ident("stream"),
            Oz::Ident("start"),
            Oz::Ident("n"),
            number(probePair),
            number(column.SrcColIdx),
            Oz::Index("columns", number(outIndex)),
            Oz::Ident("out"),
        };
        switch (column.Kind) {
        case TJoinOutputColumn::EKind::Fixed:
            args.push_back(number(ownerIndex++));
            args.push_back(number(ownerIndex++));
            args.push_back(number(column.FixedWidth));
            args.push_back(Oz::Cast(number(0), std::make_shared<TPointerType>(column.StorageType)));
            builder.Stmt(Oz::Call("jt_materialize_fixed_column", std::move(args)));
            break;
        case TJoinOutputColumn::EKind::Bool:
            args.push_back(number(ownerIndex++));
            args.push_back(number(ownerIndex++));
            builder.Stmt(Oz::Call("jt_materialize_bool_column", std::move(args)));
            break;
        case TJoinOutputColumn::EKind::BinInt:
            args.push_back(number(ownerIndex++));
            args.push_back(number(ownerIndex++));
            builder.Stmt(Oz::Call("jt_materialize_binint_column", std::move(args)));
            break;
        case TJoinOutputColumn::EKind::String:
            args.push_back(number(ownerIndex++));
            args.push_back(number(ownerIndex++));
            args.push_back(number(ownerIndex++));
            builder.Stmt(Oz::Call("jt_materialize_string_column", std::move(args)));
            break;
        case TJoinOutputColumn::EKind::Marker:
            throw NQumir::TError("GenRowJoinMaterializeAst: unexpected marker column");
        }
    };
    auto emitBuildColumn = [&](const TRowJoinColumn& column, int64_t outIndex) {
        std::vector<TExprPtr> args = {
            Oz::Ident("pairs"),
            Oz::Ident("start"),
            Oz::Ident("n"),
            number(buildPair),
            number(column.Offset),
            number(column.ValidOffset),
            Oz::Index("columns", number(outIndex)),
            Oz::Ident("out"),
        };
        switch (column.Kind) {
        case TRowJoinColumn::EKind::Fixed:
            args.push_back(number(ownerIndex++));
            args.push_back(number(ownerIndex++));
            args.push_back(number(column.FixedWidth));
            args.push_back(Oz::Cast(number(0), std::make_shared<TPointerType>(column.StorageType)));
            builder.Stmt(Oz::Call("jr_gather_fixed_column", std::move(args)));
            break;
        case TRowJoinColumn::EKind::Bool:
            args.push_back(number(ownerIndex++));
            args.push_back(number(ownerIndex++));
            builder.Stmt(Oz::Call("jr_gather_bool_column", std::move(args)));
            break;
        case TRowJoinColumn::EKind::BinInt:
            args.push_back(number(ownerIndex++));
            args.push_back(number(ownerIndex++));
            builder.Stmt(Oz::Call("jr_gather_binint_column", std::move(args)));
            break;
        case TRowJoinColumn::EKind::String:
            args.push_back(number(ownerIndex++));
            args.push_back(number(ownerIndex++));
            args.push_back(number(ownerIndex++));
            builder.Stmt(Oz::Call("jr_gather_string_column", std::move(args)));
            break;
        }
    };

    // Output order is left columns then right columns, whichever side is built.
    int64_t outIndex = 0;
    if (buildIsLeft) {
        for (const auto& column : layout.Columns) {
            emitBuildColumn(column, outIndex++);
        }
        for (const auto& column : probeColumns) {
            emitProbeColumn(column, outIndex++);
        }
    } else {
        for (const auto& column : probeColumns) {
            emitProbeColumn(column, outIndex++);
        }
        for (const auto& column : layout.Columns) {
            emitBuildColumn(column, outIndex++);
        }
    }
    builder.Stmt(Oz::Return(Oz::Ident("n")));
    return std::move(builder).Build();
}
} // namespace NQdb::NKernel
