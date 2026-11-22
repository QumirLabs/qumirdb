#include <gtest/gtest.h>

#include <qdb/modules/qumirdb.h>
#include <qdb/modules/qumirdb_types.h>
#include <qdb/io/io.h>
#include <qdb/kernel/column_value.h>
#include <qdb/plan/types/nullable.h>

#include "qumirdb_source_module.h"
#include "test_types.h"

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/parser/type.h>
#include <qumir/runner/runner_llvm.h>

#include <cstdint>
#include <algorithm>
#include <array>
#include <memory>
#include <string>

namespace {

using namespace NQumir;
using namespace NQumir::NAst;

TTypePtr FindExternalType(
    const std::vector<NQumir::NRegistry::TExternalType>& types,
    const std::string& name)
{
    for (const auto& type : types) {
        if (type.Name == name) {
            return type.Type;
        }
    }
    return nullptr;
}

struct TModuleTypes {
    TTypePtr Column = nullptr;
    TTypePtr StringView = nullptr;
};

TModuleTypes GetModuleTypes() {
    const auto types = NQumir::NRegistry::QumirDbExternalTypes(NQdb::TestTypes());
    return {
        .Column = FindExternalType(types, "TColumn"),
        .StringView = FindExternalType(types, "StringView"),
    };
}

bool ContainsField(const TExprPtr& expr, std::string_view name) {
    if (!expr) {
        return false;
    }
    if (auto field = TMaybeNode<TFieldAccessExpr>(expr)) {
        if (field.Cast()->FieldName == name) {
            return true;
        }
    }
    return std::ranges::any_of(expr->Children(), [&](const auto& child) {
        return ContainsField(child, name);
    });
}

bool ContainsField(
    const NQdb::NKernel::TColumnValueAst& value,
    std::string_view name)
{
    return ContainsField(value.Value, name) || ContainsField(value.IsValid, name) ||
        std::ranges::any_of(value.Setup, [&](const auto& expr) {
            return ContainsField(expr, name);
        });
}

std::unique_ptr<TLLVMRunner> CompileStringColumnReader(void*& entry) {
    auto types = GetModuleTypes();
    auto materialized = NQdb::NKernel::BuildColumnValueAst(NQdb::TestTypes(),
        "column", "row", "value",
        NQdb::MakeNullableType(NQdb::TestTypes(), NQdb::TestTypes().Make<TStringType>()),
        types.StringView);

    TLocation loc{};
    auto i64Type = NQdb::TestTypes().MakeInteger();
    auto boolType = NQdb::TestTypes().Make<TBoolType>();
    auto stringViewType = NQdb::TestTypes().MakeNamed(
        "StringView", types.StringView);
    auto columnType = NQdb::TestTypes().MakeNamed("TColumn", types.Column);
    auto ident = [&](const std::string& name) -> TExprPtr {
        return NQumir::MakeIntrusive<TIdentExpr>(loc, name);
    };

    std::vector<TParam> params = {
        NQumir::MakeIntrusive<TVarStmt>(loc, "column",
            NQdb::TestTypes().MakeReference(columnType)),
        NQumir::MakeIntrusive<TVarStmt>(loc, "row", i64Type),
        NQumir::MakeIntrusive<TVarStmt>(loc, "output",
            NQdb::TestTypes().MakePointer(stringViewType)),
        NQumir::MakeIntrusive<TVarStmt>(loc, "valid",
            NQdb::TestTypes().MakePointer(boolType)),
    };
    std::vector<TExprPtr> body = std::move(materialized.Setup);
    body.insert(body.end(), {
        NQumir::MakeIntrusive<TArrayAssignExpr>(loc, "output",
            std::vector<TExprPtr>{ident("row")}, materialized.Value),
        NQumir::MakeIntrusive<TArrayAssignExpr>(loc, "valid",
            std::vector<TExprPtr>{ident("row")}, materialized.IsValid),
        NQumir::MakeIntrusive<TReturnExpr>(loc,
            NQumir::MakeIntrusive<TFieldAccessExpr>(loc,
                NQumir::MakeIntrusive<TIndexExpr>(loc, ident("output"), ident("row")),
                "Size")),
    });
    auto function = NQumir::MakeIntrusive<TFunDecl>(loc, "read_string_column",
        std::vector<TGenericParam>{},
        std::move(params), NQumir::MakeIntrusive<TBlockExpr>(loc, std::move(body)),
        i64Type);
    auto program = NQumir::MakeIntrusive<TBlockExpr>(loc,
        std::vector<TExprPtr>{std::move(function)});

    TLLVMRunnerOptions options;
    options.CoreInput = true;
    options.NativeCode = true;
    NQdb::NTest::ConfigureQumirDbSourceModule(options);
    auto runner = std::make_unique<TLLVMRunner>(options, nullptr, &NQdb::TestTypes());
    std::string error;
    NQdb::NTest::AddQumirDbUse(program);
    entry = runner->CompileKernelAst(
        std::move(program), "read_string_column", &error);
    EXPECT_NE(entry, nullptr) << error;
    return runner;
}

std::unique_ptr<TLLVMRunner> CompileI32ColumnReader(void*& entry) {
    auto types = GetModuleTypes();
    auto i32Type = NQdb::TestTypes().MakeInteger(TIntegerType::I32);
    auto materialized = NQdb::NKernel::BuildColumnValueAst(NQdb::TestTypes(),
        "column", "row", "value", NQdb::MakeNullableType(NQdb::TestTypes(), i32Type),
        types.StringView);

    TLocation loc{};
    auto i64Type = NQdb::TestTypes().MakeInteger();
    auto boolType = NQdb::TestTypes().Make<TBoolType>();
    auto columnType = NQdb::TestTypes().MakeNamed("TColumn", types.Column);
    auto ident = [&](const std::string& name) -> TExprPtr {
        return NQumir::MakeIntrusive<TIdentExpr>(loc, name);
    };
    std::vector<TParam> params = {
        NQumir::MakeIntrusive<TVarStmt>(loc, "column",
            NQdb::TestTypes().MakeReference(columnType)),
        NQumir::MakeIntrusive<TVarStmt>(loc, "row", i64Type),
        NQumir::MakeIntrusive<TVarStmt>(loc, "valid",
            NQdb::TestTypes().MakePointer(boolType)),
    };
    std::vector<TExprPtr> body = std::move(materialized.Setup);
    body.insert(body.end(), {
        NQumir::MakeIntrusive<TArrayAssignExpr>(loc, "valid",
            std::vector<TExprPtr>{ident("row")}, materialized.IsValid),
        NQumir::MakeIntrusive<TReturnExpr>(loc,
            NQumir::MakeIntrusive<TCastExpr>(loc, materialized.Value, i64Type)),
    });
    auto function = NQumir::MakeIntrusive<TFunDecl>(loc, "read_i32_column",
        std::vector<TGenericParam>{},
        std::move(params), NQumir::MakeIntrusive<TBlockExpr>(loc, std::move(body)),
        i64Type);
    auto program = NQumir::MakeIntrusive<TBlockExpr>(loc,
        std::vector<TExprPtr>{std::move(function)});

    TLLVMRunnerOptions options;
    options.CoreInput = true;
    options.NativeCode = true;
    NQdb::NTest::ConfigureQumirDbSourceModule(options);
    auto runner = std::make_unique<TLLVMRunner>(options, nullptr, &NQdb::TestTypes());
    std::string error;
    NQdb::NTest::AddQumirDbUse(program);
    entry = runner->CompileKernelAst(
        std::move(program), "read_i32_column", &error);
    EXPECT_NE(entry, nullptr) << error;
    return runner;
}

std::unique_ptr<TLLVMRunner> CompileScalarColumnReader(
    void*& entry,
    TTypePtr valueType,
    const std::string& entryName)
{
    auto types = GetModuleTypes();
    auto materialized = NQdb::NKernel::BuildColumnValueAst(NQdb::TestTypes(),
        "column", "row", "value",
        NQdb::MakeNullableType(NQdb::TestTypes(), valueType), types.StringView);

    TLocation loc{};
    auto i64Type = NQdb::TestTypes().MakeInteger();
    auto boolType = NQdb::TestTypes().Make<TBoolType>();
    auto columnType = NQdb::TestTypes().MakeNamed("TColumn", types.Column);
    auto ident = [&](const std::string& name) -> TExprPtr {
        return NQumir::MakeIntrusive<TIdentExpr>(loc, name);
    };
    std::vector<TParam> params = {
        NQumir::MakeIntrusive<TVarStmt>(loc, "column",
            NQdb::TestTypes().MakeReference(columnType)),
        NQumir::MakeIntrusive<TVarStmt>(loc, "row", i64Type),
        NQumir::MakeIntrusive<TVarStmt>(loc, "valid",
            NQdb::TestTypes().MakePointer(boolType)),
    };
    std::vector<TExprPtr> body = std::move(materialized.Setup);
    body.insert(body.end(), {
        NQumir::MakeIntrusive<TArrayAssignExpr>(loc, "valid",
            std::vector<TExprPtr>{ident("row")}, materialized.IsValid),
        NQumir::MakeIntrusive<TReturnExpr>(loc, materialized.Value),
    });
    auto function = NQumir::MakeIntrusive<TFunDecl>(loc, entryName,
        std::vector<TGenericParam>{},
        std::move(params), NQumir::MakeIntrusive<TBlockExpr>(loc, std::move(body)),
        valueType);
    auto program = NQumir::MakeIntrusive<TBlockExpr>(loc,
        std::vector<TExprPtr>{std::move(function)});

    TLLVMRunnerOptions options;
    options.CoreInput = true;
    options.NativeCode = true;
    NQdb::NTest::ConfigureQumirDbSourceModule(options);
    auto runner = std::make_unique<TLLVMRunner>(options, nullptr, &NQdb::TestTypes());
    std::string error;
    NQdb::NTest::AddQumirDbUse(program);
    entry = runner->CompileKernelAst(std::move(program), entryName, &error);
    EXPECT_NE(entry, nullptr) << error;
    return runner;
}

template <typename T>
void CheckStringHandleJit(const std::string& typeName) {
    TLLVMRunnerOptions options;
    options.CoreInput = true;
    options.ResolveCoreInput = true;
    options.NativeCode = true;
    NQdb::NTest::ConfigureQumirDbSourceModule(options);

    TLLVMRunner runner(options, nullptr, &NQdb::TestTypes());

    const std::string source =
        "(block "
        "  (use qumirdb) "
        "  (fun copy_handle ((var dst <ptr " + typeName + ">) "
        "                    (var src <ptr " + typeName + ">)) -> i64 "
        "    (block "
        "      (= dst [(: 0 i64)] (index src (: 0 i64))) "
        "      (return (field (index dst (: 0 i64)) Size)))))";

    std::string error;
    void* entry = runner.CompileKernel(source, &error);
    ASSERT_NE(entry, nullptr) << error;

    uint8_t bytes[] = {'a', 0, 'b'};
    T sourceValue{.Data = bytes, .Size = 3};
    T destination{};
    auto copy = reinterpret_cast<int64_t(*)(T*, T*)>(entry);

    EXPECT_EQ(copy(&destination, &sourceValue), 3);
    EXPECT_EQ(destination.Data, bytes);
    EXPECT_EQ(destination.Size, 3);
    EXPECT_EQ(destination.Data[1], 0);
}

TEST(QumirDbStringTypes, ExternalTypesAreDistinctPodStructs) {
    const auto externalTypes = NQumir::NRegistry::QumirDbExternalTypes(NQdb::TestTypes());
    auto stringView = FindExternalType(externalTypes, "StringView");
    auto ownedString = FindExternalType(externalTypes, "OwnedString");

    ASSERT_NE(stringView, nullptr);
    ASSERT_NE(ownedString, nullptr);
    // Their layouts are one interned struct; the names keep the two types apart.
    EXPECT_NE(
        NQumir::NRegistry::QumirDbNamedType(NQdb::TestTypes(), "StringView"),
        NQumir::NRegistry::QumirDbNamedType(NQdb::TestTypes(), "OwnedString"));
    EXPECT_FALSE(TMaybeType<TStringType>(stringView));
    EXPECT_FALSE(TMaybeType<TStringType>(ownedString));

    for (const auto& type : {stringView, ownedString}) {
        auto structure = TMaybeType<TStructType>(type);
        ASSERT_TRUE(structure);
        ASSERT_EQ(structure.Cast()->Fields.size(), 2u);
        EXPECT_EQ(structure.Cast()->Fields[0].first, "Data");
        EXPECT_TRUE(TMaybeType<TPointerType>(structure.Cast()->Fields[0].second));
        EXPECT_EQ(structure.Cast()->Fields[1].first, "Size");
        EXPECT_TRUE(TMaybeType<TIntegerType>(structure.Cast()->Fields[1].second));
    }
}

TEST(QumirDbStringTypes, StringViewUsesPlainStructCopies) {
    CheckStringHandleJit<NQdb::TStringView>("StringView");
}

TEST(QumirDbStringTypes, OwnedStringUsesPlainStructCopies) {
    CheckStringHandleJit<NQdb::TOwnedString>("OwnedString");
}

TEST(QumirDbStringTypes, NonNullableMaterializersSkipValidityBitmap) {
    auto types = GetModuleTypes();
    const std::array<TTypePtr, 4> logicalTypes = {
        NQdb::TestTypes().MakeInteger(TIntegerType::I32),
        NQdb::TestTypes().MakeFloat(),
        NQdb::TestTypes().Make<TBoolType>(),
        NQdb::TestTypes().Make<TStringType>(),
    };
    for (const auto& type : logicalTypes) {
        auto value = NQdb::NKernel::BuildColumnValueAst(NQdb::TestTypes(),
            "column", "row", "value", type, types.StringView);
        EXPECT_FALSE(ContainsField(value, "Mask")) << type->ToString();
        EXPECT_FALSE(ContainsField(value, "MaskBitOffset")) << type->ToString();
        EXPECT_FALSE(NQdb::IsNullableType(value.ValueType));
        auto valid = TMaybeNode<TNumberExpr>(value.IsValid);
        ASSERT_TRUE(valid) << type->ToString();
        EXPECT_NE(valid.Cast()->IntValue, 0) << type->ToString();

        auto nullableValue = NQdb::NKernel::BuildColumnValueAst(NQdb::TestTypes(),
            "column", "row", "value",
            NQdb::MakeNullableType(NQdb::TestTypes(), type), types.StringView);
        EXPECT_TRUE(ContainsField(nullableValue, "Mask")) << type->ToString();
        EXPECT_TRUE(ContainsField(nullableValue, "MaskBitOffset"))
            << type->ToString();
        EXPECT_FALSE(NQdb::IsNullableType(nullableValue.ValueType));
    }
}

TEST(QumirDbStringTypes, MaterializesI32AndValidity) {
    void* entry = nullptr;
    auto runner = CompileI32ColumnReader(entry);
    ASSERT_NE(entry, nullptr);

    std::array<int32_t, 3> values = {17, -4, 29};
    std::array<uint8_t, 1> mask = {0b00000101};
    NQdb::TColumn column{
        .Data = reinterpret_cast<char*>(values.data()),
        .Mask = mask.data(),
    };
    std::array<bool, 3> valid{};
    auto read = reinterpret_cast<int64_t(*)(NQdb::TColumn*, int64_t, bool*)>(entry);

    EXPECT_EQ(read(&column, 0, valid.data()), 17);
    EXPECT_EQ(read(&column, 1, valid.data()), 0);
    EXPECT_EQ(read(&column, 2, valid.data()), 29);
    EXPECT_TRUE(valid[0]);
    EXPECT_FALSE(valid[1]);
    EXPECT_TRUE(valid[2]);
}

TEST(QumirDbStringTypes, MaterializesStringAndLargeStringSlices) {
    void* entry = nullptr;
    auto runner = CompileStringColumnReader(entry);
    ASSERT_NE(entry, nullptr);

    std::array<uint8_t, 1> mask = {0b00001010};
    std::array<NQdb::TStringView, 3> output{};
    std::array<bool, 3> valid{};
    auto read = reinterpret_cast<int64_t(*)(
        NQdb::TColumn*, int64_t, NQdb::TStringView*, bool*)>(entry);

    std::array<int32_t, 4> offsets32 = {10, 10, 13, 17};
    std::array<uint8_t, 7> bytes32 = {'a', 'b', 'c', 'W', 0, 'Y', 'Z'};
    NQdb::TColumn stringColumn{
        .Data = reinterpret_cast<char*>(bytes32.data()),
        .Mask = mask.data(),
        .MaskBitOffset = 1,
        .Offsets = offsets32.data(),
        .OffsetWidth = 4,
    };
    EXPECT_EQ(read(&stringColumn, 0, output.data(), valid.data()), 0);
    EXPECT_EQ(read(&stringColumn, 1, output.data(), valid.data()), 0);
    EXPECT_EQ(read(&stringColumn, 2, output.data(), valid.data()), 4);
    EXPECT_TRUE(valid[0]);
    EXPECT_FALSE(valid[1]);
    EXPECT_TRUE(valid[2]);
    EXPECT_EQ(output[1].Data, nullptr);
    EXPECT_EQ(output[2].Data, bytes32.data() + 3);
    EXPECT_EQ(output[2].Data[1], 0);

    std::array<int64_t, 3> offsets64 = {100, 102, 107};
    std::array<uint8_t, 7> bytes64 = {'o', 'k', 'h', 'e', 'l', 'l', 'o'};
    NQdb::TColumn largeStringColumn{
        .Data = reinterpret_cast<char*>(bytes64.data()),
        .Offsets = offsets64.data(),
        .OffsetWidth = 8,
    };
    EXPECT_EQ(read(&largeStringColumn, 0, output.data(), valid.data()), 2);
    EXPECT_EQ(read(&largeStringColumn, 1, output.data(), valid.data()), 5);
    EXPECT_TRUE(valid[0]);
    EXPECT_TRUE(valid[1]);
    EXPECT_EQ(output[0].Data, bytes64.data());
    EXPECT_EQ(output[1].Data, bytes64.data() + 2);
}

TEST(QumirDbStringTypes, DoesNotReadNullPayload) {
    std::array<uint8_t, 1> mask = {0};

    void* i32Entry = nullptr;
    auto i32Runner = CompileI32ColumnReader(i32Entry);
    ASSERT_NE(i32Entry, nullptr);
    NQdb::TColumn i32Column{.Data = nullptr, .Mask = mask.data()};
    bool i32Valid = true;
    auto readI32 = reinterpret_cast<int64_t(*)(
        NQdb::TColumn*, int64_t, bool*)>(i32Entry);
    EXPECT_EQ(readI32(&i32Column, 0, &i32Valid), 0);
    EXPECT_FALSE(i32Valid);

    void* stringEntry = nullptr;
    auto stringRunner = CompileStringColumnReader(stringEntry);
    ASSERT_NE(stringEntry, nullptr);
    NQdb::TColumn stringColumn{
        .Data = nullptr, .Mask = mask.data(), .Offsets = nullptr,
        .OffsetWidth = 4,
    };
    NQdb::TStringView output{.Data = reinterpret_cast<uint8_t*>(1), .Size = -1};
    bool stringValid = true;
    auto readString = reinterpret_cast<int64_t(*)(
        NQdb::TColumn*, int64_t, NQdb::TStringView*, bool*)>(stringEntry);
    EXPECT_EQ(readString(&stringColumn, 0, &output, &stringValid), 0);
    EXPECT_FALSE(stringValid);
    EXPECT_EQ(output.Data, nullptr);
    EXPECT_EQ(output.Size, 0);

    void* f64Entry = nullptr;
    auto f64Runner = CompileScalarColumnReader(
        f64Entry, NQdb::TestTypes().MakeFloat(), "read_f64_column");
    ASSERT_NE(f64Entry, nullptr);
    NQdb::TColumn f64Column{.Data = nullptr, .Mask = mask.data()};
    bool f64Valid = true;
    auto readF64 = reinterpret_cast<double(*)(
        NQdb::TColumn*, int64_t, bool*)>(f64Entry);
    EXPECT_EQ(readF64(&f64Column, 0, &f64Valid), 0.0);
    EXPECT_FALSE(f64Valid);

    void* boolEntry = nullptr;
    auto boolRunner = CompileScalarColumnReader(
        boolEntry, NQdb::TestTypes().Make<TBoolType>(), "read_bool_column");
    ASSERT_NE(boolEntry, nullptr);
    NQdb::TColumn boolColumn{.Data = nullptr, .Mask = mask.data()};
    bool boolValid = true;
    auto readBool = reinterpret_cast<bool(*)(
        NQdb::TColumn*, int64_t, bool*)>(boolEntry);
    EXPECT_FALSE(readBool(&boolColumn, 0, &boolValid));
    EXPECT_FALSE(boolValid);
}

} // namespace

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    NQumir::NCodeGen::TLLVMInitializer llvmInit;
    return RUN_ALL_TESTS();
}
