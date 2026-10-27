// The layouts of the aggregate hash table: the row layout's kernels
// (kernel/aggregation/aggregation_rows.oz) on their own, and the aggregate
// operator in each layout and across the move from the swiss table.

#include "qumirdb_source_module.h"

#include <qdb/exec/instrumentation/operator.h>
#include <qdb/exec/kernel_rowset.h>
#include <qdb/io/io.h>
#include <qdb/kernel/aggregate_key.h>
#include <qdb/kernel/compiler.h>
#include <qdb/kernel/gen.h>
#include <qdb/kernel/lib.h>
#include <qdb/kernel/spec.h>
#include <qdb/plan/types/decimal.h>
#include <qdb/plan/types/nullable.h>

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/runner/runner_llvm.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

using namespace NQdb;
using namespace NQumir::NAst;

namespace {

// ---- the row layout's kernels ----

// Mirrors the HashTable of modules/qumirdb.cpp with the row layout's structs.
struct TArena {
    uint8_t** Blocks = nullptr;
    int64_t BlockCount = 0;
    int64_t BlockCapacity = 0;
    int64_t BlockRows = 0;
    int64_t RowBytes = 0;
    int64_t Rows = 0;
    int64_t CurRows = 0;
};

struct TTable {
    uint64_t* Entries = nullptr;
    int64_t Capacity = 0;
    uint64_t Mask = 0;
    int64_t Shift = 0;
    int64_t Rows = 0;
    int64_t Keys = 0;
};

struct TScratch {
    uint64_t* Hashes = nullptr;
    uint64_t* Salts = nullptr;
    uint64_t* Offsets = nullptr;
    uint64_t* Entries = nullptr;
    int64_t* Rows = nullptr;
    int64_t* Work = nullptr;
    uint8_t* Keys = nullptr;
    int64_t Capacity = 0;
};

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
    TArena RowArena;
    TTable RowTable;
    TScratch RowScratch;
    int64_t RowSwitchGroups = 0;
    int64_t RowExpectedGroups = 0;
};
static_assert(sizeof(TArena) == TKernelCompiler::RowArenaSize);
static_assert(sizeof(TTable) == TKernelCompiler::RowTableSize);
static_assert(sizeof(TScratch) == TKernelCompiler::RowScratchSize);
static_assert(sizeof(THashTable) == TKernelCompiler::HashTableSize);

// An i64 key with one state word: rows of [hash, key, state], four rows per
// arena block, so that every test spans several blocks.
constexpr int64_t KeyOffset = 8;
constexpr int64_t StateOffset = 16;

constexpr const char* RowEntries = R"(
(block
  (fun key_owned_bytes ((var key i64)) -> i64 (block (return 0)))
  (fun key_clone_owned ((var key i64) (var storage <ptr u8>)) -> i64 (block (return key)))
  (fun test_rows_init ((var ht <ref HashTable>) (var groups i64)) -> bool
    (block (return (call agr_init ht groups 1 8 24 4))))
  (fun test_rows_reserve ((var ht <ref HashTable>) (var n i64)) -> bool
    (block (return (call rt_scratch_reserve (field ht RowScratch) n 8))))
  (fun test_rows_find ((var ht <ref HashTable>) (var count i64)) -> bool
    (block (return (call agr_find_or_insert ht count 8 8 16 (cast 0 <ptr i64>)))))
  (fun test_rows_from_swiss ((var ht <ref HashTable>)) -> bool
    (block (return (call agr_from_swiss ht 8 16 24 4 (cast 0 <ptr i64>)))))
  (fun test_swiss_init ((var ht <ref HashTable>) (var capacity i64)) -> bool
    (block (return (call aht_init ht capacity 1 8))))
  (fun test_swiss_upsert ((var ht <ref HashTable>) (var key i64)
                          (var is_new <ref i64>) (var hash u64)) -> i64
    (block (return (call aht_upsert_aggregate ht key 0 is_new hash))))
  (fun test_destroy ((var ht <ref HashTable>))
    (block (call agr_destroy ht))))
)";

class RowLayoutKernels : public testing::Test {
protected:
    void SetUp() override {
        std::vector<TExprPtr> functions;
        auto add = [&](const std::string& source, const std::string& name) {
            auto library = NKernel::ParseFunctionLibrary(source, {"aht_update"});
            ASSERT_TRUE(library) << name << ": " << library.error().ToString();
            functions.insert(functions.end(), library->begin(), library->end());
        };
        for (const char* name : {
                 "key_ops_i64.oz", "swiss_group.oz", "robin_hood_rehash_generic.oz",
                 "owned_arena_lifecycle.oz", "aggregation_hashtable_generic.oz",
                 "owned_blocks.oz", "robin_hood_dual_key.oz"}) {
            add(NKernel::ReadAggregationKernel(name), name);
        }
        add(NKernel::ReadRowTableKernel(), "row_table.oz");
        add(NKernel::ReadAggregationKernel("aggregation_rows.oz"), "aggregation_rows.oz");
        add(RowEntries, "entries");

        auto options = KernelRunnerOptions();
        options.CoreInput = true;
        options.NativeCode = true;
        NTest::ConfigureQumirDbSourceModule(options);
        Runner_ = std::make_unique<NQumir::TLLVMRunner>(options);
        auto program = std::make_shared<TBlockExpr>(NQumir::TLocation{}, std::move(functions));
        NTest::AddQumirDbUse(program);
        std::string error;
        auto kernels = Runner_->CompileKernelAst(program, {
            "test_rows_init", "test_rows_reserve", "test_rows_find", "test_rows_from_swiss",
            "test_swiss_init", "test_swiss_upsert", "test_destroy", "rh_hash",
            "swiss_group_width"}, &error);
        ASSERT_EQ(kernels.size(), 9u) << error;
        RowsInit_ = reinterpret_cast<bool(*)(THashTable*, int64_t)>(kernels.at("test_rows_init"));
        Reserve_ = reinterpret_cast<bool(*)(THashTable*, int64_t)>(kernels.at("test_rows_reserve"));
        Find_ = reinterpret_cast<bool(*)(THashTable*, int64_t)>(kernels.at("test_rows_find"));
        FromSwiss_ = reinterpret_cast<bool(*)(THashTable*)>(kernels.at("test_rows_from_swiss"));
        SwissInit_ = reinterpret_cast<bool(*)(THashTable*, int64_t)>(kernels.at("test_swiss_init"));
        SwissUpsert_ = reinterpret_cast<int64_t(*)(THashTable*, int64_t, int64_t*, uint64_t)>(
            kernels.at("test_swiss_upsert"));
        Destroy_ = reinterpret_cast<void(*)(THashTable*)>(kernels.at("test_destroy"));
        Hash_ = reinterpret_cast<uint64_t(*)(int64_t)>(kernels.at("rh_hash"));
        SwissWidth_ = reinterpret_cast<int64_t(*)()>(kernels.at("swiss_group_width"))();
    }

