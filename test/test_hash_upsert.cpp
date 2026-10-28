#include <gtest/gtest.h>

#include <qdb/kernel/compiler.h>
#include <qdb/kernel/lib.h>

#include "qumirdb_source_module.h"

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/runner/runner_llvm.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace {

// Shared aggregation/join ABI. Inspect state IDs and output order across grow.
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
    std::array<uint8_t, NQdb::TKernelCompiler::RowArenaSize + NQdb::TKernelCompiler::RowTableSize
        + NQdb::TKernelCompiler::RowScratchSize> RowLayout = {};
    int64_t RowSwitchGroups = 0;
    int64_t RowExpectedGroups = 0;
    int64_t RowRebuilds = 0;
    int64_t RowMovedFromSlots = 0;
};
static_assert(sizeof(THashTable) == NQdb::TKernelCompiler::HashTableSize);

constexpr const char* TestEntries = R"(
(block
  (var test_ownership_checks = (: 0 i64))
  (var test_clones = (: 0 i64))
  (fun key_owned_bytes ((var key i64)) -> i64
    (block
      (= test_ownership_checks (+ test_ownership_checks 1))
      (return 0)))
  (fun key_clone_owned ((var key i64) (var storage <ptr u8>)) -> i64
    (block
      (= test_clones (+ test_clones 1))
      (return key)))
  (fun test_ownership_count () -> i64 (block (return test_ownership_checks)))
  (fun test_clone_count () -> i64 (block (return test_clones)))

  (fun test_hash_init ((var ht <ref HashTable>) (var capacity i64)) -> bool
    (block (return (call aht_init ht capacity 1 8))))
  (fun test_hash_destroy ((var ht <ref HashTable>))
    (block (call aht_destroy ht)))
  (fun test_hash_upsert ((var ht <ref HashTable>) (var key i64)
                        (var is_new <ref i64>) (var hash u64)) -> i64
    (block (return (call aht_upsert_dual ht key 0 is_new hash))))
  (fun test_hash_upsert_aggregate ((var ht <ref HashTable>) (var key i64)
                                  (var is_new <ref i64>) (var hash u64)) -> i64
    (block (return (call aht_upsert_aggregate ht key 0 is_new hash))))
  (fun test_hash_lookup ((var ht <ref HashTable>) (var key i64)
                        (var hash u64)) -> i64
    (block (return (call rh_lookup_dual
      (cast (field ht Keys) <ptr i64>) (field ht Ctrl) (field ht SlotId)
      (field ht Capacity) key hash))))
  (fun test_hash_probe ((var ht <ref HashTable>) (var key i64)
                       (var hash u64) (var empty_slot <ref i64>)) -> i64
    (block (return (call rh_lookup_or_empty_dual
      (cast (field ht Keys) <ptr i64>) (field ht Ctrl) (field ht SlotId)
      (field ht Capacity) key hash empty_slot)))))
)";

class HashUpsert : public testing::Test {
protected:
    using TInit = bool(*)(THashTable*, int64_t);
    using TDestroy = void(*)(THashTable*);
    using TUpsert = int64_t(*)(THashTable*, int64_t, int64_t*, uint64_t);
    using TLookup = int64_t(*)(THashTable*, int64_t, uint64_t);
    using TProbe = int64_t(*)(THashTable*, int64_t, uint64_t, int64_t*);
    using THash = uint64_t(*)(int64_t);

