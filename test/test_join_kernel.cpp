#include <gtest/gtest.h>

#include <qdb/io/io.h>
#include <qdb/kernel/compiler.h>
#include <qdb/kernel/join_gen.h>
#include <qdb/kernel/join_key.h>
#include <qdb/kernel/lib.h>
#include <qdb/modules/qumirdb.h>
#include <qdb/modules/qumirdb_runtime.h>
#include <qdb/plan/types/decimal.h>
#include <qdb/plan/types/nullable.h>

#include "qumirdb_source_module.h"

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>
#include <qumir/runner/runner_llvm.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace NQdb;

namespace {

// Mirrors the HashTable C layout from modules/qumirdb.cpp. The join reuses it
// as each side's hash map; the dense per-slot RowId bucket lives in AggBuffers:
//   AggBuffers[0][slot] = length, [1] = capacity, [2] = data pointer (as i64).
struct THashTable {
  uint8_t *Keys = nullptr;
  uint8_t *Ctrl = nullptr;
  int64_t *SlotId = nullptr;
  uint8_t *GroupKeys = nullptr;
  int64_t **AggBuffers = nullptr;
  uint8_t **OwnedBlocks = nullptr;
  int64_t OwnedBlockCount = 0;
  int64_t OwnedBlockCapacity = 0;
  int64_t Capacity = 0;
  int64_t Size = 0;
  int64_t NumAggs = 0;
  int64_t NumKeys = 0;
  int64_t KeySize = 0;
};
static_assert(sizeof(THashTable) == TKernelCompiler::HashTableSize);

struct TPairBuffer {
  int64_t Count = 0;
  int64_t Capacity = 0;
  int64_t *Data = nullptr;
};
static_assert(sizeof(TPairBuffer) == TKernelCompiler::PairBufferSize);

constexpr int64_t JoinOpCode(EJoinKernelOp op) {
  return static_cast<int64_t>(op);
}

NQumir::NAst::TExprPtr ParseExpr(const std::string &src) {
  std::istringstream in(src);
  NQumir::NAst::NCore::TTokenStream tokens(in);
  NQumir::NAst::NCore::TParser parser;
  auto parsed = parser.Parse(tokens);
  if (!parsed) {
    throw std::runtime_error(parsed.error().ToString());
  }
  return *parsed;
}

std::unique_ptr<NQumir::TLLVMRunner>
CompileJoinEntry(const std::string &entryName, void *&entry) {
  using namespace NQumir::NAst;
  auto lib = NKernel::BuildJoinKernelLibrary();
  if (!lib) {
    ADD_FAILURE() << lib.error().ToString();
    return {};
  }
  auto i64 = std::make_shared<TIntegerType>(TIntegerType::I64);
  TStructType leftType({{"lk", i64}});
  TStructType rightType({{"rk", i64}});
  auto keyDesc =
      NKernel::BuildJoinKeyDescriptor(leftType, rightType, {{"lk", "rk"}});
  std::vector<TExprPtr> stmts;
  for (auto &f : NKernel::GenJoinKeyTypeDecls(keyDesc))
    stmts.push_back(std::move(f));
  for (auto &f : NKernel::GenJoinKeyOpsFunDecls(keyDesc))
    stmts.push_back(std::move(f));
  for (auto &f : NKernel::GenJoinKeyOwnershipFunDecls(keyDesc))
    stmts.push_back(std::move(f));
  auto dualLib = NKernel::BuildJoinDualKeyLibrary();
  if (!dualLib) {
    ADD_FAILURE() << dualLib.error().ToString();
    return {};
  }
  for (auto &f : *dualLib)
    stmts.push_back(std::move(f));
  for (auto &f : *lib)
    stmts.push_back(std::move(f));

  NQumir::TLLVMRunnerOptions options;
  options.CoreInput = true;
  options.NativeCode = true;
  options.AllowOverloads = true;
  NQdb::NTest::ConfigureQumirDbSourceModule(options);
  auto runner = std::make_unique<NQumir::TLLVMRunner>(options);
  auto program = std::make_shared<NQumir::NAst::TBlockExpr>(NQumir::TLocation{},
                                                            std::move(stmts));
  NQdb::NTest::AddQumirDbUse(program);
  std::string error;
  entry = runner->CompileKernelAst(program, entryName, &error);
  EXPECT_NE(entry, nullptr) << error;
  return runner;
}

} // namespace

TEST(JoinKernel, PairBufferLayout) {
  std::shared_ptr<NQumir::NAst::TStructType> pairBuffer;
  bool hasJoinTable = false;
  for (const auto &type : NQumir::NRegistry::QumirDbExternalTypes()) {
    if (type.Name == "PairBuffer") {
      pairBuffer =
          NQumir::NAst::TMaybeType<NQumir::NAst::TStructType>(type.Type).Cast();
    } else if (type.Name == "JoinTable") {
      hasJoinTable = true;
    }
  }
  ASSERT_NE(pairBuffer, nullptr);
  ASSERT_EQ(pairBuffer->Fields.size(), 3u);
  EXPECT_EQ(pairBuffer->Fields[2].first, "Data");
  EXPECT_FALSE(hasJoinTable); // join reuses HashTable, no separate type
}

TEST(JoinKernel, MaterializeLibraryMarksPhysicalWorkersCacheable) {
  auto library = NKernel::BuildJoinKernelLibrary();
  ASSERT_TRUE(library) << library.error().ToString();

  std::unordered_map<std::string, std::shared_ptr<NQumir::NAst::TFunDecl>>
      functions;
  for (const auto &stmt : *library) {
    auto fun = NQumir::NAst::TMaybeNode<NQumir::NAst::TFunDecl>(stmt);
    if (fun) {
      functions.emplace(fun.Cast()->Name, fun.Cast());
    }
  }

  for (const auto *name : {
           "jm_begin",
           "jm_record_owner",
           "jt_materialize_bool_column",
           "jt_materialize_binint_column",
           "jt_materialize_string_column",
       }) {
    ASSERT_TRUE(functions.contains(name)) << name;
    EXPECT_TRUE(functions.at(name)->Cacheable) << name;
  }
  ASSERT_TRUE(functions.contains("jt_materialize_fixed_column"));
  EXPECT_FALSE(
      functions.at("jt_materialize_fixed_column")->GenericParams.empty());
}

TEST(JoinKernel, InitReusesHashTableAndDestroyClears) {
  void *initEntry = nullptr;
  auto initRunner = CompileJoinEntry("jt_init", initEntry);
  ASSERT_NE(initEntry, nullptr);
  void *destroyEntry = nullptr;
  auto destroyRunner = CompileJoinEntry("jt_destroy", destroyEntry);
  ASSERT_NE(destroyEntry, nullptr);
  auto jtInit = reinterpret_cast<bool (*)(void *, int64_t, int64_t)>(initEntry);
  auto jtDestroy = reinterpret_cast<void (*)(void *)>(destroyEntry);

  void *widthEntry = nullptr;
  auto widthRunner = CompileJoinEntry("swiss_group_width", widthEntry);
  ASSERT_NE(widthEntry, nullptr);
  const auto width = reinterpret_cast<int64_t (*)()>(widthEntry)();
  THashTable ht{};
  ASSERT_TRUE(jtInit(&ht, 8, 8));
  EXPECT_EQ(ht.Capacity, width);
  EXPECT_EQ(ht.Size, 0);
  EXPECT_EQ(ht.NumAggs, 3); // three dense bucket columns
  EXPECT_EQ(ht.KeySize, 8);
  ASSERT_NE(ht.AggBuffers, nullptr);
  for (int i = 0; i < ht.Capacity; ++i) {
    EXPECT_EQ(ht.Ctrl[i], 0x80); // empty control byte
    EXPECT_EQ(ht.AggBuffers[0][i], 0); // length
    EXPECT_EQ(ht.AggBuffers[1][i], 0); // capacity
    EXPECT_EQ(ht.AggBuffers[2][i], 0); // data pointer (null)
  }

  jtDestroy(&ht);
  EXPECT_EQ(ht.Keys, nullptr);
  EXPECT_EQ(ht.AggBuffers, nullptr);
  EXPECT_EQ(ht.Capacity, 0);
}

TEST(JoinKernel, BucketAppendGrowsAndStoresRowIds) {
  void *initEntry = nullptr;
  auto initRunner = CompileJoinEntry("jt_init", initEntry);
  void *destroyEntry = nullptr;
  auto destroyRunner = CompileJoinEntry("jt_destroy", destroyEntry);
  void *appendEntry = nullptr;
  auto appendRunner = CompileJoinEntry("jb_append", appendEntry);
  ASSERT_NE(appendEntry, nullptr);
  auto jtInit = reinterpret_cast<bool (*)(void *, int64_t, int64_t)>(initEntry);
  auto jtDestroy = reinterpret_cast<void (*)(void *)>(destroyEntry);
  auto jbAppend =
      reinterpret_cast<bool (*)(void *, int64_t, int64_t)>(appendEntry);

  THashTable ht{};
  ASSERT_TRUE(jtInit(&ht, 8, 8));
  const int64_t slot = 3;
  for (int64_t i = 0; i < 10; ++i) {
    ASSERT_TRUE(jbAppend(&ht, slot, 1000 + i));
  }
  EXPECT_EQ(ht.AggBuffers[0][slot], 10);
  EXPECT_EQ(ht.AggBuffers[1][slot], 13); // 4 -> 6 -> 9 -> 13 (1.5x growth)
  auto *bucket = reinterpret_cast<int64_t *>(ht.AggBuffers[2][slot]);
  ASSERT_NE(bucket, nullptr);
  for (int64_t i = 0; i < 10; ++i) {
    EXPECT_EQ(bucket[i], 1000 + i);
  }
  EXPECT_EQ(ht.AggBuffers[0][0], 0); // untouched slot
  jtDestroy(&ht);
}