    void TearDown() override {
        if (Destroy_) {
            Destroy_(&Table_);
        }
    }

    // Finds or inserts the groups of `keys` with the given hashes; returns the
    // scratch's Entries for them: the group's row, bit 0 for the inserting row.
    std::vector<uint64_t> FindOrInsert(
        const std::vector<int64_t>& keys, const std::vector<uint64_t>& hashes)
    {
        EXPECT_TRUE(Reserve_(&Table_, static_cast<int64_t>(keys.size())));
        auto* scratchKeys = reinterpret_cast<int64_t*>(Table_.RowScratch.Keys);
        for (size_t j = 0; j < keys.size(); ++j) {
            scratchKeys[j] = keys[j];
            Table_.RowScratch.Hashes[j] = hashes[j];
        }
        EXPECT_TRUE(Find_(&Table_, static_cast<int64_t>(keys.size())));
        return {Table_.RowScratch.Entries, Table_.RowScratch.Entries + keys.size()};
    }

    std::vector<uint64_t> FindOrInsert(const std::vector<int64_t>& keys) {
        std::vector<uint64_t> hashes;
        for (const auto key : keys) {
            hashes.push_back(Hash_(key));
        }
        return FindOrInsert(keys, hashes);
    }

    static uint8_t* RowOf(uint64_t group) {
        return reinterpret_cast<uint8_t*>(group & ~uint64_t{1});
    }

    static bool Inserted(uint64_t group) {
        return (group & 1) != 0;
    }

    static int64_t KeyOf(const uint8_t* row) {
        int64_t key;
        std::memcpy(&key, row + KeyOffset, sizeof(key));
        return key;
    }

    static int64_t& StateOf(uint8_t* row) {
        return *reinterpret_cast<int64_t*>(row + StateOffset);
    }

    // Every arena row in order.
    std::vector<uint8_t*> ArenaRows() const {
        std::vector<uint8_t*> rows;
        const auto& arena = Table_.RowArena;
        for (int64_t b = 0; b < arena.BlockCount; ++b) {
            const int64_t count = b + 1 == arena.BlockCount ? arena.CurRows : arena.BlockRows;
            for (int64_t r = 0; r < count; ++r) {
                rows.push_back(arena.Blocks[b] + r * arena.RowBytes);
            }
        }
        return rows;
    }

    std::unique_ptr<NQumir::TLLVMRunner> Runner_;
    THashTable Table_;
    bool (*RowsInit_)(THashTable*, int64_t) = nullptr;
    bool (*Reserve_)(THashTable*, int64_t) = nullptr;
    bool (*Find_)(THashTable*, int64_t) = nullptr;
    bool (*FromSwiss_)(THashTable*) = nullptr;
    bool (*SwissInit_)(THashTable*, int64_t) = nullptr;
    int64_t (*SwissUpsert_)(THashTable*, int64_t, int64_t*, uint64_t) = nullptr;
    void (*Destroy_)(THashTable*) = nullptr;
    uint64_t (*Hash_)(int64_t) = nullptr;
    int64_t SwissWidth_ = 0;
};

TEST_F(RowLayoutKernels, FirstRowOfANewKeyInsertsItsGroup) {
    ASSERT_TRUE(RowsInit_(&Table_, 1));
    const auto groups = FindOrInsert({5, 5, 7, 5, 7, 9});
    EXPECT_EQ(Table_.Size, 3);
    EXPECT_EQ(Table_.RowArena.Rows, 3);
    EXPECT_TRUE(Inserted(groups[0]));
    EXPECT_FALSE(Inserted(groups[1]));
    EXPECT_TRUE(Inserted(groups[2]));
    EXPECT_FALSE(Inserted(groups[3]));
    EXPECT_FALSE(Inserted(groups[4]));
    EXPECT_TRUE(Inserted(groups[5]));
    EXPECT_EQ(RowOf(groups[1]), RowOf(groups[0]));
    EXPECT_EQ(RowOf(groups[3]), RowOf(groups[0]));
    EXPECT_EQ(RowOf(groups[4]), RowOf(groups[2]));
    EXPECT_EQ(KeyOf(RowOf(groups[0])), 5);
    EXPECT_EQ(KeyOf(RowOf(groups[2])), 7);
    EXPECT_EQ(KeyOf(RowOf(groups[5])), 9);
    // A new group's states are zero and the row keeps its hash for growth.
    EXPECT_EQ(StateOf(RowOf(groups[5])), 0);
    EXPECT_EQ(*reinterpret_cast<uint64_t*>(RowOf(groups[2])), Hash_(7));

    StateOf(RowOf(groups[0])) = 42;
    const auto again = FindOrInsert({7, 5, 11});
    EXPECT_EQ(Table_.Size, 4);
    EXPECT_EQ(again[0], groups[2] & ~uint64_t{1});
    EXPECT_EQ(again[1], groups[0] & ~uint64_t{1});
    EXPECT_TRUE(Inserted(again[2]));
    EXPECT_EQ(StateOf(RowOf(again[1])), 42);
}