    void SetUp() override {
        std::vector<NQumir::NAst::TExprPtr> functions;
        for (const char* name : {
                 "key_ops_i64.oz", "swiss_group.oz",
                 "robin_hood_rehash_generic.oz", "owned_arena_lifecycle.oz",
                 "aggregation_hashtable_generic.oz", "owned_blocks.oz",
                 "robin_hood_dual_key.oz"}) {
            auto library = NQdb::NKernel::ParseFunctionLibrary(
                NQdb::NKernel::ReadAggregationKernel(name), {"aht_update"});
            ASSERT_TRUE(library) << name << ": " << library.error().ToString();
            functions.insert(functions.end(), library->begin(), library->end());
        }
        auto entries = NQdb::NKernel::ParseFunctionLibrary(TestEntries);
        ASSERT_TRUE(entries) << entries.error().ToString();
        functions.insert(functions.end(), entries->begin(), entries->end());

        auto options = NQdb::KernelRunnerOptions();
        options.CoreInput = true;
        options.NativeCode = true;
        options.RunDefiniteAssignment = true;
        NQdb::NTest::ConfigureQumirDbSourceModule(options);
        Runner_ = std::make_unique<NQumir::TLLVMRunner>(options);
        auto program = std::make_shared<NQumir::NAst::TBlockExpr>(
            NQumir::TLocation{}, std::move(functions));
        NQdb::NTest::AddQumirDbUse(program);
        std::string error;
        auto kernels = Runner_->CompileKernelAst(program, {
            "test_hash_init", "test_hash_destroy", "test_hash_upsert",
            "test_hash_lookup", "test_hash_probe", "rh_hash", "swiss_group_width",
            "test_ownership_count", "test_clone_count", "test_hash_upsert_aggregate"}, &error);
        ASSERT_EQ(kernels.size(), 10u) << error;
        Init_ = reinterpret_cast<TInit>(kernels.at("test_hash_init"));
        Destroy_ = reinterpret_cast<TDestroy>(kernels.at("test_hash_destroy"));
        Upsert_ = reinterpret_cast<TUpsert>(kernels.at("test_hash_upsert"));
        UpsertAggregate_ = reinterpret_cast<TUpsert>(kernels.at("test_hash_upsert_aggregate"));
        Lookup_ = reinterpret_cast<TLookup>(kernels.at("test_hash_lookup"));
        Probe_ = reinterpret_cast<TProbe>(kernels.at("test_hash_probe"));
        Hash_ = reinterpret_cast<THash>(kernels.at("rh_hash"));
        Width_ = reinterpret_cast<int64_t(*)()>(kernels.at("swiss_group_width"))();
        OwnershipCount_ = reinterpret_cast<int64_t(*)()>(kernels.at("test_ownership_count"));
        CloneCount_ = reinterpret_cast<int64_t(*)()>(kernels.at("test_clone_count"));
    }

    void TearDown() override {
        if (Destroy_) {
            Destroy_(&Table_);
        }
    }

    std::unique_ptr<NQumir::TLLVMRunner> Runner_;
    THashTable Table_;
    TInit Init_ = nullptr;
    TDestroy Destroy_ = nullptr;
    TUpsert Upsert_ = nullptr;
    TUpsert UpsertAggregate_ = nullptr;
    TLookup Lookup_ = nullptr;
    TProbe Probe_ = nullptr;
    THash Hash_ = nullptr;
    int64_t Width_ = 0;
    int64_t (*OwnershipCount_)() = nullptr;
    int64_t (*CloneCount_)() = nullptr;
};

TEST_F(HashUpsert, ExistingKeySkipsOwnershipAndCloning) {
    ASSERT_TRUE(Init_(&Table_, Width_));
    int64_t isNew = -1;
    ASSERT_EQ(Upsert_(&Table_, 100, &isNew, Hash_(100)), 0);
    EXPECT_EQ(isNew, 1);
    ASSERT_EQ(OwnershipCount_(), 1);
    ASSERT_EQ(CloneCount_(), 1);
    Table_.AggBuffers[0][0] = 123;
    auto* keys = Table_.Keys;
    auto* ctrl = Table_.Ctrl;
    auto* slotIds = Table_.SlotId;
    for (int i = 0; i < 10; ++i) {
        isNew = -1;
        EXPECT_EQ(Upsert_(&Table_, 100, &isNew, Hash_(100)), 0);
        EXPECT_EQ(isNew, 0);
    }
    EXPECT_EQ(OwnershipCount_(), 1);
    EXPECT_EQ(CloneCount_(), 1);
    EXPECT_EQ(Table_.Size, 1);
    EXPECT_EQ(Table_.Capacity, Width_);
    EXPECT_EQ(Table_.Keys, keys);
    EXPECT_EQ(Table_.Ctrl, ctrl);
    EXPECT_EQ(Table_.SlotId, slotIds);
    EXPECT_EQ(Table_.AggBuffers[0][0], 123);
}