TEST(JoinKernel, PairBufferPushGrowsAndStoresPairs) {
  void *pushEntry = nullptr;
  auto pushRunner = CompileJoinEntry("pb_push", pushEntry);
  void *destroyEntry = nullptr;
  auto destroyRunner = CompileJoinEntry("pb_destroy", destroyEntry);
  auto pbPush = reinterpret_cast<bool (*)(void *, int64_t, int64_t)>(pushEntry);
  auto pbDestroy = reinterpret_cast<void (*)(void *)>(destroyEntry);

  TPairBuffer buf{};
  for (int64_t i = 0; i < 10; ++i) {
    ASSERT_TRUE(pbPush(&buf, 100 + i, 200 + i));
  }
  EXPECT_EQ(buf.Count, 10);
  ASSERT_NE(buf.Data, nullptr);
  for (int64_t i = 0; i < 10; ++i) {
    EXPECT_EQ(buf.Data[2 * i], 100 + i);
    EXPECT_EQ(buf.Data[2 * i + 1], 200 + i);
  }
  pbDestroy(&buf);
  EXPECT_EQ(buf.Count, 0);
  EXPECT_EQ(buf.Data, nullptr);
}

TEST(CompileJoin, ProducesWorkingInnerJoinKernels) {
  using namespace NQumir::NAst;
  auto i64 = [] { return std::make_shared<TIntegerType>(TIntegerType::I64); };
  TStructType leftType({{"lv", i64()}, {"lk", i64()}});
  TStructType rightType({{"rv", i64()}, {"rk", i64()}});

  TKernelCompiler compiler;
  auto spec = NKernel::BuildJoinKernelSpec(leftType, rightType, {{"lk", "rk"}},
                                           EJoinType::Inner);
  auto kernels = compiler.CompileJoin(spec);

  std::vector<int64_t> lkeys = {1, 2, 1};
  std::vector<int64_t> rkeys = {1, 1, 3};
  std::vector<int64_t> lvalues = {10, 20, 30};
  std::vector<int64_t> rvalues = {100, 200, 300};
  auto makeBatch = [](std::vector<int64_t> &keys, std::vector<int64_t> &values,
                      std::vector<TColumn> &cols) {
    cols = {TColumn{.Data = reinterpret_cast<char *>(values.data())},
            TColumn{.Data = reinterpret_cast<char *>(keys.data())}};
    return TRowSet{.Columns = cols.data(),
                   .ColumnCount = 2,
                   .RowCount = static_cast<int64_t>(keys.size()),
                   .RefCount = 1};
  };
  std::vector<TColumn> lcols, rcols;
  TRowSet lbatch = makeBatch(lkeys, lvalues, lcols);
  TRowSet rbatch = makeBatch(rkeys, rvalues, rcols);

  THashTable left{}, right{};
  TPairBuffer pairs{};
  ASSERT_TRUE(kernels.Dispatch(&left, &right, nullptr, 0, &pairs, nullptr,
                               nullptr, 8, JoinOpCode(EJoinKernelOp::Init)));
  // left_store / right_store are single-batch arrays (batch_idx 0). The default
  // (always-true) jt_residual_filter ignores them.
  ASSERT_TRUE(kernels.Dispatch(&left, &right, &lbatch, 0, &pairs, &lbatch,
                               &rbatch, 0,
                               JoinOpCode(EJoinKernelOp::UpdateLeft)));
  ASSERT_TRUE(kernels.Dispatch(&left, &right, &rbatch, 0, &pairs, &lbatch,
                               &rbatch, 0,
                               JoinOpCode(EJoinKernelOp::UpdateRight)));

  std::vector<std::tuple<int64_t, int64_t>> got;
  for (int64_t i = 0; i < pairs.Count; ++i) {
    got.emplace_back(pairs.Data[2 * i] & 0xffffffff,
                     pairs.Data[2 * i + 1] & 0xffffffff);
  }
  std::vector<std::tuple<int64_t, int64_t>> expected = {
      {0, 0}, {0, 1}, {2, 0}, {2, 1}};
  std::sort(got.begin(), got.end());
  EXPECT_EQ(got, expected);

  kernels.Dispatch(&left, &right, nullptr, 0, &pairs, nullptr, nullptr, 0,
                   JoinOpCode(EJoinKernelOp::Destroy));
}

TEST(CompileJoin, NormalizesNullableResidualIfBranch) {
  using namespace NQumir::NAst;
  auto i64 = [] { return std::make_shared<TIntegerType>(TIntegerType::I64); };
  auto f64 = [] { return std::make_shared<TFloatType>(); };
  TStructType leftType({{"lk", i64()}, {"lv", f64()}});
  TStructType rightType(
      {{"rk", i64()}, {"rv", std::make_shared<TNullable>(f64())}});

  auto spec = NKernel::BuildJoinKernelSpec(
      leftType, rightType, {{"lk", "rk"}}, EJoinType::Inner,
      ParseExpr("(> (if (< lv (: 10.0 f64)) lv rv) (: 0.0 f64))"));

  TKernelCompiler compiler;
  auto kernels = compiler.CompileJoin(spec);
  EXPECT_TRUE(static_cast<bool>(kernels.Dispatch));
}

TEST(CompileJoinHash, ProducesMatchingSideHashes) {
  using namespace NQumir::NAst;
  auto i64 = [] { return std::make_shared<TIntegerType>(TIntegerType::I64); };
  TStructType leftType({{"lk", i64()}, {"lv", i64()}});
  TStructType rightType({{"rv", i64()}, {"rk", i64()}});

  TKernelCompiler compiler;
  auto spec = NKernel::BuildJoinKernelSpec(leftType, rightType, {{"lk", "rk"}},
                                           EJoinType::Inner);
  auto kernels = compiler.CompileJoinHash(spec);

  std::vector<int64_t> lkeys = {1, 2, 1, 4};
  std::vector<int64_t> lvalues = {10, 20, 30, 40};
  std::vector<int64_t> rvalues = {100, 200, 300, 400};
  std::vector<int64_t> rkeys = {1, 3, 2, 1};
  std::vector<TColumn> lcols = {
      TColumn{.Data = reinterpret_cast<char *>(lkeys.data())},
      TColumn{.Data = reinterpret_cast<char *>(lvalues.data())},
  };
  std::vector<TColumn> rcols = {
      TColumn{.Data = reinterpret_cast<char *>(rvalues.data())},
      TColumn{.Data = reinterpret_cast<char *>(rkeys.data())},
  };
  TRowSet lbatch{
      .Columns = lcols.data(),
      .ColumnCount = 2,
      .RowCount = static_cast<int64_t>(lkeys.size()),
      .RefCount = 1,
  };
  TRowSet rbatch{
      .Columns = rcols.data(),
      .ColumnCount = 2,
      .RowCount = static_cast<int64_t>(rkeys.size()),
      .RefCount = 1,
  };

  std::vector<uint64_t> leftHashes(lkeys.size(), 0);
  std::vector<uint64_t> rightHashes(rkeys.size(), 0);
  ASSERT_TRUE(kernels.Left(&lbatch, leftHashes.data()));
  ASSERT_TRUE(kernels.Right(&rbatch, rightHashes.data()));

  EXPECT_EQ(leftHashes[0], rightHashes[0]);
  EXPECT_EQ(leftHashes[1], rightHashes[2]);
  EXPECT_EQ(leftHashes[2], rightHashes[3]);
  EXPECT_EQ(leftHashes[0], leftHashes[2]);
  EXPECT_NE(leftHashes[0], leftHashes[1]);
}

namespace {

// Join kernels for lk = rk over one i64 key column a side. A row outside the
// selection has no hash, so every hash buffer starts as NoHash_ everywhere.
class JoinKeyHash : public testing::Test {
protected:
  struct TSide {
    std::vector<int64_t> Keys;
    std::vector<uint8_t> Selection; // empty = no selection
    TColumn Column{};
    std::vector<uint64_t> Hashes;
    TRowSet Batch{};
  };

  void SetUp() override {
    using namespace NQumir::NAst;
    auto i64 = [] { return std::make_shared<TIntegerType>(TIntegerType::I64); };
    TStructType leftType({{"lk", i64()}});
    TStructType rightType({{"rk", i64()}});
    Spec_ = NKernel::BuildJoinKernelSpec(leftType, rightType, {{"lk", "rk"}},
                                         EJoinType::Inner);
    Hash_ = TKernelCompiler().CompileJoinHash(Spec_);
  }

  // Hashes the side with its own kernel and attaches the result to the batch.
  void Prepare(TSide &side, const TJoinHashKernels::TDispatch &hash) {
    side.Column = TColumn{.Data = reinterpret_cast<char *>(side.Keys.data())};
    side.Hashes.assign(side.Keys.size(), NoHash_);
    side.Batch = TRowSet{
        .Columns = &side.Column,
        .ColumnCount = 1,
        .RowCount = static_cast<int64_t>(side.Keys.size()),
        .Selection = side.Selection.empty() ? nullptr : side.Selection.data(),
        .RefCount = 1,
    };
    ASSERT_TRUE(hash(&side.Batch, side.Hashes.data()));
    side.Batch.Hash = side.Hashes.data();
  }

  std::vector<uint64_t> LeftHashes(std::vector<uint8_t> selection) {
    TSide side{.Keys = {1, 2, 3, 4, 5, 6}, .Selection = std::move(selection)};
    Prepare(side, Hash_.Left);
    return side.Hashes;
  }

  const uint64_t NoHash_ = 0x5a5a5a5a5a5a5a5aULL;
  NKernel::TOperatorKernelSpec Spec_;
  TJoinHashKernels Hash_;
};

} // namespace

TEST_F(JoinKeyHash, SparseSelectionHashesOnlySelectedRows) {
  const auto all = LeftHashes({});
  const std::vector<uint8_t> selection = {0xff, 0, 1, 0, 0, 2};
  const auto sparse = LeftHashes(selection);
  for (size_t row = 0; row < selection.size(); ++row) {
    EXPECT_EQ(sparse[row], selection[row] ? all[row] : NoHash_) << row;
  }
}

