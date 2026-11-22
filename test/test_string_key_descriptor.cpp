#include <gtest/gtest.h>

#include <qdb/kernel/aggregate_key.h>
#include <qdb/kernel/gen.h>
#include <qdb/kernel/lib.h>
#include <qdb/plan/types/nullable.h>
#include <qdb/modules/qumirdb.h>
#include <qdb/modules/qumirdb_types.h>

#include "qumirdb_source_module.h"
#include "test_types.h"

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/parser/type.h>
#include <qumir/runner/runner_llvm.h>

#include <array>
#include <algorithm>
#include <cstring>
#include <memory>
#include <string>

namespace {

using namespace NQumir::NAst;

TTypePtr Nullable(TTypePtr type) {
    return NQdb::MakeNullableType(NQdb::TestTypes(), std::move(type));
}

const TStructType* StructOf(const TTypePtr& type) {
    auto named = TMaybeType<TNamedType>(type);
    auto concrete = named ? named.Cast()->UnderlyingType() : type;
    auto structure = TMaybeType<TStructType>(concrete);
    return structure ? structure.Cast() : nullptr;
}

std::string NamedTypeName(const TTypePtr& type) {
    auto named = TMaybeType<TNamedType>(type);
    return named ? named.Cast()->Name : std::string{};
}

std::unique_ptr<NQumir::TLLVMRunner> CompileKeyOperation(
    const NQdb::NKernel::TAggregateKeyDescriptor& key,
    const std::string& entryName,
    const std::string& operation,
    const std::vector<TTypePtr>& paramTypes,
    const TTypePtr& resultType,
    void*& entry,
    bool pointerParams = false)
{
    using namespace NQumir;
    using namespace NQumir::NAst;

    std::vector<TExprPtr> stmts;
    if (!key.LookupTypeName.empty()) {
        stmts.push_back(NQumir::MakeIntrusive<TTypeDeclStmt>(
            TLocation{}, key.LookupType));
    }
    if (!key.StoredTypeName.empty()) {
        stmts.push_back(NQumir::MakeIntrusive<TTypeDeclStmt>(
            TLocation{}, key.StoredType));
    } else if (TMaybeType<TNamedType>(key.StoredType)) {
        stmts.push_back(NQumir::MakeIntrusive<TTypeDeclStmt>(
            TLocation{}, key.StoredType));
    }
    auto stringOps = NQdb::NKernel::ParseFunctionLibrary(NQdb::TestTypes(),
        NQdb::NKernel::ReadAggregationKernel("string_ops.oz"));
    if (!stringOps) {
        ADD_FAILURE() << stringOps.error().ToString();
        return {};
    }
    stmts.insert(stmts.end(), stringOps->begin(), stringOps->end());
    auto keyOps = NQdb::NKernel::GenKeyOperationFunDecls(NQdb::TestTypes(), key);
    stmts.insert(stmts.end(), keyOps.begin(), keyOps.end());
    auto ownership = NQdb::NKernel::GenKeyOwnershipFunDecls(NQdb::TestTypes(), key);
    stmts.insert(stmts.end(), ownership.begin(), ownership.end());

    std::vector<TParam> params;
    std::vector<TExprPtr> args;
    std::vector<TTypePtr> wrapperParamTypes;
    for (size_t i = 0; i < paramTypes.size(); ++i) {
        const std::string name = "arg_" + std::to_string(i);
        TTypePtr wrapperType = pointerParams
            ? NQdb::TestTypes().MakePointer(paramTypes[i])
            : paramTypes[i];
        params.push_back(NQumir::MakeIntrusive<TVarStmt>(
            TLocation{}, name, wrapperType));
        wrapperParamTypes.push_back(wrapperType);
        TExprPtr argument = NQumir::MakeIntrusive<TIdentExpr>(TLocation{}, name);
        args.push_back(pointerParams
            ? NQumir::MakeIntrusive<TIndexExpr>(TLocation{}, argument,
                NQumir::MakeIntrusive<TNumberExpr>(TLocation{}, int64_t{0}, NQdb::TestTypes()))
            : argument);
    }
    auto call = NQumir::MakeIntrusive<TCallExpr>(TLocation{},
        NQumir::MakeIntrusive<TIdentExpr>(TLocation{}, operation), std::move(args));
    auto function = NQumir::MakeIntrusive<TFunDecl>(TLocation{}, entryName,
        std::vector<TGenericParam>{},
        std::move(params), NQumir::MakeIntrusive<TBlockExpr>(TLocation{},
            std::vector<TExprPtr>{
                NQumir::MakeIntrusive<TReturnExpr>(TLocation{}, std::move(call))}),
        resultType);
    function->Type = NQdb::TestTypes().MakeFunction(
        std::move(wrapperParamTypes), resultType);
    stmts.push_back(std::move(function));

    TLLVMRunnerOptions options;
    options.CoreInput = true;
    options.NativeCode = true;
    options.AllowOverloads = true;
    NQdb::NTest::ConfigureQumirDbSourceModule(options);
    auto runner = std::make_unique<TLLVMRunner>(options, nullptr, &NQdb::TestTypes());
    auto program = NQumir::MakeIntrusive<TBlockExpr>(TLocation{}, std::move(stmts));
    NQdb::NTest::AddQumirDbUse(program);
    std::string error;
    entry = runner->CompileKernelAst(program, entryName, &error);
    EXPECT_NE(entry, nullptr) << error;
    return runner;
}

std::unique_ptr<NQumir::TLLVMRunner> CompileCloneEntry(
    const NQdb::NKernel::TAggregateKeyDescriptor& key,
    void*& entry)
{
    using namespace NQumir;
    using namespace NQumir::NAst;

    std::vector<TExprPtr> stmts;
    if (!key.LookupTypeName.empty()) {
        stmts.push_back(NQumir::MakeIntrusive<TTypeDeclStmt>(
            TLocation{}, key.LookupType));
    }
    if (!key.StoredTypeName.empty()) {
        stmts.push_back(NQumir::MakeIntrusive<TTypeDeclStmt>(
            TLocation{}, key.StoredType));
    } else if (TMaybeType<TNamedType>(key.StoredType)) {
        stmts.push_back(NQumir::MakeIntrusive<TTypeDeclStmt>(
            TLocation{}, key.StoredType));
    }
    auto stringOps = NQdb::NKernel::ParseFunctionLibrary(NQdb::TestTypes(),
        NQdb::NKernel::ReadAggregationKernel("string_ops.oz"));
    if (!stringOps) {
        ADD_FAILURE() << stringOps.error().ToString();
        return {};
    }
    stmts.insert(stmts.end(), stringOps->begin(), stringOps->end());
    auto ownership = NQdb::NKernel::GenKeyOwnershipFunDecls(NQdb::TestTypes(), key);
    stmts.insert(stmts.end(), ownership.begin(), ownership.end());

    auto u8 = NQdb::TestTypes().MakeInteger(TIntegerType::U8);
    auto i64 = NQdb::TestTypes().MakeInteger(TIntegerType::I64);
    auto ptrU8 = NQdb::TestTypes().MakePointer(u8);
    auto ptrLookup = NQdb::TestTypes().MakePointer(key.LookupType);
    auto ptrStored = NQdb::TestTypes().MakePointer(key.StoredType);
    auto ident = [&](const std::string& name) -> TExprPtr {
        return NQumir::MakeIntrusive<TIdentExpr>(TLocation{}, name);
    };
    auto indexZero = [&](const std::string& name) -> TExprPtr {
        auto zero = NQumir::MakeIntrusive<TNumberExpr>(TLocation{}, int64_t{0}, NQdb::TestTypes());
        zero->Type = i64;
        return NQumir::MakeIntrusive<TIndexExpr>(TLocation{}, ident(name), zero);
    };
    auto cloneCall = NQumir::MakeIntrusive<TCallExpr>(TLocation{},
        ident("key_clone_owned"),
        std::vector<TExprPtr>{indexZero("lookup"), ident("buffer")});
    auto assignOutput = NQumir::MakeIntrusive<TArrayAssignExpr>(TLocation{}, "stored",
        std::vector<TExprPtr>{[&]() -> TExprPtr {
            auto zero = NQumir::MakeIntrusive<TNumberExpr>(TLocation{}, int64_t{0}, NQdb::TestTypes());
            zero->Type = i64;
            return zero;
        }()}, std::move(cloneCall));
    auto bytesCall = NQumir::MakeIntrusive<TCallExpr>(TLocation{},
        ident("key_owned_bytes"),
        std::vector<TExprPtr>{indexZero("lookup")});
    std::vector<TParam> params = {
        NQumir::MakeIntrusive<TVarStmt>(TLocation{}, "lookup", ptrLookup),
        NQumir::MakeIntrusive<TVarStmt>(TLocation{}, "buffer", ptrU8),
        NQumir::MakeIntrusive<TVarStmt>(TLocation{}, "stored", ptrStored),
    };
    auto function = NQumir::MakeIntrusive<TFunDecl>(TLocation{}, "clone_key",
        std::vector<TGenericParam>{},
        std::move(params), NQumir::MakeIntrusive<TBlockExpr>(TLocation{},
            std::vector<TExprPtr>{
                std::move(assignOutput),
                NQumir::MakeIntrusive<TReturnExpr>(TLocation{}, std::move(bytesCall)),
            }), i64);
    function->Type = NQdb::TestTypes().MakeFunction(
        std::vector<TTypePtr>{ptrLookup, ptrU8, ptrStored}, i64);
    stmts.push_back(std::move(function));

    TLLVMRunnerOptions options;
    options.CoreInput = true;
    options.NativeCode = true;
    options.AllowOverloads = true;
    NQdb::NTest::ConfigureQumirDbSourceModule(options);
    auto runner = std::make_unique<TLLVMRunner>(options, nullptr, &NQdb::TestTypes());
    auto program = NQumir::MakeIntrusive<TBlockExpr>(TLocation{}, std::move(stmts));
    NQdb::NTest::AddQumirDbUse(program);
    std::string error;
    entry = runner->CompileKernelAst(program, "clone_key", &error);
    EXPECT_NE(entry, nullptr) << error;
    return runner;
}

TEST(StringKeyDescriptor, BuildsScalarLookupAndStoredTypes) {
    const auto& input = *NQdb::TestTypes().MakeStruct({{"name", Nullable(NQdb::TestTypes().Make<TStringType>())}});
    auto key = NQdb::NKernel::BuildAggregateKeyDescriptor(NQdb::TestTypes(), input, {"name"});

    ASSERT_TRUE(key.IsScalar());
    EXPECT_TRUE(key.HasDistinctLookupType());
    EXPECT_EQ(NamedTypeName(key.LookupType), key.TypeName + "_Lookup");
    EXPECT_EQ(NamedTypeName(key.StoredType), key.TypeName + "_Stored");
    EXPECT_EQ(key.KeyType, key.StoredType);
    EXPECT_EQ(key.Size, 24u);
    EXPECT_EQ(key.Alignment, 8u);
    ASSERT_EQ(key.Fields.size(), 1u);
    EXPECT_TRUE(TMaybeType<TStringType>(key.Fields[0].Type));
    auto lookup = StructOf(key.LookupType);
    auto stored = StructOf(key.StoredType);
    ASSERT_NE(lookup, nullptr);
    ASSERT_NE(stored, nullptr);
    EXPECT_EQ(lookup->Fields.front().first, "valid_0");
    EXPECT_EQ(NamedTypeName(lookup->Fields.back().second), "StringView");
    EXPECT_EQ(NamedTypeName(stored->Fields.back().second), "OwnedString");
}

TEST(StringKeyDescriptor, BuildsParallelCompositeLayouts) {
    auto i32 = NQdb::TestTypes().MakeInteger(TIntegerType::I32);
    auto i64 = NQdb::TestTypes().MakeInteger(TIntegerType::I64);
    auto string = NQdb::TestTypes().Make<TStringType>();
    const auto& input = *NQdb::TestTypes().MakeStruct({
        {"id", Nullable(i64)}, {"name", Nullable(string)},
        {"code", Nullable(i32)}});
    auto key = NQdb::NKernel::BuildAggregateKeyDescriptor(NQdb::TestTypes(),
        input, {"id", "name", "code"});

    ASSERT_FALSE(key.IsScalar());
    EXPECT_TRUE(key.HasDistinctLookupType());
    EXPECT_EQ(NamedTypeName(key.LookupType), key.TypeName + "_Lookup");
    EXPECT_EQ(NamedTypeName(key.StoredType), key.TypeName + "_Stored");
    EXPECT_EQ(key.Size, 48u);
    EXPECT_EQ(key.Alignment, 8u);
    EXPECT_EQ(key.Fields[0].Offset, 8u);
    EXPECT_EQ(key.Fields[1].Offset, 24u);
    EXPECT_EQ(key.Fields[2].Offset, 44u);

    auto lookup = StructOf(key.LookupType);
    auto stored = StructOf(key.StoredType);
    ASSERT_NE(lookup, nullptr);
    ASSERT_NE(stored, nullptr);
    ASSERT_EQ(lookup->Fields.size(), stored->Fields.size());
    for (size_t i = 0; i < lookup->Fields.size(); ++i) {
        EXPECT_EQ(lookup->Fields[i].first, stored->Fields[i].first);
    }
    auto lookupName = std::find_if(lookup->Fields.begin(), lookup->Fields.end(),
        [](const auto& field) { return field.first == "key_1"; });
    auto storedName = std::find_if(stored->Fields.begin(), stored->Fields.end(),
        [](const auto& field) { return field.first == "key_1"; });
    ASSERT_NE(lookupName, lookup->Fields.end());
    ASSERT_NE(storedName, stored->Fields.end());
    EXPECT_EQ(NamedTypeName(lookupName->second), "StringView");
    EXPECT_EQ(NamedTypeName(storedName->second), "OwnedString");
}

TEST(StringKeyDescriptor, RewritesNestedStringLeaves) {
    auto i64 = NQdb::TestTypes().MakeInteger(TIntegerType::I64);
    auto nested = NQdb::TestTypes().MakeStruct(
        std::vector<std::pair<std::string, TTypePtr>>{
            {"prefix", i64}, {"text", NQdb::TestTypes().Make<TStringType>()}});
    const auto& input = *NQdb::TestTypes().MakeStruct({{"nested", Nullable(nested)}});
    auto key = NQdb::NKernel::BuildAggregateKeyDescriptor(NQdb::TestTypes(), input, {"nested"});

    ASSERT_TRUE(key.IsScalar());
    EXPECT_TRUE(key.HasDistinctLookupType());
    EXPECT_EQ(NamedTypeName(key.LookupType), key.TypeName + "_Lookup");
    EXPECT_EQ(NamedTypeName(key.StoredType), key.TypeName + "_Stored");
    auto lookup = StructOf(key.LookupType);
    auto stored = StructOf(key.StoredType);
    ASSERT_NE(lookup, nullptr);
    ASSERT_NE(stored, nullptr);
    EXPECT_EQ(lookup->Fields.front().first, "valid_0");
    EXPECT_EQ(lookup->Fields.back().first, "key_0");
    EXPECT_EQ(key.TypeName, "AggKey_nStruct2_i64_string");
    EXPECT_EQ(key.Size, 32u);
    EXPECT_EQ(key.Alignment, 8u);
}

TEST(StringKeyDescriptor, NestedCompositeTypeNamesKeepComposition) {
    auto i32 = NQdb::TestTypes().MakeInteger(TIntegerType::I32);
    auto pair = NQdb::TestTypes().MakeStruct(
        std::vector<std::pair<std::string, TTypePtr>>{
            {"a", i32}, {"b", i32}});
    auto wrappedPair = NQdb::TestTypes().MakeStruct(
        std::vector<std::pair<std::string, TTypePtr>>{
            {"pair", pair}});
    auto wrappedScalarAndScalar = NQdb::TestTypes().MakeStruct(
        std::vector<std::pair<std::string, TTypePtr>>{
            {"first", NQdb::TestTypes().MakeStruct(
                std::vector<std::pair<std::string, TTypePtr>>{{"a", i32}})},
            {"second", i32}});

    const auto& leftInput = *NQdb::TestTypes().MakeStruct({{"k", wrappedPair}});
    const auto& rightInput = *NQdb::TestTypes().MakeStruct({{"k", wrappedScalarAndScalar}});
    auto left = NQdb::NKernel::BuildAggregateKeyDescriptor(NQdb::TestTypes(), leftInput, {"k"});
    auto right = NQdb::NKernel::BuildAggregateKeyDescriptor(NQdb::TestTypes(), rightInput, {"k"});

    EXPECT_EQ(left.TypeName, "AggKey_Struct1_Struct2_i32_i32");
    EXPECT_EQ(right.TypeName, "AggKey_Struct2_Struct1_i32_i32");
    EXPECT_NE(left.TypeName, right.TypeName);
}

TEST(StringKeyDescriptor, KeepsFixedWidthRepresentationShared) {
    auto i64 = NQdb::TestTypes().MakeInteger(TIntegerType::I64);
    const auto& input = *NQdb::TestTypes().MakeStruct({{"first", i64}, {"second", i64}});
    auto key = NQdb::NKernel::BuildAggregateKeyDescriptor(NQdb::TestTypes(),
        input, {"first", "second"});

    EXPECT_FALSE(key.HasDistinctLookupType());
    EXPECT_EQ(key.LookupType, key.StoredType);
    EXPECT_EQ(key.KeyType, key.StoredType);
    EXPECT_EQ(key.Size, 16u);
    auto stored = StructOf(key.StoredType);
    ASSERT_NE(stored, nullptr);
    EXPECT_EQ(stored->Fields.size(), 2u);
    EXPECT_EQ(stored->Fields[0].first, "key_0");
    EXPECT_EQ(stored->Fields[1].first, "key_1");
    EXPECT_FALSE(key.Fields[0].IsNullable);
    EXPECT_FALSE(key.Fields[1].IsNullable);
}

TEST(StringKeyDescriptor, AddsValidityOnlyForNullableFields) {
    auto i64 = NQdb::TestTypes().MakeInteger(TIntegerType::I64);
    const auto& input = *NQdb::TestTypes().MakeStruct({{"plain", i64}, {"optional", Nullable(i64)}});
    auto key = NQdb::NKernel::BuildAggregateKeyDescriptor(NQdb::TestTypes(),
        input, {"plain", "optional"});

    auto stored = StructOf(key.StoredType);
    ASSERT_NE(stored, nullptr);
    EXPECT_EQ(key.TypeName, "AggKey_i64_ni64");
    EXPECT_EQ(key.Size, 24u);
    EXPECT_FALSE(key.Fields[0].IsNullable);
    EXPECT_TRUE(key.Fields[1].IsNullable);
    EXPECT_EQ(stored->Fields[0].first, "key_0");
    EXPECT_EQ(stored->Fields[1].first, "valid_1");
    EXPECT_EQ(stored->Fields.back().first, "key_1");
    for (const auto& field : key.Fields) {
        EXPECT_FALSE(NQdb::IsNullableType(field.Type));
    }
}

TEST(StringKeyDescriptor, CompilesScalarCrossRepresentationOperations) {
    const auto& input = *NQdb::TestTypes().MakeStruct({{"name", Nullable(NQdb::TestTypes().Make<TStringType>())}});
    auto key = NQdb::NKernel::BuildAggregateKeyDescriptor(NQdb::TestTypes(), input, {"name"});
    auto i64 = NQdb::TestTypes().MakeInteger(TIntegerType::I64);
    auto boolean = NQdb::TestTypes().Make<TBoolType>();
    std::string bytes("same\0bytes", 10);
    struct TLookupKey {
        bool Valid;
        uint8_t Padding[7];
        NQdb::TStringView Value;
    } lookup{true, {}, {reinterpret_cast<uint8_t*>(bytes.data()),
        static_cast<int64_t>(bytes.size())}};
    struct TStoredKey {
        bool Valid;
        uint8_t Padding[7];
        NQdb::TOwnedString Value;
    } stored{true, {}, {lookup.Value.Data, lookup.Value.Size}};

    void* lookupHashEntry = nullptr;
    auto lookupHashRunner = CompileKeyOperation(key, "hash_lookup", "rh_hash",
        {key.LookupType}, i64, lookupHashEntry, true);
    ASSERT_NE(lookupHashEntry, nullptr);
    void* storedHashEntry = nullptr;
    auto storedHashRunner = CompileKeyOperation(key, "hash_stored", "rh_hash",
        {key.StoredType}, i64, storedHashEntry, true);
    ASSERT_NE(storedHashEntry, nullptr);
    void* equalEntry = nullptr;
    auto equalRunner = CompileKeyOperation(key, "equal_stored_lookup",
        "rh_key_equal", {key.StoredType, key.LookupType}, boolean, equalEntry,
        true);
    ASSERT_NE(equalEntry, nullptr);

    auto lookupHash = reinterpret_cast<int64_t(*)(TLookupKey*)>(
        lookupHashEntry);
    auto storedHash = reinterpret_cast<int64_t(*)(TStoredKey*)>(
        storedHashEntry);
    auto equal = reinterpret_cast<bool(*)(TStoredKey*, TLookupKey*)>(equalEntry);
    EXPECT_EQ(lookupHash(&lookup), storedHash(&stored));
    EXPECT_TRUE(equal(&stored, &lookup));

    std::string different = "different";
    auto differentLookup = TLookupKey{true, {}, {
        reinterpret_cast<uint8_t*>(different.data()),
        static_cast<int64_t>(different.size())}};
    EXPECT_FALSE(equal(&stored, &differentLookup));
    differentLookup.Valid = false;
    stored.Valid = false;
    EXPECT_EQ(lookupHash(&differentLookup), storedHash(&stored));
    EXPECT_TRUE(equal(&stored, &differentLookup));
}

struct TLookupPair {
    bool IdValid;
    uint8_t IdPadding[7];
    int64_t Id;
    bool NameValid;
    uint8_t NamePadding[7];
    NQdb::TStringView Name;
};

struct TStoredPair {
    bool IdValid;
    uint8_t IdPadding[7];
    int64_t Id;
    bool NameValid;
    uint8_t NamePadding[7];
    NQdb::TOwnedString Name;
};

static_assert(sizeof(TLookupPair) == 40);
static_assert(sizeof(TStoredPair) == 40);

TEST(StringKeyDescriptor, CompilesCompositeCrossRepresentationOperations) {
    auto i64 = NQdb::TestTypes().MakeInteger(TIntegerType::I64);
    const auto& input = *NQdb::TestTypes().MakeStruct({
        {"id", Nullable(i64)},
        {"name", Nullable(NQdb::TestTypes().Make<TStringType>())}});
    auto key = NQdb::NKernel::BuildAggregateKeyDescriptor(NQdb::TestTypes(), input, {"id", "name"});
    auto boolean = NQdb::TestTypes().Make<TBoolType>();

    void* equalEntry = nullptr;
    auto runner = CompileKeyOperation(key, "equal_pair", "rh_key_equal",
        {key.StoredType, key.LookupType}, boolean, equalEntry, true);
    ASSERT_NE(equalEntry, nullptr);
    auto equal = reinterpret_cast<bool(*)(TStoredPair*, TLookupPair*)>(equalEntry);

    std::string bytes = "group";
    TLookupPair lookup{
        .IdValid = true,
        .Id = 7,
        .NameValid = true,
        .Name = {
            .Data = reinterpret_cast<uint8_t*>(bytes.data()),
            .Size = static_cast<int64_t>(bytes.size()),
        },
    };
    TStoredPair stored{
        .IdValid = true,
        .Id = lookup.Id,
        .NameValid = true,
        .Name = {.Data = lookup.Name.Data, .Size = lookup.Name.Size},
    };
    EXPECT_TRUE(equal(&stored, &lookup));
    lookup.Id = 8;
    EXPECT_FALSE(equal(&stored, &lookup));
}

TEST(StringKeyDescriptor, FixedWidthOwnershipCloneIsIdentity) {
    auto i64 = NQdb::TestTypes().MakeInteger(TIntegerType::I64);
    const auto& input = *NQdb::TestTypes().MakeStruct({{"id", Nullable(i64)}});
    auto key = NQdb::NKernel::BuildAggregateKeyDescriptor(NQdb::TestTypes(), input, {"id"});
    void* entry = nullptr;
    auto runner = CompileCloneEntry(key, entry);
    ASSERT_NE(entry, nullptr);
    struct TKey { bool Valid; uint8_t Padding[7]; int64_t Value; };
    auto clone = reinterpret_cast<int64_t(*)(TKey*, uint8_t*, TKey*)>(entry);

    TKey lookup{true, {}, 42};
    TKey stored{};
    EXPECT_EQ(clone(&lookup, nullptr, &stored), 0);
    EXPECT_EQ(stored.Valid, lookup.Valid);
    EXPECT_EQ(stored.Value, lookup.Value);
}

TEST(StringKeyDescriptor, StringOwnershipCloneCopiesBytes) {
    const auto& input = *NQdb::TestTypes().MakeStruct({{"name", Nullable(NQdb::TestTypes().Make<TStringType>())}});
    auto key = NQdb::NKernel::BuildAggregateKeyDescriptor(NQdb::TestTypes(), input, {"name"});
    void* entry = nullptr;
    auto runner = CompileCloneEntry(key, entry);
    ASSERT_NE(entry, nullptr);
    struct TLookupKey {
        bool Valid; uint8_t Padding[7]; NQdb::TStringView Value;
    };
    struct TStoredKey {
        bool Valid; uint8_t Padding[7]; NQdb::TOwnedString Value;
    };
    auto clone = reinterpret_cast<int64_t(*)(
        TLookupKey*, uint8_t*, TStoredKey*)>(entry);

    std::string source("copy\0me", 7);
    TLookupKey lookup{true, {}, {
        reinterpret_cast<uint8_t*>(source.data()),
        static_cast<int64_t>(source.size())}};
    std::array<uint8_t, 7> buffer{};
    TStoredKey stored{};
    EXPECT_EQ(clone(&lookup, buffer.data(), &stored), 7);
    EXPECT_TRUE(stored.Valid);
    EXPECT_EQ(stored.Value.Data, buffer.data());
    EXPECT_EQ(stored.Value.Size, 7);
    EXPECT_EQ(std::memcmp(stored.Value.Data, source.data(), source.size()), 0);
}

struct TLookupStrings {
    bool IdValid;
    uint8_t IdPadding[7];
    int64_t Id;
    bool FirstValid;
    uint8_t FirstPadding[7];
    NQdb::TStringView First;
    bool SecondValid;
    uint8_t SecondPadding[7];
    NQdb::TStringView Second;
};

struct TStoredStrings {
    bool IdValid;
    uint8_t IdPadding[7];
    int64_t Id;
    bool FirstValid;
    uint8_t FirstPadding[7];
    NQdb::TOwnedString First;
    bool SecondValid;
    uint8_t SecondPadding[7];
    NQdb::TOwnedString Second;
};

static_assert(sizeof(TLookupStrings) == 64);
static_assert(sizeof(TStoredStrings) == 64);

TEST(StringKeyDescriptor, CompositeOwnershipCloneUsesOneBlock) {
    auto i64 = NQdb::TestTypes().MakeInteger(TIntegerType::I64);
    auto string = NQdb::TestTypes().Make<TStringType>();
    const auto& input = *NQdb::TestTypes().MakeStruct({
        {"id", Nullable(i64)}, {"first", Nullable(string)},
        {"second", Nullable(string)}});
    auto key = NQdb::NKernel::BuildAggregateKeyDescriptor(NQdb::TestTypes(),
        input, {"id", "first", "second"});
    void* entry = nullptr;
    auto runner = CompileCloneEntry(key, entry);
    ASSERT_NE(entry, nullptr);
    auto clone = reinterpret_cast<int64_t(*)(
        TLookupStrings*, uint8_t*, TStoredStrings*)>(entry);

    std::string first = "left";
    std::string second("r\0ght", 5);
    TLookupStrings lookup{
        .IdValid = true,
        .Id = 9,
        .FirstValid = true,
        .First = {
            .Data = reinterpret_cast<uint8_t*>(first.data()),
            .Size = static_cast<int64_t>(first.size()),
        },
        .SecondValid = true,
        .Second = {
            .Data = reinterpret_cast<uint8_t*>(second.data()),
            .Size = static_cast<int64_t>(second.size()),
        },
    };
    std::array<uint8_t, 9> buffer{};
    TStoredStrings stored{};
    EXPECT_EQ(clone(&lookup, buffer.data(), &stored), 9);
    EXPECT_EQ(stored.Id, lookup.Id);
    EXPECT_EQ(stored.First.Data, buffer.data());
    EXPECT_EQ(stored.Second.Data, buffer.data() + first.size());
    EXPECT_EQ(std::memcmp(stored.First.Data, first.data(), first.size()), 0);
    EXPECT_EQ(std::memcmp(stored.Second.Data, second.data(), second.size()), 0);
}

} // namespace

int main(int argc, char** argv) {
    NQumir::NRegistry::EnsureQumirDbRuntimeSymbolsLinked();
    testing::InitGoogleTest(&argc, argv);
    NQumir::NCodeGen::TLLVMInitializer initializer;
    return RUN_ALL_TESTS();
}