TEST_F(HashUpsert, FailedMissDoesNotCloneOrModifyTheTable) {
    ASSERT_TRUE(Init_(&Table_, Width_));
    // Saturated control bytes exercise the no-empty-slot failure, independently
    // of normal tables' 87.5 percent growth limit.
    std::fill_n(Table_.Ctrl, Width_, uint8_t{37});
    for (int64_t slot = 0; slot < Width_; ++slot) {
        reinterpret_cast<int64_t*>(Table_.Keys)[slot] = slot;
        Table_.SlotId[slot] = slot;
        Table_.AggBuffers[0][slot] = slot + 100;
    }
    int64_t isNew = -1;
    EXPECT_EQ(Upsert_(&Table_, 100, &isNew, 37), -1);
    EXPECT_EQ(isNew, 0);
    EXPECT_EQ(OwnershipCount_(), 0);
    EXPECT_EQ(CloneCount_(), 0);
    EXPECT_EQ(Table_.Size, 0);
    EXPECT_EQ(Table_.Capacity, Width_);
    EXPECT_EQ(Table_.OwnedBlockCount, 0);
    for (int64_t slot = 0; slot < Width_; ++slot) {
        EXPECT_EQ(Table_.Ctrl[slot], 37);
        EXPECT_EQ(reinterpret_cast<int64_t*>(Table_.Keys)[slot], slot);
        EXPECT_EQ(Table_.SlotId[slot], slot);
        EXPECT_EQ(Table_.AggBuffers[0][slot], slot + 100);
    }
}

TEST_F(HashUpsert, CollisionsCrossGroupsAndWrapAround) {
    ASSERT_TRUE(Init_(&Table_, 4 * Width_));
    // All keys share H1 and H2. Start in the last group, wrap to group zero,
    // then follow the next triangular step to group two.
    constexpr uint64_t hash = (uint64_t{3} << 7) | 37;
    for (int64_t key = 0; key < 5 * Width_ / 2; ++key) {
        int64_t empty = -1;
        EXPECT_EQ(Probe_(&Table_, key, hash, &empty), -1);
        ASSERT_GE(empty, 0);
        EXPECT_EQ(Table_.Ctrl[empty], 0x80);
        int64_t isNew = -1;
        ASSERT_EQ(Upsert_(&Table_, key, &isNew, hash), key);
        EXPECT_EQ(isNew, 1);
        EXPECT_EQ(Table_.SlotId[empty], key);
        EXPECT_EQ(Table_.Ctrl[empty], 37);
        EXPECT_EQ(Table_.AggBuffers[0][key], 0);
        Table_.AggBuffers[0][key] = 100 + key;
    }
    EXPECT_EQ(Table_.SlotId[3 * Width_], 0);
    EXPECT_EQ(Table_.SlotId[0], Width_);
    EXPECT_EQ(Table_.SlotId[2 * Width_], 2 * Width_);
    EXPECT_EQ(Table_.Size, 5 * Width_ / 2);

    for (int64_t key = 0; key < 5 * Width_ / 2; ++key) {
        int64_t empty = 999;
        EXPECT_EQ(Probe_(&Table_, key, hash, &empty), key);
        EXPECT_EQ(empty, -1);
        EXPECT_EQ(Lookup_(&Table_, key, hash), key);
        int64_t isNew = -1;
        EXPECT_EQ(Upsert_(&Table_, key, &isNew, hash), key);
        EXPECT_EQ(isNew, 0);
        EXPECT_EQ(Table_.AggBuffers[0][key], 100 + key);
    }
    EXPECT_EQ(Lookup_(&Table_, 100, hash), -1);
    EXPECT_EQ(Table_.Size, 5 * Width_ / 2);
    EXPECT_EQ(Table_.Capacity, 4 * Width_);
}