TEST_F(JoinKeyHash, EmptySelectionWritesNoHash) {
  EXPECT_EQ(LeftHashes({0, 0, 0, 0, 0, 0}),
            std::vector<uint64_t>(6, NoHash_));
}

TEST_F(JoinKeyHash, FullSelectionMatchesNoSelection) {
  const auto all = LeftHashes({});
  EXPECT_EQ(std::ranges::count(all, NoHash_), 0);
  EXPECT_EQ(LeftHashes({1, 0xff, 2, 1, 0xff, 3}), all);
}

// The join reads the hash of a selected row only, so the rows that the hash
// kernel skipped do not show up in the result.
TEST_F(JoinKeyHash, JoinWithPrecomputedHashesSkipsUnselectedRows) {
  TSide left{.Keys = {1, 2, 1, 3}, .Selection = {1, 0, 0xff, 1}};
  TSide right{.Keys = {1, 2, 3, 1}, .Selection = {0, 1, 1, 0xff}};
  Prepare(left, Hash_.Left);
  Prepare(right, Hash_.Right);

  Spec_.HasPrecomputedHash = true;
  auto kernels = TKernelCompiler().CompileJoin(Spec_);
  THashTable leftTable{}, rightTable{};
  TPairBuffer pairs{};
  ASSERT_TRUE(kernels.Dispatch(&leftTable, &rightTable, nullptr, 0, &pairs,
                               nullptr, nullptr, 8,
                               JoinOpCode(EJoinKernelOp::Init)));
  ASSERT_TRUE(kernels.Dispatch(&leftTable, &rightTable, &left.Batch, 0, &pairs,
                               &left.Batch, &right.Batch, 0,
                               JoinOpCode(EJoinKernelOp::UpdateLeft)));
  ASSERT_TRUE(kernels.Dispatch(&leftTable, &rightTable, &right.Batch, 0, &pairs,
                               &left.Batch, &right.Batch, 0,
                               JoinOpCode(EJoinKernelOp::UpdateRight)));

  std::vector<std::tuple<int64_t, int64_t>> got;
  for (int64_t i = 0; i < pairs.Count; ++i) {
    got.emplace_back(pairs.Data[2 * i] & 0xffffffff,
                     pairs.Data[2 * i + 1] & 0xffffffff);
  }
  std::sort(got.begin(), got.end());
  EXPECT_EQ(got, (std::vector<std::tuple<int64_t, int64_t>>{
                     {0, 3}, {2, 3}, {3, 2}}));

  kernels.Dispatch(&leftTable, &rightTable, nullptr, 0, &pairs, nullptr,
                   nullptr, 0, JoinOpCode(EJoinKernelOp::Destroy));
}

TEST(CompileJoin, ProbeOnlyStreamsWithoutInserting) {
  using namespace NQumir::NAst;
  auto i64 = [] { return std::make_shared<TIntegerType>(TIntegerType::I64); };
  TStructType leftType({{"lk", i64()}, {"lv", i64()}});
  TStructType rightType({{"rk", i64()}, {"rv", i64()}});

  TKernelCompiler compiler;
  auto spec = NKernel::BuildJoinKernelSpec(leftType, rightType, {{"lk", "rk"}},
                                           EJoinType::Inner);
  auto kernels = compiler.CompileJoin(spec);

  std::vector<int64_t> lkeys = {1, 2, 1};
  std::vector<int64_t> rkeys = {1, 1, 3};
  auto makeBatch = [](std::vector<int64_t> &keys, std::vector<TColumn> &cols) {
    cols = {TColumn{.Data = reinterpret_cast<char *>(keys.data())},
            TColumn{.Data = reinterpret_cast<char *>(keys.data())}};
    return TRowSet{.Columns = cols.data(),
                   .ColumnCount = 2,
                   .RowCount = static_cast<int64_t>(keys.size()),
                   .RefCount = 1};
  };
  std::vector<TColumn> lcols, rcols;
  TRowSet lbatch = makeBatch(lkeys, lcols);
  TRowSet rbatch = makeBatch(rkeys, rcols);

  THashTable left{}, right{};
  TPairBuffer pairs{};
  ASSERT_TRUE(kernels.Dispatch(&left, &right, nullptr, 0, &pairs, nullptr,
                               nullptr, 8, JoinOpCode(EJoinKernelOp::Init)));
  ASSERT_TRUE(kernels.Dispatch(&left, &right, &lbatch, 0, &pairs, &lbatch,
                               &rbatch, 0,
                               JoinOpCode(EJoinKernelOp::UpdateLeft)));
  ASSERT_EQ(pairs.Count, 0);
  ASSERT_TRUE(kernels.Dispatch(&left, &right, &rbatch, 7, &pairs, &lbatch,
                               &rbatch, 0,
                               JoinOpCode(EJoinKernelOp::StreamRight)));
  EXPECT_EQ(right.Size, 0);

  std::vector<std::tuple<int64_t, int64_t>> got;
  for (int64_t i = 0; i < pairs.Count; ++i) {
    got.emplace_back(pairs.Data[2 * i] & 0xffffffff,
                     pairs.Data[2 * i + 1] & 0xffffffff);
  }
  std::vector<std::tuple<int64_t, int64_t>> expected = {
      {0, 0}, {0, 1}, {2, 0}, {2, 1}};
  std::sort(got.begin(), got.end());
  EXPECT_EQ(got, expected);

  pairs.Count = 0;
  kernels.Dispatch(&left, &right, nullptr, 0, &pairs, nullptr, nullptr, 0,
                   JoinOpCode(EJoinKernelOp::Destroy));
  THashTable left2{}, right2{};
  ASSERT_TRUE(kernels.Dispatch(&left2, &right2, nullptr, 0, &pairs, nullptr,
                               nullptr, 8, JoinOpCode(EJoinKernelOp::Init)));
  ASSERT_TRUE(kernels.Dispatch(&left2, &right2, &rbatch, 0, &pairs, &lbatch,
                               &rbatch, 0,
                               JoinOpCode(EJoinKernelOp::UpdateRight)));
  ASSERT_EQ(pairs.Count, 0);
  ASSERT_TRUE(kernels.Dispatch(&left2, &right2, &lbatch, 9, &pairs, &lbatch,
                               &rbatch, 0,
                               JoinOpCode(EJoinKernelOp::StreamLeft)));
  EXPECT_EQ(left2.Size, 0);

  got.clear();
  for (int64_t i = 0; i < pairs.Count; ++i) {
    got.emplace_back(pairs.Data[2 * i] & 0xffffffff,
                     pairs.Data[2 * i + 1] & 0xffffffff);
  }
  std::sort(got.begin(), got.end());
  EXPECT_EQ(got, expected);

  kernels.Dispatch(&left2, &right2, nullptr, 0, &pairs, nullptr, nullptr, 0,
                   JoinOpCode(EJoinKernelOp::Destroy));
}

TEST(CompileJoin, AcceptsStringKeyAndRejectsUnsupportedSpecs) {
  using namespace NQumir::NAst;
  auto i64 = [] { return std::make_shared<TIntegerType>(TIntegerType::I64); };
  TKernelCompiler compiler;

  TStructType strLeft({{"lk", std::make_shared<TStringType>()}});
  TStructType strRight({{"rk", std::make_shared<TStringType>()}});
  auto stringSpec = NKernel::BuildJoinKernelSpec(
      strLeft, strRight, {{"lk", "rk"}}, EJoinType::Inner);
  EXPECT_NO_THROW(compiler.CompileJoin(stringSpec));

  TStructType leftType({{"lk", i64()}});
  TStructType rightType({{"rk", i64()}});
  // FULL OUTER is supported (left- + right-unmatched finalize).
  auto fullSpec = NKernel::BuildJoinKernelSpec(leftType, rightType,
                                               {{"lk", "rk"}}, EJoinType::Full);
  EXPECT_NO_THROW(compiler.CompileJoin(fullSpec));
  // Unsupported join type (RightSemi is not implemented yet).
  auto rightSemiSpec = NKernel::BuildJoinKernelSpec(
      leftType, rightType, {{"lk", "rk"}}, EJoinType::RightSemi);
  EXPECT_THROW(compiler.CompileJoin(rightSemiSpec), NQumir::TError);
  // Missing column.
  EXPECT_THROW(NKernel::BuildJoinKernelSpec(
                   leftType, rightType, {{"missing", "rk"}}, EJoinType::Inner),
               std::runtime_error);
  // Incompatible key types (i64 vs string).
  auto incompatibleSpec = NKernel::BuildJoinKernelSpec(
      leftType, strRight, {{"lk", "rk"}}, EJoinType::Inner);
  EXPECT_THROW(compiler.CompileJoin(incompatibleSpec), NQumir::TError);
}