TEST_F(RowLayoutKernels, EqualSaltsCompareKeys) {
    ASSERT_TRUE(RowsInit_(&Table_, 1));
    // One hash for every key: one home entry and one salt, so every candidate
    // matches by salt and only the key tells the groups apart.
    std::vector<int64_t> keys;
    for (int64_t i = 0; i < 300; ++i) {
        keys.push_back(i % 120);
    }
    std::vector<uint64_t> hashes(keys.size(), 0x1234'5678'9abc'def0ull);
    const auto groups = FindOrInsert(keys, hashes);
    EXPECT_EQ(Table_.Size, 120);
    std::map<int64_t, uint8_t*> rows;
    for (size_t j = 0; j < keys.size(); ++j) {
        EXPECT_EQ(Inserted(groups[j]), j < 120) << j;
        EXPECT_EQ(KeyOf(RowOf(groups[j])), keys[j]);
        auto [it, inserted] = rows.emplace(keys[j], RowOf(groups[j]));
        EXPECT_EQ(it->second, RowOf(groups[j]));
    }
    // Rows that meet a salt match with another key move on one entry a pass;
    // when they reach the end of the run, they could fill more than half of
    // the entries, so the table grows in that pass and they seek again.
    keys.clear();
    for (int64_t i = 0; i < 600; ++i) {
        keys.push_back(1000 + i);
    }
    hashes.assign(keys.size(), 0x1234'5678'9abc'def0ull);
    const int64_t capacity = Table_.RowTable.Capacity;
    const auto more = FindOrInsert(keys, hashes);
    EXPECT_GT(Table_.RowTable.Capacity, capacity);
    EXPECT_EQ(Table_.Size, 720);
    for (size_t j = 0; j < keys.size(); ++j) {
        EXPECT_TRUE(Inserted(more[j]));
        EXPECT_EQ(KeyOf(RowOf(more[j])), keys[j]);
    }
    keys.clear();
    for (const auto& [key, row] : rows) {
        keys.push_back(key);
    }
    hashes.assign(keys.size(), 0x1234'5678'9abc'def0ull);
    const auto found = FindOrInsert(keys, hashes);
    for (size_t j = 0; j < keys.size(); ++j) {
        EXPECT_EQ(RowOf(found[j]), rows.at(keys[j]));
        EXPECT_FALSE(Inserted(found[j]));
    }
}

TEST_F(RowLayoutKernels, GrowthRewritesEntriesAndKeepsRows) {
    ASSERT_TRUE(RowsInit_(&Table_, 1));
    EXPECT_EQ(Table_.RowTable.Capacity, 1024);
    std::vector<int64_t> first;
    for (int64_t key = 0; key < 1000; ++key) {
        first.push_back(key * 7919);
    }
    const auto firstGroups = FindOrInsert(first);
    for (size_t j = 0; j < first.size(); ++j) {
        StateOf(RowOf(firstGroups[j])) = first[j] + 1;
    }
    for (int64_t chunk = 1; chunk < 20; ++chunk) {
        std::vector<int64_t> keys;
        for (int64_t i = 0; i < 1000; ++i) {
            keys.push_back((chunk * 1000 + i) * 7919);
        }
        FindOrInsert(keys);
    }
    EXPECT_EQ(Table_.Size, 20000);
    EXPECT_GE(Table_.RowTable.Capacity, 2 * Table_.Size);
    EXPECT_EQ(Table_.RowArena.BlockCount, 5000);
    const auto again = FindOrInsert(first);
    for (size_t j = 0; j < first.size(); ++j) {
        EXPECT_EQ(again[j], firstGroups[j] & ~uint64_t{1});
        EXPECT_EQ(StateOf(RowOf(again[j])), first[j] + 1);
    }
    // The arena holds every group once.
    std::set<int64_t> arenaKeys;
    for (const auto* row : ArenaRows()) {
        EXPECT_TRUE(arenaKeys.insert(KeyOf(row)).second);
    }
    ASSERT_EQ(arenaKeys.size(), 20000u);
    for (int64_t i = 0; i < 20000; ++i) {
        EXPECT_TRUE(arenaKeys.contains(i * 7919));
    }
}

TEST_F(RowLayoutKernels, SwissTableMovesItsGroupsInDenseOrder) {
    ASSERT_TRUE(SwissInit_(&Table_, SwissWidth_));
    std::vector<int64_t> keys;
    for (int64_t i = 0; i < 100; ++i) {
        keys.push_back((i * 37) % 101 - 50);
    }
    for (const auto key : keys) {
        int64_t isNew = 0;
        const int64_t slot = SwissUpsert_(&Table_, key, &isNew, Hash_(key));
        ASSERT_GE(slot, 0);
        Table_.AggBuffers[0][slot] = key * 10;
    }
    ASSERT_TRUE(FromSwiss_(&Table_));
    EXPECT_EQ(Table_.Keys, nullptr);
    EXPECT_EQ(Table_.Ctrl, nullptr);
    EXPECT_EQ(Table_.AggBuffers, nullptr);
    EXPECT_NE(Table_.RowTable.Entries, nullptr);
    EXPECT_EQ(Table_.Size, 100);
    EXPECT_EQ(Table_.NumAggs, 1);
    const auto rows = ArenaRows();
    ASSERT_EQ(rows.size(), keys.size());
    for (size_t i = 0; i < keys.size(); ++i) {
        EXPECT_EQ(KeyOf(rows[i]), keys[i]);
        EXPECT_EQ(StateOf(rows[i]), keys[i] * 10);
    }
    const auto groups = FindOrInsert(keys);
    for (size_t i = 0; i < keys.size(); ++i) {
        EXPECT_EQ(RowOf(groups[i]), rows[i]);
        EXPECT_FALSE(Inserted(groups[i]));
    }
    EXPECT_EQ(Table_.Size, 100);
}

TEST_F(RowLayoutKernels, SwissTableAsksToMoveInsteadOfGrowing) {
    ASSERT_TRUE(SwissInit_(&Table_, SwissWidth_));
    Table_.RowSwitchGroups = 2;
    // The table grows before it would cross 7/8 of its slots.
    const int64_t full = SwissWidth_ - SwissWidth_ / 8;
    for (int64_t key = 0; key < full; ++key) {
        int64_t isNew = 0;
        ASSERT_GE(SwissUpsert_(&Table_, key, &isNew, Hash_(key)), 0);
    }
    int64_t isNew = 0;
    EXPECT_GE(SwissUpsert_(&Table_, 0, &isNew, Hash_(0)), 0);
    EXPECT_EQ(SwissUpsert_(&Table_, full, &isNew, Hash_(full)), -2);
    EXPECT_EQ(Table_.Size, full);
    EXPECT_EQ(Table_.Capacity, SwissWidth_);
    // Below its switch, the table grows as before.
    Table_.RowSwitchGroups = full + 1;
    EXPECT_GE(SwissUpsert_(&Table_, full, &isNew, Hash_(full)), 0);
    EXPECT_EQ(Table_.Capacity, 2 * SwissWidth_);
}

// ---- the aggregate in each layout ----

TAggregateSpec Agg(const std::string& name, const std::string& func, const std::string& arg = {}) {
    return {
        .Name = name,
        .Func = func,
        .Arg = arg.empty() ? nullptr : std::make_shared<TIdentExpr>(NQumir::TLocation{}, arg),
    };
}

// The value kinds of input and output columns.
enum class EValue {
    I64,
    I32,
    F64,
    Wide,
    Bool,
};

// A batch of columns built row by row; NULL is std::nullopt.
class TBatch {
public:
    explicit TBatch(std::vector<EValue> kinds)
        : Kinds_(std::move(kinds))
        , Values_(Kinds_.size())
        , Valid_(Kinds_.size())
    {}

    void Add(const std::vector<std::optional<__int128>>& row, bool selected = true) {
        for (size_t c = 0; c < Kinds_.size(); ++c) {
            Valid_[c].push_back(row[c].has_value());
            Values_[c].push_back(row[c].value_or(-777));
        }
        Selection_.push_back(selected);
    }

    // Doubles travel as their bits.
    static __int128 Bits(double value) {
        return std::bit_cast<int64_t>(value);
    }

    TRowSet RowSet() {
        const size_t rows = Selection_.size();
        Storage_.clear();
        Columns_.assign(Kinds_.size(), TColumn{});
        for (size_t c = 0; c < Kinds_.size(); ++c) {
            auto& data = Storage_.emplace_back();
            switch (Kinds_[c]) {
            case EValue::I64:
            case EValue::F64:
                data.resize(rows * 8);
                for (size_t r = 0; r < rows; ++r) {
                    const auto value = static_cast<int64_t>(Values_[c][r]);
                    std::memcpy(data.data() + r * 8, &value, 8);
                }
                break;
            case EValue::I32:
                data.resize(rows * 4);
                for (size_t r = 0; r < rows; ++r) {
                    const auto value = static_cast<int32_t>(Values_[c][r]);
                    std::memcpy(data.data() + r * 4, &value, 4);
                }
                break;
            case EValue::Wide:
                data.resize(rows * 16);
                std::memcpy(data.data(), Values_[c].data(), rows * 16);
                break;
            case EValue::Bool:
                data.assign((rows + 7) / 8, 0);
                for (size_t r = 0; r < rows; ++r) {
                    if (Values_[c][r] != 0) {
                        data[r / 8] |= uint8_t{1} << (r % 8);
                    }
                }
                break;
            }
            Columns_[c].Data = reinterpret_cast<char*>(data.data());
            if (std::ranges::any_of(Valid_[c], [](bool valid) { return !valid; })) {
                auto& mask = Storage_.emplace_back((rows + 7) / 8, 0);
                for (size_t r = 0; r < rows; ++r) {
                    if (Valid_[c][r]) {
                        mask[r / 8] |= uint8_t{1} << (r % 8);
                    }
                }
                Columns_[c].Mask = mask.data();
            }
        }
        Selected_.assign(Selection_.begin(), Selection_.end());
        return TRowSet{
            .Columns = Columns_.data(),
            .ColumnCount = static_cast<int64_t>(Columns_.size()),
            .RowCount = static_cast<int64_t>(rows),
            .Selection = Selected_.data(),
            .RefCount = 1,
        };
    }

private:
    std::vector<EValue> Kinds_;
    std::vector<std::vector<__int128>> Values_;
    std::vector<std::vector<bool>> Valid_;
    std::vector<bool> Selection_;
    std::deque<std::vector<uint8_t>> Storage_;
    std::vector<TColumn> Columns_;
    std::vector<uint8_t> Selected_;
};

bool ValidAt(const TColumn& column, int64_t row) {
    return !column.Mask || ((column.Mask[(row + column.MaskBitOffset) / 8]
        >> ((row + column.MaskBitOffset) % 8)) & 1);
}

std::string Render(const TColumn& column, EValue kind, int64_t row) {
    if (!ValidAt(column, row)) {
        return "NULL";
    }
    switch (kind) {
    case EValue::I64:
        return std::to_string(reinterpret_cast<const int64_t*>(column.Data)[row]);
    case EValue::I32:
        return std::to_string(reinterpret_cast<const int32_t*>(column.Data)[row]);
    case EValue::F64: {
        // Exact bits: both layouts add a group's values in the same order.
        const auto bits = std::bit_cast<uint64_t>(reinterpret_cast<const double*>(column.Data)[row]);
        return "f" + std::to_string(bits);
    }
    case EValue::Wide: {
        const auto value = reinterpret_cast<const __int128*>(column.Data)[row];
        return "w" + std::to_string(static_cast<int64_t>(value >> 64)) + ":" +
            std::to_string(static_cast<uint64_t>(value));
    }
    case EValue::Bool: {
        const auto* bits = reinterpret_cast<const uint8_t*>(column.Data);
        return ((bits[row / 8] >> (row % 8)) & 1) ? "true" : "false";
    }
    }
    return "?";
}

// The kernels of an aggregate whose table may take the row layout.
TAggregateKernels CompileWithRows(
    const TStructType& input,
    const std::vector<std::string>& keys,
    const std::vector<TAggregateSpec>& aggs)
{
    auto spec = NKernel::BuildAggregateKernelSpec(input, keys, aggs);
    spec.AggregateRowLayout = true;
    return TKernelCompiler().CompileAggregate(spec);
}

// How a test starts a table: in the row layout, or as a swiss table that may
// move there.
struct TTablePlan {
    bool Rows = false;
    // The groups a table that starts in the row layout has entries for.
    int64_t InitialGroups = 0;
    int64_t InitialCapacity = 8;
    // SwitchToRowsAt and ExpectRowGroups, sent when not 0.
    int64_t RowSwitchGroups = 0;
    int64_t RowExpectedGroups = 0;
};

// Drives one table through agg_dispatch and agg_finish_rowset as the aggregate
// operator (exec/aggregate_exec.cpp) does, with the layout operations of a
// TTablePlan, which the operator does not send.
class TTableDriver {
public:
    TTableDriver(
        const TAggregateKernels& kernels,
        const TTablePlan& plan,
        TOperatorInstrumentation* counters = nullptr)
        : Kernels_(kernels)
        , Counters_(counters)
    {
        const bool initialized = plan.Rows
            ? Dispatch(EAggregateKernelOp::InitRows, plan.InitialGroups) != 0
            : Dispatch(EAggregateKernelOp::Init, plan.InitialCapacity) != 0;
        if (!initialized) {
            throw std::runtime_error("aggregate initialization failed");
        }
        Live_ = true;
        if (plan.RowSwitchGroups != 0) {
            EXPECT_EQ(Dispatch(EAggregateKernelOp::SwitchToRowsAt, plan.RowSwitchGroups), 1);
        }
        if (plan.RowExpectedGroups != 0) {
            EXPECT_EQ(Dispatch(EAggregateKernelOp::ExpectRowGroups, plan.RowExpectedGroups), 1);
        }
        Observe();
    }

    ~TTableDriver() {
        if (Live_) {
            Dispatch(EAggregateKernelOp::Destroy, 0);
        }
    }

    void Add(TRowSet& batch) {
        EXPECT_GE(Kernels_.Dispatch(
            &Table_, &batch, 0, static_cast<int64_t>(EAggregateKernelOp::Update)), 0);
        Observe();
    }

    // The groups; the table is destroyed.
    TRowSet Finish() {
        const int64_t size = Table_.Size;
        TRowSet result{};
        EXPECT_EQ(Kernels_.FinishRowSet(&Table_, &result), size);
        Live_ = false;
        result.Destroy = DestroyKernelOwnedRowSet;
        return result;
    }

    const THashTable& Table() const {
        return Table_;
    }

private:
    int64_t Dispatch(EAggregateKernelOp op, int64_t arg) {
        return Kernels_.Dispatch(&Table_, nullptr, arg, static_cast<int64_t>(op));
    }

    void Observe() {
        if (Counters_) {
            Counters_->ObserveHash(&Table_, 0, false, Kernels_.StringMetadataBuffers);
        }
    }

    const TAggregateKernels& Kernels_;
    TOperatorInstrumentation* Counters_ = nullptr;
    THashTable Table_;
    bool Live_ = false;
};

struct TRun {
    // The output rows in output order, one string per row.
    std::vector<std::string> Rows;
    bool EndedInRows = false;
};

TRun Aggregate(
    const TAggregateKernels& kernels,
    const TTablePlan& plan,
    std::vector<TBatch>& batches,
    const std::vector<EValue>& outputs)
{
    static const auto measure = TKernelCompiler().CompileInstrumentation();
    TOperatorInstrumentation counters;
    counters.Kernel = measure;
    TTableDriver table(kernels, plan, &counters);
    for (auto& batch : batches) {
        auto rowSet = batch.RowSet();
        table.Add(rowSet);
    }
    TRun run{.EndedInRows = counters.RowLayout};
    TRowSet result = table.Finish();
    EXPECT_EQ(result.ColumnCount, static_cast<int64_t>(outputs.size()));
    for (int64_t row = 0; row < result.RowCount; ++row) {
        std::string text;
        for (size_t c = 0; c < outputs.size(); ++c) {
            text += (c ? "|" : "") + Render(result.Columns[c], outputs[c], row);
        }
        run.Rows.push_back(std::move(text));
    }
    Release(&result);
    return run;
}

std::vector<std::string> Sorted(std::vector<std::string> rows) {
    std::ranges::sort(rows);
    return rows;
}

// Runs the aggregate as a swiss table, as a row table, and as a swiss table
// that moves to rows after a few groups, and checks that all give the swiss
// table's groups. Returns the swiss run.
TRun CheckLayoutsAgree(
    const TStructType& input,
    const std::vector<std::string>& keys,
    const std::vector<TAggregateSpec>& aggs,
    std::vector<TBatch>& batches,
    const std::vector<EValue>& outputs)
{
    const auto kernels = CompileWithRows(input, keys, aggs);
    EXPECT_GT(kernels.RowBytes, 0);
    const auto swiss = Aggregate(kernels, {}, batches, outputs);
    EXPECT_FALSE(swiss.EndedInRows);
    const auto expected = Sorted(swiss.Rows);
    const auto rows = Aggregate(kernels, {.Rows = true}, batches, outputs);
    EXPECT_TRUE(rows.EndedInRows);
    EXPECT_EQ(Sorted(rows.Rows), expected) << "row layout";
    for (const int64_t switchGroups : {1, 13, 200}) {
        const auto moved = Aggregate(
            kernels, {.RowSwitchGroups = switchGroups}, batches, outputs);
        EXPECT_EQ(moved.EndedInRows, static_cast<int64_t>(expected.size()) >= switchGroups);
        EXPECT_EQ(Sorted(moved.Rows), expected) << "switch at " << switchGroups;
    }
    return swiss;
}

TEST(AggregateTable, EveryFixedWidthStateAgreesAcrossLayouts) {
    auto i64 = std::make_shared<TIntegerType>();
    auto i32 = std::make_shared<TIntegerType>(TIntegerType::I32);
    TStructType input({
        {"k", i64},
        {"v", std::make_shared<TNullable>(i64)},
        {"w", i32},
        {"f", std::make_shared<TNullable>(std::make_shared<TFloatType>())},
        {"g", std::make_shared<TFloatType>()},
        {"d", std::make_shared<TNullable>(std::make_shared<TDecimal>(28, 2))},
        {"e", std::make_shared<TDecimal>(18, 0)},
        {"b", std::make_shared<TNullable>(std::make_shared<TBoolType>())},
    });
    std::vector<TAggregateSpec> aggs = {
        Agg("c", "count"), Agg("cv", "count", "v"),
        Agg("sv", "sum", "v"), Agg("nv", "min", "v"), Agg("xv", "max", "v"),
        Agg("sw", "sum", "w"), Agg("nw", "min", "w"), Agg("xw", "max", "w"),
        Agg("sf", "sum", "f"), Agg("nf", "min", "f"), Agg("xf", "max", "f"),
        Agg("sg", "sum", "g"), Agg("ng", "min", "g"), Agg("xg", "max", "g"),
        Agg("sd", "sum", "d"), Agg("nd", "min", "d"), Agg("xd", "max", "d"),
        Agg("se", "sum", "e"), Agg("ne", "min", "e"), Agg("xe", "max", "e"),
        Agg("nb", "min", "b"), Agg("xb", "max", "b"),
    };
    const std::vector<EValue> kinds = {
        EValue::I64, EValue::I64, EValue::I32, EValue::F64, EValue::F64,
        EValue::Wide, EValue::Wide, EValue::Bool,
    };
    std::vector<TBatch> batches;
    uint64_t seed = 12345;
    auto next = [&] {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        return seed >> 33;
    };
    for (int b = 0; b < 12; ++b) {
        auto& batch = batches.emplace_back(kinds);
        for (int r = 0; r < 700; ++r) {
            const auto key = static_cast<int64_t>(next() % 2500) - 1000;
            const auto x = static_cast<int64_t>(next() % 2000) - 1000;
            auto maybe = [&](__int128 value) -> std::optional<__int128> {
                return next() % 5 == 0 ? std::nullopt : std::optional<__int128>(value);
            };
            batch.Add({
                key, maybe(x), x * 3,
                maybe(TBatch::Bits(x * 0.125)), TBatch::Bits(x * 0.5 - 0.25),
                maybe(static_cast<__int128>(x) << 70), x * 1000003,
                maybe(x % 3 == 0),
            }, next() % 7 != 0);
        }
    }
    std::vector<EValue> outputs = {EValue::I64, EValue::I64, EValue::I64};
    for (const auto kind : {EValue::I64, EValue::I64, EValue::F64, EValue::F64,
                            EValue::Wide, EValue::Wide}) {
        outputs.insert(outputs.end(), 3, kind);
    }
    outputs[3 + 3] = EValue::I64;
    outputs.push_back(EValue::Bool);
    outputs.push_back(EValue::Bool);
    const auto swiss = CheckLayoutsAgree(input, {"k"}, aggs, batches, outputs);
    EXPECT_GT(swiss.Rows.size(), 2000u);
}

TEST(AggregateTable, CompositeAndNullKeysAgreeAcrossLayouts) {
    auto i64 = std::make_shared<TIntegerType>();
    auto i32 = std::make_shared<TIntegerType>(TIntegerType::I32);
    // The decimal key comes first: after a nullable flag or a narrower key it
    // is misplaced in the key struct (#163).
    TStructType input({
        {"d", std::make_shared<TDecimal>(20, 0)},
        {"a", std::make_shared<TNullable>(i64)},
        {"b", i32},
        {"f", std::make_shared<TFloatType>()},
        {"t", std::make_shared<TNullable>(std::make_shared<TBoolType>())},
        {"v", i64},
    });
    std::vector<TAggregateSpec> aggs = {Agg("c", "count"), Agg("s", "sum", "v")};
    const std::vector<EValue> kinds = {
        EValue::Wide, EValue::I64, EValue::I32, EValue::F64, EValue::Bool, EValue::I64,
    };
    std::vector<TBatch> batches;
    std::set<std::tuple<int, int, int, int, int>> expected;
    for (int b = 0; b < 6; ++b) {
        auto& batch = batches.emplace_back(kinds);
        for (int r = 0; r < 900; ++r) {
            const int i = b * 900 + r;
            // A NULL key carries a payload that must not split its group, and
            // 0 is a group of its own. +0.0 and -0.0 are one group.
            const bool aNull = i % 11 == 0;
            const bool tNull = i % 13 == 0;
            const double f = i % 5 == 0 ? (i % 10 == 0 ? 0.0 : -0.0) : (i % 5) * 1.5;
            batch.Add({
                static_cast<__int128>(i % 3) << 66,
                aNull ? std::nullopt : std::optional<__int128>(i % 4),
                i % 6,
                TBatch::Bits(f),
                tNull ? std::nullopt : std::optional<__int128>(i % 2),
                i,
            });
            expected.emplace(i % 3, aNull ? -1 : i % 4, i % 6, i % 5, tNull ? -1 : i % 2);
        }
    }
    const auto swiss = CheckLayoutsAgree(input, {"d", "a", "b", "f", "t"}, aggs, batches, {
        EValue::Wide, EValue::I64, EValue::I32, EValue::F64, EValue::Bool,
        EValue::I64, EValue::I64,
    });
    EXPECT_EQ(swiss.Rows.size(), expected.size());
}

TEST(AggregateTable, MovesToRowsInsideABatch) {
    auto i64 = std::make_shared<TIntegerType>();
    TStructType input({{"k", i64}, {"v", i64}});
    std::vector<TAggregateSpec> aggs = {
        Agg("c", "count"), Agg("s", "sum", "v"), Agg("n", "min", "v"), Agg("x", "max", "v"),
    };
    const auto kernels = CompileWithRows(input, {"k"}, aggs);
    // One batch of 10,000 rows: more selected rows than one find-or-insert
    // chunk, and a switch long before its end.
    std::vector<TBatch> batches;
    auto& batch = batches.emplace_back(std::vector<EValue>{EValue::I64, EValue::I64});
    std::map<int64_t, std::array<int64_t, 4>> expected;
    for (int64_t r = 0; r < 10000; ++r) {
        const int64_t key = (r * 7919) % 6007;
        const int64_t value = (r * 31) % 1000 - 500;
        const bool selected = r % 9 != 4;
        batch.Add({key, value}, selected);
        if (!selected) {
            continue;
        }
        auto [it, inserted] = expected.try_emplace(key, std::array<int64_t, 4>{0, 0, value, value});
        auto& state = it->second;
        ++state[0];
        state[1] += value;
        state[2] = std::min(state[2], value);
        state[3] = std::max(state[3], value);
    }
    const std::vector<EValue> outputs(5, EValue::I64);
    std::vector<std::string> rows;
    for (const auto& [key, state] : expected) {
        rows.push_back(std::to_string(key) + "|" + std::to_string(state[0]) + "|" +
            std::to_string(state[1]) + "|" + std::to_string(state[2]) + "|" +
            std::to_string(state[3]));
    }
    std::ranges::sort(rows);
    for (const TTablePlan plan : {
             TTablePlan{},
             TTablePlan{.Rows = true},
             TTablePlan{.RowSwitchGroups = 100},
         })
    {
        const auto run = Aggregate(kernels, plan, batches, outputs);
        EXPECT_EQ(run.EndedInRows, plan.Rows || plan.RowSwitchGroups != 0);
        EXPECT_EQ(Sorted(run.Rows), rows);
    }
}

TEST(AggregateTable, RowLayoutAlignsKeysThatHoldADecimal) {
    auto i64 = std::make_shared<TIntegerType>();
    auto i32 = std::make_shared<TIntegerType>(TIntegerType::I32);
    TStructType input({
        {"d", std::make_shared<TDecimal>(20, 0)},
        {"b", i32},
        {"k", i64},
    });
    const auto states = NKernel::BuildAggReducerLayout(
        {"count", "sum"}, {NKernel::TAggArg{}, NKernel::TAggArg{.ColumnIndex = 2}});
    // LLVM loads an i128 key field with instructions that need 16-byte
    // alignment on x86-64, so the key's offset in the row, the row stride and
    // the stride of the scratch keys are multiples of 16.
    for (const std::vector<std::string>& keys : std::vector<std::vector<std::string>>{
             {"d"}, {"d", "b"}, {"d", "b", "k"}})
    {
        const auto key = NKernel::BuildAggregateKeyDescriptor(input, keys);
        const auto rows = NKernel::BuildAggRowLayout(key, states, false);
        ASSERT_TRUE(rows);
        EXPECT_EQ(rows->KeyAlignment, 16);
        EXPECT_EQ(rows->KeyOffset % 16, 0);
        EXPECT_EQ(rows->KeyStride % 16, 0);
        EXPECT_EQ(rows->RowBytes % 16, 0);
        EXPECT_GE(rows->KeyStride, static_cast<int64_t>(key.Size));
        EXPECT_EQ(rows->StateOffset, rows->KeyOffset + rows->KeyStride);
        EXPECT_GE(rows->RowBytes, rows->StateOffset + 16);
    }
    const auto narrow = NKernel::BuildAggRowLayout(
        NKernel::BuildAggregateKeyDescriptor(input, {"k", "b"}), states, false);
    ASSERT_TRUE(narrow);
    EXPECT_EQ(narrow->KeyOffset, 8);
    EXPECT_EQ(narrow->KeyStride, 16);
    EXPECT_EQ(narrow->RowBytes, 40);
}

TEST(AggregateTable, RowLayoutReportsItsMemory) {
    auto i64 = std::make_shared<TIntegerType>();
    TStructType input({{"k", i64}, {"v", i64}});
    const auto kernels = CompileWithRows(input, {"k"}, {Agg("c", "count"), Agg("s", "sum", "v")});
    // A hash word, the key and two state words.
    EXPECT_EQ(kernels.RowBytes, 32);
    EXPECT_EQ(kernels.StateWords, 2u);
    TOperatorInstrumentation counters;
    counters.Kernel = TKernelCompiler().CompileInstrumentation();
    TTableDriver table(kernels, {.Rows = true}, &counters);
    TBatch batch({EValue::I64, EValue::I64});
    for (int64_t key = 0; key < 3000; ++key) {
        batch.Add({key, key});
    }
    auto rowSet = batch.RowSet();
    table.Add(rowSet);
    EXPECT_TRUE(counters.RowLayout);
    EXPECT_EQ(counters.Groups, 3000u);
    // Entries for 3000 groups at most half full.
    EXPECT_EQ(counters.HashCapacity, 8192u);
    // The entries and one arena block of 16,384 rows.
    EXPECT_GE(counters.HashMemoryPeakBytes, 8192u * 8 + 16384u * 32);
    TRowSet result = table.Finish();
    EXPECT_EQ(result.RowCount, 3000);
    Release(&result);
}

// The top-level functions of the aggregate program, with or without a row layout.
std::set<std::string> ProgramFunctions(
    const TStructType& input,
    const std::vector<std::string>& keys,
    const std::optional<NKernel::TAggRowLayout>& rows)
{
    const auto key = NKernel::BuildAggregateKeyDescriptor(input, keys);
    const auto layout = NKernel::BuildAggReducerLayout(
        {"count", "sum"}, {NKernel::TAggArg{}, NKernel::TAggArg{.ColumnIndex = 1}});
    auto named = [](const char* name) { return std::make_shared<TNamedType>(name, nullptr); };
    auto program = NKernel::BuildGenericAggregateFusedProgramAst(
        input, key, layout, named("TColumn"), named("TRowSet"), named("HashTable"),
        false, {}, rows);
    EXPECT_TRUE(program) << program.error().ToString();
    std::set<std::string> names;
    if (program) {
        for (const auto& stmt : TMaybeNode<TBlockExpr>(*program).Cast()->Stmts) {
            if (auto fun = TMaybeNode<TFunDecl>(stmt)) {
                names.insert(fun.Cast()->Name);
            }
        }
    }
    return names;
}

TEST(AggregateTable, RowLayoutNeedsTheSpecFlag) {
    auto i64 = std::make_shared<TIntegerType>();
    TStructType input({{"k", i64}, {"v", i64}});
    const std::vector<TAggregateSpec> aggs = {Agg("c", "count"), Agg("s", "sum", "v")};
    auto spec = NKernel::BuildAggregateKernelSpec(input, {"k"}, aggs);
    ASSERT_FALSE(spec.AggregateRowLayout);
    const auto swiss = TKernelCompiler().CompileAggregate(spec);
    EXPECT_EQ(swiss.RowBytes, 0);
    EXPECT_EQ(CompileWithRows(input, {"k"}, aggs).RowBytes, 32);

    // Without the flag, the program is the swiss table's alone.
    const auto rows = NKernel::BuildAggRowLayout(
        NKernel::BuildAggregateKeyDescriptor(input, {"k"}),
        NKernel::BuildAggReducerLayout(
            {"count", "sum"}, {NKernel::TAggArg{}, NKernel::TAggArg{.ColumnIndex = 1}}),
        false);
    ASSERT_TRUE(rows);
    const auto swissFunctions = ProgramFunctions(input, {"k"}, std::nullopt);
    const auto rowFunctions = ProgramFunctions(input, {"k"}, rows);
    for (const char* name : {"agg_rows_update", "agr_destroy", "agr_from_swiss", "rt_init"}) {
        EXPECT_FALSE(swissFunctions.contains(name)) << name;
        EXPECT_TRUE(rowFunctions.contains(name)) << name;
    }
    EXPECT_TRUE(swissFunctions.contains("agg_dispatch"));

    // Its kernel serves none of the row layout's operations and stays swiss.
    THashTable table;
    ASSERT_NE(swiss.Dispatch(&table, nullptr, 8, static_cast<int64_t>(EAggregateKernelOp::Init)), 0);
    for (const auto op : {EAggregateKernelOp::InitRows, EAggregateKernelOp::SwitchToRowsAt,
                          EAggregateKernelOp::ExpectRowGroups}) {
        EXPECT_EQ(swiss.Dispatch(&table, nullptr, 2, static_cast<int64_t>(op)), 0);
    }
    EXPECT_EQ(table.RowSwitchGroups, 0);
    EXPECT_EQ(table.RowExpectedGroups, 0);
    TBatch batch({EValue::I64, EValue::I64});
    for (int64_t key = 0; key < 100; ++key) {
        batch.Add({key, key});
    }
    auto rowSet = batch.RowSet();
    EXPECT_EQ(swiss.Dispatch(&table, &rowSet, 0, static_cast<int64_t>(EAggregateKernelOp::Update)), 0);
    EXPECT_EQ(table.Size, 100);
    EXPECT_NE(table.Keys, nullptr);
    EXPECT_EQ(table.RowTable.Entries, nullptr);
    EXPECT_NE(swiss.Dispatch(&table, nullptr, 0, static_cast<int64_t>(EAggregateKernelOp::Destroy)), 0);
}

TEST(AggregateTable, StringsAndEmptyInputGroupsKeepTheSwissTable) {
    auto i64 = std::make_shared<TIntegerType>();
    TStructType strings({{"k", std::make_shared<TStringType>()}, {"v", i64}});
    EXPECT_EQ(CompileWithRows(strings, {"k"}, {Agg("c", "count")}).RowBytes, 0);
    TStructType numbers({{"k", i64}, {"s", std::make_shared<TStringType>()}});
    EXPECT_EQ(CompileWithRows(numbers, {"k"}, {Agg("m", "min", "s")}).RowBytes, 0);
    auto seeded = NKernel::BuildAggregateKernelSpec(
        numbers, {"k"}, {Agg("c", "count")}, nullptr, {1});
    seeded.AggregateRowLayout = true;
    EXPECT_EQ(TKernelCompiler().CompileAggregate(seeded).RowBytes, 0);
}

// The move sizes its entries for the groups the caller expects when that is
// more than twice the groups it moves, so that they need not grow again.
TEST(AggregateTable, ExpectRowGroupsSizesTheEntriesOfTheMove) {
    auto i64 = std::make_shared<TIntegerType>();
    TStructType input({{"k", i64}, {"v", i64}});
    const auto kernels = CompileWithRows(input, {"k"}, {Agg("c", "count"), Agg("s", "sum", "v")});
    auto batchOf = [](int64_t from, int64_t to) {
        TBatch batch({EValue::I64, EValue::I64});
        for (int64_t key = from; key < to; ++key) {
            batch.Add({key, key});
        }
        return batch;
    };
    // The swiss table of 512 slots must grow at 448 groups and moves then.
    // Entries for 896 groups are 2048, for 500 groups 1024.
    struct TCase {
        int64_t ExpectedGroups;
        // Entries after the move and after 3000 groups.
        int64_t MovedCapacity;
        int64_t FinalCapacity;
    };
    for (const auto& [expected, moved, final] : {
             TCase{0, 2048, 8192},
             TCase{500, 2048, 8192},
             TCase{50000, 131072, 131072},
         })
    {
        TTableDriver table(kernels, {
            .InitialCapacity = 512, .RowSwitchGroups = 100, .RowExpectedGroups = expected});
        auto first = batchOf(0, 600);
        auto firstRows = first.RowSet();
        table.Add(firstRows);
        ASSERT_NE(table.Table().RowTable.Entries, nullptr) << expected;
        EXPECT_EQ(table.Table().RowTable.Capacity, moved) << expected;
        auto second = batchOf(600, 3000);
        auto secondRows = second.RowSet();
        table.Add(secondRows);
        EXPECT_EQ(table.Table().Size, 3000);
        EXPECT_EQ(table.Table().RowTable.Capacity, final) << expected;
        TRowSet result = table.Finish();
        EXPECT_EQ(result.RowCount, 3000);
        Release(&result);
    }
}


} // namespace

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    NQumir::NCodeGen::TLLVMInitializer llvmInit;
    return RUN_ALL_TESTS();
}