TEST_F(HashUpsert, ExistingKeyAtLoadLimitDoesNotGrow) {
    ASSERT_TRUE(Init_(&Table_, Width_));
    for (int64_t key = 0; key < Width_ - Width_ / 8; ++key) {
        int64_t isNew = -1;
        ASSERT_EQ(Upsert_(&Table_, key, &isNew, Hash_(key)), key);
        ASSERT_EQ(isNew, 1);
        Table_.AggBuffers[0][key] = 100 + key;
    }
    for (int64_t key = 0; key < Width_ - Width_ / 8; ++key) {
        int64_t isNew = -1;
        EXPECT_EQ(Upsert_(&Table_, key, &isNew, Hash_(key)), key);
        EXPECT_EQ(isNew, 0);
        EXPECT_EQ(Table_.AggBuffers[0][key], 100 + key);
    }
    EXPECT_EQ(Table_.Size, Width_ - Width_ / 8);
    EXPECT_EQ(Table_.Capacity, Width_);
}

TEST_F(HashUpsert, GrowDiscardsSavedSlotAndPreservesDenseStates) {
    ASSERT_TRUE(Init_(&Table_, Width_));
    for (int64_t key = 0; key < Width_ - Width_ / 8; ++key) {
        int64_t isNew = -1;
        ASSERT_EQ(Upsert_(&Table_, key, &isNew, Hash_(key)), key);
        Table_.AggBuffers[0][key] = 100 + key;
    }
    // The old table has one group. Doubling adds group one; the incoming
    // key must probe there, so reusing the saved slot in group zero loses it.
    const auto limit = Width_ - Width_ / 8;
    int64_t newKey = limit;
    while ((Hash_(newKey) & 128) == 0) {
        ++newKey;
    }
    int64_t empty = -1;
    ASSERT_EQ(Probe_(&Table_, newKey, Hash_(newKey), &empty), -1);
    ASSERT_EQ(empty, limit);
    int64_t isNew = -1;
    ASSERT_EQ(Upsert_(&Table_, newKey, &isNew, Hash_(newKey)), limit);
    EXPECT_EQ(isNew, 1);
    ASSERT_EQ(Table_.Capacity, 2 * Width_);
    EXPECT_EQ(Table_.Size, limit + 1);
    EXPECT_EQ(Lookup_(&Table_, newKey, Hash_(newKey)), limit);
    EXPECT_EQ(Table_.AggBuffers[0][limit], 0);
    EXPECT_EQ(reinterpret_cast<int64_t*>(Table_.GroupKeys)[limit], newKey);
    for (int64_t key = 0; key < Width_ - Width_ / 8; ++key) {
        EXPECT_EQ(Lookup_(&Table_, key, Hash_(key)), key);
        EXPECT_EQ(Table_.AggBuffers[0][key], 100 + key);
    }
}

TEST_F(HashUpsert, MultipleGrowsKeepEveryKeyAndDuplicateState) {
    ASSERT_TRUE(Init_(&Table_, Width_));
    constexpr int64_t count = 1024;
    for (int64_t key = 0; key < count; ++key) {
        int64_t isNew = -1;
        ASSERT_EQ(Upsert_(&Table_, key, &isNew, Hash_(key)), key);
        ASSERT_EQ(isNew, 1);
        Table_.AggBuffers[0][key] = 100 + key;
    }
    const auto capacity = Table_.Capacity;
    for (int64_t key = count - 1; key >= 0; --key) {
        EXPECT_EQ(Lookup_(&Table_, key, Hash_(key)), key);
        EXPECT_EQ(reinterpret_cast<int64_t*>(Table_.GroupKeys)[key], key);
        int64_t isNew = -1;
        EXPECT_EQ(Upsert_(&Table_, key, &isNew, Hash_(key)), key);
        EXPECT_EQ(isNew, 0);
        EXPECT_EQ(Table_.AggBuffers[0][key], 100 + key);
    }
    EXPECT_EQ(Table_.Size, count);
    EXPECT_EQ(Table_.Capacity, capacity);
}

TEST_F(HashUpsert, FullTableProbeTerminatesWithoutAnEmptySlot) {
    ASSERT_TRUE(Init_(&Table_, Width_));
    std::fill_n(Table_.Ctrl, Width_, uint8_t{37});
    for (int64_t slot = 0; slot < Width_; ++slot) {
        reinterpret_cast<int64_t*>(Table_.Keys)[slot] = slot;
        Table_.SlotId[slot] = slot;
    }
    int64_t empty = 999;
    EXPECT_EQ(Probe_(&Table_, 100, 37, &empty), -1);
    EXPECT_EQ(empty, -1);
    EXPECT_EQ(Lookup_(&Table_, 100, 37), -1);
    EXPECT_EQ(Lookup_(&Table_, 3, 37), 3);
}