namespace {

// Compiles one entry of the GENERIC join program: key-ops overloads for the
// join key + the shared library + generated jt_process_left/right.
std::unique_ptr<NQumir::TLLVMRunner>
CompileGenericJoin(const NKernel::TJoinKeyDescriptor &keyDesc,
                   const std::string &entry, void *&fn) {
  using namespace NQumir::NAst;
  TTypePtr columnType, rowSetType, hashTableType, pairBufferType,
      stringViewType;
  for (const auto &et : NQumir::NRegistry::QumirDbExternalTypes()) {
    if (et.Name == "TColumn")
      columnType = et.Type;
    else if (et.Name == "TRowSet")
      rowSetType = et.Type;
    else if (et.Name == "HashTable")
      hashTableType = et.Type;
    else if (et.Name == "PairBuffer")
      pairBufferType = et.Type;
    else if (et.Name == "StringView")
      stringViewType = et.Type;
  }
  auto lib = NKernel::BuildJoinKernelLibrary();
  if (!lib) {
    ADD_FAILURE() << lib.error().ToString();
    return {};
  }
  std::vector<TExprPtr> program;
  for (auto &f : NKernel::GenJoinKeyTypeDecls(keyDesc))
    program.push_back(std::move(f));
  if (keyDesc.HasDistinctLookupType()) {
    auto stringOps = NKernel::ParseFunctionLibrary(
        NKernel::ReadAggregationKernel("string_ops.oz"));
    if (!stringOps) {
      ADD_FAILURE() << stringOps.error().ToString();
      return {};
    }
    for (auto &f : *stringOps)
      program.push_back(std::move(f));
  }
  for (auto &f : NKernel::GenJoinKeyOpsFunDecls(keyDesc))
    program.push_back(std::move(f));
  for (auto &f : NKernel::GenJoinKeyOwnershipFunDecls(keyDesc))
    program.push_back(std::move(f));
  auto dualLib = NKernel::BuildJoinDualKeyLibrary();
  if (!dualLib) {
    ADD_FAILURE() << dualLib.error().ToString();
    return {};
  }
  for (auto &f : *dualLib)
    program.push_back(std::move(f));
  for (auto &f : *lib)
    program.push_back(std::move(f));
  program.push_back(NKernel::GenJoinProcessAst(
      keyDesc, true, "jt_process_left", columnType, rowSetType, hashTableType,
      pairBufferType, stringViewType));
  program.push_back(NKernel::GenJoinProcessAst(
      keyDesc, false, "jt_process_right", columnType, rowSetType, hashTableType,
      pairBufferType, stringViewType));
  program.push_back(NKernel::GenJoinProbeAst(
      keyDesc, true, "jt_probe_left_stream", columnType, rowSetType,
      hashTableType, pairBufferType, stringViewType));
  program.push_back(NKernel::GenJoinProbeAst(
      keyDesc, false, "jt_probe_right_stream", columnType, rowSetType,
      hashTableType, pairBufferType, stringViewType));

  NQumir::TLLVMRunnerOptions options;
  options.CoreInput = true;
  options.NativeCode = true;
  options.AllowOverloads = true;
  NQdb::NTest::ConfigureQumirDbSourceModule(options);
  auto runner = std::make_unique<NQumir::TLLVMRunner>(options);
  auto prog =
      std::make_shared<TBlockExpr>(NQumir::TLocation{}, std::move(program));
  NQdb::NTest::AddQumirDbUse(prog);
  std::string error;
  fn = runner->CompileKernelAst(prog, entry, &error);
  EXPECT_NE(fn, nullptr) << error;
  return runner;
}

} // namespace

TEST(JoinKernelGeneric, Int32KeyMatchesNestedLoop) {
  using namespace NQumir::NAst;
  auto i32 = std::make_shared<TIntegerType>(TIntegerType::I32);
  auto i64 = std::make_shared<TIntegerType>(TIntegerType::I64);
  TStructType leftType({{"lk", i32}, {"lv", i64}});
  TStructType rightType({{"rk", i32}, {"rv", i64}});
  auto keyDesc =
      NKernel::BuildJoinKeyDescriptor(leftType, rightType, {{"lk", "rk"}});

  void *initFn = nullptr;
  auto r1 = CompileGenericJoin(keyDesc, "jt_init", initFn);
  void *destroyFn = nullptr;
  auto r2 = CompileGenericJoin(keyDesc, "jt_destroy", destroyFn);
  void *pbDestroyFn = nullptr;
  auto r3 = CompileGenericJoin(keyDesc, "pb_destroy", pbDestroyFn);
  void *leftFn = nullptr;
  auto r4 = CompileGenericJoin(keyDesc, "jt_process_left", leftFn);
  void *rightFn = nullptr;
  auto r5 = CompileGenericJoin(keyDesc, "jt_process_right", rightFn);
  ASSERT_NE(leftFn, nullptr);
  ASSERT_NE(rightFn, nullptr);

  auto jtInit = reinterpret_cast<bool (*)(void *, int64_t, int64_t)>(initFn);
  auto jtDestroy = reinterpret_cast<void (*)(void *)>(destroyFn);
  auto pbDestroy = reinterpret_cast<void (*)(void *)>(pbDestroyFn);
  auto procLeft =
      reinterpret_cast<bool (*)(void *, void *, TRowSet *, const int64_t *,
                                int64_t, void *, TRowSet *, TRowSet *)>(leftFn);
  auto procRight =
      reinterpret_cast<bool (*)(void *, void *, TRowSet *, const int64_t *,
                                int64_t, void *, TRowSet *, TRowSet *)>(
          rightFn);

  // int32 keys, i64 values.
  std::vector<int32_t> lk = {1, 2, 1};
  std::vector<int64_t> lv = {10, 20, 30};
  std::vector<int32_t> rk = {1, 1, 3};
  std::vector<int64_t> rv = {100, 200, 300};
  auto batch = [](int32_t *keys, int64_t *vals, int64_t rows,
                  std::vector<TColumn> &cols) {
    cols = {TColumn{.Data = reinterpret_cast<char *>(keys)},
            TColumn{.Data = reinterpret_cast<char *>(vals)}};
    return TRowSet{.Columns = cols.data(),
                   .ColumnCount = 2,
                   .RowCount = rows,
                   .RefCount = 1};
  };
  std::vector<TColumn> lcols, rcols;
  TRowSet lbatch = batch(lk.data(), lv.data(), 3, lcols);
  TRowSet rbatch = batch(rk.data(), rv.data(), 3, rcols);

  // The Key descriptor's struct may be wider than 4 bytes; jt_init uses its
  // Size.
  const int64_t keySize = static_cast<int64_t>(keyDesc.Size);
  const int64_t leftKeyColumns[] = {keyDesc.Fields[0].LeftColumnIndex};
  const int64_t rightKeyColumns[] = {keyDesc.Fields[0].RightColumnIndex};
  THashTable left{}, right{};
  ASSERT_TRUE(jtInit(&left, 8, keySize));
  ASSERT_TRUE(jtInit(&right, 8, keySize));
  TPairBuffer pairs{};
  ASSERT_TRUE(procLeft(&left, &right, &lbatch, leftKeyColumns, 0, &pairs,
                       &lbatch, &rbatch));
  ASSERT_TRUE(procRight(&right, &left, &rbatch, rightKeyColumns, 0, &pairs,
                        &lbatch, &rbatch));

  std::vector<std::tuple<int64_t, int64_t>> got;
  for (int64_t i = 0; i < pairs.Count; ++i) {
    got.emplace_back(pairs.Data[2 * i] & 0xffffffff,
                     pairs.Data[2 * i + 1] & 0xffffffff);
  }
  std::vector<std::tuple<int64_t, int64_t>> expected = {
      {0, 0}, {0, 1}, {2, 0}, {2, 1}};
  std::sort(got.begin(), got.end());
  EXPECT_EQ(got, expected);

  pbDestroy(&pairs);
  jtDestroy(&left);
  jtDestroy(&right);
}

TEST(JoinKernelGeneric, Int32KeyTriggersRehash) {
  using namespace NQumir::NAst;
  auto i32 = std::make_shared<TIntegerType>(TIntegerType::I32);
  TStructType leftType({{"lk", i32}});
  TStructType rightType({{"rk", i32}});
  auto keyDesc =
      NKernel::BuildJoinKeyDescriptor(leftType, rightType, {{"lk", "rk"}});

  void *initFn = nullptr;
  auto r1 = CompileGenericJoin(keyDesc, "jt_init", initFn);
  void *destroyFn = nullptr;
  auto r2 = CompileGenericJoin(keyDesc, "jt_destroy", destroyFn);
  void *pbDestroyFn = nullptr;
  auto r3 = CompileGenericJoin(keyDesc, "pb_destroy", pbDestroyFn);
  void *leftFn = nullptr;
  auto r4 = CompileGenericJoin(keyDesc, "jt_process_left", leftFn);
  void *rightFn = nullptr;
  auto r5 = CompileGenericJoin(keyDesc, "jt_process_right", rightFn);
  ASSERT_NE(leftFn, nullptr);
  ASSERT_NE(rightFn, nullptr);

  auto jtInit = reinterpret_cast<bool (*)(void *, int64_t, int64_t)>(initFn);
  auto jtDestroy = reinterpret_cast<void (*)(void *)>(destroyFn);
  auto pbDestroy = reinterpret_cast<void (*)(void *)>(pbDestroyFn);
  auto procLeft =
      reinterpret_cast<bool (*)(void *, void *, TRowSet *, const int64_t *,
                                int64_t, void *, TRowSet *, TRowSet *)>(leftFn);
  auto procRight =
      reinterpret_cast<bool (*)(void *, void *, TRowSet *, const int64_t *,
                                int64_t, void *, TRowSet *, TRowSet *)>(
          rightFn);

  // Enough distinct keys to rehash with either 8-slot or 16-slot groups.
  // Unsigned arithmetic keeps the generator's wraparound well-defined.
  uint32_t seed = 12345;
  auto rnd = [&]() {
    seed = (seed * 1103515245u + 12345u) & 0x7fffffffu;
    return seed;
  };
  std::vector<int32_t> lk, rk;
  for (int i = 0; i < 60; ++i)
    lk.push_back(rnd() % 25);
  for (int i = 0; i < 60; ++i)
    rk.push_back(rnd() % 25);
  auto batch = [](std::vector<int32_t> &keys, std::vector<TColumn> &cols) {
    cols = {TColumn{.Data = reinterpret_cast<char *>(keys.data())}};
    return TRowSet{.Columns = cols.data(),
                   .ColumnCount = 1,
                   .RowCount = static_cast<int64_t>(keys.size()),
                   .RefCount = 1};
  };
  std::vector<TColumn> lcols, rcols;
  TRowSet lbatch = batch(lk, lcols);
  TRowSet rbatch = batch(rk, rcols);

  const int64_t keySize = static_cast<int64_t>(keyDesc.Size);
  const int64_t leftKeyColumns[] = {keyDesc.Fields[0].LeftColumnIndex};
  const int64_t rightKeyColumns[] = {keyDesc.Fields[0].RightColumnIndex};
  THashTable left{}, right{};
  // Smallest capacity a SwissTable group allows -> still forces a rehash.
  ASSERT_TRUE(jtInit(&left, 8, keySize));
  ASSERT_TRUE(jtInit(&right, 8, keySize));
  const auto initialCapacity = left.Capacity;
  TPairBuffer pairs{};
  ASSERT_TRUE(procLeft(&left, &right, &lbatch, leftKeyColumns, 0, &pairs,
                       &lbatch, &rbatch));
  ASSERT_TRUE(procRight(&right, &left, &rbatch, rightKeyColumns, 0, &pairs,
                        &lbatch, &rbatch));

  int64_t matched = 0;
  for (int64_t i = 0; i < pairs.Count; ++i) {
    int64_t leftRow = pairs.Data[2 * i] & 0xffffffff;
    int64_t rightRow = pairs.Data[2 * i + 1] & 0xffffffff;
    ASSERT_EQ(lk[leftRow],
              rk[rightRow]); // every emitted pair has matching keys
    ++matched;
  }
  int64_t expected = 0;
  for (int32_t a : lk)
    for (int32_t b : rk)
      if (a == b)
        ++expected;
  EXPECT_EQ(matched, expected);
  EXPECT_GT(left.Capacity, initialCapacity); // bucket pointers survived rehash

  pbDestroy(&pairs);
  jtDestroy(&left);
  jtDestroy(&right);
}

