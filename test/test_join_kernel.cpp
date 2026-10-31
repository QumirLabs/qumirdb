#include <gtest/gtest.h>

#include <qdb/io/io.h>
#include <qdb/exec/row_join_exec.h>
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
#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
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
    uint8_t* Keys = nullptr;
    uint8_t* Ctrl = nullptr;
    int64_t* SlotId = nullptr;
    uint8_t* GroupKeys = nullptr;
    int64_t** AggBuffers = nullptr;
    uint8_t** OwnedBlocks = nullptr;
    int64_t OwnedBlockCount = 0;
    int64_t OwnedBlockCapacity = 0;
    int64_t Capacity = 0;
    int64_t Size = 0;
    int64_t NumAggs = 0;
    int64_t NumKeys = 0;
    int64_t KeySize = 0;
    // The row layout of an aggregate, which a join does not use.
    std::array<uint8_t, TKernelCompiler::RowArenaSize + TKernelCompiler::RowTableSize
        + TKernelCompiler::RowScratchSize> RowLayout = {};
    int64_t RowSwitchGroups = 0;
    int64_t RowExpectedGroups = 0;
    int64_t RowRebuilds = 0;
    int64_t RowMovedFromSlots = 0;
};
static_assert(sizeof(THashTable) == TKernelCompiler::HashTableSize);

struct TPairBuffer {
    int64_t Count = 0;
    int64_t Capacity = 0;
    int64_t* Data = nullptr;
};
static_assert(sizeof(TPairBuffer) == TKernelCompiler::PairBufferSize);

constexpr int64_t JoinOpCode(EJoinKernelOp op) {
    return static_cast<int64_t>(op);
}

NQumir::NAst::TExprPtr ParseExpr(const std::string& src) {
    std::istringstream in(src);
    NQumir::NAst::NCore::TTokenStream tokens(in);
    NQumir::NAst::NCore::TParser parser;
    auto parsed = parser.Parse(tokens);
    if (!parsed) {
        throw std::runtime_error(parsed.error().ToString());
    }
    return *parsed;
}

std::unique_ptr<NQumir::TLLVMRunner> CompileJoinEntry(const std::string& entryName, void*& entry) {
    using namespace NQumir::NAst;
    auto lib = NKernel::BuildJoinKernelLibrary();
    if (!lib) {
        ADD_FAILURE() << lib.error().ToString();
        return {};
    }
    auto i64 = std::make_shared<TIntegerType>(TIntegerType::I64);
    TStructType leftType({{"lk", i64}});
    TStructType rightType({{"rk", i64}});
    auto keyDesc = NKernel::BuildJoinKeyDescriptor(leftType, rightType, {{"lk", "rk"}});
    std::vector<TExprPtr> stmts;
    for (auto& f : NKernel::GenJoinKeyTypeDecls(keyDesc)) {
        stmts.push_back(std::move(f));
    }
    for (auto& f : NKernel::GenJoinKeyOpsFunDecls(keyDesc)) {
        stmts.push_back(std::move(f));
    }
    for (auto& f : NKernel::GenJoinKeyOwnershipFunDecls(keyDesc)) {
        stmts.push_back(std::move(f));
    }
    auto dualLib = NKernel::BuildJoinDualKeyLibrary();
    if (!dualLib) {
        ADD_FAILURE() << dualLib.error().ToString();
        return {};
    }
    for (auto& f : *dualLib) {
        stmts.push_back(std::move(f));
    }
    for (auto& f : *lib) {
        stmts.push_back(std::move(f));
    }

    NQumir::TLLVMRunnerOptions options;
    options.CoreInput = true;
    options.NativeCode = true;
    options.AllowOverloads = true;
    NQdb::NTest::ConfigureQumirDbSourceModule(options);
    auto runner = std::make_unique<NQumir::TLLVMRunner>(options);
    auto program =
        std::make_shared<NQumir::NAst::TBlockExpr>(NQumir::TLocation{}, std::move(stmts));
    NQdb::NTest::AddQumirDbUse(program);
    std::string error;
    entry = runner->CompileKernelAst(program, entryName, &error);
    EXPECT_NE(entry, nullptr) << error;
    return runner;
}

} // namespace

TEST(JoinKernel, PairBufferLayout) {
    std::shared_ptr<NQumir::NAst::TStructType> pairBuffer;
    std::shared_ptr<NQumir::NAst::TStructType> joinTable;
    for (const auto& type : NQumir::NRegistry::QumirDbExternalTypes()) {
        if (type.Name == "PairBuffer") {
            pairBuffer = NQumir::NAst::TMaybeType<NQumir::NAst::TStructType>(type.Type).Cast();
        } else if (type.Name == "RowTable") {
            joinTable = NQumir::NAst::TMaybeType<NQumir::NAst::TStructType>(type.Type).Cast();
        }
    }
    ASSERT_NE(pairBuffer, nullptr);
    ASSERT_EQ(pairBuffer->Fields.size(), 3u);
    EXPECT_EQ(pairBuffer->Fields[2].first, "Data");
    // The symmetric join reuses HashTable; the row join table (join_rows.oz)
    // has its own entry table type, mirrored by TRowJoinProcessor.
    ASSERT_NE(joinTable, nullptr);
    ASSERT_EQ(joinTable->Fields.size(), 6u);
    EXPECT_EQ(joinTable->Fields[0].first, "Entries");
}

TEST(JoinKernel, MaterializeLibraryMarksPhysicalWorkersCacheable) {
    auto library = NKernel::BuildJoinKernelLibrary();
    ASSERT_TRUE(library) << library.error().ToString();

    std::unordered_map<std::string, std::shared_ptr<NQumir::NAst::TFunDecl>> functions;
    for (const auto& stmt : *library) {
        auto fun = NQumir::NAst::TMaybeNode<NQumir::NAst::TFunDecl>(stmt);
        if (fun) {
            functions.emplace(fun.Cast()->Name, fun.Cast());
        }
    }

    for (
        const auto* name :
        {
            "jm_begin",
            "jm_record_owner",
            "jt_materialize_bool_column",
            "jt_materialize_binint_column",
            "jt_materialize_string_column",
        })
    {
        ASSERT_TRUE(functions.contains(name)) << name;
        EXPECT_TRUE(functions.at(name)->Cacheable) << name;
    }
    ASSERT_TRUE(functions.contains("jt_materialize_fixed_column"));
    EXPECT_FALSE(functions.at("jt_materialize_fixed_column")->GenericParams.empty());
}

TEST(JoinKernel, InitReusesHashTableAndDestroyClears) {
    void* initEntry = nullptr;
    auto initRunner = CompileJoinEntry("jt_init", initEntry);
    ASSERT_NE(initEntry, nullptr);
    void* destroyEntry = nullptr;
    auto destroyRunner = CompileJoinEntry("jt_destroy", destroyEntry);
    ASSERT_NE(destroyEntry, nullptr);
    auto jtInit = reinterpret_cast<bool (*)(void*, int64_t, int64_t)>(initEntry);
    auto jtDestroy = reinterpret_cast<void (*)(void*)>(destroyEntry);

    void* widthEntry = nullptr;
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
    void* initEntry = nullptr;
    auto initRunner = CompileJoinEntry("jt_init", initEntry);
    void* destroyEntry = nullptr;
    auto destroyRunner = CompileJoinEntry("jt_destroy", destroyEntry);
    void* appendEntry = nullptr;
    auto appendRunner = CompileJoinEntry("jb_append", appendEntry);
    ASSERT_NE(appendEntry, nullptr);
    auto jtInit = reinterpret_cast<bool (*)(void*, int64_t, int64_t)>(initEntry);
    auto jtDestroy = reinterpret_cast<void (*)(void*)>(destroyEntry);
    auto jbAppend = reinterpret_cast<bool (*)(void*, int64_t, int64_t)>(appendEntry);

    THashTable ht{};
    ASSERT_TRUE(jtInit(&ht, 8, 8));
    const int64_t slot = 3;
    for (int64_t i = 0; i < 10; ++i) {
        ASSERT_TRUE(jbAppend(&ht, slot, 1000 + i));
    }
    EXPECT_EQ(ht.AggBuffers[0][slot], 10);
    EXPECT_EQ(ht.AggBuffers[1][slot], 13); // 4 -> 6 -> 9 -> 13 (1.5x growth)
    auto* bucket = reinterpret_cast<int64_t*>(ht.AggBuffers[2][slot]);
    ASSERT_NE(bucket, nullptr);
    for (int64_t i = 0; i < 10; ++i) {
        EXPECT_EQ(bucket[i], 1000 + i);
    }
    EXPECT_EQ(ht.AggBuffers[0][0], 0); // untouched slot
    jtDestroy(&ht);
}

TEST(JoinKernel, PairBufferPushGrowsAndStoresPairs) {
    void* pushEntry = nullptr;
    auto pushRunner = CompileJoinEntry("pb_push", pushEntry);
    void* destroyEntry = nullptr;
    auto destroyRunner = CompileJoinEntry("pb_destroy", destroyEntry);
    auto pbPush = reinterpret_cast<bool (*)(void*, int64_t, int64_t)>(pushEntry);
    auto pbDestroy = reinterpret_cast<void (*)(void*)>(destroyEntry);

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
    auto i64 = [] {
        return std::make_shared<TIntegerType>(TIntegerType::I64);
    };
    TStructType leftType({{"lv", i64()}, {"lk", i64()}});
    TStructType rightType({{"rv", i64()}, {"rk", i64()}});

    TKernelCompiler compiler;
    auto spec = NKernel::BuildJoinKernelSpec(leftType, rightType, {{"lk", "rk"}}, EJoinType::Inner);
    auto kernels = compiler.CompileJoin(spec);

    std::vector<int64_t> lkeys = {1, 2, 1};
    std::vector<int64_t> rkeys = {1, 1, 3};
    std::vector<int64_t> lvalues = {10, 20, 30};
    std::vector<int64_t> rvalues = {100, 200, 300};
    auto makeBatch =
        [](std::vector<int64_t>& keys, std::vector<int64_t>& values, std::vector<TColumn>& cols) {
            cols = {
                TColumn{.Data = reinterpret_cast<char*>(values.data())},
                TColumn{.Data = reinterpret_cast<char*>(keys.data())},
            };
            return TRowSet{
                .Columns = cols.data(),
                .ColumnCount = 2,
                .RowCount = static_cast<int64_t>(keys.size()),
                .RefCount = 1,
            };
        };
    std::vector<TColumn> lcols, rcols;
    TRowSet lbatch = makeBatch(lkeys, lvalues, lcols);
    TRowSet rbatch = makeBatch(rkeys, rvalues, rcols);

    THashTable left{}, right{};
    TPairBuffer pairs{};
    ASSERT_TRUE(kernels.Dispatch(
        &left,
        &right,
        nullptr,
        0,
        &pairs,
        nullptr,
        nullptr,
        8,
        JoinOpCode(EJoinKernelOp::Init)));
    // left_store / right_store are single-batch arrays (batch_idx 0). The default
    // (always-true) jt_residual_filter ignores them.
    ASSERT_TRUE(kernels.Dispatch(
        &left,
        &right,
        &lbatch,
        0,
        &pairs,
        &lbatch,
        &rbatch,
        0,
        JoinOpCode(EJoinKernelOp::UpdateLeft)));
    ASSERT_TRUE(kernels.Dispatch(
        &left,
        &right,
        &rbatch,
        0,
        &pairs,
        &lbatch,
        &rbatch,
        0,
        JoinOpCode(EJoinKernelOp::UpdateRight)));

    std::vector<std::tuple<int64_t, int64_t>> got;
    for (int64_t i = 0; i < pairs.Count; ++i) {
        got.emplace_back(pairs.Data[2 * i] & 0xffffffff, pairs.Data[2 * i + 1] & 0xffffffff);
    }
    std::vector<std::tuple<int64_t, int64_t>> expected = {{0, 0}, {0, 1}, {2, 0}, {2, 1}};
    std::sort(got.begin(), got.end());
    EXPECT_EQ(got, expected);

    kernels.Dispatch(
        &left,
        &right,
        nullptr,
        0,
        &pairs,
        nullptr,
        nullptr,
        0,
        JoinOpCode(EJoinKernelOp::Destroy));
}

TEST(CompileJoin, NormalizesNullableResidualIfBranch) {
    using namespace NQumir::NAst;
    auto i64 = [] {
        return std::make_shared<TIntegerType>(TIntegerType::I64);
    };
    auto f64 = [] {
        return std::make_shared<TFloatType>();
    };
    TStructType leftType({{"lk", i64()}, {"lv", f64()}});
    TStructType rightType({{"rk", i64()}, {"rv", std::make_shared<TNullable>(f64())}});

    auto spec = NKernel::BuildJoinKernelSpec(
        leftType,
        rightType,
        {{"lk", "rk"}},
        EJoinType::Inner,
        ParseExpr("(> (if (< lv (: 10.0 f64)) lv rv) (: 0.0 f64))"));

    TKernelCompiler compiler;
    auto kernels = compiler.CompileJoin(spec);
    EXPECT_TRUE(static_cast<bool>(kernels.Dispatch));
}

TEST(CompileJoin, RejectsGeneralOuterFinalizationWithResidual) {
    using namespace NQumir::NAst;
    auto i64 = [] {
        return std::make_shared<TIntegerType>(TIntegerType::I64);
    };
    TStructType leftType({{"lk", i64()}, {"lv", i64()}});
    TStructType rightType({{"rk", i64()}, {"rv", i64()}});
    TKernelCompiler compiler;

    for (auto type : {EJoinType::Left, EJoinType::Right}) {
        SCOPED_TRACE(static_cast<int>(type));
        auto spec = NKernel::BuildJoinKernelSpec(
            leftType,
            rightType,
            {{"lk", "rk"}},
            type,
            ParseExpr("(< lv rv)"));
        auto kernels = compiler.CompileJoin(spec);
        THashTable left{}, right{};
        TPairBuffer pairs{};
        ASSERT_TRUE(kernels.Dispatch(
            &left,
            &right,
            nullptr,
            0,
            &pairs,
            nullptr,
            nullptr,
            8,
            JoinOpCode(EJoinKernelOp::Init)));
        EXPECT_FALSE(kernels.Dispatch(
            &left,
            &right,
            nullptr,
            0,
            &pairs,
            nullptr,
            nullptr,
            0,
            JoinOpCode(EJoinKernelOp::Finalize)));
        EXPECT_TRUE(kernels.Dispatch(
            &left,
            &right,
            nullptr,
            0,
            &pairs,
            nullptr,
            nullptr,
            0,
            JoinOpCode(EJoinKernelOp::FinalizeStream)));
        EXPECT_EQ(pairs.Count, 0);
        EXPECT_TRUE(kernels.Dispatch(
            &left,
            &right,
            nullptr,
            0,
            &pairs,
            nullptr,
            nullptr,
            0,
            JoinOpCode(EJoinKernelOp::Destroy)));
    }
}

