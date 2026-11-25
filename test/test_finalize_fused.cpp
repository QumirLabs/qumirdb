#include <gtest/gtest.h>

#include <qdb/kernel/finalize_fused.h>
#include <qdb/kernel/join_gen.h>
#include <qdb/kernel/join_key.h>

#include <qumir/parser/ast.h>
#include <qumir/parser/type.h>

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "test_types.h"

namespace {

using namespace NQumir;
using namespace NQumir::NAst;
using NQdb::TestTypes;

TTypePtr I64() { return TestTypes().MakeInteger(TIntegerType::I64); }

TTypePtr F64() { return TestTypes().MakeFloat(); }

TTypePtr Void() { return TestTypes().Make<TVoidType>(); }

TTypePtr Struct(std::vector<std::pair<std::string, TTypePtr>> fields) {
  return TestTypes().MakeStruct(std::move(fields));
}

TTypePtr Named(std::string name, TTypePtr underlying = {}) {
  return TestTypes().MakeNamed(std::move(name), std::move(underlying));
}

TExprPtr TypeDecl(std::string name, TTypePtr underlying) {
  return NQumir::MakeIntrusive<TTypeDeclStmt>(
      TLocation{}, Named(std::move(name), std::move(underlying)));
}

TExprPtr Function(std::string name, std::vector<TTypePtr> paramTypes = {}) {
  std::vector<TParam> params;
  params.reserve(paramTypes.size());
  for (size_t i = 0; i < paramTypes.size(); ++i) {
    params.push_back(NQumir::MakeIntrusive<TVarStmt>(
        TLocation{}, "arg" + std::to_string(i), paramTypes[i]));
  }
  auto ret = Void();
  auto fun = NQumir::MakeIntrusive<TFunDecl>(
      TLocation{}, std::move(name), std::vector<TGenericParam>{},
      std::move(params),
      NQumir::MakeIntrusive<TBlockExpr>(TLocation{}, std::vector<TExprPtr>{}), ret);
  fun->Type = TestTypes().MakeFunction(std::move(paramTypes), ret);
  return fun;
}

TExprPtr MarkerFunction(std::string name, int64_t marker) {
  auto ret = Void();
  auto value = NQumir::MakeIntrusive<TNumberExpr>(TLocation{}, marker, TestTypes());
  value->Type = I64();
  auto fun = NQumir::MakeIntrusive<TFunDecl>(
      TLocation{}, std::move(name), std::vector<TGenericParam>{},
      std::vector<TParam>{},
      NQumir::MakeIntrusive<TBlockExpr>(TLocation{},
                                   std::vector<TExprPtr>{std::move(value)}),
      ret);
  fun->Type = TestTypes().MakeFunction(std::vector<TTypePtr>{}, ret);
  return fun;
}

TExprPtr CallingFunction(std::string name, std::string callee) {
  auto ret = Void();
  auto call = NQumir::MakeIntrusive<TCallExpr>(
      TLocation{}, NQumir::MakeIntrusive<TIdentExpr>(TLocation{}, std::move(callee)),
      std::vector<TExprPtr>{});
  call->Type = ret;
  auto fun = NQumir::MakeIntrusive<TFunDecl>(
      TLocation{}, std::move(name), std::vector<TGenericParam>{},
      std::vector<TParam>{},
      NQumir::MakeIntrusive<TBlockExpr>(TLocation{},
                                   std::vector<TExprPtr>{std::move(call)}),
      ret);
  fun->Type = TestTypes().MakeFunction(std::vector<TTypePtr>{}, ret);
  return fun;
}

NQdb::TGeneratedKernel Kernel(std::string name, std::vector<TExprPtr> stmts,
                              std::vector<std::string> entrypoints) {
  return NQdb::TGeneratedKernel{
      .Name = std::move(name),
      .Stage = "stage",
      .Entrypoints = std::move(entrypoints),
      .Ast = NQumir::MakeIntrusive<TBlockExpr>(TLocation{}, std::move(stmts)),
  };
}

std::vector<std::string> TypeDeclNames(const TExprPtr &root) {
  auto block = TMaybeNode<TBlockExpr>(root);
  EXPECT_TRUE(block);
  std::vector<std::string> names;
  for (const auto &stmt : block.Cast()->Stmts) {
    auto typeDecl = TMaybeNode<TTypeDeclStmt>(stmt);
    if (!typeDecl) {
      continue;
    }
    auto named = TMaybeType<TNamedType>(typeDecl.Cast()->Type);
    if (!named) {
      ADD_FAILURE() << "type declaration is not a named type";
      return names;
    }
    names.push_back(named.Cast()->Name);
  }
  return names;
}

std::vector<std::string> FunctionDeclNames(const TExprPtr &root) {
  auto block = TMaybeNode<TBlockExpr>(root);
  EXPECT_TRUE(block);
  std::vector<std::string> names;
  for (const auto &stmt : block.Cast()->Stmts) {
    auto fun = TMaybeNode<TFunDecl>(stmt);
    if (fun) {
      names.push_back(fun.Cast()->Name);
    }
  }
  return names;
}

NQdb::TGeneratedKernel
JoinHashKernel(std::string name, const NQdb::NKernel::TJoinKeyDescriptor &key) {
  auto columnType = Named("TColumn");
  auto rowSetType = Named("TRowSet");
  auto stringViewType = Named("StringView");
  std::vector<TExprPtr> stmts;
  for (auto &decl : NQdb::NKernel::GenJoinKeyTypeDecls(key)) {
    stmts.push_back(std::move(decl));
  }
  for (auto &fun : NQdb::NKernel::GenJoinKeyOpsFunDecls(TestTypes(), key)) {
    stmts.push_back(std::move(fun));
  }
  stmts.push_back(NQdb::NKernel::GenJoinHashBatchAst(
      TestTypes(), key, "jt_hash_batch", columnType, rowSetType, stringViewType));
  stmts.push_back(
      NQdb::NKernel::GenJoinHashEntrypointAst(TestTypes(), key, "jt_hash_left", rowSetType));
  return Kernel(std::move(name), std::move(stmts), {"jt_hash_left"});
}

} // namespace