namespace {

// Packs (batch_idx << 32) | row the way the join kernels emit row ids.
constexpr int64_t PackRowId(int64_t batchIdx, int64_t row) {
  return (batchIdx << 32) | (row & 0xffffffff);
}

// Frees the buffers a jt_materialize call allocated (mirrors the host's
// kernel-owned-rowset Destroy callback).
void FreeMaterializedRowSet(TRowSet &rowSet) {
  auto *owners = static_cast<int64_t *>(rowSet.Private);
  ASSERT_NE(owners, nullptr);
  for (int64_t i = 0; i < owners[0]; ++i) {
    qdb_free(reinterpret_cast<void *>(owners[i + 1]));
  }
  qdb_free(owners);
  rowSet.Private = nullptr;
}

bool MaskBit(const TColumn &col, int64_t row) {
  return ((col.Mask[row / 8] >> (row % 8)) & 1) != 0;
}

} // namespace

TEST(JoinMaterialize, GathersFixedAndStringColumnsWithNullPadding) {
  using namespace NQumir::NAst;
  auto i64 = std::make_shared<TIntegerType>(TIntegerType::I64);
  auto i32 = std::make_shared<TIntegerType>(TIntegerType::I32);
  TStructType leftType(
      {{"lk", i64}, {"lname", std::make_shared<TStringType>()}});
  TStructType rightType({{"rk", i64}, {"rv", i32}});

  TKernelCompiler compiler;
  auto spec = NKernel::BuildJoinKernelSpec(leftType, rightType, {{"lk", "rk"}},
                                           EJoinType::Left);
  auto kernels = compiler.CompileJoin(spec);

  // Left store: one batch of 3 rows with a string payload column.
  std::vector<int64_t> lk = {1, 2, 3};
  std::string lnameBytes = "abb"; // "a", "bb", ""
  std::vector<int64_t> lnameOffsets = {0, 1, 3, 3};
  std::vector<TColumn> lcols = {
      TColumn{.Data = reinterpret_cast<char *>(lk.data())},
      TColumn{.Data = lnameBytes.data(),
              .Offsets = lnameOffsets.data(),
              .OffsetWidth = 8},
  };
  TRowSet lbatch{
      .Columns = lcols.data(), .ColumnCount = 2, .RowCount = 3, .RefCount = 1};

  // Right store: one batch of 2 rows with an i32 payload column.
  std::vector<int64_t> rk = {1, 2};
  std::vector<int32_t> rv = {10, 20};
  std::vector<TColumn> rcols = {
      TColumn{.Data = reinterpret_cast<char *>(rk.data())},
      TColumn{.Data = reinterpret_cast<char *>(rv.data())},
  };
  TRowSet rbatch{
      .Columns = rcols.data(), .ColumnCount = 2, .RowCount = 2, .RefCount = 1};

  // Hand-built pairs: two matches plus one outer-padded left row (right -1).
  std::vector<int64_t> pairData = {
      PackRowId(0, 0), PackRowId(0, 0), PackRowId(0, 1),
      PackRowId(0, 1), PackRowId(0, 2), -1,
  };
  TPairBuffer pairs{.Count = 3, .Capacity = 3, .Data = pairData.data()};

  TRowSet out{};
  ASSERT_EQ(kernels.Materialize(&pairs, &lbatch, &rbatch, &lbatch, &rbatch, 0,
                                100, &out),
            3);
  ASSERT_EQ(out.RowCount, 3);
  ASSERT_EQ(out.ColumnCount, 4);
  EXPECT_EQ(out.Selection, nullptr);
  EXPECT_EQ(out.RefCount, 1);

  // lk: all rows valid.
  const auto *outLk = reinterpret_cast<const int64_t *>(out.Columns[0].Data);
  EXPECT_EQ(outLk[0], 1);
  EXPECT_EQ(outLk[1], 2);
  EXPECT_EQ(outLk[2], 3);
  for (int64_t j = 0; j < 3; ++j) {
    EXPECT_TRUE(MaskBit(out.Columns[0], j));
  }

  // lname: string payloads follow pair order.
  const auto &lname = out.Columns[1];
  ASSERT_EQ(lname.OffsetWidth, 8);
  const auto *offsets = static_cast<const int64_t *>(lname.Offsets);
  EXPECT_EQ(std::string(lname.Data + offsets[0], lname.Data + offsets[1]), "a");
  EXPECT_EQ(std::string(lname.Data + offsets[1], lname.Data + offsets[2]),
            "bb");
  EXPECT_EQ(offsets[3], offsets[2]); // ""

  // rv (i32): matched rows carry values, the padded row is NULL.
  const auto *outRv = reinterpret_cast<const int32_t *>(out.Columns[3].Data);
  EXPECT_EQ(outRv[0], 10);
  EXPECT_EQ(outRv[1], 20);
  EXPECT_EQ(outRv[2], 0); // zeroed padding
  EXPECT_TRUE(MaskBit(out.Columns[3], 0));
  EXPECT_TRUE(MaskBit(out.Columns[3], 1));
  EXPECT_FALSE(MaskBit(out.Columns[3], 2));
  EXPECT_FALSE(MaskBit(out.Columns[2], 2)); // rk padded NULL too

  FreeMaterializedRowSet(out);

  // start/limit cursoring: one row starting at pair 1.
  TRowSet slice{};
  ASSERT_EQ(kernels.Materialize(&pairs, &lbatch, &rbatch, &lbatch, &rbatch, 1,
                                1, &slice),
            1);
  ASSERT_EQ(slice.RowCount, 1);
  EXPECT_EQ(reinterpret_cast<const int64_t *>(slice.Columns[0].Data)[0], 2);
  EXPECT_EQ(reinterpret_cast<const int32_t *>(slice.Columns[3].Data)[0], 20);
  FreeMaterializedRowSet(slice);

  // Exhausted / empty cursor positions leave the output untouched.
  TRowSet empty{};
  EXPECT_EQ(kernels.Materialize(&pairs, &lbatch, &rbatch, &lbatch, &rbatch, 3,
                                100, &empty),
            0);
  EXPECT_EQ(empty.Columns, nullptr);
  TPairBuffer noPairs{};
  EXPECT_EQ(kernels.Materialize(&noPairs, &lbatch, &rbatch, &lbatch, &rbatch, 0,
                                100, &empty),
            0);
}

TEST(JoinMaterialize, PropagatesSourceNullsAndReadsStreamBatches) {
  using namespace NQumir::NAst;
  auto i64 = std::make_shared<TIntegerType>(TIntegerType::I64);
  TStructType leftType({{"lk", i64}, {"lv", std::make_shared<TNullable>(i64)}});
  TStructType rightType({{"rk", i64}});

  TKernelCompiler compiler;
  auto spec = NKernel::BuildJoinKernelSpec(leftType, rightType, {{"lk", "rk"}},
                                           EJoinType::Inner);
  auto kernels = compiler.CompileJoin(spec);

  // Left store: 2 rows; lv row 1 is NULL at the source.
  std::vector<int64_t> lk = {1, 2};
  std::vector<int64_t> lv = {100, 200};
  std::vector<uint8_t> lvMask = {0b01}; // row 0 valid, row 1 null
  std::vector<TColumn> lcols = {
      TColumn{.Data = reinterpret_cast<char *>(lk.data())},
      TColumn{.Data = reinterpret_cast<char *>(lv.data()),
              .Mask = lvMask.data()},
  };
  TRowSet lbatch{
      .Columns = lcols.data(), .ColumnCount = 2, .RowCount = 2, .RefCount = 1};

  // Right store batch and a DIFFERENT right stream batch: pairs with a
  // stream-encoded right id (batch -1) must read the stream batch.
  std::vector<int64_t> rkStore = {7};
  std::vector<TColumn> rcolsStore = {
      TColumn{.Data = reinterpret_cast<char *>(rkStore.data())}};
  TRowSet rstore{.Columns = rcolsStore.data(),
                 .ColumnCount = 1,
                 .RowCount = 1,
                 .RefCount = 1};
  std::vector<int64_t> rkStream = {42, 43};
  std::vector<TColumn> rcolsStream = {
      TColumn{.Data = reinterpret_cast<char *>(rkStream.data())}};
  TRowSet rstream{.Columns = rcolsStream.data(),
                  .ColumnCount = 1,
                  .RowCount = 2,
                  .RefCount = 1};

  std::vector<int64_t> pairData = {
      PackRowId(0, 0),
      PackRowId(0, 0), // store right row -> 7
      PackRowId(0, 1),
      PackRowId(-1, 1), // stream right row -> 43
  };
  TPairBuffer pairs{.Count = 2, .Capacity = 2, .Data = pairData.data()};

  TRowSet out{};
  ASSERT_EQ(kernels.Materialize(&pairs, &lbatch, &rstore, &lbatch, &rstream, 0,
                                100, &out),
            2);
  ASSERT_EQ(out.RowCount, 2);
  ASSERT_EQ(out.ColumnCount, 3);

  // lv: source null propagated.
  EXPECT_EQ(reinterpret_cast<const int64_t *>(out.Columns[1].Data)[0], 100);
  EXPECT_TRUE(MaskBit(out.Columns[1], 0));
  EXPECT_FALSE(MaskBit(out.Columns[1], 1));

  // rk: row 0 from the store, row 1 from the stream batch.
  const auto *outRk = reinterpret_cast<const int64_t *>(out.Columns[2].Data);
  EXPECT_EQ(outRk[0], 7);
  EXPECT_EQ(outRk[1], 43);

  FreeMaterializedRowSet(out);
}

