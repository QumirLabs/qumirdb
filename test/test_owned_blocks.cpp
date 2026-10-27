#include <gtest/gtest.h>

#include <qdb/kernel/lib.h>
#include <qdb/modules/qumirdb.h>
#include <qdb/modules/qumirdb_runtime.h>

#include "qumirdb_source_module.h"

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/runner/runner_llvm.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace {

using TArenaAllocate =
    uint8_t*(*)(uint8_t***, int64_t*, int64_t*, int64_t);
using TArenaDestroy = void(*)(uint8_t***, int64_t*, int64_t*);
using TArenaRewind = void(*)(uint8_t**, int64_t, uint8_t*, int64_t);
using TArenaAdapterTest = bool(*)();

constexpr const char* ArenaAdapterTestSource = R"(
(block
  ;; Exercise the HashTable adapters without exporting or mirroring its ABI.
  (fun test_aht_owned_arena_adapters () -> bool
    (block
      (var ht HashTable)
      (field_assign ht OwnedBlocks (cast 0 <ptr <ptr u8>>))
      (field_assign ht OwnedBlockCount 0)
      (field_assign ht OwnedBlockCapacity 0)
      (var ok = #t)
      (var allocation = (cast 0 <ptr u8>))
      (var index = 0)
      (while (&& ok (< index 10000))
        (block
          (= allocation (call aht_owned_arena_alloc ht 17))
          (if (== (cast allocation i64) 0)
            (block (= ok #f)))
          (= index (+ index 1))))

      ;; Six chunks force the registry to reallocate from four to eight slots.
      (if (!= (field ht OwnedBlockCount) 6)
        (block (= ok #f)))
      (if (!= (field ht OwnedBlockCapacity) 8)
        (block (= ok #f)))

      (var rewind_target = (call aht_owned_arena_alloc ht 19))
      (if (== (cast rewind_target i64) 0)
        (block (= ok #f)))
      (call aht_owned_arena_rewind ht rewind_target 19)
      (var reused = (call aht_owned_arena_alloc ht 19))
      (if (!= (cast reused i64) (cast rewind_target i64))
        (block (= ok #f)))

      (call owned_arena_destroy
        (field ht OwnedBlocks)
        (field ht OwnedBlockCount)
        (field ht OwnedBlockCapacity))
      (return (&& ok
                  (== (cast (field ht OwnedBlocks) i64) 0)
                  (== (field ht OwnedBlockCount) 0)
                  (== (field ht OwnedBlockCapacity) 0))))))
)";

std::unique_ptr<NQumir::TLLVMRunner> CompileOwnedBlocks(
    const std::string& entryName,
    void*& entry)
{
    std::vector<NQumir::NAst::TExprPtr> functions;
    for (const char* name : {
             "owned_arena_lifecycle.oz", "owned_blocks.oz"}) {
        auto library = NQdb::NKernel::ParseFunctionLibrary(
            NQdb::NKernel::ReadAggregationKernel(name));
        if (!library) {
            ADD_FAILURE() << name << ": " << library.error().ToString();
            return {};
        }
        functions.insert(functions.end(), library->begin(), library->end());
    }
    auto adapter = NQdb::NKernel::ParseFunctionLibrary(ArenaAdapterTestSource);
    if (!adapter) {
        ADD_FAILURE() << "arena adapter test: " << adapter.error().ToString();
        return {};
    }
    functions.insert(functions.end(), adapter->begin(), adapter->end());
    NQumir::TLLVMRunnerOptions options;
    options.CoreInput = true;
    options.NativeCode = true;
    NQdb::NTest::ConfigureQumirDbSourceModule(options);
    auto runner = std::make_unique<NQumir::TLLVMRunner>(options);
    auto program = std::make_shared<NQumir::NAst::TBlockExpr>(
        NQumir::TLocation{}, std::move(functions));
    NQdb::NTest::AddQumirDbUse(program);
    std::string error;
    entry = runner->CompileKernelAst(program, entryName, &error);
    EXPECT_NE(entry, nullptr) << error;
    return runner;
}

TEST(OwnedBlocks, ArenaPacksSmallAllocationsIntoGrowingChunks) {
    void* allocateEntry = nullptr;
    auto allocateRunner =
        CompileOwnedBlocks("owned_arena_alloc", allocateEntry);
    ASSERT_NE(allocateEntry, nullptr);
    void* destroyEntry = nullptr;
    auto destroyRunner =
        CompileOwnedBlocks("owned_arena_destroy", destroyEntry);
    ASSERT_NE(destroyEntry, nullptr);
    auto allocate = reinterpret_cast<TArenaAllocate>(allocateEntry);
    auto destroy = reinterpret_cast<TArenaDestroy>(destroyEntry);

    uint8_t** blocks = nullptr;
    int64_t count = 0;
    int64_t capacity = 0;
    std::vector<uint8_t*> allocations;
    allocations.reserve(10'000);
    for (int64_t i = 0; i < 10'000; ++i) {
        auto* bytes = allocate(&blocks, &count, &capacity, 17);
        ASSERT_NE(bytes, nullptr);
        std::fill_n(bytes, 17, static_cast<uint8_t>(i));
        allocations.push_back(bytes);
    }

    ASSERT_EQ(count, 6);
    int64_t expectedCapacity = 4'096;
    for (int64_t i = 0; i < count; ++i) {
        EXPECT_EQ(reinterpret_cast<int64_t*>(blocks[i])[1], expectedCapacity);
        expectedCapacity *= 2;
    }
    for (int64_t i = 0; i < 10'000; ++i) {
        EXPECT_EQ(allocations[i][0], static_cast<uint8_t>(i));
        EXPECT_EQ(allocations[i][16], static_cast<uint8_t>(i));
    }

    destroy(&blocks, &count, &capacity);
    EXPECT_EQ(blocks, nullptr);
    EXPECT_EQ(count, 0);
    EXPECT_EQ(capacity, 0);
}

TEST(OwnedBlocks, ArenaCanRewindMostRecentAllocation) {
    void* allocateEntry = nullptr;
    auto allocateRunner =
        CompileOwnedBlocks("owned_arena_alloc", allocateEntry);
    ASSERT_NE(allocateEntry, nullptr);
    void* rewindEntry = nullptr;
    auto rewindRunner = CompileOwnedBlocks("owned_arena_rewind", rewindEntry);
    ASSERT_NE(rewindEntry, nullptr);
    void* destroyEntry = nullptr;
    auto destroyRunner =
        CompileOwnedBlocks("owned_arena_destroy", destroyEntry);
    ASSERT_NE(destroyEntry, nullptr);
    auto allocate = reinterpret_cast<TArenaAllocate>(allocateEntry);
    auto rewind = reinterpret_cast<TArenaRewind>(rewindEntry);
    auto destroy = reinterpret_cast<TArenaDestroy>(destroyEntry);

    uint8_t** blocks = nullptr;
    int64_t count = 0;
    int64_t capacity = 0;
    auto* first = allocate(&blocks, &count, &capacity, 11);
    auto* second = allocate(&blocks, &count, &capacity, 37);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    rewind(blocks, count, first, 11);
    auto* third = allocate(&blocks, &count, &capacity, 19);
    EXPECT_EQ(third, second + 37);
    rewind(blocks, count, third, 19);
    EXPECT_EQ(allocate(&blocks, &count, &capacity, 19), third);
    EXPECT_EQ(count, 1);

    destroy(&blocks, &count, &capacity);
}

TEST(OwnedBlocks, ArenaCapsGrowthAfterOversizedAllocation) {
    void* allocateEntry = nullptr;
    auto allocateRunner =
        CompileOwnedBlocks("owned_arena_alloc", allocateEntry);
    ASSERT_NE(allocateEntry, nullptr);
    void* destroyEntry = nullptr;
    auto destroyRunner =
        CompileOwnedBlocks("owned_arena_destroy", destroyEntry);
    ASSERT_NE(destroyEntry, nullptr);
    auto allocate = reinterpret_cast<TArenaAllocate>(allocateEntry);
    auto destroy = reinterpret_cast<TArenaDestroy>(destroyEntry);

    uint8_t** blocks = nullptr;
    int64_t count = 0;
    int64_t capacity = 0;
    ASSERT_NE(allocate(&blocks, &count, &capacity, 3 * 1024 * 1024), nullptr);
    ASSERT_NE(allocate(&blocks, &count, &capacity, 1), nullptr);
    ASSERT_EQ(count, 2);
    EXPECT_EQ(reinterpret_cast<int64_t*>(blocks[0])[1], 3 * 1024 * 1024);
    EXPECT_EQ(reinterpret_cast<int64_t*>(blocks[1])[1], 2 * 1024 * 1024);

    destroy(&blocks, &count, &capacity);
}

TEST(OwnedBlocks, ArenaRejectsForeignBlockWithoutMutation) {
    void* allocateEntry = nullptr;
    auto allocateRunner =
        CompileOwnedBlocks("owned_arena_alloc", allocateEntry);
    ASSERT_NE(allocateEntry, nullptr);
    void* destroyEntry = nullptr;
    auto destroyRunner =
        CompileOwnedBlocks("owned_arena_destroy", destroyEntry);
    ASSERT_NE(destroyEntry, nullptr);
    auto allocate = reinterpret_cast<TArenaAllocate>(allocateEntry);
    auto destroy = reinterpret_cast<TArenaDestroy>(destroyEntry);

    auto** blocks = static_cast<uint8_t**>(qdb_alloc(sizeof(uint8_t*)));
    ASSERT_NE(blocks, nullptr);
    auto* foreign = static_cast<uint8_t*>(qdb_alloc(64));
    ASSERT_NE(foreign, nullptr);
    auto* plausibleHeader = reinterpret_cast<int64_t*>(foreign);
    plausibleHeader[0] = 4'096;
    plausibleHeader[1] = 0;
    blocks[0] = foreign;
    int64_t count = 1;
    int64_t capacity = 1;

    auto* bytes = allocate(&blocks, &count, &capacity, 17);
    EXPECT_EQ(bytes, nullptr);
    EXPECT_EQ(count, 1);
    EXPECT_EQ(capacity, 1);
    EXPECT_EQ(blocks[0], foreign);
    EXPECT_EQ(plausibleHeader[1], 0);

    destroy(&blocks, &count, &capacity);
}

TEST(OwnedBlocks, HashTableAdaptersWriteBackReallocatedRegistry) {
    void* entry = nullptr;
    auto runner = CompileOwnedBlocks("test_aht_owned_arena_adapters", entry);
    ASSERT_NE(entry, nullptr);
    auto testAdapters = reinterpret_cast<TArenaAdapterTest>(entry);
    EXPECT_TRUE(testAdapters());
}

TEST(OwnedBlocks, HashTableUsesFormerScratchSlots) {
    std::shared_ptr<NQumir::NAst::TStructType> hashTable;
    for (const auto& type : NQumir::NRegistry::QumirDbExternalTypes()) {
        if (type.Name == "HashTable") {
            hashTable = NQumir::NAst::TMaybeType<NQumir::NAst::TStructType>(
                type.Type).Cast();
            break;
        }
    }
    ASSERT_NE(hashTable, nullptr);
    ASSERT_EQ(hashTable->Fields.size(), 18u);
    EXPECT_EQ(hashTable->Fields[5].first, "OwnedBlocks");
    EXPECT_EQ(hashTable->Fields[6].first, "OwnedBlockCount");
    EXPECT_EQ(hashTable->Fields[7].first, "OwnedBlockCapacity");
    EXPECT_TRUE(NQumir::NAst::TMaybeType<NQumir::NAst::TPointerType>(
        hashTable->Fields[5].second));
    EXPECT_TRUE(NQumir::NAst::TMaybeType<NQumir::NAst::TIntegerType>(
        hashTable->Fields[6].second));
    EXPECT_TRUE(NQumir::NAst::TMaybeType<NQumir::NAst::TIntegerType>(
        hashTable->Fields[7].second));
}

} // namespace

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    NQumir::NCodeGen::TLLVMInitializer initializer;
    return RUN_ALL_TESTS();
}