// Generators name their types uniquely: in one query factory a name is one type,
// which the fused program declares once.
TEST(FinalizeFused, DeclaresSharedTypesOnce) {
  auto k0 = Kernel("k0",
                   {
                       TypeDecl("SharedA", Struct({{"x", I64()}})),
                       TypeDecl("SharedB", Struct({{"a", Named("SharedA")}})),
                       Function("entry0"),
                   },
                   {"entry0"});
  auto k1 = Kernel("k1",
                   {
                       TypeDecl("SharedA", Struct({{"x", I64()}})),
                       TypeDecl("SharedB", Struct({{"a", Named("SharedA")}})),
                       Function("entry1"),
                   },
                   {"entry1"});

  std::array<NQdb::TGeneratedKernel *, 2> kernels{&k0, &k1};
  auto fused = NQdb::BuildFusedProgram(
      std::span<NQdb::TGeneratedKernel *const>(kernels.data(), kernels.size()));

  EXPECT_EQ(fused.TypeDeclCount, 2u);
  EXPECT_EQ(TypeDeclNames(fused.Program),
            (std::vector<std::string>{"SharedA", "SharedB"}));
}

TEST(FinalizeFused, RejectsTwoTypesOfOneName) {
  auto k0 = Kernel("k0", {TypeDecl("ConflictA", Struct({{"x", I64()}})), Function("entry0")},
                   {"entry0"});
  auto k1 = Kernel("k1", {TypeDecl("ConflictA", Struct({{"x", F64()}})), Function("entry1")},
                   {"entry1"});

  std::array<NQdb::TGeneratedKernel *, 2> kernels{&k0, &k1};
  EXPECT_THROW(
      NQdb::BuildFusedProgram(
          std::span<NQdb::TGeneratedKernel *const>(kernels.data(), kernels.size())),
      std::runtime_error);
}

TEST(FinalizeFused,
     RenamesDuplicateEntrypointNamesEvenWithDifferentSignatures) {
  auto k0 = Kernel("k0", {Function("filter", {I64()})}, {"filter"});
  auto k1 = Kernel("k1", {Function("filter", {F64()})}, {"filter"});

  std::array<NQdb::TGeneratedKernel *, 2> kernels{&k0, &k1};
  auto fused = NQdb::BuildFusedProgram(
      std::span<NQdb::TGeneratedKernel *const>(kernels.data(), kernels.size()));

  ASSERT_EQ(fused.UniqueEntrypoints.size(), 2u);
  EXPECT_EQ(fused.UniqueEntrypoints[0], (std::vector<std::string>{"filter"}));
  EXPECT_EQ(fused.UniqueEntrypoints[1],
            (std::vector<std::string>{"__qdb_k1_filter"}));
  EXPECT_EQ(fused.Entrypoints,
            (std::vector<std::string>{"filter", "__qdb_k1_filter"}));
  EXPECT_EQ(FunctionDeclNames(fused.Program),
            (std::vector<std::string>{"filter", "__qdb_k1_filter"}));
}

TEST(FinalizeFused, ReusesJoinHashWorkerAcrossColumnPositions) {
  auto i64 = I64();
  const auto& left0 = *TestTypes().MakeStruct({{"key", i64}, {"value", i64}});
  const auto& right0 = *TestTypes().MakeStruct({{"value", i64}, {"key", i64}});
  const auto& left1 = *TestTypes().MakeStruct({{"value", i64}, {"key", i64}});
  const auto& right1 = *TestTypes().MakeStruct({{"key", i64}, {"value", i64}});
  auto key0 =
      NQdb::NKernel::BuildJoinKeyDescriptor(TestTypes(), left0, right0, {{"key", "key"}});
  auto key1 =
      NQdb::NKernel::BuildJoinKeyDescriptor(TestTypes(), left1, right1, {{"key", "key"}});
  auto k0 = JoinHashKernel("hash0", key0);
  auto k1 = JoinHashKernel("hash1", key1);

  std::array<NQdb::TGeneratedKernel *, 2> kernels{&k0, &k1};
  auto fused = NQdb::BuildFusedProgram(
      std::span<NQdb::TGeneratedKernel *const>(kernels.data(), kernels.size()));
  const auto names = FunctionDeclNames(fused.Program);

  EXPECT_EQ(std::ranges::count(names, "jt_hash_batch"), 1);
  EXPECT_EQ(std::ranges::count(names, "__qdb_k1_jt_hash_batch"), 0);
  EXPECT_EQ(fused.UniqueEntrypoints[1],
            (std::vector<std::string>{"__qdb_k1_jt_hash_left"}));
}