TEST(JoinMaterialize, FullOuterPadsBothSides) {
  using namespace NQumir::NAst;
  auto i64 = std::make_shared<TIntegerType>(TIntegerType::I64);
  TStructType leftType({{"lk", i64}});
  TStructType rightType({{"rk", i64}});

  TKernelCompiler compiler;
  auto spec = NKernel::BuildJoinKernelSpec(leftType, rightType, {{"lk", "rk"}},
                                           EJoinType::Full);
  auto kernels = compiler.CompileJoin(spec);

  std::vector<int64_t> leftValues = {10, 20};
  std::vector<TColumn> leftColumns = {
      TColumn{.Data = reinterpret_cast<char *>(leftValues.data())},
  };
  TRowSet leftBatch{.Columns = leftColumns.data(),
                    .ColumnCount = 1,
                    .RowCount = 2,
                    .RefCount = 1};

  std::vector<int64_t> rightValues = {100, 200};
  std::vector<TColumn> rightColumns = {
      TColumn{.Data = reinterpret_cast<char *>(rightValues.data())},
  };
  TRowSet rightBatch{.Columns = rightColumns.data(),
                     .ColumnCount = 1,
                     .RowCount = 2,
                     .RefCount = 1};

  std::vector<int64_t> pairData = {
      PackRowId(0, 0), PackRowId(0, 0), PackRowId(0, 1), -1, -1,
      PackRowId(0, 1),
  };
  TPairBuffer pairs{.Count = 3, .Capacity = 3, .Data = pairData.data()};

  TRowSet out{};
  ASSERT_EQ(kernels.Materialize(&pairs, &leftBatch, &rightBatch, &leftBatch,
                                &rightBatch, 0, 100, &out),
            3);
  ASSERT_EQ(out.ColumnCount, 2);

  const auto *outLeft = reinterpret_cast<const int64_t *>(out.Columns[0].Data);
  const auto *outRight = reinterpret_cast<const int64_t *>(out.Columns[1].Data);
  EXPECT_EQ((std::vector<int64_t>{outLeft[0], outLeft[1], outLeft[2]}),
            (std::vector<int64_t>{10, 20, 0}));
  EXPECT_EQ((std::vector<int64_t>{outRight[0], outRight[1], outRight[2]}),
            (std::vector<int64_t>{100, 0, 200}));
  EXPECT_TRUE(MaskBit(out.Columns[0], 0));
  EXPECT_TRUE(MaskBit(out.Columns[0], 1));
  EXPECT_FALSE(MaskBit(out.Columns[0], 2));
  EXPECT_TRUE(MaskBit(out.Columns[1], 0));
  EXPECT_FALSE(MaskBit(out.Columns[1], 1));
  EXPECT_TRUE(MaskBit(out.Columns[1], 2));

  FreeMaterializedRowSet(out);
}

TEST(JoinMaterialize, SemiAntiOutputsLeftColumnsOnly) {
  using namespace NQumir::NAst;
  auto i64 = std::make_shared<TIntegerType>(TIntegerType::I64);
  TStructType leftType({{"lk", i64}, {"lv", i64}});
  TStructType rightType({{"rk", i64}});

  TKernelCompiler compiler;
  auto spec = NKernel::BuildJoinKernelSpec(leftType, rightType, {{"lk", "rk"}},
                                           EJoinType::LeftSemi);
  auto kernels = compiler.CompileJoin(spec);

  std::vector<int64_t> lk = {1, 2};
  std::vector<int64_t> lv = {10, 20};
  std::vector<TColumn> lcols = {
      TColumn{.Data = reinterpret_cast<char *>(lk.data())},
      TColumn{.Data = reinterpret_cast<char *>(lv.data())},
  };
  TRowSet lbatch{
      .Columns = lcols.data(), .ColumnCount = 2, .RowCount = 2, .RefCount = 1};

  // Semi/anti pairs carry right = -1; the right side must not be read.
  std::vector<int64_t> pairData = {
      PackRowId(0, 1),
      -1,
      PackRowId(0, 0),
      -1,
  };
  TPairBuffer pairs{.Count = 2, .Capacity = 2, .Data = pairData.data()};

  TRowSet out{};
  ASSERT_EQ(kernels.Materialize(&pairs, &lbatch, nullptr, &lbatch, nullptr, 0,
                                100, &out),
            2);
  ASSERT_EQ(out.RowCount, 2);
  ASSERT_EQ(out.ColumnCount, 2); // left columns only
  const auto *outLk = reinterpret_cast<const int64_t *>(out.Columns[0].Data);
  const auto *outLv = reinterpret_cast<const int64_t *>(out.Columns[1].Data);
  EXPECT_EQ(outLk[0], 2);
  EXPECT_EQ(outLv[0], 20);
  EXPECT_EQ(outLk[1], 1);
  EXPECT_EQ(outLv[1], 10);

  FreeMaterializedRowSet(out);
}

TEST(JoinMaterialize, GathersBoolAndDecimalWorkers) {
  using namespace NQumir::NAst;
  auto i64 = std::make_shared<TIntegerType>(TIntegerType::I64);
  TStructType leftType({{"lk", i64}, {"flag", std::make_shared<TBoolType>()}});
  TStructType rightType(
      {{"rk", i64}, {"amount", std::make_shared<TDecimal>(7, 2)}});

  TKernelCompiler compiler;
  auto spec = NKernel::BuildJoinKernelSpec(leftType, rightType, {{"lk", "rk"}},
                                           EJoinType::Left);
  auto kernels = compiler.CompileJoin(spec);

  std::vector<int64_t> lk = {1, 2};
  std::vector<uint8_t> flags = {0b00000100}; // bit offset 2: true, false
  std::vector<TColumn> lcols = {
      TColumn{.Data = reinterpret_cast<char *>(lk.data())},
      TColumn{.Data = reinterpret_cast<char *>(flags.data()),
              .DataBitOffset = 2},
  };
  TRowSet lbatch{
      .Columns = lcols.data(), .ColumnCount = 2, .RowCount = 2, .RefCount = 1};

  std::vector<int64_t> rk = {1};
  std::vector<qdb_bin_int> amounts = {{.Lo = 1234, .Hi = 0}};
  std::vector<TColumn> rcols = {
      TColumn{.Data = reinterpret_cast<char *>(rk.data())},
      TColumn{.Data = reinterpret_cast<char *>(amounts.data())},
  };
  TRowSet rbatch{
      .Columns = rcols.data(), .ColumnCount = 2, .RowCount = 1, .RefCount = 1};

  std::vector<int64_t> pairData = {
      PackRowId(0, 0),
      PackRowId(0, 0),
      PackRowId(0, 1),
      -1,
  };
  TPairBuffer pairs{.Count = 2, .Capacity = 2, .Data = pairData.data()};

  TRowSet out{};
  ASSERT_EQ(kernels.Materialize(&pairs, &lbatch, &rbatch, &lbatch, &rbatch, 0,
                                100, &out),
            2);
  ASSERT_EQ(out.ColumnCount, 4);

  EXPECT_TRUE(MaskBit(out.Columns[1], 0));
  EXPECT_TRUE(MaskBit(out.Columns[1], 1));
  EXPECT_TRUE((static_cast<uint8_t>(out.Columns[1].Data[0]) & 0b1) != 0);
  EXPECT_FALSE((static_cast<uint8_t>(out.Columns[1].Data[0]) & 0b10) != 0);

  const auto *outAmounts =
      reinterpret_cast<const qdb_bin_int *>(out.Columns[3].Data);
  EXPECT_EQ(outAmounts[0].Lo, 1234u);
  EXPECT_EQ(outAmounts[0].Hi, 0u);
  EXPECT_EQ(outAmounts[1].Lo, 0u);
  EXPECT_EQ(outAmounts[1].Hi, 0u);
  EXPECT_TRUE(MaskBit(out.Columns[3], 0));
  EXPECT_FALSE(MaskBit(out.Columns[3], 1));

  FreeMaterializedRowSet(out);
}