TEST_F(HashUpsert, AggregateHitReturnsPhysicalStateAndSkipsCloning) {
    ASSERT_TRUE(Init_(&Table_, 4 * Width_));
    constexpr uint64_t hash = (uint64_t{3} << 7) | 37;
    int64_t isNew = -1;
    const auto state = UpsertAggregate_(&Table_, 100, &isNew, hash);
    ASSERT_EQ(state, 3 * Width_);
    ASSERT_EQ(isNew, 1);
    ASSERT_EQ(Table_.SlotId[0], state);
    EXPECT_EQ(reinterpret_cast<int64_t*>(Table_.GroupKeys)[0], 100);
    EXPECT_EQ(Table_.AggBuffers[0][state], 0);
    Table_.AggBuffers[0][state] = 123;
    for (int i = 0; i < 10; ++i) {
        isNew = -1;
        EXPECT_EQ(UpsertAggregate_(&Table_, 100, &isNew, hash), state);
        EXPECT_EQ(isNew, 0);
        EXPECT_EQ(Table_.AggBuffers[0][state], 123);
    }
    EXPECT_EQ(OwnershipCount_(), 1);
    EXPECT_EQ(CloneCount_(), 1);
    EXPECT_EQ(Table_.Size, 1);
    EXPECT_EQ(Table_.Capacity, 4 * Width_);
}

TEST_F(HashUpsert, AggregateGrowthMovesStatesAndPreservesOutputOrder) {
    ASSERT_TRUE(Init_(&Table_, Width_));
    constexpr int64_t count = 1024;
    int grows = 0;
    bool movedState = false;
    for (int64_t key = 0; key < count; ++key) {
        const auto oldCapacity = Table_.Capacity;
        std::vector<int64_t> oldSlots(Table_.SlotId, Table_.SlotId + key);
        int64_t isNew = -1;
        const auto state = UpsertAggregate_(&Table_, key, &isNew, Hash_(key));
        ASSERT_GE(state, 0);
        ASSERT_LT(state, Table_.Capacity);
        ASSERT_EQ(isNew, 1);
        EXPECT_EQ(Table_.SlotId[key], state);
        EXPECT_EQ(Table_.AggBuffers[0][state], 0);
        Table_.AggBuffers[0][state] = 100 + key;
        if (Table_.Capacity == oldCapacity) {
            continue;
        }
        ++grows;
        for (int64_t dense = 0; dense <= key; ++dense) {
            const auto slot = Table_.SlotId[dense];
            ASSERT_GE(slot, 0);
            ASSERT_LT(slot, Table_.Capacity);
            EXPECT_EQ(reinterpret_cast<int64_t*>(Table_.GroupKeys)[dense], dense);
            EXPECT_EQ(reinterpret_cast<int64_t*>(Table_.Keys)[slot], dense);
            EXPECT_EQ(Table_.Ctrl[slot], Hash_(dense) & 127);
            EXPECT_EQ(Table_.AggBuffers[0][slot], 100 + dense);
            if (dense < key && oldSlots[dense] != slot) {
                movedState = true;
            }
        }
    }
    EXPECT_GT(grows, 3);
    EXPECT_TRUE(movedState);
    for (int64_t key = count - 1; key >= 0; --key) {
        int64_t isNew = -1;
        const auto state = UpsertAggregate_(&Table_, key, &isNew, Hash_(key));
        ASSERT_EQ(state, Table_.SlotId[key]);
        EXPECT_EQ(isNew, 0);
        EXPECT_EQ(Table_.AggBuffers[0][state], 100 + key);
    }
    EXPECT_EQ(Table_.Size, count);
    EXPECT_EQ(CloneCount_(), count);
}

} // namespace

int main(int argc, char** argv) {
    NQumir::NCodeGen::TLLVMInitializer initializer;
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