TEST(CompileJoinHash, ProducesMatchingSideHashes) {
    using namespace NQumir::NAst;
    auto i64 = [] {
        return std::make_shared<TIntegerType>(TIntegerType::I64);
    };
    TStructType leftType({{"lk", i64()}, {"lv", i64()}});
    TStructType rightType({{"rv", i64()}, {"rk", i64()}});

    TKernelCompiler compiler;
    auto spec = NKernel::BuildJoinKernelSpec(leftType, rightType, {{"lk", "rk"}}, EJoinType::Inner);
    auto kernels = compiler.CompileJoinHash(spec);

    std::vector<int64_t> lkeys = {1, 2, 1, 4};
    std::vector<int64_t> lvalues = {10, 20, 30, 40};
    std::vector<int64_t> rvalues = {100, 200, 300, 400};
    std::vector<int64_t> rkeys = {1, 3, 2, 1};
    std::vector<TColumn> lcols = {
        TColumn{.Data = reinterpret_cast<char*>(lkeys.data())},
        TColumn{.Data = reinterpret_cast<char*>(lvalues.data())},
    };
    std::vector<TColumn> rcols = {
        TColumn{.Data = reinterpret_cast<char*>(rvalues.data())},
        TColumn{.Data = reinterpret_cast<char*>(rkeys.data())},
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

// An f32 key hashes its 4 canonical bytes: signed zeroes and NaN payloads
// meet, next to i32 and i64 key parts of a composite key.
TEST(CompileJoinHash, F32KeyCanonicalizesZeroAndNaN) {
    using namespace NQumir::NAst;
    auto f32 = [] { return std::make_shared<TFloatType>(TFloatType::F32); };
    auto i32 = [] { return std::make_shared<TIntegerType>(TIntegerType::I32); };
    auto i64 = [] { return std::make_shared<TIntegerType>(TIntegerType::I64); };
    TStructType leftType({{"lf", f32()}, {"li", i32()}, {"ll", i64()}});
    TStructType rightType({{"rf", f32()}, {"ri", i32()}, {"rl", i64()}});

    TKernelCompiler compiler;
    auto spec = NKernel::BuildJoinKernelSpec(
        leftType, rightType, {{"lf", "rf"}, {"li", "ri"}, {"ll", "rl"}}, EJoinType::Inner);
    auto kernels = compiler.CompileJoinHash(spec);

    const float nan1 = std::numeric_limits<float>::quiet_NaN();
    const float nan2 = std::bit_cast<float>(UINT32_C(0x7f800001));
    std::vector<float> lf = {0.0f, 1.5f, nan1, 1.5f};
    std::vector<float> rf = {-0.0f, 1.5f, nan2, -1.5f};
    std::vector<int32_t> li = {7, 7, 7, 7};
    std::vector<int64_t> ll = {9, 9, 9, 9};
    std::vector<TColumn> lcols = {
        TColumn{.Data = reinterpret_cast<char*>(lf.data())},
        TColumn{.Data = reinterpret_cast<char*>(li.data())},
        TColumn{.Data = reinterpret_cast<char*>(ll.data())},
    };
    std::vector<TColumn> rcols = {
        TColumn{.Data = reinterpret_cast<char*>(rf.data())},
        TColumn{.Data = reinterpret_cast<char*>(li.data())},
        TColumn{.Data = reinterpret_cast<char*>(ll.data())},
    };
    TRowSet lbatch{.Columns = lcols.data(), .ColumnCount = 3, .RowCount = 4, .RefCount = 1};
    TRowSet rbatch{.Columns = rcols.data(), .ColumnCount = 3, .RowCount = 4, .RefCount = 1};
    std::vector<uint64_t> leftHashes(4, 0);
    std::vector<uint64_t> rightHashes(4, 0);
    ASSERT_TRUE(kernels.Left(&lbatch, leftHashes.data()));
    ASSERT_TRUE(kernels.Right(&rbatch, rightHashes.data()));

    EXPECT_EQ(leftHashes[0], rightHashes[0]); // +0.0 and -0.0
    EXPECT_EQ(leftHashes[1], rightHashes[1]);
    EXPECT_EQ(leftHashes[2], rightHashes[2]); // two NaN payloads
    EXPECT_NE(leftHashes[3], rightHashes[3]); // 1.5 and -1.5
    EXPECT_NE(leftHashes[0], leftHashes[1]);
}

// Hash equality alone is not enough: the key compare must also treat signed
// zeroes and NaN payloads as equal.
TEST(CompileJoin, F32KeyMatchesSignedZeroAndNaN) {
    using namespace NQumir::NAst;
    auto f32 = [] { return std::make_shared<TFloatType>(TFloatType::F32); };
    auto i64 = [] { return std::make_shared<TIntegerType>(TIntegerType::I64); };
    TStructType leftType({{"lv", i64()}, {"lk", f32()}});
    TStructType rightType({{"rv", i64()}, {"rk", f32()}});

    TKernelCompiler compiler;
    auto spec = NKernel::BuildJoinKernelSpec(leftType, rightType, {{"lk", "rk"}}, EJoinType::Inner);
    auto kernels = compiler.CompileJoin(spec);

    const float nan1 = std::numeric_limits<float>::quiet_NaN();
    const float nan2 = std::bit_cast<float>(UINT32_C(0x7f800001));
    std::vector<float> lkeys = {0.0f, 1.5f, nan1, 2.5f};
    std::vector<float> rkeys = {-0.0f, nan2, 1.5f, -1.5f};
    std::vector<int64_t> lvalues = {10, 20, 30, 40};
    std::vector<int64_t> rvalues = {100, 200, 300, 400};
    auto makeBatch = [](std::vector<float>& keys, std::vector<int64_t>& values,
                        std::vector<TColumn>& cols) {
        cols = {
            TColumn{.Data = reinterpret_cast<char*>(values.data())},
            TColumn{.Data = reinterpret_cast<char*>(keys.data())},
        };
        return TRowSet{
            .Columns = cols.data(),
            .ColumnCount = 2,
            .RowCount = static_cast<int64_t>(keys.size()),
            .RefCount = 1,
        };
    };
    std::vector<TColumn> lcols, rcols;
    TRowSet lbatch = makeBatch(lkeys, lvalues, lcols);
    TRowSet rbatch = makeBatch(rkeys, rvalues, rcols);

    THashTable left{}, right{};
    TPairBuffer pairs{};
    ASSERT_TRUE(kernels.Dispatch(
        &left, &right, nullptr, 0, &pairs, nullptr, nullptr, 8,
        JoinOpCode(EJoinKernelOp::Init)));
    ASSERT_TRUE(kernels.Dispatch(
        &left, &right, &lbatch, 0, &pairs, &lbatch, &rbatch, 0,
        JoinOpCode(EJoinKernelOp::UpdateLeft)));
    ASSERT_TRUE(kernels.Dispatch(
        &left, &right, &rbatch, 0, &pairs, &lbatch, &rbatch, 0,
        JoinOpCode(EJoinKernelOp::UpdateRight)));

    std::vector<std::tuple<int64_t, int64_t>> got;
    for (int64_t i = 0; i < pairs.Count; ++i) {
        got.emplace_back(pairs.Data[2 * i] & 0xffffffff, pairs.Data[2 * i + 1] & 0xffffffff);
    }
    std::sort(got.begin(), got.end());
    // +0.0 = -0.0, 1.5 = 1.5, nan1 = nan2.
    std::vector<std::tuple<int64_t, int64_t>> expected = {{0, 0}, {1, 2}, {2, 1}};
    EXPECT_EQ(got, expected);

    kernels.Dispatch(
        &left, &right, nullptr, 0, &pairs, nullptr, nullptr, 0,
        JoinOpCode(EJoinKernelOp::Destroy));
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
        auto i64 = [] {
            return std::make_shared<TIntegerType>(TIntegerType::I64);
        };
        TStructType leftType({{"lk", i64()}});
        TStructType rightType({{"rk", i64()}});
        Spec_ = NKernel::BuildJoinKernelSpec(leftType, rightType, {{"lk", "rk"}}, EJoinType::Inner);
        Hash_ = TKernelCompiler().CompileJoinHash(Spec_);
    }

    // Hashes the side with its own kernel and attaches the result to the batch.
    void Prepare(TSide& side, const TJoinHashKernels::TDispatch& hash) {
        side.Column = TColumn{.Data = reinterpret_cast<char*>(side.Keys.data())};
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
    EXPECT_EQ(LeftHashes({0, 0, 0, 0, 0, 0}), std::vector<uint64_t>(6, NoHash_));
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
    ASSERT_TRUE(kernels.Dispatch(
        &leftTable,
        &rightTable,
        nullptr,
        0,
        &pairs,
        nullptr,
        nullptr,
        8,
        JoinOpCode(EJoinKernelOp::Init)));
    ASSERT_TRUE(kernels.Dispatch(
        &leftTable,
        &rightTable,
        &left.Batch,
        0,
        &pairs,
        &left.Batch,
        &right.Batch,
        0,
        JoinOpCode(EJoinKernelOp::UpdateLeft)));
    ASSERT_TRUE(kernels.Dispatch(
        &leftTable,
        &rightTable,
        &right.Batch,
        0,
        &pairs,
        &left.Batch,
        &right.Batch,
        0,
        JoinOpCode(EJoinKernelOp::UpdateRight)));

    std::vector<std::tuple<int64_t, int64_t>> got;
    for (int64_t i = 0; i < pairs.Count; ++i) {
        got.emplace_back(pairs.Data[2 * i] & 0xffffffff, pairs.Data[2 * i + 1] & 0xffffffff);
    }
    std::sort(got.begin(), got.end());
    EXPECT_EQ(got, (std::vector<std::tuple<int64_t, int64_t>>{{0, 3}, {2, 3}, {3, 2}}));

    kernels.Dispatch(
        &leftTable,
        &rightTable,
        nullptr,
        0,
        &pairs,
        nullptr,
        nullptr,
        0,
        JoinOpCode(EJoinKernelOp::Destroy));
}

TEST(CompileJoin, ProbeOnlyStreamsWithoutInserting) {
    using namespace NQumir::NAst;
    auto i64 = [] {
        return std::make_shared<TIntegerType>(TIntegerType::I64);
    };
    TStructType leftType({{"lk", i64()}, {"lv", i64()}});
    TStructType rightType({{"rk", i64()}, {"rv", i64()}});

    TKernelCompiler compiler;
    auto spec = NKernel::BuildJoinKernelSpec(leftType, rightType, {{"lk", "rk"}}, EJoinType::Inner);
    auto kernels = compiler.CompileJoin(spec);

    std::vector<int64_t> lkeys = {1, 2, 1};
    std::vector<int64_t> rkeys = {1, 1, 3};
    auto makeBatch = [](std::vector<int64_t>& keys, std::vector<TColumn>& cols) {
        cols = {
            TColumn{.Data = reinterpret_cast<char*>(keys.data())},
            TColumn{.Data = reinterpret_cast<char*>(keys.data())},
        };
        return TRowSet{
            .Columns = cols.data(),
            .ColumnCount = 2,
            .RowCount = static_cast<int64_t>(keys.size()),
            .RefCount = 1,
        };
    };
    std::vector<TColumn> lcols, rcols;
    TRowSet lbatch = makeBatch(lkeys, lcols);
    TRowSet rbatch = makeBatch(rkeys, rcols);

    THashTable left{}, right{};
    TPairBuffer pairs{};
    ASSERT_TRUE(kernels.Dispatch(
        &left,
        &right,
        nullptr,
        0,
        &pairs,
        nullptr,
        nullptr,
        8,
        JoinOpCode(EJoinKernelOp::Init)));
    ASSERT_TRUE(kernels.Dispatch(
        &left,
        &right,
        &lbatch,
        0,
        &pairs,
        &lbatch,
        &rbatch,
        0,
        JoinOpCode(EJoinKernelOp::UpdateLeft)));
    ASSERT_EQ(pairs.Count, 0);
    ASSERT_TRUE(kernels.Dispatch(
        &left,
        &right,
        &rbatch,
        7,
        &pairs,
        &lbatch,
        &rbatch,
        0,
        JoinOpCode(EJoinKernelOp::StreamRight)));
    EXPECT_EQ(right.Size, 0);

    std::vector<std::tuple<int64_t, int64_t>> got;
    for (int64_t i = 0; i < pairs.Count; ++i) {
        got.emplace_back(pairs.Data[2 * i] & 0xffffffff, pairs.Data[2 * i + 1] & 0xffffffff);
    }
    std::vector<std::tuple<int64_t, int64_t>> expected = {{0, 0}, {0, 1}, {2, 0}, {2, 1}};
    std::sort(got.begin(), got.end());
    EXPECT_EQ(got, expected);

    pairs.Count = 0;
    kernels.Dispatch(
        &left,
        &right,
        nullptr,
        0,
        &pairs,
        nullptr,
        nullptr,
        0,
        JoinOpCode(EJoinKernelOp::Destroy));
    THashTable left2{}, right2{};
    ASSERT_TRUE(kernels.Dispatch(
        &left2,
        &right2,
        nullptr,
        0,
        &pairs,
        nullptr,
        nullptr,
        8,
        JoinOpCode(EJoinKernelOp::Init)));
    ASSERT_TRUE(kernels.Dispatch(
        &left2,
        &right2,
        &rbatch,
        0,
        &pairs,
        &lbatch,
        &rbatch,
        0,
        JoinOpCode(EJoinKernelOp::UpdateRight)));
    ASSERT_EQ(pairs.Count, 0);
    ASSERT_TRUE(kernels.Dispatch(
        &left2,
        &right2,
        &lbatch,
        9,
        &pairs,
        &lbatch,
        &rbatch,
        0,
        JoinOpCode(EJoinKernelOp::StreamLeft)));
    EXPECT_EQ(left2.Size, 0);

    got.clear();
    for (int64_t i = 0; i < pairs.Count; ++i) {
        got.emplace_back(pairs.Data[2 * i] & 0xffffffff, pairs.Data[2 * i + 1] & 0xffffffff);
    }
    std::sort(got.begin(), got.end());
    EXPECT_EQ(got, expected);

    kernels.Dispatch(
        &left2,
        &right2,
        nullptr,
        0,
        &pairs,
        nullptr,
        nullptr,
        0,
        JoinOpCode(EJoinKernelOp::Destroy));
}

TEST(CompileJoin, AcceptsStringKeyAndRejectsUnsupportedSpecs) {
    using namespace NQumir::NAst;
    auto i64 = [] {
        return std::make_shared<TIntegerType>(TIntegerType::I64);
    };
    TKernelCompiler compiler;

    TStructType strLeft({{"lk", std::make_shared<TStringType>()}});
    TStructType strRight({{"rk", std::make_shared<TStringType>()}});
    auto stringSpec =
        NKernel::BuildJoinKernelSpec(strLeft, strRight, {{"lk", "rk"}}, EJoinType::Inner);
    EXPECT_NO_THROW(compiler.CompileJoin(stringSpec));

    TStructType leftType({{"lk", i64()}});
    TStructType rightType({{"rk", i64()}});
    // FULL OUTER is supported (left- + right-unmatched finalize).
    auto fullSpec =
        NKernel::BuildJoinKernelSpec(leftType, rightType, {{"lk", "rk"}}, EJoinType::Full);
    EXPECT_NO_THROW(compiler.CompileJoin(fullSpec));
    // Unsupported join type (RightSemi is not implemented yet).
    auto rightSemiSpec =
        NKernel::BuildJoinKernelSpec(leftType, rightType, {{"lk", "rk"}}, EJoinType::RightSemi);
    EXPECT_THROW(compiler.CompileJoin(rightSemiSpec), NQumir::TError);
    // Missing column.
    EXPECT_THROW(
        NKernel::BuildJoinKernelSpec(leftType, rightType, {{"missing", "rk"}}, EJoinType::Inner),
        std::runtime_error);
    // Incompatible key types (i64 vs string).
    auto incompatibleSpec =
        NKernel::BuildJoinKernelSpec(leftType, strRight, {{"lk", "rk"}}, EJoinType::Inner);
    EXPECT_THROW(compiler.CompileJoin(incompatibleSpec), NQumir::TError);
}

namespace {

// Compiles one entry of the GENERIC join program: key-ops overloads for the
// join key + the shared library + generated jt_process_left/right.
std::unique_ptr<NQumir::TLLVMRunner> CompileGenericJoin(
    const NKernel::TJoinKeyDescriptor& keyDesc,
    const std::string& entry,
    void*& fn)
{
    using namespace NQumir::NAst;
    TTypePtr columnType, rowSetType, hashTableType, pairBufferType, stringViewType;
    for (const auto& et : NQumir::NRegistry::QumirDbExternalTypes()) {
        if (et.Name == "TColumn") {
            columnType = et.Type;
        } else if (et.Name == "TRowSet") {
            rowSetType = et.Type;
        } else if (et.Name == "HashTable") {
            hashTableType = et.Type;
        } else if (et.Name == "PairBuffer") {
            pairBufferType = et.Type;
        } else if (et.Name == "StringView") {
            stringViewType = et.Type;
        }
    }
    auto lib = NKernel::BuildJoinKernelLibrary();
    if (!lib) {
        ADD_FAILURE() << lib.error().ToString();
        return {};
    }
    std::vector<TExprPtr> program;
    for (auto& f : NKernel::GenJoinKeyTypeDecls(keyDesc)) {
        program.push_back(std::move(f));
    }
    if (keyDesc.HasDistinctLookupType()) {
        auto stringOps =
            NKernel::ParseFunctionLibrary(NKernel::ReadAggregationKernel("string_ops.oz"));
        if (!stringOps) {
            ADD_FAILURE() << stringOps.error().ToString();
            return {};
        }
        for (auto& f : *stringOps) {
            program.push_back(std::move(f));
        }
    }
    for (auto& f : NKernel::GenJoinKeyOpsFunDecls(keyDesc)) {
        program.push_back(std::move(f));
    }
    for (auto& f : NKernel::GenJoinKeyOwnershipFunDecls(keyDesc)) {
        program.push_back(std::move(f));
    }
    auto dualLib = NKernel::BuildJoinDualKeyLibrary();
    if (!dualLib) {
        ADD_FAILURE() << dualLib.error().ToString();
        return {};
    }
    for (auto& f : *dualLib) {
        program.push_back(std::move(f));
    }
    for (auto& f : *lib) {
        program.push_back(std::move(f));
    }
    program.push_back(
        NKernel::GenJoinProcessAst(
            keyDesc,
            true,
            "jt_process_left",
            columnType,
            rowSetType,
            hashTableType,
            pairBufferType,
            stringViewType));
    program.push_back(
        NKernel::GenJoinProcessAst(
            keyDesc,
            false,
            "jt_process_right",
            columnType,
            rowSetType,
            hashTableType,
            pairBufferType,
            stringViewType));
    program.push_back(
        NKernel::GenJoinProbeAst(
            keyDesc,
            true,
            "jt_probe_left_stream",
            columnType,
            rowSetType,
            hashTableType,
            pairBufferType,
            stringViewType));
    program.push_back(
        NKernel::GenJoinProbeAst(
            keyDesc,
            false,
            "jt_probe_right_stream",
            columnType,
            rowSetType,
            hashTableType,
            pairBufferType,
            stringViewType));

    NQumir::TLLVMRunnerOptions options;
    options.CoreInput = true;
    options.NativeCode = true;
    options.AllowOverloads = true;
    NQdb::NTest::ConfigureQumirDbSourceModule(options);
    auto runner = std::make_unique<NQumir::TLLVMRunner>(options);
    auto prog = std::make_shared<TBlockExpr>(NQumir::TLocation{}, std::move(program));
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
    auto keyDesc = NKernel::BuildJoinKeyDescriptor(leftType, rightType, {{"lk", "rk"}});

    void* initFn = nullptr;
    auto r1 = CompileGenericJoin(keyDesc, "jt_init", initFn);
    void* destroyFn = nullptr;
    auto r2 = CompileGenericJoin(keyDesc, "jt_destroy", destroyFn);
    void* pbDestroyFn = nullptr;
    auto r3 = CompileGenericJoin(keyDesc, "pb_destroy", pbDestroyFn);
    void* leftFn = nullptr;
    auto r4 = CompileGenericJoin(keyDesc, "jt_process_left", leftFn);
    void* rightFn = nullptr;
    auto r5 = CompileGenericJoin(keyDesc, "jt_process_right", rightFn);
    ASSERT_NE(leftFn, nullptr);
    ASSERT_NE(rightFn, nullptr);

    auto jtInit = reinterpret_cast<bool (*)(void*, int64_t, int64_t)>(initFn);
    auto jtDestroy = reinterpret_cast<void (*)(void*)>(destroyFn);
    auto pbDestroy = reinterpret_cast<void (*)(void*)>(pbDestroyFn);
    auto procLeft = reinterpret_cast<
        bool (*)(void*, void*, TRowSet*, const int64_t*, int64_t, void*, TRowSet*, TRowSet*)>(
        leftFn);
    auto procRight = reinterpret_cast<
        bool (*)(void*, void*, TRowSet*, const int64_t*, int64_t, void*, TRowSet*, TRowSet*)>(
        rightFn);

    // int32 keys, i64 values.
    std::vector<int32_t> lk = {1, 2, 1};
    std::vector<int64_t> lv = {10, 20, 30};
    std::vector<int32_t> rk = {1, 1, 3};
    std::vector<int64_t> rv = {100, 200, 300};
    auto batch = [](int32_t* keys, int64_t* vals, int64_t rows, std::vector<TColumn>& cols) {
        cols = {
            TColumn{.Data = reinterpret_cast<char*>(keys)},
            TColumn{.Data = reinterpret_cast<char*>(vals)},
        };
        return TRowSet{.Columns = cols.data(), .ColumnCount = 2, .RowCount = rows, .RefCount = 1};
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
    ASSERT_TRUE(procLeft(&left, &right, &lbatch, leftKeyColumns, 0, &pairs, &lbatch, &rbatch));
    ASSERT_TRUE(procRight(&right, &left, &rbatch, rightKeyColumns, 0, &pairs, &lbatch, &rbatch));

    std::vector<std::tuple<int64_t, int64_t>> got;
    for (int64_t i = 0; i < pairs.Count; ++i) {
        got.emplace_back(pairs.Data[2 * i] & 0xffffffff, pairs.Data[2 * i + 1] & 0xffffffff);
    }
    std::vector<std::tuple<int64_t, int64_t>> expected = {{0, 0}, {0, 1}, {2, 0}, {2, 1}};
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
    auto keyDesc = NKernel::BuildJoinKeyDescriptor(leftType, rightType, {{"lk", "rk"}});

    void* initFn = nullptr;
    auto r1 = CompileGenericJoin(keyDesc, "jt_init", initFn);
    void* destroyFn = nullptr;
    auto r2 = CompileGenericJoin(keyDesc, "jt_destroy", destroyFn);
    void* pbDestroyFn = nullptr;
    auto r3 = CompileGenericJoin(keyDesc, "pb_destroy", pbDestroyFn);
    void* leftFn = nullptr;
    auto r4 = CompileGenericJoin(keyDesc, "jt_process_left", leftFn);
    void* rightFn = nullptr;
    auto r5 = CompileGenericJoin(keyDesc, "jt_process_right", rightFn);
    ASSERT_NE(leftFn, nullptr);
    ASSERT_NE(rightFn, nullptr);

    auto jtInit = reinterpret_cast<bool (*)(void*, int64_t, int64_t)>(initFn);
    auto jtDestroy = reinterpret_cast<void (*)(void*)>(destroyFn);
    auto pbDestroy = reinterpret_cast<void (*)(void*)>(pbDestroyFn);
    auto procLeft = reinterpret_cast<
        bool (*)(void*, void*, TRowSet*, const int64_t*, int64_t, void*, TRowSet*, TRowSet*)>(
        leftFn);
    auto procRight = reinterpret_cast<
        bool (*)(void*, void*, TRowSet*, const int64_t*, int64_t, void*, TRowSet*, TRowSet*)>(
        rightFn);

    // Enough distinct keys to rehash with either 8-slot or 16-slot groups.
    // Unsigned arithmetic keeps the generator's wraparound well-defined.
    uint32_t seed = 12345;
    auto rnd = [&]() {
        seed = (seed * 1103515245u + 12345u) & 0x7fffffffu;
        return seed;
    };
    std::vector<int32_t> lk, rk;
    for (int i = 0; i < 60; ++i) {
        lk.push_back(rnd() % 25);
    }
    for (int i = 0; i < 60; ++i) {
        rk.push_back(rnd() % 25);
    }
    auto batch = [](std::vector<int32_t>& keys, std::vector<TColumn>& cols) {
        cols = {TColumn{.Data = reinterpret_cast<char*>(keys.data())}};
        return TRowSet{
            .Columns = cols.data(),
            .ColumnCount = 1,
            .RowCount = static_cast<int64_t>(keys.size()),
            .RefCount = 1,
        };
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
    ASSERT_TRUE(procLeft(&left, &right, &lbatch, leftKeyColumns, 0, &pairs, &lbatch, &rbatch));
    ASSERT_TRUE(procRight(&right, &left, &rbatch, rightKeyColumns, 0, &pairs, &lbatch, &rbatch));

    int64_t matched = 0;
    for (int64_t i = 0; i < pairs.Count; ++i) {
        int64_t leftRow = pairs.Data[2 * i] & 0xffffffff;
        int64_t rightRow = pairs.Data[2 * i + 1] & 0xffffffff;
        ASSERT_EQ(lk[leftRow],
            rk[rightRow]); // every emitted pair has matching keys
        ++matched;
    }
    int64_t expected = 0;
    for (int32_t a : lk) {
        for (int32_t b : rk) {
            if (a == b) {
                ++expected;
            }
        }
    }
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
void FreeMaterializedRowSet(TRowSet& rowSet) {
    auto* owners = static_cast<int64_t*>(rowSet.Private);
    ASSERT_NE(owners, nullptr);
    for (int64_t i = 0; i < owners[0]; ++i) {
        qdb_free(reinterpret_cast<void*>(owners[i + 1]));
    }
    qdb_free(owners);
    rowSet.Private = nullptr;
}

bool MaskBit(const TColumn& col, int64_t row) {
    return ((col.Mask[row / 8] >> (row % 8)) & 1) != 0;
}

} // namespace

TEST(JoinMaterialize, GathersFixedAndStringColumnsWithNullPadding) {
    using namespace NQumir::NAst;
    auto i64 = std::make_shared<TIntegerType>(TIntegerType::I64);
    auto i32 = std::make_shared<TIntegerType>(TIntegerType::I32);
    TStructType leftType({{"lk", i64}, {"lname", std::make_shared<TStringType>()}});
    TStructType rightType({{"rk", i64}, {"rv", i32}});

    TKernelCompiler compiler;
    auto spec = NKernel::BuildJoinKernelSpec(leftType, rightType, {{"lk", "rk"}}, EJoinType::Left);
    auto kernels = compiler.CompileJoin(spec);

    // Left store: one batch of 3 rows with a string payload column.
    std::vector<int64_t> lk = {1, 2, 3};
    std::string lnameBytes = "abb"; // "a", "bb", ""
    std::vector<int64_t> lnameOffsets = {0, 1, 3, 3};
    std::vector<TColumn> lcols = {
        TColumn{.Data = reinterpret_cast<char*>(lk.data())},
        TColumn{.Data = lnameBytes.data(), .Offsets = lnameOffsets.data(), .OffsetWidth = 8},
    };
    TRowSet lbatch{.Columns = lcols.data(), .ColumnCount = 2, .RowCount = 3, .RefCount = 1};

    // Right store: one batch of 2 rows with an i32 payload column.
    std::vector<int64_t> rk = {1, 2};
    std::vector<int32_t> rv = {10, 20};
    std::vector<TColumn> rcols = {
        TColumn{.Data = reinterpret_cast<char*>(rk.data())},
        TColumn{.Data = reinterpret_cast<char*>(rv.data())},
    };
    TRowSet rbatch{.Columns = rcols.data(), .ColumnCount = 2, .RowCount = 2, .RefCount = 1};

    // Hand-built pairs: two matches plus one outer-padded left row (right -1).
    std::vector<int64_t> pairData = {
        PackRowId(0, 0),
        PackRowId(0, 0),
        PackRowId(0, 1),
        PackRowId(0, 1),
        PackRowId(0, 2),
        -1,
    };
    TPairBuffer pairs{.Count = 3, .Capacity = 3, .Data = pairData.data()};

    TRowSet out{};
    ASSERT_EQ(kernels.Materialize(&pairs, &lbatch, &rbatch, &lbatch, &rbatch, 0, 100, &out), 3);
    ASSERT_EQ(out.RowCount, 3);
    ASSERT_EQ(out.ColumnCount, 4);
    EXPECT_EQ(out.Selection, nullptr);
    EXPECT_EQ(out.RefCount, 1);

    // lk: all rows valid.
    const auto* outLk = reinterpret_cast<const int64_t*>(out.Columns[0].Data);
    EXPECT_EQ(outLk[0], 1);
    EXPECT_EQ(outLk[1], 2);
    EXPECT_EQ(outLk[2], 3);
    for (int64_t j = 0; j < 3; ++j) {
        EXPECT_TRUE(MaskBit(out.Columns[0], j));
    }

    // lname: string payloads follow pair order.
    const auto& lname = out.Columns[1];
    ASSERT_EQ(lname.OffsetWidth, 8);
    const auto* offsets = static_cast<const int64_t*>(lname.Offsets);
    EXPECT_EQ(std::string(lname.Data + offsets[0], lname.Data + offsets[1]), "a");
    EXPECT_EQ(std::string(lname.Data + offsets[1], lname.Data + offsets[2]), "bb");
    EXPECT_EQ(offsets[3], offsets[2]); // ""

    // rv (i32): matched rows carry values, the padded row is NULL.
    const auto* outRv = reinterpret_cast<const int32_t*>(out.Columns[3].Data);
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
    ASSERT_EQ(kernels.Materialize(&pairs, &lbatch, &rbatch, &lbatch, &rbatch, 1, 1, &slice), 1);
    ASSERT_EQ(slice.RowCount, 1);
    EXPECT_EQ(reinterpret_cast<const int64_t*>(slice.Columns[0].Data)[0], 2);
    EXPECT_EQ(reinterpret_cast<const int32_t*>(slice.Columns[3].Data)[0], 20);
    FreeMaterializedRowSet(slice);

    // Exhausted / empty cursor positions leave the output untouched.
    TRowSet empty{};
    EXPECT_EQ(kernels.Materialize(&pairs, &lbatch, &rbatch, &lbatch, &rbatch, 3, 100, &empty), 0);
    EXPECT_EQ(empty.Columns, nullptr);
    TPairBuffer noPairs{};
    EXPECT_EQ(kernels.Materialize(&noPairs, &lbatch, &rbatch, &lbatch, &rbatch, 0, 100, &empty), 0);
}

TEST(JoinMaterialize, PropagatesSourceNullsAndReadsStreamBatches) {
    using namespace NQumir::NAst;
    auto i64 = std::make_shared<TIntegerType>(TIntegerType::I64);
    TStructType leftType({{"lk", i64}, {"lv", std::make_shared<TNullable>(i64)}});
    TStructType rightType({{"rk", i64}});

    TKernelCompiler compiler;
    auto spec = NKernel::BuildJoinKernelSpec(leftType, rightType, {{"lk", "rk"}}, EJoinType::Inner);
    auto kernels = compiler.CompileJoin(spec);

    // Left store: 2 rows; lv row 1 is NULL at the source.
    std::vector<int64_t> lk = {1, 2};
    std::vector<int64_t> lv = {100, 200};
    std::vector<uint8_t> lvMask = {0b01}; // row 0 valid, row 1 null
    std::vector<TColumn> lcols = {
        TColumn{.Data = reinterpret_cast<char*>(lk.data())},
        TColumn{.Data = reinterpret_cast<char*>(lv.data()), .Mask = lvMask.data()},
    };
    TRowSet lbatch{.Columns = lcols.data(), .ColumnCount = 2, .RowCount = 2, .RefCount = 1};

    // Right store batch and a DIFFERENT right stream batch: pairs with a
    // stream-encoded right id (batch -1) must read the stream batch.
    std::vector<int64_t> rkStore = {7};
    std::vector<TColumn> rcolsStore = {TColumn{.Data = reinterpret_cast<char*>(rkStore.data())}};
    TRowSet rstore{.Columns = rcolsStore.data(), .ColumnCount = 1, .RowCount = 1, .RefCount = 1};
    std::vector<int64_t> rkStream = {42, 43};
    std::vector<TColumn> rcolsStream = {TColumn{.Data = reinterpret_cast<char*>(rkStream.data())}};
    TRowSet rstream{.Columns = rcolsStream.data(), .ColumnCount = 1, .RowCount = 2, .RefCount = 1};

    std::vector<int64_t> pairData = {
        PackRowId(0, 0),
        PackRowId(0, 0), // store right row -> 7
        PackRowId(0, 1),
        PackRowId(-1, 1), // stream right row -> 43
    };
    TPairBuffer pairs{.Count = 2, .Capacity = 2, .Data = pairData.data()};

    TRowSet out{};
    ASSERT_EQ(kernels.Materialize(&pairs, &lbatch, &rstore, &lbatch, &rstream, 0, 100, &out), 2);
    ASSERT_EQ(out.RowCount, 2);
    ASSERT_EQ(out.ColumnCount, 3);

    // lv: source null propagated.
    EXPECT_EQ(reinterpret_cast<const int64_t*>(out.Columns[1].Data)[0], 100);
    EXPECT_TRUE(MaskBit(out.Columns[1], 0));
    EXPECT_FALSE(MaskBit(out.Columns[1], 1));

    // rk: row 0 from the store, row 1 from the stream batch.
    const auto* outRk = reinterpret_cast<const int64_t*>(out.Columns[2].Data);
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
    auto spec = NKernel::BuildJoinKernelSpec(leftType, rightType, {{"lk", "rk"}}, EJoinType::Full);
    auto kernels = compiler.CompileJoin(spec);

    std::vector<int64_t> leftValues = {10, 20};
    std::vector<TColumn> leftColumns = {
        TColumn{.Data = reinterpret_cast<char*>(leftValues.data())},
    };
    TRowSet
        leftBatch{.Columns = leftColumns.data(), .ColumnCount = 1, .RowCount = 2, .RefCount = 1};

    std::vector<int64_t> rightValues = {100, 200};
    std::vector<TColumn> rightColumns = {
        TColumn{.Data = reinterpret_cast<char*>(rightValues.data())},
    };
    TRowSet
        rightBatch{.Columns = rightColumns.data(), .ColumnCount = 1, .RowCount = 2, .RefCount = 1};

    std::vector<int64_t> pairData = {
        PackRowId(0, 0),
        PackRowId(0, 0),
        PackRowId(0, 1),
        -1,
        -1,
        PackRowId(0, 1),
    };
    TPairBuffer pairs{.Count = 3, .Capacity = 3, .Data = pairData.data()};

    TRowSet out{};
    ASSERT_EQ(
        kernels.Materialize(&pairs, &leftBatch, &rightBatch, &leftBatch, &rightBatch, 0, 100, &out),
        3);
    ASSERT_EQ(out.ColumnCount, 2);

    const auto* outLeft = reinterpret_cast<const int64_t*>(out.Columns[0].Data);
    const auto* outRight = reinterpret_cast<const int64_t*>(out.Columns[1].Data);
    EXPECT_EQ(
        (std::vector<int64_t>{outLeft[0], outLeft[1], outLeft[2]}),
        (std::vector<int64_t>{10, 20, 0}));
    EXPECT_EQ(
        (std::vector<int64_t>{outRight[0], outRight[1], outRight[2]}),
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
    auto spec =
        NKernel::BuildJoinKernelSpec(leftType, rightType, {{"lk", "rk"}}, EJoinType::LeftSemi);
    auto kernels = compiler.CompileJoin(spec);

    std::vector<int64_t> lk = {1, 2};
    std::vector<int64_t> lv = {10, 20};
    std::vector<TColumn> lcols = {
        TColumn{.Data = reinterpret_cast<char*>(lk.data())},
        TColumn{.Data = reinterpret_cast<char*>(lv.data())},
    };
    TRowSet lbatch{.Columns = lcols.data(), .ColumnCount = 2, .RowCount = 2, .RefCount = 1};

    // Semi/anti pairs carry right = -1; the right side must not be read.
    std::vector<int64_t> pairData = {
        PackRowId(0, 1),
        -1,
        PackRowId(0, 0),
        -1,
    };
    TPairBuffer pairs{.Count = 2, .Capacity = 2, .Data = pairData.data()};

    TRowSet out{};
    ASSERT_EQ(kernels.Materialize(&pairs, &lbatch, nullptr, &lbatch, nullptr, 0, 100, &out), 2);
    ASSERT_EQ(out.RowCount, 2);
    ASSERT_EQ(out.ColumnCount, 2); // left columns only
    const auto* outLk = reinterpret_cast<const int64_t*>(out.Columns[0].Data);
    const auto* outLv = reinterpret_cast<const int64_t*>(out.Columns[1].Data);
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
    // DECIMAL(20,2) is stored in i128 (BinInt); one of p <= 18 would be a plain i64.
    TStructType rightType({{"rk", i64}, {"amount", std::make_shared<TDecimal>(20, 2)}});

    TKernelCompiler compiler;
    auto spec = NKernel::BuildJoinKernelSpec(leftType, rightType, {{"lk", "rk"}}, EJoinType::Left);
    auto kernels = compiler.CompileJoin(spec);

    std::vector<int64_t> lk = {1, 2};
    std::vector<uint8_t> flags = {0b00000100}; // bit offset 2: true, false
    std::vector<TColumn> lcols = {
        TColumn{.Data = reinterpret_cast<char*>(lk.data())},
        TColumn{.Data = reinterpret_cast<char*>(flags.data()), .DataBitOffset = 2},
    };
    TRowSet lbatch{.Columns = lcols.data(), .ColumnCount = 2, .RowCount = 2, .RefCount = 1};

    std::vector<int64_t> rk = {1};
    std::vector<qdb_bin_int> amounts = {{.Lo = 1234, .Hi = 0}};
    std::vector<TColumn> rcols = {
        TColumn{.Data = reinterpret_cast<char*>(rk.data())},
        TColumn{.Data = reinterpret_cast<char*>(amounts.data())},
    };
    TRowSet rbatch{.Columns = rcols.data(), .ColumnCount = 2, .RowCount = 1, .RefCount = 1};

    std::vector<int64_t> pairData = {
        PackRowId(0, 0),
        PackRowId(0, 0),
        PackRowId(0, 1),
        -1,
    };
    TPairBuffer pairs{.Count = 2, .Capacity = 2, .Data = pairData.data()};

    TRowSet out{};
    ASSERT_EQ(kernels.Materialize(&pairs, &lbatch, &rbatch, &lbatch, &rbatch, 0, 100, &out), 2);
    ASSERT_EQ(out.ColumnCount, 4);

    EXPECT_TRUE(MaskBit(out.Columns[1], 0));
    EXPECT_TRUE(MaskBit(out.Columns[1], 1));
    EXPECT_TRUE((static_cast<uint8_t>(out.Columns[1].Data[0]) & 0b1) != 0);
    EXPECT_FALSE((static_cast<uint8_t>(out.Columns[1].Data[0]) & 0b10) != 0);

    const auto* outAmounts = reinterpret_cast<const qdb_bin_int*>(out.Columns[3].Data);
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
    TStructType leftType({{"lk", i64}, {"lname", std::make_shared<TStringType>()}});
    TStructType rightType({{"rv", i64}});

    TKernelCompiler compiler;
    auto spec = NKernel::BuildCrossJoinKernelSpec(leftType, rightType);
    auto kernels = compiler.CompileCrossJoin(spec);

    std::vector<int64_t> lk = {1, 2, 3};
    std::string lnameBytes = "abbccc";
    std::vector<int64_t> lnameOffsets = {0, 1, 3, 6};
    std::vector<uint8_t> lsel = {1, 0, 1};
    std::vector<TColumn> lcols = {
        TColumn{.Data = reinterpret_cast<char*>(lk.data())},
        TColumn{.Data = lnameBytes.data(), .Offsets = lnameOffsets.data(), .OffsetWidth = 8},
    };
    TRowSet lbatch{
        .Columns = lcols.data(),
        .ColumnCount = 2,
        .RowCount = 3,
        .Selection = lsel.data(),
        .RefCount = 1,
    };

    std::vector<int64_t> rv = {10, 20};
    std::vector<uint8_t> rsel = {0, 1};
    std::vector<TColumn> rcols = {
        TColumn{.Data = reinterpret_cast<char*>(rv.data())},
    };
    TRowSet rbatch{
        .Columns = rcols.data(),
        .ColumnCount = 1,
        .RowCount = 2,
        .Selection = rsel.data(),
        .RefCount = 1,
    };

    TPairBuffer pairs{};
    ASSERT_TRUE(kernels.Dispatch(
        &lbatch,
        0,
        &rbatch,
        1,
        nullptr,
        &pairs,
        static_cast<int64_t>(ECrossJoinKernelOp::Emit)));
    ASSERT_EQ(pairs.Count, 2);
    EXPECT_EQ(pairs.Data[0], PackRowId(0, 0));
    EXPECT_EQ(pairs.Data[1], PackRowId(0, 1));
    EXPECT_EQ(pairs.Data[2], PackRowId(0, 2));
    EXPECT_EQ(pairs.Data[3], PackRowId(0, 1));

    TRowSet out{};
    ASSERT_EQ(kernels.Materialize(&pairs, &lbatch, &rbatch, &lbatch, &rbatch, 0, 100, &out), 2);
    ASSERT_EQ(out.RowCount, 2);
    ASSERT_EQ(out.ColumnCount, 3);

    const auto* outLk = reinterpret_cast<const int64_t*>(out.Columns[0].Data);
    EXPECT_EQ(outLk[0], 1);
    EXPECT_EQ(outLk[1], 3);
    const auto& outName = out.Columns[1];
    const auto* offsets = static_cast<const int64_t*>(outName.Offsets);
    EXPECT_EQ(std::string(outName.Data + offsets[0], outName.Data + offsets[1]), "a");
    EXPECT_EQ(std::string(outName.Data + offsets[1], outName.Data + offsets[2]), "ccc");
    const auto* outRv = reinterpret_cast<const int64_t*>(out.Columns[2].Data);
    EXPECT_EQ(outRv[0], 20);
    EXPECT_EQ(outRv[1], 20);

    FreeMaterializedRowSet(out);
    kernels.Dispatch(
        nullptr,
        0,
        nullptr,
        0,
        nullptr,
        &pairs,
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
    std::vector<TColumn> LeftColumns = {TColumn{.Data = reinterpret_cast<char*>(Lk.data())}};
    std::vector<TColumn> RightColumns0 = {TColumn{.Data = reinterpret_cast<char*>(Rv0.data())}};
    std::vector<TColumn> RightColumns1 = {TColumn{.Data = reinterpret_cast<char*>(Rv1.data())}};
    TRowSet Left{
        .Columns = LeftColumns.data(),
        .ColumnCount = 1,
        .RowCount = 3,
        .Selection = LeftSelection.data(),
        .RefCount = 1,
    };
    std::vector<TRowSet> Right = {
        TRowSet{.Columns = RightColumns0.data(), .ColumnCount = 1, .RowCount = 2, .RefCount = 1},
        TRowSet{
            .Columns = RightColumns1.data(),
            .ColumnCount = 1,
            .RowCount = 2,
            .Selection = RightSelection1.data(),
            .RefCount = 1,
        },
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
    return compiler.CompileCrossJoin(
        NKernel::BuildCrossJoinKernelSpec(leftType, rightType, type, ParseExpr("(< lk rv)")));
}

std::vector<int64_t> CrossJoinPairs(
    TCrossJoinKernels& kernels,
    TCrossJoinResidualInput& input,
    ECrossJoinKernelOp op,
    TPairBuffer& pairs)
{
    EXPECT_TRUE(kernels.Dispatch(
        &input.Left,
        0,
        input.Right.data(),
        2,
        input.RightMatched.data(),
        &pairs,
        static_cast<int64_t>(op)));
    return std::vector<int64_t>(pairs.Data, pairs.Data + 2 * pairs.Count);
}

} // namespace

TEST(CompileCrossJoin, FullJoinPadsUnmatchedRowsOfBothSides) {
    auto kernels = CompileResidualCrossJoin(EJoinType::Full);
    TCrossJoinResidualInput input;
    TPairBuffer pairs{};

    EXPECT_EQ(
        CrossJoinPairs(kernels, input, ECrossJoinKernelOp::Emit, pairs),
        (std::vector<int64_t>{
            PackRowId(0, 0),
            PackRowId(0, 0),
            PackRowId(0, 0),
            PackRowId(0, 1),
            PackRowId(0, 1),
            -1,
        }));
    EXPECT_EQ(input.RightMatched, (std::vector<uint8_t>{1, 1, 0, 0}));
    pairs.Count = 0;
    EXPECT_EQ(
        CrossJoinPairs(kernels, input, ECrossJoinKernelOp::EmitUnmatchedRight, pairs),
        (std::vector<int64_t>{-1, PackRowId(1, 0)}));

    kernels.Dispatch(
        nullptr,
        0,
        nullptr,
        0,
        nullptr,
        &pairs,
        static_cast<int64_t>(ECrossJoinKernelOp::Destroy));
}

TEST(CompileCrossJoin, SemiAndAntiEmitEachLeftRowOnceByMatch) {
    for (
        const auto [type, row] :
        {std::pair{EJoinType::LeftSemi, 0}, std::pair{EJoinType::LeftAnti, 1}})
    {
        auto kernels = CompileResidualCrossJoin(type);
        TCrossJoinResidualInput input;
        TPairBuffer pairs{};

        EXPECT_EQ(
            CrossJoinPairs(kernels, input, ECrossJoinKernelOp::Emit, pairs),
            (std::vector<int64_t>{PackRowId(0, row), -1}))
            << JoinTypeName(type);

        kernels.Dispatch(
            nullptr,
            0,
            nullptr,
            0,
            nullptr,
            &pairs,
            static_cast<int64_t>(ECrossJoinKernelOp::Destroy));
    }
}

namespace {

// Owned backing storage for one string column (payload bytes + i64 offsets).
struct TStringColumn {
    std::string Bytes;
    std::vector<int64_t> Offsets;

    explicit TStringColumn(const std::vector<std::string>& values) {
        Offsets.push_back(0);
        for (const auto& value : values) {
            Bytes += value;
            Offsets.push_back(static_cast<int64_t>(Bytes.size()));
        }
    }

    TColumn Column() {
        return TColumn{.Data = Bytes.data(), .Offsets = Offsets.data(), .OffsetWidth = 8};
    }
};

std::string ReadString(const TColumn& col, int64_t row) {
    const auto* offsets = static_cast<const int64_t*>(col.Offsets);
    return std::string(col.Data + offsets[row], col.Data + offsets[row + 1]);
}

} // namespace

TEST(JoinStringKey, InnerJoinMatchesAndMaterializes) {
    using namespace NQumir::NAst;
    auto i64 = std::make_shared<TIntegerType>(TIntegerType::I64);
    TStructType leftType({{"lk", std::make_shared<TStringType>()}, {"lv", i64}});
    TStructType rightType({{"rk", std::make_shared<TStringType>()}, {"rv", i64}});

    TKernelCompiler compiler;
    auto spec = NKernel::BuildJoinKernelSpec(leftType, rightType, {{"lk", "rk"}}, EJoinType::Inner);
    auto kernels = compiler.CompileJoin(spec);

    TStringColumn lk({"apple", "banana", "apple"});
    std::vector<int64_t> lv = {10, 20, 30};
    std::vector<TColumn> lcols = {lk.Column(), TColumn{.Data = reinterpret_cast<char*>(lv.data())}};
    TRowSet lbatch{.Columns = lcols.data(), .ColumnCount = 2, .RowCount = 3, .RefCount = 1};

    TStringColumn rk({"banana", "apple", "cherry"});
    std::vector<int64_t> rv = {100, 200, 300};
    std::vector<TColumn> rcols = {rk.Column(), TColumn{.Data = reinterpret_cast<char*>(rv.data())}};
    TRowSet rbatch{.Columns = rcols.data(), .ColumnCount = 2, .RowCount = 3, .RefCount = 1};

    THashTable left{}, right{};
    TPairBuffer pairs{};
    ASSERT_TRUE(kernels.Dispatch(
        &left,
        &right,
        nullptr,
        0,
        &pairs,
        nullptr,
        nullptr,
        8,
        JoinOpCode(EJoinKernelOp::Init)));
    ASSERT_TRUE(kernels.Dispatch(
        &left,
        &right,
        &lbatch,
        0,
        &pairs,
        &lbatch,
        &rbatch,
        0,
        JoinOpCode(EJoinKernelOp::UpdateLeft)));
    ASSERT_TRUE(kernels.Dispatch(
        &left,
        &right,
        &rbatch,
        0,
        &pairs,
        &lbatch,
        &rbatch,
        0,
        JoinOpCode(EJoinKernelOp::UpdateRight)));

    TRowSet out{};
    ASSERT_EQ(kernels.Materialize(&pairs, &lbatch, &rbatch, &lbatch, &rbatch, 0, 100, &out), 3);
    std::vector<std::tuple<std::string, int64_t, int64_t>> got;
    for (int64_t i = 0; i < out.RowCount; ++i) {
        got.emplace_back(
            ReadString(out.Columns[0], i),
            reinterpret_cast<const int64_t*>(out.Columns[1].Data)[i],
            reinterpret_cast<const int64_t*>(out.Columns[3].Data)[i]);
        EXPECT_EQ(ReadString(out.Columns[0], i), ReadString(out.Columns[2], i));
    }
    std::vector<std::tuple<std::string, int64_t, int64_t>> expected =
        {{"apple", 10, 200}, {"apple", 30, 200}, {"banana", 20, 100}};
    std::sort(got.begin(), got.end());
    EXPECT_EQ(got, expected);
    FreeMaterializedRowSet(out);

    kernels.Dispatch(
        &left,
        &right,
        nullptr,
        0,
        &pairs,
        nullptr,
        nullptr,
        0,
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
    auto spec =
        NKernel::BuildJoinKernelSpec(leftType, rightType, {{"lk", "rk"}}, EJoinType::LeftAnti);
    auto kernels = compiler.CompileJoin(spec);

    TStringColumn lk(leftKeys);
    std::vector<TColumn> lcols = {lk.Column(), TColumn{.Data = reinterpret_cast<char*>(lv.data())}};
    TRowSet lbatch{
        .Columns = lcols.data(),
        .ColumnCount = 2,
        .RowCount = static_cast<int64_t>(leftKeys.size()),
        .RefCount = 1,
    };

    TStringColumn rk(rightKeys);
    std::vector<TColumn> rcols = {rk.Column()};
    TRowSet rbatch{
        .Columns = rcols.data(),
        .ColumnCount = 1,
        .RowCount = static_cast<int64_t>(rightKeys.size()),
        .RefCount = 1,
    };

    THashTable left{}, right{};
    TPairBuffer pairs{};
    ASSERT_TRUE(kernels.Dispatch(
        &left,
        &right,
        nullptr,
        0,
        &pairs,
        nullptr,
        nullptr,
        8,
        JoinOpCode(EJoinKernelOp::Init)));
    const auto initialCapacity = right.Capacity;
    ASSERT_TRUE(kernels.Dispatch(
        &left,
        &right,
        &lbatch,
        0,
        &pairs,
        &lbatch,
        nullptr,
        0,
        JoinOpCode(EJoinKernelOp::UpdateLeft)));
    // Right batches are released immediately by the host, so the kernel must
    // clone right keys into owned storage; scribbling over the source payload
    // after the update proves it.
    ASSERT_TRUE(kernels.Dispatch(
        &left,
        &right,
        &rbatch,
        -1,
        &pairs,
        &lbatch,
        nullptr,
        0,
        JoinOpCode(EJoinKernelOp::UpdateRight)));
    std::fill(rk.Bytes.begin(), rk.Bytes.end(), '#');
    EXPECT_GT(right.Capacity, initialCapacity); // right keys survived rehash

    ASSERT_TRUE(kernels.Dispatch(
        &left,
        &right,
        nullptr,
        0,
        &pairs,
        &lbatch,
        nullptr,
        0,
        JoinOpCode(EJoinKernelOp::Finalize)));

    TRowSet out{};
    std::vector<std::pair<std::string, int64_t>> got;
    if (pairs.Count > 0) {
        ASSERT_EQ(
            kernels.Materialize(&pairs, &lbatch, nullptr, &lbatch, nullptr, 0, 1000, &out),
            pairs.Count);
        for (int64_t i = 0; i < out.RowCount; ++i) {
            got.emplace_back(
                ReadString(out.Columns[0], i),
                reinterpret_cast<const int64_t*>(out.Columns[1].Data)[i]);
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

    kernels.Dispatch(
        &left,
        &right,
        nullptr,
        0,
        &pairs,
        nullptr,
        nullptr,
        0,
        JoinOpCode(EJoinKernelOp::Destroy));
}

TEST(JoinStringKey, CompositeStringIntKey) {
    using namespace NQumir::NAst;
    auto i64 = std::make_shared<TIntegerType>(TIntegerType::I64);
    TStructType leftType({{"lk", std::make_shared<TStringType>()}, {"ln", i64}, {"lv", i64}});
    TStructType rightType({{"rk", std::make_shared<TStringType>()}, {"rn", i64}, {"rv", i64}});

    TKernelCompiler compiler;
    auto spec = NKernel::BuildJoinKernelSpec(
        leftType,
        rightType,
        {{"lk", "rk"}, {"ln", "rn"}},
        EJoinType::Inner);
    auto kernels = compiler.CompileJoin(spec);

    TStringColumn lk({"a", "a", "b"});
    std::vector<int64_t> ln = {1, 2, 1};
    std::vector<int64_t> lv = {10, 20, 30};
    std::vector<TColumn> lcols = {
        lk.Column(),
        TColumn{.Data = reinterpret_cast<char*>(ln.data())},
        TColumn{.Data = reinterpret_cast<char*>(lv.data())},
    };
    TRowSet lbatch{.Columns = lcols.data(), .ColumnCount = 3, .RowCount = 3, .RefCount = 1};

    TStringColumn rk({"a", "b", "a"});
    std::vector<int64_t> rn = {1, 1, 3};
    std::vector<int64_t> rv = {100, 200, 300};
    std::vector<TColumn> rcols = {
        rk.Column(),
        TColumn{.Data = reinterpret_cast<char*>(rn.data())},
        TColumn{.Data = reinterpret_cast<char*>(rv.data())},
    };
    TRowSet rbatch{.Columns = rcols.data(), .ColumnCount = 3, .RowCount = 3, .RefCount = 1};

    THashTable left{}, right{};
    TPairBuffer pairs{};
    ASSERT_TRUE(kernels.Dispatch(
        &left,
        &right,
        nullptr,
        0,
        &pairs,
        nullptr,
        nullptr,
        8,
        JoinOpCode(EJoinKernelOp::Init)));
    ASSERT_TRUE(kernels.Dispatch(
        &left,
        &right,
        &lbatch,
        0,
        &pairs,
        &lbatch,
        &rbatch,
        0,
        JoinOpCode(EJoinKernelOp::UpdateLeft)));
    ASSERT_TRUE(kernels.Dispatch(
        &left,
        &right,
        &rbatch,
        0,
        &pairs,
        &lbatch,
        &rbatch,
        0,
        JoinOpCode(EJoinKernelOp::UpdateRight)));

    TRowSet out{};
    ASSERT_EQ(kernels.Materialize(&pairs, &lbatch, &rbatch, &lbatch, &rbatch, 0, 100, &out), 2);
    std::vector<std::tuple<std::string, int64_t, int64_t, int64_t>> got;
    for (int64_t i = 0; i < out.RowCount; ++i) {
        got.emplace_back(
            ReadString(out.Columns[0], i),
            reinterpret_cast<const int64_t*>(out.Columns[1].Data)[i],
            reinterpret_cast<const int64_t*>(out.Columns[2].Data)[i],
            reinterpret_cast<const int64_t*>(out.Columns[5].Data)[i]);
    }
    std::vector<std::tuple<std::string, int64_t, int64_t, int64_t>> expected = {
        {"a", 1, 10, 100},
        {"b", 1, 30, 200},
    };
    std::sort(got.begin(), got.end());
    EXPECT_EQ(got, expected);
    FreeMaterializedRowSet(out);

    kernels.Dispatch(
        &left,
        &right,
        nullptr,
        0,
        &pairs,
        nullptr,
        nullptr,
        0,
        JoinOpCode(EJoinKernelOp::Destroy));
}

namespace {

using TMarker = std::optional<bool>;

// SQL `key IN keys`; nullopt is NULL.
template <class T>
TMarker InReference(const std::optional<T>& key, const std::vector<std::optional<T>>& keys) {
    if (keys.empty()) {
        return false;
    }
    if (!key) {
        return std::nullopt;
    }
    bool hasNull = false;
    for (const auto& value : keys) {
        if (!value) {
            hasNull = true;
        } else if (*value == *key) {
            return true;
        }
    }
    if (hasNull) {
        return std::nullopt;
    }
    return false;
}

// Expected mark join output: every left row once with its marker, as
// (row index, marker) pairs.
template <class T>
std::vector<std::pair<int64_t, TMarker>> MarkJoinReference(
    const std::vector<std::optional<T>>& leftKeys,
    const std::vector<std::optional<T>>& rightKeys)
{
    std::vector<std::pair<int64_t, TMarker>> rows;
    for (size_t i = 0; i < leftKeys.size(); ++i) {
        rows.emplace_back(static_cast<int64_t>(i), InReference(leftKeys[i], rightKeys));
    }
    return rows;
}

TMarker ReadMarker(const TColumn& col, int64_t row) {
    if (!MaskBit(col, row)) {
        return std::nullopt;
    }
    return ((static_cast<uint8_t>(col.Data[row / 8]) >> (row % 8)) & 1) != 0;
}

std::vector<uint8_t> ValidityMask(const std::vector<bool>& valid) {
    std::vector<uint8_t> mask((valid.size() + 7) / 8, 0);
    for (size_t i = 0; i < valid.size(); ++i) {
        if (valid[i]) {
            mask[i / 8] |= static_cast<uint8_t>(1u << (i % 8));
        }
    }
    return mask;
}

// Backing storage for a nullable fixed-width column. A NULL row keeps
// `nullPayload` in its data slot: the kernel must ignore it.
template <class T> struct TNullableColumn {
    std::vector<T> Values;
    std::vector<uint8_t> Mask;

    TNullableColumn(const std::vector<std::optional<T>>& values, T nullPayload = T{}) {
        std::vector<bool> valid;
        for (const auto& value : values) {
            Values.push_back(value.value_or(nullPayload));
            valid.push_back(value.has_value());
        }
        Mask = ValidityMask(valid);
    }

    TColumn Column() {
        return TColumn{.Data = reinterpret_cast<char*>(Values.data()), .Mask = Mask.data()};
    }
};

std::vector<int64_t> RowIndices(size_t count) {
    std::vector<int64_t> indices(count);
    for (size_t i = 0; i < count; ++i) {
        indices[i] = static_cast<int64_t>(i);
    }
    return indices;
}

NKernel::TOperatorKernelSpec MarkJoinSpec(
    const NQumir::NAst::TStructType& leftType,
    const NQumir::NAst::TStructType& rightType,
    const std::vector<TJoinKey>& keys)
{
    return NKernel::BuildJoinKernelSpec(
        leftType,
        rightType,
        keys,
        EJoinType::LeftMark,
        nullptr,
        "marker");
}

// Dispatch orders a mark join can be driven in. The right side always goes
// through UpdateRight with batch index -1 (a key-only build that keeps no
// rows), and Finalize always runs last.
enum class EMarkJoinShape {
    // The right set is complete before any left row; left rows are streamed.
    ProbeLeft,
    // Left rows are stored through UpdateLeft and marked by Finalize.
    Symmetric,
    // The first half of the left rows is stored, the second half is streamed.
    Mixed,
};

const char* ShapeName(EMarkJoinShape shape) {
    switch (shape) {
    case EMarkJoinShape::ProbeLeft:
        return "probe-left";
    case EMarkJoinShape::Symmetric:
        return "symmetric";
    case EMarkJoinShape::Mixed:
        return "mixed";
    }
    return "";
}

constexpr EMarkJoinShape AllMarkJoinShapes[] = {
    EMarkJoinShape::ProbeLeft,
    EMarkJoinShape::Symmetric,
    EMarkJoinShape::Mixed,
};

struct TMarkJoinRun {
    // (left row index read from the last left column, marker), sorted.
    std::vector<std::pair<int64_t, TMarker>> Rows;
    int64_t InitialCapacity = 0;
    int64_t LeftCapacity = 0;
    int64_t RightCapacity = 0;
};

TMarkJoinRun RunMarkJoin(
    const TJoinKernels& kernels,
    const TRowSet& left,
    const TRowSet& right,
    EMarkJoinShape shape)
{
    // Mixed splits the left batch through two complementary selections.
    std::vector<uint8_t> firstHalf(left.RowCount);
    std::vector<uint8_t> secondHalf(left.RowCount);
    for (int64_t i = 0; i < left.RowCount; ++i) {
        const bool selected = !left.Selection || left.Selection[i] != 0;
        firstHalf[i] = selected && i < left.RowCount / 2;
        secondHalf[i] = selected && i >= left.RowCount / 2;
    }
    TRowSet stored = left;
    TRowSet streamed = left;
    if (shape == EMarkJoinShape::Mixed) {
        stored.Selection = firstHalf.data();
        streamed.Selection = secondHalf.data();
    }
    TRowSet rightBatch = right;

    THashTable leftTable{}, rightTable{};
    TPairBuffer pairs{};
    auto dispatch = [&](TRowSet* batch, int64_t batchIdx, int64_t arg, EJoinKernelOp op) {
        return kernels.Dispatch(
            &leftTable,
            &rightTable,
            batch,
            batchIdx,
            &pairs,
            &stored,
            nullptr,
            arg,
            JoinOpCode(op));
    };

    TMarkJoinRun run;
    EXPECT_TRUE(dispatch(nullptr, 0, 8, EJoinKernelOp::Init));
    run.InitialCapacity = rightTable.Capacity;
    if (shape != EMarkJoinShape::ProbeLeft) {
        EXPECT_TRUE(dispatch(&stored, 0, 0, EJoinKernelOp::UpdateLeft));
    }
    EXPECT_TRUE(dispatch(&rightBatch, -1, 0, EJoinKernelOp::UpdateRight));
    if (shape != EMarkJoinShape::Symmetric) {
        EXPECT_TRUE(dispatch(&streamed, -1, 0, EJoinKernelOp::StreamLeft));
    }
    EXPECT_TRUE(dispatch(nullptr, 0, 0, EJoinKernelOp::Finalize));
    run.LeftCapacity = leftTable.Capacity;
    run.RightCapacity = rightTable.Capacity;

    if (pairs.Count > 0) {
        TRowSet out{};
        EXPECT_EQ(
            kernels.Materialize(&pairs, &stored, nullptr, &streamed, nullptr, 0, pairs.Count, &out),
            pairs.Count);
        EXPECT_EQ(out.ColumnCount, left.ColumnCount + 1);
        const auto* rowIndex =
            reinterpret_cast<const int64_t*>(out.Columns[left.ColumnCount - 1].Data);
        for (int64_t row = 0; row < out.RowCount; ++row) {
            run.Rows.emplace_back(rowIndex[row], ReadMarker(out.Columns[left.ColumnCount], row));
        }
        FreeMaterializedRowSet(out);
    }
    dispatch(nullptr, 0, 0, EJoinKernelOp::Destroy);
    std::sort(run.Rows.begin(), run.Rows.end());
    return run;
}

// Random nullable keys in [0, domain); about one in `nullEvery` is NULL
// (never when nullEvery is 0).
std::vector<std::optional<int64_t>>
RandomNullableKeys(uint32_t& seed, size_t count, uint32_t domain, uint32_t nullEvery) {
    auto rnd = [&]() {
        seed = (seed * 1103515245u + 12345u) & 0x7fffffffu;
        return seed;
    };
    std::vector<std::optional<int64_t>> keys;
    for (size_t i = 0; i < count; ++i) {
        const uint32_t value = rnd();
        if (nullEvery != 0 && value % nullEvery == 0) {
            keys.push_back(std::nullopt);
        } else {
            keys.push_back(static_cast<int64_t>((value / 7) % domain));
        }
    }
    return keys;
}

} // namespace

TEST(MarkJoinKernel, NullableKeyFollowsThreeValuedIn) {
    using namespace NQumir::NAst;
    auto i64 = std::make_shared<TIntegerType>(TIntegerType::I64);
    auto nullableI64 = std::make_shared<TNullable>(i64);
    TStructType leftType({{"lk", nullableI64}, {"lv", i64}});
    TStructType rightType({{"rk", nullableI64}});

    TKernelCompiler compiler;
    auto kernels = compiler.CompileJoin(MarkJoinSpec(leftType, rightType, {{"lk", "rk"}}));

    struct TCase {
        std::string Name;
        std::vector<std::optional<int64_t>> Left;
        std::vector<std::optional<int64_t>> Right;
        std::vector<uint8_t> RightSelection; // empty = every row
        std::vector<TMarker> Expected;
    };
    const std::vector<TCase> cases = {
        {
            .Name = "match, miss, NULL key, duplicate keys in S",
            .Left = {1, 4, std::nullopt, 2, 1},
            .Right = {1, 2, 2, 3, 1},
            .Expected = {true, false, std::nullopt, true, true},
        },
        {
            .Name = "S contains NULL",
            .Left = {1, 5, std::nullopt},
            .Right = {std::nullopt, 1},
            .Expected = {true, std::nullopt, std::nullopt},
        },
        {
            .Name = "S holds only NULLs",
            .Left = {7, 1, std::nullopt},
            .Right = {std::nullopt, std::nullopt},
            .Expected = {std::nullopt, std::nullopt, std::nullopt},
        },
        {
            .Name = "empty S",
            .Left = {1, std::nullopt},
            .Right = {},
            .Expected = {false, false},
        },
        {
            .Name = "S emptied by the selection",
            .Left = {1, std::nullopt},
            .Right = {std::nullopt, 1},
            .RightSelection = {0, 0},
            .Expected = {false, false},
        },
    };

    for (const auto& c : cases) {
        std::vector<std::optional<int64_t>> selectedRight;
        for (size_t i = 0; i < c.Right.size(); ++i) {
            if (c.RightSelection.empty() || c.RightSelection[i] != 0) {
                selectedRight.push_back(c.Right[i]);
            }
        }
        std::vector<std::pair<int64_t, TMarker>> expected;
        for (size_t i = 0; i < c.Expected.size(); ++i) {
            expected.emplace_back(static_cast<int64_t>(i), c.Expected[i]);
        }
        ASSERT_EQ(MarkJoinReference(c.Left, selectedRight), expected) << c.Name;

        // NULL rows carry the payload 1, a key of S in most cases.
        TNullableColumn<int64_t> lk(c.Left, 1);
        std::vector<int64_t> lv = RowIndices(c.Left.size());
        std::vector<TColumn> lcols = {
            lk.Column(),
            TColumn{.Data = reinterpret_cast<char*>(lv.data())},
        };
        TRowSet lbatch{
            .Columns = lcols.data(),
            .ColumnCount = 2,
            .RowCount = static_cast<int64_t>(c.Left.size()),
            .RefCount = 1,
        };
        TNullableColumn<int64_t> rk(c.Right, 1);
        std::vector<TColumn> rcols = {rk.Column()};
        auto rightSelection = c.RightSelection;
        TRowSet rbatch{
            .Columns = rcols.data(),
            .ColumnCount = 1,
            .RowCount = static_cast<int64_t>(c.Right.size()),
            .Selection = rightSelection.empty() ? nullptr : rightSelection.data(),
            .RefCount = 1,
        };

        for (auto shape : AllMarkJoinShapes) {
            EXPECT_EQ(RunMarkJoin(kernels, lbatch, rbatch, shape).Rows, expected)
                << c.Name << ", " << ShapeName(shape);
        }
    }
}

TEST(MarkJoinKernel, NonNullableCompositeKeyIsTwoValued) {
    using namespace NQumir::NAst;
    auto i64 = std::make_shared<TIntegerType>(TIntegerType::I64);
    auto i32 = std::make_shared<TIntegerType>(TIntegerType::I32);
    TStructType leftType({{"la", i64}, {"lb", i32}, {"lv", i64}});
    TStructType rightType({{"rb", i32}, {"ra", i64}});

    TKernelCompiler compiler;
    auto kernels =
        compiler.CompileJoin(MarkJoinSpec(leftType, rightType, {{"la", "ra"}, {"lb", "rb"}}));

    uint32_t seed = 777;
    auto rnd = [&]() {
        seed = (seed * 1103515245u + 12345u) & 0x7fffffffu;
        return seed;
    };
    std::vector<int64_t> la, ra;
    std::vector<int32_t> lb, rb;
    std::vector<std::optional<std::pair<int64_t, int32_t>>> leftKeys, rightKeys;
    for (int i = 0; i < 40; ++i) {
        la.push_back(rnd() % 5);
        lb.push_back(static_cast<int32_t>(rnd() % 4));
        leftKeys.emplace_back(std::pair{la.back(), lb.back()});
    }
    for (int i = 0; i < 15; ++i) {
        ra.push_back(rnd() % 5);
        rb.push_back(static_cast<int32_t>(rnd() % 4));
        rightKeys.emplace_back(std::pair{ra.back(), rb.back()});
    }
    std::vector<int64_t> lv = RowIndices(la.size());
    std::vector<TColumn> lcols = {
        TColumn{.Data = reinterpret_cast<char*>(la.data())},
        TColumn{.Data = reinterpret_cast<char*>(lb.data())},
        TColumn{.Data = reinterpret_cast<char*>(lv.data())},
    };
    TRowSet lbatch{
        .Columns = lcols.data(),
        .ColumnCount = 3,
        .RowCount = static_cast<int64_t>(la.size()),
        .RefCount = 1,
    };
    std::vector<TColumn> rcols = {
        TColumn{.Data = reinterpret_cast<char*>(rb.data())},
        TColumn{.Data = reinterpret_cast<char*>(ra.data())},
    };
    TRowSet rbatch{
        .Columns = rcols.data(),
        .ColumnCount = 2,
        .RowCount = static_cast<int64_t>(ra.size()),
        .RefCount = 1,
    };

    const auto expected = MarkJoinReference(leftKeys, rightKeys);
    ASSERT_TRUE(
        std::ranges::any_of(expected, [](const auto& row) { return row.second == TMarker(true); }));
    ASSERT_TRUE(std::ranges::any_of(expected, [](const auto& row) {
        return row.second == TMarker(false);
    }));
    for (auto shape : AllMarkJoinShapes) {
        EXPECT_EQ(RunMarkJoin(kernels, lbatch, rbatch, shape).Rows, expected) << ShapeName(shape);
    }
}

TEST(MarkJoinKernel, NullableStringKeys) {
    using namespace NQumir::NAst;
    auto i64 = std::make_shared<TIntegerType>(TIntegerType::I64);
    auto nullableString = std::make_shared<TNullable>(std::make_shared<TStringType>());
    TStructType leftType({{"lk", nullableString}, {"lv", i64}});
    TStructType rightType({{"rk", nullableString}});

    TKernelCompiler compiler;
    auto kernels = compiler.CompileJoin(MarkJoinSpec(leftType, rightType, {{"lk", "rk"}}));

    // A NULL row keeps a payload that equals a key of the other side.
    const std::vector<std::optional<std::string>> leftKeys =
        {"apple", "cherry", std::nullopt, "banana", "apple"};
    const std::vector<std::string> leftPayload = {"apple", "cherry", "banana", "banana", "apple"};
    const std::vector<std::optional<std::string>> withNull =
        {"banana", std::nullopt, "apple", "banana"};
    const std::vector<std::string> withNullPayload = {"banana", "cherry", "apple", "banana"};
    const std::vector<std::optional<std::string>> withoutNull = {"banana", "apple"};

    TStringColumn lk(leftPayload);
    std::vector<bool> leftValid;
    for (const auto& key : leftKeys) {
        leftValid.push_back(key.has_value());
    }
    const auto leftMask = ValidityMask(leftValid);
    std::vector<int64_t> lv = RowIndices(leftKeys.size());
    std::vector<TColumn> lcols = {lk.Column(), TColumn{.Data = reinterpret_cast<char*>(lv.data())}};
    lcols[0].Mask = leftMask.data();
    TRowSet lbatch{
        .Columns = lcols.data(),
        .ColumnCount = 2,
        .RowCount = static_cast<int64_t>(leftKeys.size()),
        .RefCount = 1,
    };

    TStringColumn rkWithNull(withNullPayload);
    const auto withNullMask = ValidityMask({true, false, true, true});
    std::vector<TColumn> rcolsWithNull = {rkWithNull.Column()};
    rcolsWithNull[0].Mask = withNullMask.data();
    TRowSet rbatchWithNull{
        .Columns = rcolsWithNull.data(),
        .ColumnCount = 1,
        .RowCount = 4,
        .RefCount = 1,
    };

    TStringColumn rkWithoutNull({"banana", "apple"});
    std::vector<TColumn> rcolsWithoutNull = {rkWithoutNull.Column()};
    TRowSet rbatchWithoutNull{
        .Columns = rcolsWithoutNull.data(),
        .ColumnCount = 1,
        .RowCount = 2,
        .RefCount = 1,
    };

    const std::vector<std::pair<int64_t, TMarker>> expectedWithNull =
        {{0, true}, {1, std::nullopt}, {2, std::nullopt}, {3, true}, {4, true}};
    const std::vector<std::pair<int64_t, TMarker>> expectedWithoutNull =
        {{0, true}, {1, false}, {2, std::nullopt}, {3, true}, {4, true}};
    ASSERT_EQ(MarkJoinReference(leftKeys, withNull), expectedWithNull);
    ASSERT_EQ(MarkJoinReference(leftKeys, withoutNull), expectedWithoutNull);
    for (auto shape : AllMarkJoinShapes) {
        EXPECT_EQ(RunMarkJoin(kernels, lbatch, rbatchWithNull, shape).Rows, expectedWithNull)
            << ShapeName(shape);
        EXPECT_EQ(RunMarkJoin(kernels, lbatch, rbatchWithoutNull, shape).Rows, expectedWithoutNull)
            << ShapeName(shape);
    }
}

TEST(MarkJoinKernel, RehashSizedSidesMatchReference) {
    using namespace NQumir::NAst;
    auto i64 = std::make_shared<TIntegerType>(TIntegerType::I64);
    auto nullableI64 = std::make_shared<TNullable>(i64);
    TStructType leftType({{"lk", nullableI64}, {"lv", i64}});
    TStructType rightType({{"rk", nullableI64}});

    TKernelCompiler compiler;
    auto kernels = compiler.CompileJoin(MarkJoinSpec(leftType, rightType, {{"lk", "rk"}}));

    uint32_t seed = 2024;
    const auto leftKeys = RandomNullableKeys(seed, 300, 400, 10);
    TNullableColumn<int64_t> lk(leftKeys);
    std::vector<int64_t> lv = RowIndices(leftKeys.size());
    std::vector<TColumn> lcols = {lk.Column(), TColumn{.Data = reinterpret_cast<char*>(lv.data())}};
    TRowSet lbatch{
        .Columns = lcols.data(),
        .ColumnCount = 2,
        .RowCount = static_cast<int64_t>(leftKeys.size()),
        .RefCount = 1,
    };

    // Enough distinct keys a side to rehash either table; once without NULLs
    // (misses are FALSE) and once with NULLs (misses are NULL).
    for (uint32_t nullEvery : {0u, 10u}) {
        const auto rightKeys = RandomNullableKeys(seed, 200, 300, nullEvery);
        TNullableColumn<int64_t> rk(rightKeys);
        std::vector<TColumn> rcols = {rk.Column()};
        TRowSet rbatch{
            .Columns = rcols.data(),
            .ColumnCount = 1,
            .RowCount = static_cast<int64_t>(rightKeys.size()),
            .RefCount = 1,
        };

        const auto expected = MarkJoinReference(leftKeys, rightKeys);
        ASSERT_TRUE(std::ranges::any_of(expected, [](const auto& row) {
            return row.second == TMarker(true);
        }));
        ASSERT_TRUE(std::ranges::any_of(expected, [&](const auto& row) {
            return row.second == (nullEvery == 0 ? TMarker(false) : std::nullopt);
        }));
        for (auto shape : AllMarkJoinShapes) {
            const auto run = RunMarkJoin(kernels, lbatch, rbatch, shape);
            EXPECT_EQ(run.Rows, expected) << nullEvery << ", " << ShapeName(shape);
            EXPECT_GT(run.RightCapacity, run.InitialCapacity);
            if (shape == EMarkJoinShape::Symmetric) {
                EXPECT_GT(run.LeftCapacity, run.InitialCapacity);
            }
        }
    }
}

TEST(MarkJoinKernel, ReadsPrecomputedLeftHashes) {
    using namespace NQumir::NAst;
    auto i64 = std::make_shared<TIntegerType>(TIntegerType::I64);
    auto nullableI64 = std::make_shared<TNullable>(i64);
    TStructType leftType({{"lk", nullableI64}, {"lv", i64}});
    TStructType rightType({{"rk", nullableI64}});

    TKernelCompiler compiler;
    auto spec = MarkJoinSpec(leftType, rightType, {{"lk", "rk"}});
    auto hash = compiler.CompileJoinHash(spec);
    spec.HasPrecomputedHash = true;
    auto kernels = compiler.CompileJoin(spec);

    uint32_t seed = 99;
    const auto leftKeys = RandomNullableKeys(seed, 60, 30, 8);
    const auto rightKeys = RandomNullableKeys(seed, 20, 30, 8);
    TNullableColumn<int64_t> lk(leftKeys);
    std::vector<int64_t> lv = RowIndices(leftKeys.size());
    std::vector<TColumn> lcols = {lk.Column(), TColumn{.Data = reinterpret_cast<char*>(lv.data())}};
    TRowSet lbatch{
        .Columns = lcols.data(),
        .ColumnCount = 2,
        .RowCount = static_cast<int64_t>(leftKeys.size()),
        .RefCount = 1,
    };
    std::vector<uint64_t> leftHashes(leftKeys.size());
    ASSERT_TRUE(hash.Left(&lbatch, leftHashes.data()));
    lbatch.Hash = leftHashes.data();

    TNullableColumn<int64_t> rk(rightKeys);
    std::vector<TColumn> rcols = {rk.Column()};
    TRowSet rbatch{
        .Columns = rcols.data(),
        .ColumnCount = 1,
        .RowCount = static_cast<int64_t>(rightKeys.size()),
        .RefCount = 1,
    };

    const auto expected = MarkJoinReference(leftKeys, rightKeys);
    for (auto shape : AllMarkJoinShapes) {
        EXPECT_EQ(RunMarkJoin(kernels, lbatch, rbatch, shape).Rows, expected) << ShapeName(shape);
    }
}

TEST(MarkJoinKernel, RejectsUnsupportedSpecs) {
    using namespace NQumir::NAst;
    auto i64 = std::make_shared<TIntegerType>(TIntegerType::I64);
    auto nullableI64 = std::make_shared<TNullable>(i64);
    TStructType leftType({{"lk", nullableI64}, {"ln", i64}, {"lv", i64}});
    TStructType rightType({{"rk", nullableI64}, {"rn", i64}});
    TKernelCompiler compiler;

    auto expectError = [&](const NKernel::TOperatorKernelSpec& spec, const std::string& message) {
        try {
            compiler.CompileJoin(spec);
            ADD_FAILURE() << "expected error: " << message;
        } catch (const NQumir::TError& error) {
            EXPECT_NE(error.ToString().find(message), std::string::npos) << error.ToString();
        }
    };

    expectError(
        NKernel::BuildJoinKernelSpec(
            leftType,
            rightType,
            {{"lk", "rk"}},
            EJoinType::LeftMark,
            ParseExpr("(> lv rn)"),
            "marker"),
        "residual filter is not yet supported for this join type");
    expectError(
        MarkJoinSpec(leftType, rightType, {{.Left = "lk", .Right = "rk", .NullsEqual = true}}),
        "left_mark join does not support NULL-safe keys");
    expectError(
        MarkJoinSpec(leftType, rightType, {{"lk", "rk"}, {"ln", "rn"}}),
        "left_mark join does not support a nullable multi-column key");
    EXPECT_THROW(
        NKernel::BuildJoinKernelSpec(leftType, rightType, {{"lk", "rk"}}, EJoinType::LeftMark),
        std::runtime_error);

    // Without equi-keys there is no key set to answer IN: the nested-loop kernel
    // rejects the type.
    EXPECT_THROW(
        NKernel::BuildCrossJoinKernelSpec(leftType, rightType, EJoinType::LeftMark),
        std::runtime_error);
    auto keyless = NKernel::BuildCrossJoinKernelSpec(leftType, rightType);
    keyless.JoinType = EJoinType::LeftMark;
    EXPECT_THROW(compiler.CompileCrossJoin(keyless), NQumir::TError);

    // Right rows cannot be streamed past stored left rows: a marker needs the
    // complete right side.
    auto kernels = compiler.CompileJoin(MarkJoinSpec(leftType, rightType, {{"ln", "rn"}}));
    THashTable left{}, right{};
    TPairBuffer pairs{};
    std::vector<int64_t> rn = {1};
    std::vector<TColumn> rcols = {
        TColumn{.Data = reinterpret_cast<char*>(rn.data())},
        TColumn{.Data = reinterpret_cast<char*>(rn.data())},
    };
    TRowSet rbatch{.Columns = rcols.data(), .ColumnCount = 2, .RowCount = 1, .RefCount = 1};
    ASSERT_TRUE(kernels.Dispatch(
        &left,
        &right,
        nullptr,
        0,
        &pairs,
        nullptr,
        nullptr,
        8,
        JoinOpCode(EJoinKernelOp::Init)));
    EXPECT_FALSE(kernels.Dispatch(
        &left,
        &right,
        &rbatch,
        -1,
        &pairs,
        nullptr,
        nullptr,
        0,
        JoinOpCode(EJoinKernelOp::StreamRight)));
    EXPECT_EQ(pairs.Count, 0);
    kernels.Dispatch(
        &left,
        &right,
        nullptr,
        0,
        &pairs,
        nullptr,
        nullptr,
        0,
        JoinOpCode(EJoinKernelOp::Destroy));
}

namespace {

// Rows of `rowSet` an ordinary compiled filter keeps for `predicate`, read
// against the column types of `type`.
std::vector<int64_t>
FilterRows(const NQumir::NAst::TTypePtr& type, TRowSet rowSet, const std::string& predicate) {
    auto schema = NQumir::NAst::TMaybeType<NQumir::NAst::TStructType>(type);
    EXPECT_TRUE(schema);
    auto dispatch = TKernelCompiler().CompileFilter(
        NKernel::BuildFilterKernelSpec(*schema.Cast(), ParseExpr(predicate)));
    std::vector<uint8_t> selection(rowSet.RowCount);
    rowSet.Selection = selection.data();
    dispatch(rowSet, nullptr);
    std::vector<int64_t> rows;
    for (int64_t row = 0; row < rowSet.RowCount; ++row) {
        if (selection[row] != 0) {
            rows.push_back(row);
        }
    }
    return rows;
}

} // namespace

// The materialized marker column must read back through the ordinary column
// ABI (bit-packed Data, Mask bit 1 = valid) as the type the spec declares.
TEST(MarkJoinKernel, MarkerColumnReadsBackAsDeclaredType) {
    using namespace NQumir::NAst;
    auto i64 = std::make_shared<TIntegerType>(TIntegerType::I64);
    auto nullableI64 = std::make_shared<TNullable>(i64);

    auto materialize = [](
        const NKernel::TOperatorKernelSpec& spec,
        const TRowSet& left,
        const TRowSet& right,
        TPairBuffer& pairs,
        THashTable& leftTable,
        THashTable& rightTable,
        TRowSet& out)
    {
        auto kernels = TKernelCompiler().CompileJoin(spec);
        TRowSet leftBatch = left;
        TRowSet rightBatch = right;
        ASSERT_TRUE(kernels.Dispatch(
            &leftTable,
            &rightTable,
            nullptr,
            0,
            &pairs,
            nullptr,
            nullptr,
            8,
            JoinOpCode(EJoinKernelOp::Init)));
        ASSERT_TRUE(kernels.Dispatch(
            &leftTable,
            &rightTable,
            &rightBatch,
            -1,
            &pairs,
            nullptr,
            nullptr,
            0,
            JoinOpCode(EJoinKernelOp::UpdateRight)));
        ASSERT_TRUE(kernels.Dispatch(
            &leftTable,
            &rightTable,
            &leftBatch,
            -1,
            &pairs,
            nullptr,
            nullptr,
            0,
            JoinOpCode(EJoinKernelOp::StreamLeft)));
        ASSERT_EQ(
            kernels
                .Materialize(&pairs, nullptr, nullptr, &leftBatch, nullptr, 0, pairs.Count, &out),
            left.RowCount);
        kernels.Dispatch(
            &leftTable,
            &rightTable,
            nullptr,
            0,
            &pairs,
            nullptr,
            nullptr,
            0,
            JoinOpCode(EJoinKernelOp::Destroy));
    };

    // Nullable keys, S with NULL: TRUE, NULL (missing key), NULL (NULL key),
    // TRUE. The streamed probe keeps the left row order.
    {
        TStructType leftType({{"lk", nullableI64}, {"lv", i64}});
        TStructType rightType({{"rk", nullableI64}});
        auto spec = MarkJoinSpec(leftType, rightType, {{"lk", "rk"}});
        TNullableColumn<int64_t> lk({1, 5, std::nullopt, 2});
        std::vector<int64_t> lv = RowIndices(4);
        std::vector<TColumn> lcols = {
            lk.Column(),
            TColumn{.Data = reinterpret_cast<char*>(lv.data())},
        };
        TRowSet left{.Columns = lcols.data(), .ColumnCount = 2, .RowCount = 4, .RefCount = 1};
        TNullableColumn<int64_t> rk({1, std::nullopt, 2});
        std::vector<TColumn> rcols = {rk.Column()};
        TRowSet right{.Columns = rcols.data(), .ColumnCount = 1, .RowCount = 3, .RefCount = 1};

        THashTable leftTable{}, rightTable{};
        TPairBuffer pairs{};
        TRowSet out{};
        materialize(spec, left, right, pairs, leftTable, rightTable, out);
        ASSERT_EQ(out.RowCount, 4);
        EXPECT_EQ(FilterRows(spec.OutputSchema, out, "marker"), (std::vector<int64_t>{0, 3}));
        EXPECT_EQ(FilterRows(spec.OutputSchema, out, "(! marker)"), (std::vector<int64_t>{}));
        EXPECT_EQ(
            FilterRows(spec.OutputSchema, out, "(call qdb_is_null marker)"),
            (std::vector<int64_t>{1, 2}));
        EXPECT_EQ(
            FilterRows(spec.OutputSchema, out, "(! (call coalesce marker #f))"),
            (std::vector<int64_t>{1, 2}));
        FreeMaterializedRowSet(out);
    }

    // Nullable keys, S without NULL: the missing key is FALSE, the NULL key NULL.
    {
        TStructType leftType({{"lk", nullableI64}, {"lv", i64}});
        TStructType rightType({{"rk", i64}});
        auto spec = MarkJoinSpec(leftType, rightType, {{"lk", "rk"}});
        TNullableColumn<int64_t> lk({1, 5, std::nullopt});
        std::vector<int64_t> lv = RowIndices(3);
        std::vector<TColumn> lcols = {
            lk.Column(),
            TColumn{.Data = reinterpret_cast<char*>(lv.data())},
        };
        TRowSet left{.Columns = lcols.data(), .ColumnCount = 2, .RowCount = 3, .RefCount = 1};
        std::vector<int64_t> rk = {1, 2};
        std::vector<TColumn> rcols = {TColumn{.Data = reinterpret_cast<char*>(rk.data())}};
        TRowSet right{.Columns = rcols.data(), .ColumnCount = 1, .RowCount = 2, .RefCount = 1};

        THashTable leftTable{}, rightTable{};
        TPairBuffer pairs{};
        TRowSet out{};
        materialize(spec, left, right, pairs, leftTable, rightTable, out);
        ASSERT_EQ(out.RowCount, 3);
        EXPECT_EQ(FilterRows(spec.OutputSchema, out, "marker"), (std::vector<int64_t>{0}));
        EXPECT_EQ(FilterRows(spec.OutputSchema, out, "(! marker)"), (std::vector<int64_t>{1}));
        EXPECT_EQ(
            FilterRows(spec.OutputSchema, out, "(call qdb_is_null marker)"),
            (std::vector<int64_t>{2}));
        FreeMaterializedRowSet(out);
    }

    // Non-nullable keys: a plain bool marker.
    {
        TStructType leftType({{"lk", i64}, {"lv", i64}});
        TStructType rightType({{"rk", i64}});
        auto spec = MarkJoinSpec(leftType, rightType, {{"lk", "rk"}});
        std::vector<int64_t> lk = {1, 5, 2, 7};
        std::vector<int64_t> lv = RowIndices(4);
        std::vector<TColumn> lcols = {
            TColumn{.Data = reinterpret_cast<char*>(lk.data())},
            TColumn{.Data = reinterpret_cast<char*>(lv.data())},
        };
        TRowSet left{.Columns = lcols.data(), .ColumnCount = 2, .RowCount = 4, .RefCount = 1};
        std::vector<int64_t> rk = {2, 1};
        std::vector<TColumn> rcols = {TColumn{.Data = reinterpret_cast<char*>(rk.data())}};
        TRowSet right{.Columns = rcols.data(), .ColumnCount = 1, .RowCount = 2, .RefCount = 1};

        THashTable leftTable{}, rightTable{};
        TPairBuffer pairs{};
        TRowSet out{};
        materialize(spec, left, right, pairs, leftTable, rightTable, out);
        ASSERT_EQ(out.RowCount, 4);
        EXPECT_EQ(FilterRows(spec.OutputSchema, out, "marker"), (std::vector<int64_t>{0, 2}));
        EXPECT_EQ(FilterRows(spec.OutputSchema, out, "(! marker)"), (std::vector<int64_t>{1, 3}));
        FreeMaterializedRowSet(out);
    }
}

// Probe bytes a row join processor may hold in the tests that do not exercise
// the budget; ample for their tiny batches.
constexpr size_t TestProbeHoldBytes = 1 << 20;

// The row join table (kernel/join/join_rows.oz): rows with an equal key chain
// behind one entry, NULL keys never match, and a string payload is gathered
// from the retained batch through the row's StringView.
TEST(RowJoinProcessor, ChainsNullKeysAndStringPayload) {
    using namespace NQumir::NAst;
    auto i64 = std::make_shared<TIntegerType>(TIntegerType::I64);
    auto nullableI64 = std::make_shared<TNullable>(i64);
    TStructType leftType({{"lk", nullableI64}, {"lname", std::make_shared<TStringType>()}});
    TStructType rightType({{"rk", nullableI64}, {"rv", i64}});

    // Probe (left): keys 1, 2, NULL, 1, 7 with string payloads.
    std::vector<int64_t> lk = {1, 2, 0, 1, 7};
    std::vector<uint8_t> lkMask = {0b11011};
    std::string lnameBytes = "abbcdddd"; // "a", "bb", "", "c", "dddd"
    std::vector<int64_t> lnameOffsets = {0, 1, 3, 3, 4, 8};
    std::vector<TColumn> lcols = {
        TColumn{.Data = reinterpret_cast<char*>(lk.data()), .Mask = lkMask.data()},
        TColumn{.Data = lnameBytes.data(), .Offsets = lnameOffsets.data(), .OffsetWidth = 8},
    };
    TRowSet leftBatch{.Columns = lcols.data(), .ColumnCount = 2, .RowCount = 5, .RefCount = 1};

    // Build (right): key 1 twice (a chain), NULL, 2, 9.
    std::vector<int64_t> rk = {1, 1, 0, 2, 9};
    std::vector<uint8_t> rkMask = {0b11011};
    std::vector<int64_t> rv = {100, 200, 300, 400, 500};
    std::vector<TColumn> rcols = {
        TColumn{.Data = reinterpret_cast<char*>(rk.data()), .Mask = rkMask.data()},
        TColumn{.Data = reinterpret_cast<char*>(rv.data())},
    };
    TRowSet rightBatch{.Columns = rcols.data(), .ColumnCount = 2, .RowCount = 5, .RefCount = 1};

    TKernelCompiler compiler;
    auto spec = NKernel::BuildJoinKernelSpec(leftType, rightType, {{"lk", "rk"}}, EJoinType::Inner);
    TRowJoinProcessor processor(
        compiler.CompileRowJoin(spec, /*buildIsLeft=*/false),
        EJoinBuildSide::Right,
        TestProbeHoldBytes);

    bool leftDone = false;
    bool rightDone = false;
    bool rightFinished = false;
    auto left = [&](TRowSet& rowSet) {
        // The build side never runs dry here, so the probe side is read only
        // after it ended.
        EXPECT_TRUE(rightFinished);
        if (leftDone) {
            return EJoinFetchResult::FINISHED;
        }
        rowSet = leftBatch;
        leftDone = true;
        return EJoinFetchResult::OK;
    };
    auto right = [&](TRowSet& rowSet) {
        if (rightDone) {
            rightFinished = true;
            return EJoinFetchResult::FINISHED;
        }
        rowSet = rightBatch;
        rightDone = true;
        return EJoinFetchResult::OK;
    };

    using TRow = std::tuple<int64_t, std::string, int64_t, int64_t>;
    std::vector<TRow> got;
    TRowSet out{};
    for (;;) {
        EXPECT_EQ(
            processor.RequiredInputSide(),
            rightFinished ? EJoinBuildSide::Left : EJoinBuildSide::Auto);
        auto result = processor.Process(left, right, out);
        if (result == EJoinProcessorResult::NEED_DATA) {
            continue;
        }
        if (result == EJoinProcessorResult::FINISHED) {
            break;
        }
        ASSERT_EQ(out.ColumnCount, 4);
        const auto* c0 = reinterpret_cast<const int64_t*>(out.Columns[0].Data);
        const auto* c2 = reinterpret_cast<const int64_t*>(out.Columns[2].Data);
        const auto* c3 = reinterpret_cast<const int64_t*>(out.Columns[3].Data);
        const auto& lname = out.Columns[1];
        ASSERT_EQ(lname.OffsetWidth, 8);
        const auto* offsets = reinterpret_cast<const int64_t*>(lname.Offsets);
        for (int64_t i = 0; i < out.RowCount; ++i) {
            EXPECT_TRUE((out.Columns[0].Mask[i / 8] >> (i % 8)) & 1);
            EXPECT_TRUE((out.Columns[2].Mask[i / 8] >> (i % 8)) & 1);
            got.emplace_back(
                c0[i],
                std::string(lname.Data + (offsets[i] - offsets[0]), offsets[i + 1] - offsets[i]),
                c2[i],
                c3[i]);
        }
        Release(&out);
    }

    std::vector<TRow> expected =
        {{1, "a", 1, 100}, {1, "a", 1, 200}, {1, "c", 1, 100}, {1, "c", 1, 200}, {2, "bb", 2, 400}};
    std::sort(got.begin(), got.end());
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(got, expected);
}

namespace {

// Runs a row join over the given batches of each side; the build side is read
// to its end before the first probe batch. Returns the output batches, which
// the caller releases.
std::vector<TRowSet> RunRowJoin(
    const NKernel::TOperatorKernelSpec& spec,
    EJoinBuildSide buildSide,
    std::vector<TRowSet> leftBatches,
    std::vector<TRowSet> rightBatches)
{
    TKernelCompiler compiler;
    TRowJoinProcessor processor(
        compiler.CompileRowJoin(spec, buildSide == EJoinBuildSide::Left),
        buildSide,
        TestProbeHoldBytes);
    size_t nextLeft = 0;
    size_t nextRight = 0;
    auto left = [&](TRowSet& rowSet) {
        if (nextLeft == leftBatches.size()) {
            return EJoinFetchResult::FINISHED;
        }
        rowSet = leftBatches[nextLeft++];
        return EJoinFetchResult::OK;
    };
    auto right = [&](TRowSet& rowSet) {
        if (nextRight == rightBatches.size()) {
            return EJoinFetchResult::FINISHED;
        }
        rowSet = rightBatches[nextRight++];
        return EJoinFetchResult::OK;
    };
    std::vector<TRowSet> outputs;
    for (;;) {
        TRowSet out{};
        auto result = processor.Process(left, right, out);
        if (result == EJoinProcessorResult::FINISHED) {
            break;
        }
        if (result == EJoinProcessorResult::OK) {
            outputs.push_back(out);
        }
    }
    return outputs;
}

std::string StringAt(const TColumn& column, int64_t row) {
    const auto* offsets = reinterpret_cast<const int64_t*>(column.Offsets);
    return std::string(
        column.Data + (offsets[row] - offsets[0]),
        offsets[row + 1] - offsets[row]);
}

} // namespace

// Build on the left with a string payload: the build side's strings are
// gathered from the retained build batch, and the rows of one key come out in
// build order.
TEST(RowJoinProcessor, BuildLeftStringPayloadInBuildOrder) {
    using namespace NQumir::NAst;
    auto i64 = std::make_shared<TIntegerType>(TIntegerType::I64);
    TStructType leftType({{"lk", i64}, {"lname", std::make_shared<TStringType>()}});
    TStructType rightType({{"rk", i64}, {"rv", i64}});

    std::vector<int64_t> lk = {5, 5, 6, 5};
    std::string lnameBytes = "xyywzzz"; // "x", "yy", "w", "zzz"
    std::vector<int64_t> lnameOffsets = {0, 1, 3, 4, 7};
    std::vector<TColumn> lcols = {
        TColumn{.Data = reinterpret_cast<char*>(lk.data())},
        TColumn{.Data = lnameBytes.data(), .Offsets = lnameOffsets.data(), .OffsetWidth = 8},
    };
    TRowSet build{.Columns = lcols.data(), .ColumnCount = 2, .RowCount = 4, .RefCount = 1};

    std::vector<int64_t> rk = {5, 6, 8};
    std::vector<int64_t> rv = {50, 60, 80};
    std::vector<TColumn> rcols = {
        TColumn{.Data = reinterpret_cast<char*>(rk.data())},
        TColumn{.Data = reinterpret_cast<char*>(rv.data())},
    };
    TRowSet probe{.Columns = rcols.data(), .ColumnCount = 2, .RowCount = 3, .RefCount = 1};

    auto spec = NKernel::BuildJoinKernelSpec(leftType, rightType, {{"lk", "rk"}}, EJoinType::Inner);
    using TRow = std::tuple<int64_t, std::string, int64_t, int64_t>;
    std::vector<TRow> got;
    for (auto& out : RunRowJoin(spec, EJoinBuildSide::Left, {build}, {probe})) {
        ASSERT_EQ(out.ColumnCount, 4);
        const auto* c0 = reinterpret_cast<const int64_t*>(out.Columns[0].Data);
        const auto* c2 = reinterpret_cast<const int64_t*>(out.Columns[2].Data);
        const auto* c3 = reinterpret_cast<const int64_t*>(out.Columns[3].Data);
        for (int64_t i = 0; i < out.RowCount; ++i) {
            got.emplace_back(c0[i], StringAt(out.Columns[1], i), c2[i], c3[i]);
        }
        Release(&out);
    }

    std::vector<TRow> expected = {
        {5, "x", 5, 50},
        {5, "yy", 5, 50},
        {5, "zzz", 5, 50},
        {6, "w", 6, 60},
    };
    EXPECT_EQ(got, expected);
}

// While the build side has no batch ready, probe batches are pulled and held,
// so the probe pipeline does not wait for the build. They are probed once the
// table exists, also when the probe side ended first.
TEST(RowJoinProcessor, HoldsProbeBatchesUntilTheTableIsBuilt) {
    using namespace NQumir::NAst;
    auto i64 = std::make_shared<TIntegerType>(TIntegerType::I64);
    TStructType leftType({{"lk", i64}, {"lv", i64}});
    TStructType rightType({{"rk", i64}, {"rv", i64}});

    std::vector<int64_t> lk1 = {1, 2};
    std::vector<int64_t> lv1 = {10, 20};
    std::vector<int64_t> lk2 = {2, 3};
    std::vector<int64_t> lv2 = {30, 40};
    std::vector<TColumn> lcols1 = {
        TColumn{.Data = reinterpret_cast<char*>(lk1.data())},
        TColumn{.Data = reinterpret_cast<char*>(lv1.data())},
    };
    std::vector<TColumn> lcols2 = {
        TColumn{.Data = reinterpret_cast<char*>(lk2.data())},
        TColumn{.Data = reinterpret_cast<char*>(lv2.data())},
    };
    std::vector<TRowSet> probes = {
        TRowSet{.Columns = lcols1.data(), .ColumnCount = 2, .RowCount = 2, .RefCount = 1},
        TRowSet{.Columns = lcols2.data(), .ColumnCount = 2, .RowCount = 2, .RefCount = 1},
    };
    std::vector<int64_t> rk = {2, 3, 3};
    std::vector<int64_t> rv = {200, 300, 301};
    std::vector<TColumn> rcols = {
        TColumn{.Data = reinterpret_cast<char*>(rk.data())},
        TColumn{.Data = reinterpret_cast<char*>(rv.data())},
    };
    TRowSet build{.Columns = rcols.data(), .ColumnCount = 2, .RowCount = 3, .RefCount = 1};

    TKernelCompiler compiler;
    auto spec = NKernel::BuildJoinKernelSpec(leftType, rightType, {{"lk", "rk"}}, EJoinType::Inner);
    TRowJoinProcessor processor(
        compiler.CompileRowJoin(spec, /*buildIsLeft=*/false),
        EJoinBuildSide::Right,
        TestProbeHoldBytes);

    // The build side is not ready for two calls, then delivers its batch,
    // is not ready again and ends.
    const std::vector<EJoinFetchResult> buildSteps = {
        EJoinFetchResult::NO_DATA,
        EJoinFetchResult::NO_DATA,
        EJoinFetchResult::OK,
        EJoinFetchResult::NO_DATA,
        EJoinFetchResult::FINISHED,
    };
    size_t buildStep = 0;
    bool buildFinished = false;
    size_t nextProbe = 0;
    size_t probesBeforeBuild = 0;
    bool probeFinished = false;
    auto left = [&](TRowSet& rowSet) {
        EXPECT_FALSE(probeFinished) << "the probe side is read after it ended";
        if (nextProbe == probes.size()) {
            probeFinished = true;
            return EJoinFetchResult::FINISHED;
        }
        probesBeforeBuild += buildFinished ? 0 : 1;
        rowSet = probes[nextProbe++];
        return EJoinFetchResult::OK;
    };
    auto right = [&](TRowSet& rowSet) {
        EXPECT_LT(buildStep, buildSteps.size());
        const auto step = buildSteps[std::min(buildStep++, buildSteps.size() - 1)];
        if (step == EJoinFetchResult::OK) {
            rowSet = build;
        }
        buildFinished = step == EJoinFetchResult::FINISHED;
        return step;
    };

    using TRow = std::tuple<int64_t, int64_t, int64_t, int64_t>;
    std::vector<TRow> got;
    bool finished = false;
    for (int calls = 0; calls < 100 && !finished; ++calls) {
        if (!buildFinished) {
            EXPECT_EQ(
                processor.RequiredInputSide(),
                probeFinished ? EJoinBuildSide::Right : EJoinBuildSide::Auto);
        }
        TRowSet out{};
        const auto result = processor.Process(left, right, out);
        if (result == EJoinProcessorResult::FINISHED) {
            finished = true;
        } else if (result == EJoinProcessorResult::OK) {
            ASSERT_EQ(out.ColumnCount, 4);
            std::array<const int64_t*, 4> cols;
            for (size_t c = 0; c < cols.size(); ++c) {
                cols[c] = reinterpret_cast<const int64_t*>(out.Columns[c].Data);
            }
            for (int64_t i = 0; i < out.RowCount; ++i) {
                got.emplace_back(cols[0][i], cols[1][i], cols[2][i], cols[3][i]);
            }
            Release(&out);
        }
    }

    EXPECT_TRUE(finished);
    EXPECT_EQ(probesBeforeBuild, probes.size()) << "both probe batches wait for the table";
    std::vector<TRow> expected = {
        {2, 20, 2, 200},
        {2, 30, 2, 200},
        {3, 40, 3, 300},
        {3, 40, 3, 301},
    };
    std::sort(got.begin(), got.end());
    EXPECT_EQ(got, expected);
}

namespace {

struct THeldProbeRun {
    // Probe batches read before the build ended.
    size_t ProbesBeforeBuild = 0;
    // The side RequiredInputSide named, sampled before each Process call while
    // the build was running and the probe side had not ended.
    std::vector<EJoinBuildSide> Required;
    std::vector<std::tuple<int64_t, int64_t, int64_t, int64_t>> Rows;
    bool Finished = false;
};

// Joins `probeBatches` batches of two rows (32 bytes each) with a build side
// that is not ready for `idleSteps` calls, then delivers its batch and ends.
// The probe side is the left input, or the right one when `buildLeft`. With
// `stringProbe` the left probe batch is {lk i64, lname string} with two
// 8-byte names, 32 payload bytes again, so the string offsets are measured.
THeldProbeRun RunHeldProbe(
    size_t probeHoldBytes,
    size_t probeBatches,
    size_t idleSteps,
    bool buildLeft = false,
    bool stringProbe = false)
{
    using namespace NQumir::NAst;
    auto i64 = std::make_shared<TIntegerType>(TIntegerType::I64);
    TStructType leftType({{"lk", i64}, {stringProbe ? "lname" : "lv", stringProbe
        ? std::static_pointer_cast<TType>(std::make_shared<TStringType>()) : i64}});
    TStructType rightType({{"rk", i64}, {"rv", i64}});

    std::vector<int64_t> lk = {1, 2};
    std::vector<int64_t> lv = {10, 20};
    std::vector<TColumn> lcols = {
        TColumn{.Data = reinterpret_cast<char*>(lk.data())},
        TColumn{.Data = reinterpret_cast<char*>(lv.data())},
    };
    // Names of lengths 8 and 8; the key 2 row carries the second.
    const char names[] = "aaaaaaaabbbbbbbb";
    int32_t nameOffsets[] = {0, 8, 16};
    if (stringProbe) {
        lcols[1] = TColumn{
            .Data = const_cast<char*>(names), .Offsets = nameOffsets, .OffsetWidth = 4};
    }
    std::vector<int64_t> rk = {2, 3};
    std::vector<int64_t> rv = {200, 300};
    std::vector<TColumn> rcols = {
        TColumn{.Data = reinterpret_cast<char*>(rk.data())},
        TColumn{.Data = reinterpret_cast<char*>(rv.data())},
    };

    TKernelCompiler compiler;
    auto spec = NKernel::BuildJoinKernelSpec(leftType, rightType, {{"lk", "rk"}}, EJoinType::Inner);
    TRowJoinProcessor processor(
        compiler.CompileRowJoin(spec, buildLeft),
        buildLeft ? EJoinBuildSide::Left : EJoinBuildSide::Right,
        probeHoldBytes);

    const auto& probeCols = buildLeft ? rcols : lcols;
    const auto& buildCols = buildLeft ? lcols : rcols;
    THeldProbeRun run;
    size_t buildStep = 0;
    bool buildFinished = false;
    size_t nextProbe = 0;
    bool probeFinished = false;
    auto probe = [&](TRowSet& rowSet) {
        if (nextProbe == probeBatches) {
            probeFinished = true;
            return EJoinFetchResult::FINISHED;
        }
        ++nextProbe;
        run.ProbesBeforeBuild += buildFinished ? 0 : 1;
        // Each batch is released by the join, so each gets its own count.
        rowSet = TRowSet{
            .Columns = const_cast<TColumn*>(probeCols.data()),
            .ColumnCount = 2,
            .RowCount = 2,
            .RefCount = 1,
        };
        return EJoinFetchResult::OK;
    };
    auto build = [&](TRowSet& rowSet) {
        if (buildStep < idleSteps) {
            ++buildStep;
            return EJoinFetchResult::NO_DATA;
        }
        if (buildStep++ == idleSteps) {
            rowSet = TRowSet{
                .Columns = const_cast<TColumn*>(buildCols.data()),
                .ColumnCount = 2,
                .RowCount = 2,
                .RefCount = 1,
            };
            return EJoinFetchResult::OK;
        }
        buildFinished = true;
        return EJoinFetchResult::FINISHED;
    };
    const TRowJoinProcessor::TFetch buildFetch = build;
    const TRowJoinProcessor::TFetch probeFetch = probe;
    const auto& left = buildLeft ? buildFetch : probeFetch;
    const auto& right = buildLeft ? probeFetch : buildFetch;

    for (int calls = 0; calls < 100 && !run.Finished; ++calls) {
        if (!buildFinished && !probeFinished) {
            run.Required.push_back(processor.RequiredInputSide());
        }
        TRowSet out{};
        const auto result = processor.Process(left, right, out);
        if (result == EJoinProcessorResult::FINISHED) {
            run.Finished = true;
        } else if (result == EJoinProcessorResult::OK) {
            const auto* c0 = reinterpret_cast<const int64_t*>(out.Columns[0].Data);
            const auto* c1 = reinterpret_cast<const int64_t*>(out.Columns[1].Data);
            const auto* c2 = reinterpret_cast<const int64_t*>(out.Columns[2].Data);
            const auto* c3 = reinterpret_cast<const int64_t*>(out.Columns[3].Data);
            for (int64_t i = 0; i < out.RowCount; ++i) {
                // A string column is recorded as its length, in place of lv.
                int64_t second = stringProbe ? 0 : c1[i];
                if (stringProbe) {
                    EXPECT_EQ(out.Columns[1].OffsetWidth, 8);
                    const auto* offsets = static_cast<const int64_t*>(out.Columns[1].Offsets);
                    second = offsets[i + 1] - offsets[i];
                }
                run.Rows.emplace_back(c0[i], second, c2[i], c3[i]);
            }
            Release(&out);
        }
    }
    return run;
}

} // namespace

// The hold is bounded: at the budget the processor asks for the build side
// only and does not read the probe side, then reads it again after the build.
TEST(RowJoinProcessor, StopsHoldingProbeBatchesAtTheBudget) {
    // Four 32-byte probe batches against a 64-byte budget: two are held.
    const auto run = RunHeldProbe(/*probeHoldBytes=*/64, /*probeBatches=*/4, /*idleSteps=*/6);
    EXPECT_TRUE(run.Finished);
    EXPECT_EQ(run.ProbesBeforeBuild, 2u);
    ASSERT_GE(run.Required.size(), 5u);
    EXPECT_EQ(run.Required[0], EJoinBuildSide::Auto);
    EXPECT_EQ(run.Required[1], EJoinBuildSide::Auto);
    for (size_t i = 2; i < run.Required.size(); ++i) {
        EXPECT_EQ(run.Required[i], EJoinBuildSide::Right) << "call " << i;
    }
    // All four probe batches match key 2 once: the budget changes no result.
    EXPECT_EQ(run.Rows, (std::vector<std::tuple<int64_t, int64_t, int64_t, int64_t>>(
        4, {2, 20, 2, 200})));

    // A left build holds the right probe batches by the same budget.
    const auto left = RunHeldProbe(
        /*probeHoldBytes=*/64, /*probeBatches=*/4, /*idleSteps=*/6, /*buildLeft=*/true);
    EXPECT_TRUE(left.Finished);
    EXPECT_EQ(left.ProbesBeforeBuild, 2u);
    ASSERT_GE(left.Required.size(), 5u);
    for (size_t i = 2; i < left.Required.size(); ++i) {
        EXPECT_EQ(left.Required[i], EJoinBuildSide::Left) << "call " << i;
    }
    EXPECT_EQ(left.Rows, run.Rows);

    // A string probe column counts its payload, not its offsets: four batches
    // of 32 bytes (16 of key, 16 of names) are held below a 100-byte budget
    // until 96 bytes are held; the offsets (12 per batch) would stop it at 3.
    const auto strings = RunHeldProbe(
        /*probeHoldBytes=*/100, /*probeBatches=*/5, /*idleSteps=*/8,
        /*buildLeft=*/false, /*stringProbe=*/true);
    EXPECT_TRUE(strings.Finished);
    EXPECT_EQ(strings.ProbesBeforeBuild, 4u);
    EXPECT_EQ(strings.Rows, (std::vector<std::tuple<int64_t, int64_t, int64_t, int64_t>>(
        5, {2, 8, 2, 200})));
}

// A batch larger than the budget is still taken when nothing is held, so the
// processor cannot wait on probe input it refuses to read.
TEST(RowJoinProcessor, TakesOneProbeBatchLargerThanTheBudget) {
    const auto run = RunHeldProbe(/*probeHoldBytes=*/8, /*probeBatches=*/3, /*idleSteps=*/4);
    EXPECT_TRUE(run.Finished);
    EXPECT_EQ(run.ProbesBeforeBuild, 1u);
    EXPECT_EQ(run.Rows.size(), 3u);
}

// A zero budget disables the hold: the build is awaited before any probe read.
TEST(RowJoinProcessor, ZeroBudgetHoldsNothing) {
    const auto run = RunHeldProbe(/*probeHoldBytes=*/0, /*probeBatches=*/3, /*idleSteps=*/4);
    EXPECT_TRUE(run.Finished);
    EXPECT_EQ(run.ProbesBeforeBuild, 0u);
    EXPECT_EQ(run.Rows.size(), 3u);
}

// A query can be torn down while the build is still running, for example after
// an error elsewhere or a satisfied LIMIT. The probe batches the join holds for
// the table must then be released with it.
TEST(RowJoinProcessor, ReleasesHeldProbeBatchesWhenDestroyedBeforeTheBuild) {
    using namespace NQumir::NAst;
    auto i64 = std::make_shared<TIntegerType>(TIntegerType::I64);
    TStructType leftType({{"lk", i64}});
    TStructType rightType({{"rk", i64}});

    std::vector<int64_t> keys = {1, 2};
    std::vector<TColumn> cols = {TColumn{.Data = reinterpret_cast<char*>(keys.data())}};
    int destroyed = 0;
    auto countDestroy = [](TRowSet* rs) {
        ++*static_cast<int*>(rs->Private);
    };
    TKernelCompiler compiler;
    auto spec = NKernel::BuildJoinKernelSpec(leftType, rightType, {{"lk", "rk"}}, EJoinType::Inner);
    {
        TRowJoinProcessor processor(
            compiler.CompileRowJoin(spec, /*buildIsLeft=*/false),
            EJoinBuildSide::Right,
            TestProbeHoldBytes);
        int probesSent = 0;
        auto left = [&](TRowSet& rowSet) {
            if (probesSent == 2) {
                return EJoinFetchResult::NO_DATA;
            }
            ++probesSent;
            rowSet = TRowSet{
                .Columns = cols.data(),
                .ColumnCount = 1,
                .RowCount = 2,
                .Destroy = countDestroy,
                .Private = &destroyed,
                .RefCount = 1,
            };
            return EJoinFetchResult::OK;
        };
        auto right = [](TRowSet&) {
            return EJoinFetchResult::NO_DATA;
        };
        for (int calls = 0; calls < 4; ++calls) {
            TRowSet out{};
            EXPECT_EQ(processor.Process(left, right, out), EJoinProcessorResult::NEED_DATA);
        }
        EXPECT_EQ(probesSent, 2);
        EXPECT_EQ(destroyed, 0) << "held probe batches stay alive until the table is built";
    }
    EXPECT_EQ(destroyed, 2);
}

// A build side of 500,000 rows spans 31 arena blocks of 16,384 rows and fills
// its 2^20-entry table to 0.48. Two million probe keys, three quarters of them
// absent, meet about thirty entries whose salt matches a different key; the
// key comparison must reject those and keep probing.
TEST(RowJoinProcessor, ManyArenaBlocksAndSaltCollisions) {
    using namespace NQumir::NAst;
    auto i64 = std::make_shared<TIntegerType>(TIntegerType::I64);
    TStructType leftType({{"lk", i64}});
    TStructType rightType({{"rk", i64}, {"rv", i64}});

    constexpr int64_t buildRows = 500000;
    constexpr int64_t probeRows = 2000000;
    constexpr int64_t probeBatchRows = 65536;
    std::vector<int64_t> rk(buildRows);
    std::vector<int64_t> rv(buildRows);
    for (int64_t i = 0; i < buildRows; ++i) {
        rk[i] = 2 * i;
        rv[i] = 3 * rk[i];
    }
    std::vector<TColumn> rcols = {
        TColumn{.Data = reinterpret_cast<char*>(rk.data())},
        TColumn{.Data = reinterpret_cast<char*>(rv.data())},
    };
    TRowSet build{.Columns = rcols.data(), .ColumnCount = 2, .RowCount = buildRows, .RefCount = 1};

    std::vector<int64_t> lk(probeRows);
    for (int64_t i = 0; i < probeRows; ++i) {
        lk[i] = i;
    }
    std::vector<std::vector<TColumn>> lcols;
    std::vector<TRowSet> probes;
    for (int64_t start = 0; start < probeRows; start += probeBatchRows) {
        lcols.push_back({TColumn{.Data = reinterpret_cast<char*>(lk.data() + start)}});
    }
    for (size_t b = 0; b < lcols.size(); ++b) {
        const int64_t start = static_cast<int64_t>(b) * probeBatchRows;
        probes.push_back(TRowSet{
            .Columns = lcols[b].data(),
            .ColumnCount = 1,
            .RowCount = std::min(probeBatchRows, probeRows - start),
            .RefCount = 1,
        });
    }

    auto spec = NKernel::BuildJoinKernelSpec(leftType, rightType, {{"lk", "rk"}}, EJoinType::Inner);
    int64_t matches = 0;
    int64_t keySum = 0;
    int64_t wrong = 0;
    for (auto& out : RunRowJoin(spec, EJoinBuildSide::Right, probes, {build})) {
        ASSERT_EQ(out.ColumnCount, 3);
        const auto* c0 = reinterpret_cast<const int64_t*>(out.Columns[0].Data);
        const auto* c1 = reinterpret_cast<const int64_t*>(out.Columns[1].Data);
        const auto* c2 = reinterpret_cast<const int64_t*>(out.Columns[2].Data);
        for (int64_t i = 0; i < out.RowCount; ++i) {
            wrong += c0[i] != c1[i] || c2[i] != 3 * c1[i];
            keySum += c0[i];
        }
        matches += out.RowCount;
        Release(&out);
    }

    EXPECT_EQ(wrong, 0);
    EXPECT_EQ(matches, buildRows);
    EXPECT_EQ(keySum, buildRows * (buildRows - 1)); // the sum of 2i for i < buildRows
}

namespace {

std::optional<int64_t> I64At(const TColumn& column, int64_t row) {
    if (column.Mask && !((column.Mask[row / 8] >> (row % 8)) & 1)) {
        return std::nullopt;
    }
    return reinterpret_cast<const int64_t*>(column.Data)[row];
}

std::optional<std::string> StringOrNullAt(const TColumn& column, int64_t row) {
    if (column.Mask && !((column.Mask[row / 8] >> (row % 8)) & 1)) {
        return std::nullopt;
    }
    return StringAt(column, row);
}

} // namespace

// A LEFT join that builds its null-supplying right side: every selected
// preserved row comes out with its matches, or once with NULLs for the right
// columns. NULL keys on both sides match nothing, a build key with two rows
// yields two pairs, and the build side's string and nullable payloads read as
// NULL on a padded row. The unselected probe row is not part of the input.
TEST(RowJoinProcessor, LeftOuterPadsUnmatchedProbeRows) {
    using namespace NQumir::NAst;
    auto i64 = std::make_shared<TIntegerType>(TIntegerType::I64);
    auto nullableI64 = std::make_shared<TNullable>(i64);
    auto nullableString = std::make_shared<TNullable>(std::make_shared<TStringType>());
    TStructType leftType({{"lk", nullableI64}, {"lname", std::make_shared<TStringType>()}});
    TStructType rightType({{"rk", nullableI64}, {"rv", nullableI64}, {"rs", nullableString}});

    // Probe batch 1: keys 1, NULL, 2, 7, 1 (unselected).
    std::vector<int64_t> lk1 = {1, 0, 2, 7, 1};
    std::vector<uint8_t> lk1Mask = {0b11101};
    std::string lname1Bytes = "anbbsevenz"; // "a", "n", "bb", "seven", "z"
    std::vector<int64_t> lname1Offsets = {0, 1, 2, 4, 9, 10};
    std::vector<uint8_t> selection1 = {1, 1, 1, 1, 0};
    std::vector<TColumn> lcols1 = {
        TColumn{.Data = reinterpret_cast<char*>(lk1.data()), .Mask = lk1Mask.data()},
        TColumn{.Data = lname1Bytes.data(), .Offsets = lname1Offsets.data(), .OffsetWidth = 8},
    };
    TRowSet probe1{
        .Columns = lcols1.data(),
        .ColumnCount = 2,
        .RowCount = 5,
        .Selection = selection1.data(),
        .RefCount = 1,
    };
    // Probe batch 2: keys 1, NULL.
    std::vector<int64_t> lk2 = {1, 0};
    std::vector<uint8_t> lk2Mask = {0b01};
    std::string lname2Bytes = "cm";
    std::vector<int64_t> lname2Offsets = {0, 1, 2};
    std::vector<TColumn> lcols2 = {
        TColumn{.Data = reinterpret_cast<char*>(lk2.data()), .Mask = lk2Mask.data()},
        TColumn{.Data = lname2Bytes.data(), .Offsets = lname2Offsets.data(), .OffsetWidth = 8},
    };
    TRowSet probe2{.Columns = lcols2.data(), .ColumnCount = 2, .RowCount = 2, .RefCount = 1};

    // Build: (1, 100, "x"), (1, NULL, "yy"), (NULL, 300, "null"), (2, 400, NULL), (9, 500, "nine").
    std::vector<int64_t> rk = {1, 1, 0, 2, 9};
    std::vector<uint8_t> rkMask = {0b11011};
    std::vector<int64_t> rv = {100, 0, 300, 400, 500};
    std::vector<uint8_t> rvMask = {0b11101};
    std::string rsBytes = "xyynullnine";
    std::vector<int64_t> rsOffsets = {0, 1, 3, 7, 7, 11};
    std::vector<uint8_t> rsMask = {0b10111};
    std::vector<TColumn> rcols = {
        TColumn{.Data = reinterpret_cast<char*>(rk.data()), .Mask = rkMask.data()},
        TColumn{.Data = reinterpret_cast<char*>(rv.data()), .Mask = rvMask.data()},
        TColumn{
            .Data = rsBytes.data(),
            .Mask = rsMask.data(),
            .Offsets = rsOffsets.data(),
            .OffsetWidth = 8,
        },
    };
    TRowSet build{.Columns = rcols.data(), .ColumnCount = 3, .RowCount = 5, .RefCount = 1};

    auto spec = NKernel::BuildJoinKernelSpec(leftType, rightType, {{"lk", "rk"}}, EJoinType::Left);
    using TRow = std::tuple<
        std::optional<int64_t>,
        std::string,
        std::optional<int64_t>,
        std::optional<int64_t>,
        std::optional<std::string>>;
    std::vector<TRow> got;
    for (auto& out : RunRowJoin(spec, EJoinBuildSide::Right, {probe1, probe2}, {build})) {
        EXPECT_EQ(out.ColumnCount, 5);
        for (int64_t i = 0; i < out.RowCount; ++i) {
            got.emplace_back(
                I64At(out.Columns[0], i),
                StringAt(out.Columns[1], i),
                I64At(out.Columns[2], i),
                I64At(out.Columns[3], i),
                StringOrNullAt(out.Columns[4], i));
        }
        Release(&out);
    }

    const auto null = std::nullopt;
    std::vector<TRow> expected = {
        {1, "a", 1, 100, "x"},
        {1, "a", 1, null, "yy"},
        {null, "n", null, null, null},
        {2, "bb", 2, 400, null},
        {7, "seven", null, null, null},
        {1, "c", 1, 100, "x"},
        {1, "c", 1, null, "yy"},
        {null, "m", null, null, null},
    };
    std::sort(got.begin(), got.end());
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(got, expected);
}

// A RIGHT join builds its null-supplying left side. With a two-column key a
// row matches only on both components: a probe row that agrees on one of them
// is padded, and so is one whose nullable component is NULL.
TEST(RowJoinProcessor, RightOuterMultiColumnKeyBuildsLeft) {
    using namespace NQumir::NAst;
    auto i64 = std::make_shared<TIntegerType>(TIntegerType::I64);
    auto nullableI64 = std::make_shared<TNullable>(i64);
    TStructType leftType({{"la", i64}, {"lb", nullableI64}, {"lv", i64}});
    TStructType rightType({{"ra", i64}, {"rb", nullableI64}, {"rv", i64}});

    // Build: (1, 10, 100), (1, 20, 200), (2, 10, 300), (1, 10, 400), (3, NULL, 500).
    std::vector<int64_t> la = {1, 1, 2, 1, 3};
    std::vector<int64_t> lb = {10, 20, 10, 10, 0};
    std::vector<uint8_t> lbMask = {0b01111};
    std::vector<int64_t> lv = {100, 200, 300, 400, 500};
    std::vector<TColumn> lcols = {
        TColumn{.Data = reinterpret_cast<char*>(la.data())},
        TColumn{.Data = reinterpret_cast<char*>(lb.data()), .Mask = lbMask.data()},
        TColumn{.Data = reinterpret_cast<char*>(lv.data())},
    };
    TRowSet build{.Columns = lcols.data(), .ColumnCount = 3, .RowCount = 5, .RefCount = 1};

    // Probe: (1, 10, 1), (1, 30, 2), (2, 20, 3), (3, NULL, 4), (2, 10, 5).
    std::vector<int64_t> ra = {1, 1, 2, 3, 2};
    std::vector<int64_t> rb = {10, 30, 20, 0, 10};
    std::vector<uint8_t> rbMask = {0b10111};
    std::vector<int64_t> rv = {1, 2, 3, 4, 5};
    std::vector<TColumn> rcols = {
        TColumn{.Data = reinterpret_cast<char*>(ra.data())},
        TColumn{.Data = reinterpret_cast<char*>(rb.data()), .Mask = rbMask.data()},
        TColumn{.Data = reinterpret_cast<char*>(rv.data())},
    };
    TRowSet probe{.Columns = rcols.data(), .ColumnCount = 3, .RowCount = 5, .RefCount = 1};

    auto spec = NKernel::BuildJoinKernelSpec(
        leftType, rightType, {{"la", "ra"}, {"lb", "rb"}}, EJoinType::Right);
    using TRow = std::array<std::optional<int64_t>, 6>;
    std::vector<TRow> got;
    for (auto& out : RunRowJoin(spec, EJoinBuildSide::Left, {build}, {probe})) {
        EXPECT_EQ(out.ColumnCount, 6);
        for (int64_t i = 0; i < out.RowCount; ++i) {
            TRow row;
            for (int c = 0; c < 6; ++c) {
                row[c] = I64At(out.Columns[c], i);
            }
            got.push_back(row);
        }
        Release(&out);
    }

    const auto null = std::nullopt;
    std::vector<TRow> expected = {
        TRow{1, 10, 100, 1, 10, 1},
        TRow{1, 10, 400, 1, 10, 1},
        TRow{null, null, null, 1, 30, 2},
        TRow{null, null, null, 2, 20, 3},
        TRow{null, null, null, 3, null, 4},
        TRow{2, 10, 300, 2, 10, 5},
    };
    std::sort(got.begin(), got.end());
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(got, expected);
}

// An empty build side pads every preserved row; an empty preserved side
// produces nothing.
TEST(RowJoinProcessor, LeftOuterEmptySides) {
    using namespace NQumir::NAst;
    auto i64 = std::make_shared<TIntegerType>(TIntegerType::I64);
    TStructType leftType({{"lk", i64}});
    TStructType rightType({{"rk", i64}, {"rs", std::make_shared<TStringType>()}});
    auto spec = NKernel::BuildJoinKernelSpec(leftType, rightType, {{"lk", "rk"}}, EJoinType::Left);

    std::vector<int64_t> lk = {4, 5, 4};
    std::vector<TColumn> lcols = {TColumn{.Data = reinterpret_cast<char*>(lk.data())}};
    TRowSet probe{.Columns = lcols.data(), .ColumnCount = 1, .RowCount = 3, .RefCount = 1};
    std::vector<std::pair<int64_t, std::optional<std::string>>> got;
    for (auto& out : RunRowJoin(spec, EJoinBuildSide::Right, {probe}, {})) {
        for (int64_t i = 0; i < out.RowCount; ++i) {
            EXPECT_EQ(I64At(out.Columns[1], i), std::nullopt);
            got.emplace_back(*I64At(out.Columns[0], i), StringOrNullAt(out.Columns[2], i));
        }
        Release(&out);
    }
    std::sort(got.begin(), got.end());
    EXPECT_EQ(got, (std::vector<std::pair<int64_t, std::optional<std::string>>>{
        {4, std::nullopt}, {4, std::nullopt}, {5, std::nullopt}}));

    std::vector<int64_t> rk = {4};
    std::string rsBytes = "four";
    std::vector<int64_t> rsOffsets = {0, 4};
    std::vector<TColumn> rcols = {
        TColumn{.Data = reinterpret_cast<char*>(rk.data())},
        TColumn{.Data = rsBytes.data(), .Offsets = rsOffsets.data(), .OffsetWidth = 8},
    };
    TRowSet build{.Columns = rcols.data(), .ColumnCount = 2, .RowCount = 1, .RefCount = 1};
    EXPECT_TRUE(RunRowJoin(spec, EJoinBuildSide::Right, {}, {build}).empty());
}

namespace {

using TSemiAntiRow = std::tuple<std::optional<int64_t>, std::string, std::optional<int64_t>>;

// Collects (lk, lname, lv) from the output batches of a SEMI/ANTI row join,
// which carry the left columns only, and releases them.
std::vector<TSemiAntiRow> SemiAntiRows(std::vector<TRowSet> outputs) {
    std::vector<TSemiAntiRow> got;
    for (auto& out : outputs) {
        EXPECT_EQ(out.ColumnCount, 3);
        for (int64_t i = 0; i < out.RowCount; ++i) {
            got.emplace_back(
                I64At(out.Columns[0], i),
                StringAt(out.Columns[1], i),
                I64At(out.Columns[2], i));
        }
        Release(&out);
    }
    std::sort(got.begin(), got.end());
    return got;
}

} // namespace

// A SEMI/ANTI join that builds its left side marks the build rows the probe
// matches and emits them once the probe side ends: SEMI the marked rows, ANTI
// the others. Key 1 has two build rows and four probe rows in two batches, and
// each build row comes out once. A NULL key matches nothing, on either side:
// ANTI emits its build rows, NOT EXISTS style, with their string and nullable
// payloads. Unselected rows of both sides take no part.
TEST(RowJoinProcessor, SemiAntiBuildLeftEmitsEachLeftRowOnce) {
    using namespace NQumir::NAst;
    auto i64 = std::make_shared<TIntegerType>(TIntegerType::I64);
    auto nullableI64 = std::make_shared<TNullable>(i64);
    TStructType leftType(
        {{"lk", nullableI64}, {"lname", std::make_shared<TStringType>()}, {"lv", nullableI64}});
    TStructType rightType({{"rk", nullableI64}});

    // Build batch 1: (1, "a", 10), (2, "bb", NULL), (NULL, "n", 30), (1, "c", 40).
    TNullableColumn<int64_t> lk1({1, 2, std::nullopt, 1});
    TStringColumn lname1({"a", "bb", "n", "c"});
    TNullableColumn<int64_t> lv1({10, std::nullopt, 30, 40});
    std::vector<TColumn> lcols1 = {lk1.Column(), lname1.Column(), lv1.Column()};
    TRowSet build1{.Columns = lcols1.data(), .ColumnCount = 3, .RowCount = 4, .RefCount = 1};
    // Build batch 2: (5, "e", 50), (7, "z", 70) unselected, (NULL, "m", NULL), (2, "d", 80).
    TNullableColumn<int64_t> lk2({5, 7, std::nullopt, 2});
    TStringColumn lname2({"e", "z", "m", "d"});
    TNullableColumn<int64_t> lv2({50, 70, std::nullopt, 80});
    std::vector<uint8_t> selection2 = {1, 0, 1, 1};
    std::vector<TColumn> lcols2 = {lk2.Column(), lname2.Column(), lv2.Column()};
    TRowSet build2{
        .Columns = lcols2.data(),
        .ColumnCount = 3,
        .RowCount = 4,
        .Selection = selection2.data(),
        .RefCount = 1,
    };

    // Probe batch 1: 1, 1, NULL, 9. Probe batch 2: 1, 2, 5 (unselected), 1, NULL.
    TNullableColumn<int64_t> rk1({1, 1, std::nullopt, 9});
    std::vector<TColumn> rcols1 = {rk1.Column()};
    TRowSet probe1{.Columns = rcols1.data(), .ColumnCount = 1, .RowCount = 4, .RefCount = 1};
    TNullableColumn<int64_t> rk2({1, 2, 5, 1, std::nullopt});
    std::vector<uint8_t> probeSelection2 = {1, 1, 0, 1, 1};
    std::vector<TColumn> rcols2 = {rk2.Column()};
    TRowSet probe2{
        .Columns = rcols2.data(),
        .ColumnCount = 1,
        .RowCount = 5,
        .Selection = probeSelection2.data(),
        .RefCount = 1,
    };

    const auto null = std::nullopt;
    for (auto type : {EJoinType::LeftSemi, EJoinType::LeftAnti}) {
        SCOPED_TRACE(JoinTypeName(type));
        for (auto* batch : {&build1, &build2, &probe1, &probe2}) {
            batch->RefCount = 1;
        }
        auto spec = NKernel::BuildJoinKernelSpec(leftType, rightType, {{"lk", "rk"}}, type);
        auto got = SemiAntiRows(
            RunRowJoin(spec, EJoinBuildSide::Left, {build1, build2}, {probe1, probe2}));
        std::vector<TSemiAntiRow> expected = type == EJoinType::LeftSemi
            ? std::vector<TSemiAntiRow>{{1, "a", 10}, {1, "c", 40}, {2, "bb", null}, {2, "d", 80}}
            : std::vector<TSemiAntiRow>{{null, "m", null}, {null, "n", 30}, {5, "e", 50}};
        std::sort(expected.begin(), expected.end());
        EXPECT_EQ(got, expected);
    }
}

// Without probe rows SEMI emits nothing and ANTI every selected build row,
// NULL keys included; without build rows neither emits anything.
TEST(RowJoinProcessor, SemiAntiBuildLeftEmptySides) {
    using namespace NQumir::NAst;
    auto i64 = std::make_shared<TIntegerType>(TIntegerType::I64);
    auto nullableI64 = std::make_shared<TNullable>(i64);
    TStructType leftType(
        {{"lk", nullableI64}, {"lname", std::make_shared<TStringType>()}, {"lv", nullableI64}});
    TStructType rightType({{"rk", nullableI64}});

    TNullableColumn<int64_t> lk({4, std::nullopt, 4});
    TStringColumn lname({"x", "y", "z"});
    TNullableColumn<int64_t> lv({1, 2, std::nullopt});
    std::vector<TColumn> lcols = {lk.Column(), lname.Column(), lv.Column()};
    TRowSet build{.Columns = lcols.data(), .ColumnCount = 3, .RowCount = 3, .RefCount = 1};
    TNullableColumn<int64_t> rk({4});
    std::vector<TColumn> rcols = {rk.Column()};
    TRowSet probe{.Columns = rcols.data(), .ColumnCount = 1, .RowCount = 1, .RefCount = 1};

    const auto null = std::nullopt;
    for (auto type : {EJoinType::LeftSemi, EJoinType::LeftAnti}) {
        SCOPED_TRACE(JoinTypeName(type));
        auto spec = NKernel::BuildJoinKernelSpec(leftType, rightType, {{"lk", "rk"}}, type);
        build.RefCount = 1;
        const auto got = SemiAntiRows(RunRowJoin(spec, EJoinBuildSide::Left, {build}, {}));
        const std::vector<TSemiAntiRow> expected = type == EJoinType::LeftSemi
            ? std::vector<TSemiAntiRow>{}
            : std::vector<TSemiAntiRow>{{null, "y", 2}, {4, "x", 1}, {4, "z", null}};
        EXPECT_EQ(got, expected);
        probe.RefCount = 1;
        EXPECT_TRUE(RunRowJoin(spec, EJoinBuildSide::Left, {}, {probe}).empty());
    }
}

// 40,000 unique dense keys fill three arena blocks, and the emit scan must
// read the full ones and the partly filled last one. Every third key is probed
// three times, in batches that each match it once, so a SEMI row is emitted
// once however often its key is matched.
TEST(RowJoinProcessor, SemiAntiBuildLeftManyBlocksUniqueKeys) {
    using namespace NQumir::NAst;
    auto i64 = std::make_shared<TIntegerType>(TIntegerType::I64);
    TStructType leftType({{"lk", i64}, {"lv", i64}});
    TStructType rightType({{"rk", i64}});

    constexpr int64_t buildRows = 40000;
    std::vector<int64_t> lk(buildRows), lv(buildRows);
    for (int64_t i = 0; i < buildRows; ++i) {
        lk[i] = i;
        lv[i] = i * 10;
    }
    std::vector<TColumn> lcols = {
        TColumn{.Data = reinterpret_cast<char*>(lk.data())},
        TColumn{.Data = reinterpret_cast<char*>(lv.data())},
    };
    TRowSet build{.Columns = lcols.data(), .ColumnCount = 2, .RowCount = buildRows, .RefCount = 1};
    std::vector<int64_t> rk;
    for (int64_t i = 0; i < buildRows; i += 3) {
        rk.push_back(i);
    }
    const auto probeRows = static_cast<int64_t>(rk.size());
    std::vector<TColumn> rcols = {TColumn{.Data = reinterpret_cast<char*>(rk.data())}};

    for (auto type : {EJoinType::LeftSemi, EJoinType::LeftAnti}) {
        SCOPED_TRACE(JoinTypeName(type));
        build.RefCount = 1;
        std::vector<TRowSet> probes(3, TRowSet{
            .Columns = rcols.data(), .ColumnCount = 1, .RowCount = probeRows, .RefCount = 1});
        auto spec = NKernel::BuildJoinKernelSpec(leftType, rightType, {{"lk", "rk"}}, type);
        std::vector<int64_t> got;
        for (auto& out : RunRowJoin(spec, EJoinBuildSide::Left, {build}, probes)) {
            for (int64_t i = 0; i < out.RowCount; ++i) {
                const auto key = *I64At(out.Columns[0], i);
                EXPECT_EQ(*I64At(out.Columns[1], i), key * 10);
                got.push_back(key);
            }
            Release(&out);
        }
        std::vector<int64_t> expected;
        for (int64_t i = 0; i < buildRows; ++i) {
            if ((i % 3 == 0) == (type == EJoinType::LeftSemi)) {
                expected.push_back(i);
            }
        }
        // EmitByMark scans the arena in build order.
        EXPECT_EQ(got, expected);
    }
}

// The row join table marks only the left rows of a SEMI/ANTI join and runs no
// residual, so it cannot build the preserved side of an outer join, the right
// side of a SEMI/ANTI join, or decide a residual's matches.
TEST(RowJoinProcessor, RejectsJoinsItCannotStream) {
    using namespace NQumir::NAst;
    auto i64 = std::make_shared<TIntegerType>(TIntegerType::I64);
    TStructType leftType({{"lk", i64}, {"lv", i64}});
    TStructType rightType({{"rk", i64}, {"rv", i64}});
    TKernelCompiler compiler;
    auto spec = [&](EJoinType type, TExprPtr residual = nullptr) {
        return NKernel::BuildJoinKernelSpec(
            leftType, rightType, {{"lk", "rk"}}, type, residual);
    };
    EXPECT_THROW(compiler.CompileRowJoin(spec(EJoinType::Left), /*buildIsLeft=*/true),
        NQumir::TError);
    EXPECT_THROW(compiler.CompileRowJoin(spec(EJoinType::Right), /*buildIsLeft=*/false),
        NQumir::TError);
    EXPECT_THROW(compiler.CompileRowJoin(spec(EJoinType::Full), /*buildIsLeft=*/false),
        NQumir::TError);
    EXPECT_THROW(compiler.CompileRowJoin(spec(EJoinType::LeftSemi), /*buildIsLeft=*/false),
        NQumir::TError);
    EXPECT_THROW(compiler.CompileRowJoin(spec(EJoinType::LeftAnti), /*buildIsLeft=*/false),
        NQumir::TError);
    using namespace NQumir::NAst::NLiterals;
    auto residual = std::make_shared<TBinaryExpr>(
        NQumir::TLocation{},
        "<"_op,
        std::make_shared<TIdentExpr>(NQumir::TLocation{}, "lv"),
        std::make_shared<TIdentExpr>(NQumir::TLocation{}, "rv"));
    EXPECT_THROW(
        compiler.CompileRowJoin(spec(EJoinType::Left, residual), /*buildIsLeft=*/false),
        NQumir::TError);
    EXPECT_THROW(
        compiler.CompileRowJoin(spec(EJoinType::LeftSemi, residual), /*buildIsLeft=*/true),
        NQumir::TError);
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    NQumir::NCodeGen::TLLVMInitializer initializer;
    return RUN_ALL_TESTS();
}