TEST(CompileCrossJoin, EmitsSelectedPairsAndMaterializesStringPayload) {
  using namespace NQumir::NAst;
  auto i64 = std::make_shared<TIntegerType>(TIntegerType::I64);
  TStructType leftType(
      {{"lk", i64}, {"lname", std::make_shared<TStringType>()}});
  TStructType rightType({{"rv", i64}});

  TKernelCompiler compiler;
  auto spec = NKernel::BuildCrossJoinKernelSpec(leftType, rightType);
  auto kernels = compiler.CompileCrossJoin(spec);

  std::vector<int64_t> lk = {1, 2, 3};
  std::string lnameBytes = "abbccc";
  std::vector<int64_t> lnameOffsets = {0, 1, 3, 6};
  std::vector<uint8_t> lsel = {1, 0, 1};
  std::vector<TColumn> lcols = {
      TColumn{.Data = reinterpret_cast<char *>(lk.data())},
      TColumn{.Data = lnameBytes.data(),
              .Offsets = lnameOffsets.data(),
              .OffsetWidth = 8},
  };
  TRowSet lbatch{.Columns = lcols.data(),
                 .ColumnCount = 2,
                 .RowCount = 3,
                 .Selection = lsel.data(),
                 .RefCount = 1};

  std::vector<int64_t> rv = {10, 20};
  std::vector<uint8_t> rsel = {0, 1};
  std::vector<TColumn> rcols = {
      TColumn{.Data = reinterpret_cast<char *>(rv.data())},
  };
  TRowSet rbatch{.Columns = rcols.data(),
                 .ColumnCount = 1,
                 .RowCount = 2,
                 .Selection = rsel.data(),
                 .RefCount = 1};

  TPairBuffer pairs{};
  ASSERT_TRUE(kernels.Dispatch(&lbatch, 0, &rbatch, 1, nullptr, &pairs,
                               static_cast<int64_t>(ECrossJoinKernelOp::Emit)));
  ASSERT_EQ(pairs.Count, 2);
  EXPECT_EQ(pairs.Data[0], PackRowId(0, 0));
  EXPECT_EQ(pairs.Data[1], PackRowId(0, 1));
  EXPECT_EQ(pairs.Data[2], PackRowId(0, 2));
  EXPECT_EQ(pairs.Data[3], PackRowId(0, 1));

  TRowSet out{};
  ASSERT_EQ(kernels.Materialize(&pairs, &lbatch, &rbatch, &lbatch, &rbatch, 0,
                                100, &out),
            2);
  ASSERT_EQ(out.RowCount, 2);
  ASSERT_EQ(out.ColumnCount, 3);

  const auto *outLk = reinterpret_cast<const int64_t *>(out.Columns[0].Data);
  EXPECT_EQ(outLk[0], 1);
  EXPECT_EQ(outLk[1], 3);
  const auto &outName = out.Columns[1];
  const auto *offsets = static_cast<const int64_t *>(outName.Offsets);
  EXPECT_EQ(std::string(outName.Data + offsets[0], outName.Data + offsets[1]),
            "a");
  EXPECT_EQ(std::string(outName.Data + offsets[1], outName.Data + offsets[2]),
            "ccc");
  const auto *outRv = reinterpret_cast<const int64_t *>(out.Columns[2].Data);
  EXPECT_EQ(outRv[0], 20);
  EXPECT_EQ(outRv[1], 20);

  FreeMaterializedRowSet(out);
  kernels.Dispatch(nullptr, 0, nullptr, 0, nullptr, &pairs,
                   static_cast<int64_t>(ECrossJoinKernelOp::Destroy));
}

namespace {

// Nested-loop fixture for the residual lk < rv: left lk = {1, 5, 3} with row
// 2 unselected; right rv = {2, 4} and {0, 6} with row 6 unselected. Left row
// 0 matches right rows (0, 0) and (0, 1); left row 1 and right row (1, 0)
// match nothing.
struct TCrossJoinResidualInput {
  std::vector<int64_t> Lk = {1, 5, 3};
  std::vector<uint8_t> LeftSelection = {1, 1, 0};
  std::vector<int64_t> Rv0 = {2, 4};
  std::vector<int64_t> Rv1 = {0, 6};
  std::vector<uint8_t> RightSelection1 = {1, 0};
  std::vector<TColumn> LeftColumns = {
      TColumn{.Data = reinterpret_cast<char *>(Lk.data())}};
  std::vector<TColumn> RightColumns0 = {
      TColumn{.Data = reinterpret_cast<char *>(Rv0.data())}};
  std::vector<TColumn> RightColumns1 = {
      TColumn{.Data = reinterpret_cast<char *>(Rv1.data())}};
  TRowSet Left{.Columns = LeftColumns.data(),
               .ColumnCount = 1,
               .RowCount = 3,
               .Selection = LeftSelection.data(),
               .RefCount = 1};
  std::vector<TRowSet> Right = {
      TRowSet{.Columns = RightColumns0.data(),
              .ColumnCount = 1,
              .RowCount = 2,
              .RefCount = 1},
      TRowSet{.Columns = RightColumns1.data(),
              .ColumnCount = 1,
              .RowCount = 2,
              .Selection = RightSelection1.data(),
              .RefCount = 1},
  };
  // One flag per right row in store order.
  std::vector<uint8_t> RightMatched = std::vector<uint8_t>(4, 0);
};

TCrossJoinKernels CompileResidualCrossJoin(EJoinType type) {
  using namespace NQumir::NAst;
  auto i64 = std::make_shared<TIntegerType>(TIntegerType::I64);
  TStructType leftType({{"lk", i64}});
  TStructType rightType({{"rv", i64}});
  TKernelCompiler compiler;
  return compiler.CompileCrossJoin(NKernel::BuildCrossJoinKernelSpec(
      leftType, rightType, type, ParseExpr("(< lk rv)")));
}

std::vector<int64_t> CrossJoinPairs(TCrossJoinKernels &kernels,
                                    TCrossJoinResidualInput &input,
                                    ECrossJoinKernelOp op, TPairBuffer &pairs) {
  EXPECT_TRUE(kernels.Dispatch(&input.Left, 0, input.Right.data(), 2,
                               input.RightMatched.data(), &pairs,
                               static_cast<int64_t>(op)));
  return std::vector<int64_t>(pairs.Data, pairs.Data + 2 * pairs.Count);
}

} // namespace

TEST(CompileCrossJoin, FullJoinPadsUnmatchedRowsOfBothSides) {
  auto kernels = CompileResidualCrossJoin(EJoinType::Full);
  TCrossJoinResidualInput input;
  TPairBuffer pairs{};

  EXPECT_EQ(CrossJoinPairs(kernels, input, ECrossJoinKernelOp::Emit, pairs),
            (std::vector<int64_t>{PackRowId(0, 0), PackRowId(0, 0),
                                  PackRowId(0, 0), PackRowId(0, 1),
                                  PackRowId(0, 1), -1}));
  EXPECT_EQ(input.RightMatched, (std::vector<uint8_t>{1, 1, 0, 0}));
  pairs.Count = 0;
  EXPECT_EQ(CrossJoinPairs(kernels, input,
                           ECrossJoinKernelOp::EmitUnmatchedRight, pairs),
            (std::vector<int64_t>{-1, PackRowId(1, 0)}));

  kernels.Dispatch(nullptr, 0, nullptr, 0, nullptr, &pairs,
                   static_cast<int64_t>(ECrossJoinKernelOp::Destroy));
}

TEST(CompileCrossJoin, SemiAndAntiEmitEachLeftRowOnceByMatch) {
  for (const auto [type, row] :
       {std::pair{EJoinType::LeftSemi, 0}, std::pair{EJoinType::LeftAnti, 1}}) {
    auto kernels = CompileResidualCrossJoin(type);
    TCrossJoinResidualInput input;
    TPairBuffer pairs{};

    EXPECT_EQ(CrossJoinPairs(kernels, input, ECrossJoinKernelOp::Emit, pairs),
              (std::vector<int64_t>{PackRowId(0, row), -1}))
        << JoinTypeName(type);

    kernels.Dispatch(nullptr, 0, nullptr, 0, nullptr, &pairs,
                     static_cast<int64_t>(ECrossJoinKernelOp::Destroy));
  }
}

namespace {

// Owned backing storage for one string column (payload bytes + i64 offsets).
struct TStringColumn {
  std::string Bytes;
  std::vector<int64_t> Offsets;

  explicit TStringColumn(const std::vector<std::string> &values) {
    Offsets.push_back(0);
    for (const auto &value : values) {
      Bytes += value;
      Offsets.push_back(static_cast<int64_t>(Bytes.size()));
    }
  }

  TColumn Column() {
    return TColumn{
        .Data = Bytes.data(), .Offsets = Offsets.data(), .OffsetWidth = 8};
  }
};

std::string ReadString(const TColumn &col, int64_t row) {
  const auto *offsets = static_cast<const int64_t *>(col.Offsets);
  return std::string(col.Data + offsets[row], col.Data + offsets[row + 1]);
}

} // namespace