TEST(FinalizeFused, KeepsTypedJoinHashWorkersAsOverloads) {
  auto i32 = TestTypes().MakeInteger(TIntegerType::I32);
  auto i64 = I64();
  const auto& left32 = *TestTypes().MakeStruct({{"key", i32}});
  const auto& right32 = *TestTypes().MakeStruct({{"key", i32}});
  const auto& left64 = *TestTypes().MakeStruct({{"key", i64}});
  const auto& right64 = *TestTypes().MakeStruct({{"key", i64}});
  auto key32 =
      NQdb::NKernel::BuildJoinKeyDescriptor(TestTypes(), left32, right32, {{"key", "key"}});
  auto key64 =
      NQdb::NKernel::BuildJoinKeyDescriptor(TestTypes(), left64, right64, {{"key", "key"}});
  auto k0 = JoinHashKernel("hash32", key32);
  auto k1 = JoinHashKernel("hash64", key64);

  std::array<NQdb::TGeneratedKernel *, 2> kernels{&k0, &k1};
  auto fused = NQdb::BuildFusedProgram(
      std::span<NQdb::TGeneratedKernel *const>(kernels.data(), kernels.size()));
  const auto names = FunctionDeclNames(fused.Program);

  EXPECT_EQ(std::ranges::count(names, "jt_hash_batch"), 2);
  EXPECT_EQ(std::ranges::count(names, "__qdb_k1_jt_hash_batch"), 0);
}

TEST(FinalizeFused, ReusesPreviouslyRenamedFunctionComponent) {
  auto k0 = Kernel("k0",
                   {
                       MarkerFunction("helper", 0),
                       CallingFunction("entry", "helper"),
                   },
                   {"entry"});
  auto k1 = Kernel("k1",
                   {
                       MarkerFunction("helper", 1),
                       CallingFunction("entry", "helper"),
                   },
                   {"entry"});
  auto k2 = Kernel("k2",
                   {
                       MarkerFunction("helper", 1),
                       CallingFunction("entry", "helper"),
                   },
                   {"entry"});

  std::array<NQdb::TGeneratedKernel *, 3> kernels{&k0, &k1, &k2};
  auto fused = NQdb::BuildFusedProgram(
      std::span<NQdb::TGeneratedKernel *const>(kernels.data(), kernels.size()));

  ASSERT_EQ(fused.UniqueEntrypoints.size(), 3u);
  EXPECT_EQ(fused.UniqueEntrypoints[1],
            (std::vector<std::string>{"__qdb_k1_entry"}));
  EXPECT_EQ(fused.UniqueEntrypoints[2], fused.UniqueEntrypoints[1]);
  EXPECT_EQ(fused.UniqueRenames[2], fused.UniqueRenames[1]);
  EXPECT_EQ(FunctionDeclNames(fused.Program),
            (std::vector<std::string>{"helper", "entry", "__qdb_k1_helper",
                                      "__qdb_k1_entry"}));
}

// Library functions are keyed by their file: equal names and signatures from two files
// stay apart, clones of one file are shared.
TEST(FinalizeFused, KeysLibraryFunctionsByFile) {
  auto libraryHelper = [](std::string origin, int64_t marker) {
    auto fun = MarkerFunction("helper", marker);
    fun->Origin = std::move(origin);
    return fun;
  };
  auto k0 = Kernel("k0",
                   {libraryHelper("join/a.oz", 0), CallingFunction("entry0", "helper")},
                   {"entry0"});
  auto k1 = Kernel("k1",
                   {libraryHelper("join/a.oz", 0), CallingFunction("entry1", "helper")},
                   {"entry1"});
  auto k2 = Kernel("k2",
                   {libraryHelper("sort/a.oz", 1), CallingFunction("entry2", "helper")},
                   {"entry2"});

  std::array<NQdb::TGeneratedKernel *, 3> kernels{&k0, &k1, &k2};
  auto fused = NQdb::BuildFusedProgram(
      std::span<NQdb::TGeneratedKernel *const>(kernels.data(), kernels.size()));

  EXPECT_EQ(FunctionDeclNames(fused.Program),
            (std::vector<std::string>{"helper", "entry0", "entry1",
                                      "__qdb_k2_helper", "__qdb_k2_entry2"}));
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