TEST(JoinStringKey, InnerJoinMatchesAndMaterializes) {
  using namespace NQumir::NAst;
  auto i64 = std::make_shared<TIntegerType>(TIntegerType::I64);
  TStructType leftType({{"lk", std::make_shared<TStringType>()}, {"lv", i64}});
  TStructType rightType({{"rk", std::make_shared<TStringType>()}, {"rv", i64}});

  TKernelCompiler compiler;
  auto spec = NKernel::BuildJoinKernelSpec(leftType, rightType, {{"lk", "rk"}},
                                           EJoinType::Inner);
  auto kernels = compiler.CompileJoin(spec);

  TStringColumn lk({"apple", "banana", "apple"});
  std::vector<int64_t> lv = {10, 20, 30};
  std::vector<TColumn> lcols = {
      lk.Column(), TColumn{.Data = reinterpret_cast<char *>(lv.data())}};
  TRowSet lbatch{
      .Columns = lcols.data(), .ColumnCount = 2, .RowCount = 3, .RefCount = 1};

  TStringColumn rk({"banana", "apple", "cherry"});
  std::vector<int64_t> rv = {100, 200, 300};
  std::vector<TColumn> rcols = {
      rk.Column(), TColumn{.Data = reinterpret_cast<char *>(rv.data())}};
  TRowSet rbatch{
      .Columns = rcols.data(), .ColumnCount = 2, .RowCount = 3, .RefCount = 1};

  THashTable left{}, right{};
  TPairBuffer pairs{};
  ASSERT_TRUE(kernels.Dispatch(&left, &right, nullptr, 0, &pairs, nullptr,
                               nullptr, 8, JoinOpCode(EJoinKernelOp::Init)));
  ASSERT_TRUE(kernels.Dispatch(&left, &right, &lbatch, 0, &pairs, &lbatch,
                               &rbatch, 0,
                               JoinOpCode(EJoinKernelOp::UpdateLeft)));
  ASSERT_TRUE(kernels.Dispatch(&left, &right, &rbatch, 0, &pairs, &lbatch,
                               &rbatch, 0,
                               JoinOpCode(EJoinKernelOp::UpdateRight)));

  TRowSet out{};
  ASSERT_EQ(kernels.Materialize(&pairs, &lbatch, &rbatch, &lbatch, &rbatch, 0,
                                100, &out),
            3);
  std::vector<std::tuple<std::string, int64_t, int64_t>> got;
  for (int64_t i = 0; i < out.RowCount; ++i) {
    got.emplace_back(ReadString(out.Columns[0], i),
                     reinterpret_cast<const int64_t *>(out.Columns[1].Data)[i],
                     reinterpret_cast<const int64_t *>(out.Columns[3].Data)[i]);
    EXPECT_EQ(ReadString(out.Columns[0], i), ReadString(out.Columns[2], i));
  }
  std::vector<std::tuple<std::string, int64_t, int64_t>> expected = {
      {"apple", 10, 200}, {"apple", 30, 200}, {"banana", 20, 100}};
  std::sort(got.begin(), got.end());
  EXPECT_EQ(got, expected);
  FreeMaterializedRowSet(out);

  kernels.Dispatch(&left, &right, nullptr, 0, &pairs, nullptr, nullptr, 0,
                   JoinOpCode(EJoinKernelOp::Destroy));
}

TEST(JoinStringKey, SemiAntiClonesRightKeysAndSurvivesRehash) {
  using namespace NQumir::NAst;
  auto i64 = std::make_shared<TIntegerType>(TIntegerType::I64);
  TStructType leftType({{"lk", std::make_shared<TStringType>()}, {"lv", i64}});
  TStructType rightType({{"rk", std::make_shared<TStringType>()}});

  // Enough distinct string keys to rehash with 8-slot or 16-slot groups,
  // exercising owned-key cloning and carry across rehash.
  std::vector<std::string> leftKeys, rightKeys;
  std::vector<int64_t> lv;
  uint32_t seed = 4242;
  auto rnd = [&]() {
    seed = (seed * 1103515245u + 12345u) & 0x7fffffffu;
    return seed;
  };
  for (int i = 0; i < 30; ++i) {
    leftKeys.push_back("key_" + std::to_string(rnd() % 30));
    lv.push_back(i);
  }
  for (int i = 0; i < 40; ++i) {
    rightKeys.push_back("key_" + std::to_string(rnd() % 20));
  }

  TKernelCompiler compiler;
  auto spec = NKernel::BuildJoinKernelSpec(leftType, rightType, {{"lk", "rk"}},
                                           EJoinType::LeftAnti);
  auto kernels = compiler.CompileJoin(spec);

  TStringColumn lk(leftKeys);
  std::vector<TColumn> lcols = {
      lk.Column(), TColumn{.Data = reinterpret_cast<char *>(lv.data())}};
  TRowSet lbatch{.Columns = lcols.data(),
                 .ColumnCount = 2,
                 .RowCount = static_cast<int64_t>(leftKeys.size()),
                 .RefCount = 1};

  TStringColumn rk(rightKeys);
  std::vector<TColumn> rcols = {rk.Column()};
  TRowSet rbatch{.Columns = rcols.data(),
                 .ColumnCount = 1,
                 .RowCount = static_cast<int64_t>(rightKeys.size()),
                 .RefCount = 1};

  THashTable left{}, right{};
  TPairBuffer pairs{};
  ASSERT_TRUE(kernels.Dispatch(&left, &right, nullptr, 0, &pairs, nullptr,
                               nullptr, 8, JoinOpCode(EJoinKernelOp::Init)));
  const auto initialCapacity = right.Capacity;
  ASSERT_TRUE(kernels.Dispatch(&left, &right, &lbatch, 0, &pairs, &lbatch,
                               nullptr, 0,
                               JoinOpCode(EJoinKernelOp::UpdateLeft)));
  // Right batches are released immediately by the host, so the kernel must
  // clone right keys into owned storage; scribbling over the source payload
  // after the update proves it.
  ASSERT_TRUE(kernels.Dispatch(&left, &right, &rbatch, -1, &pairs, &lbatch,
                               nullptr, 0,
                               JoinOpCode(EJoinKernelOp::UpdateRight)));
  std::fill(rk.Bytes.begin(), rk.Bytes.end(), '#');
  EXPECT_GT(right.Capacity, initialCapacity); // right keys survived rehash

  ASSERT_TRUE(kernels.Dispatch(&left, &right, nullptr, 0, &pairs, &lbatch,
                               nullptr, 0,
                               JoinOpCode(EJoinKernelOp::Finalize)));

  TRowSet out{};
  std::vector<std::pair<std::string, int64_t>> got;
  if (pairs.Count > 0) {
    ASSERT_EQ(kernels.Materialize(&pairs, &lbatch, nullptr, &lbatch, nullptr, 0,
                                  1000, &out),
              pairs.Count);
    for (int64_t i = 0; i < out.RowCount; ++i) {
      got.emplace_back(
          ReadString(out.Columns[0], i),
          reinterpret_cast<const int64_t *>(out.Columns[1].Data)[i]);
    }
    FreeMaterializedRowSet(out);
  }

  std::unordered_set<std::string> rset(rightKeys.begin(), rightKeys.end());
  std::vector<std::pair<std::string, int64_t>> expected;
  for (size_t i = 0; i < leftKeys.size(); ++i) {
    if (!rset.count(leftKeys[i])) {
      expected.emplace_back(leftKeys[i], lv[i]);
    }
  }
  std::sort(got.begin(), got.end());
  std::sort(expected.begin(), expected.end());
  EXPECT_EQ(got, expected);

  kernels.Dispatch(&left, &right, nullptr, 0, &pairs, nullptr, nullptr, 0,
                   JoinOpCode(EJoinKernelOp::Destroy));
}

TEST(JoinStringKey, CompositeStringIntKey) {
  using namespace NQumir::NAst;
  auto i64 = std::make_shared<TIntegerType>(TIntegerType::I64);
  TStructType leftType(
      {{"lk", std::make_shared<TStringType>()}, {"ln", i64}, {"lv", i64}});
  TStructType rightType(
      {{"rk", std::make_shared<TStringType>()}, {"rn", i64}, {"rv", i64}});

  TKernelCompiler compiler;
  auto spec = NKernel::BuildJoinKernelSpec(
      leftType, rightType, {{"lk", "rk"}, {"ln", "rn"}}, EJoinType::Inner);
  auto kernels = compiler.CompileJoin(spec);

  TStringColumn lk({"a", "a", "b"});
  std::vector<int64_t> ln = {1, 2, 1};
  std::vector<int64_t> lv = {10, 20, 30};
  std::vector<TColumn> lcols = {
      lk.Column(), TColumn{.Data = reinterpret_cast<char *>(ln.data())},
      TColumn{.Data = reinterpret_cast<char *>(lv.data())}};
  TRowSet lbatch{
      .Columns = lcols.data(), .ColumnCount = 3, .RowCount = 3, .RefCount = 1};

  TStringColumn rk({"a", "b", "a"});
  std::vector<int64_t> rn = {1, 1, 3};
  std::vector<int64_t> rv = {100, 200, 300};
  std::vector<TColumn> rcols = {
      rk.Column(), TColumn{.Data = reinterpret_cast<char *>(rn.data())},
      TColumn{.Data = reinterpret_cast<char *>(rv.data())}};
  TRowSet rbatch{
      .Columns = rcols.data(), .ColumnCount = 3, .RowCount = 3, .RefCount = 1};

  THashTable left{}, right{};
  TPairBuffer pairs{};
  ASSERT_TRUE(kernels.Dispatch(&left, &right, nullptr, 0, &pairs, nullptr,
                               nullptr, 8, JoinOpCode(EJoinKernelOp::Init)));
  ASSERT_TRUE(kernels.Dispatch(&left, &right, &lbatch, 0, &pairs, &lbatch,
                               &rbatch, 0,
                               JoinOpCode(EJoinKernelOp::UpdateLeft)));
  ASSERT_TRUE(kernels.Dispatch(&left, &right, &rbatch, 0, &pairs, &lbatch,
                               &rbatch, 0,
                               JoinOpCode(EJoinKernelOp::UpdateRight)));

  TRowSet out{};
  ASSERT_EQ(kernels.Materialize(&pairs, &lbatch, &rbatch, &lbatch, &rbatch, 0,
                                100, &out),
            2);
  std::vector<std::tuple<std::string, int64_t, int64_t, int64_t>> got;
  for (int64_t i = 0; i < out.RowCount; ++i) {
    got.emplace_back(ReadString(out.Columns[0], i),
                     reinterpret_cast<const int64_t *>(out.Columns[1].Data)[i],
                     reinterpret_cast<const int64_t *>(out.Columns[2].Data)[i],
                     reinterpret_cast<const int64_t *>(out.Columns[5].Data)[i]);
  }
  std::vector<std::tuple<std::string, int64_t, int64_t, int64_t>> expected = {
      {"a", 1, 10, 100}, {"b", 1, 30, 200}};
  std::sort(got.begin(), got.end());
  EXPECT_EQ(got, expected);
  FreeMaterializedRowSet(out);

  kernels.Dispatch(&left, &right, nullptr, 0, &pairs, nullptr, nullptr, 0,
                   JoinOpCode(EJoinKernelOp::Destroy));
}

int main(int argc, char **argv) {
  testing::InitGoogleTest(&argc, argv);
  NQumir::NCodeGen::TLLVMInitializer initializer;
  return RUN_ALL_TESTS();
}
