#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <numeric>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

// TODO: values should be vector of pairs
using TTableValues = std::map<std::string, std::string>;

struct TTableRow {
    int Key;
    TTableValues Values;
};

using TTable = std::vector<TTableRow>;

using TJoinFilter = std::function<bool(const TTableRow& row)>;

int RandomInt(int* seed) {
    *seed = (*seed * 1103515245 + 12345) & 0x7fffffff;
    return *seed;
}

std::string RandomString(int* seed, size_t length) {
    const std::string chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::string result;
    result.reserve(length);
    for (size_t i = 0; i < length; ++i) {
        result += chars[RandomInt(seed) % chars.size()];
    }
    return result;
}

std::vector<TTableRow> GenerateTable(int* seed, int numKeys, int numRows, std::set<std::string> columns) {
    std::vector<TTableRow> table;
    table.reserve(numRows);
    for (int i = 0; i < numRows; ++i) {
        auto key = RandomInt(seed) % numKeys;
        TTableValues values;
        for (const auto& col : columns) {
            values[col] = RandomString(seed, 10);
        }
        table.push_back({key, values});
    }
    return table;
}

TTable InnerJoin(const TTable& left, const TTable& right, TJoinFilter filter = {}) {
    int batchSize = 10;
    // key -> rows
    using THashMap = std::unordered_map<int, std::vector<TTableValues>>;
    THashMap leftHash;
    THashMap rightHash;
    int leftIt = 0;
    int rightIt = 0;

    TTable result;

    auto progress = [&](auto& leftIt, const TTable& left, const TTable& right, THashMap& leftHash, THashMap& rightHash, bool isLeft) -> bool {
        bool hasMore = leftIt < left.size();
        if (!hasMore) {
            return false;
        }
        for (int i = 0; i < batchSize && leftIt < left.size(); ++i, ++leftIt) {
            auto& tableRow = left[leftIt];
            auto& key = tableRow.Key;
            auto& row = tableRow.Values;
            // 1. try join with right rows for this key
            auto rightIt = rightHash.find(key);
            if (rightIt != rightHash.end()) {
                for (const auto& rightRow : rightIt->second) {
                    TTableRow joinedRow;
                    joinedRow.Key = key;
                    if (isLeft) {
                        joinedRow.Values.insert(row.begin(), row.end());
                        joinedRow.Values.insert(rightRow.begin(), rightRow.end());
                    } else {
                        joinedRow.Values.insert(rightRow.begin(), rightRow.end());
                        joinedRow.Values.insert(row.begin(), row.end());
                    }
                    if (filter && !filter(joinedRow)) {
                        continue;
                    }
                    result.push_back(joinedRow);
                }
            }
            // 2. store left row for future join
            leftHash[key].push_back(row);
        }
        return true;
    };

    while (progress(leftIt, left, right, leftHash, rightHash, true) || progress(rightIt, right, left, rightHash, leftHash, false)) {
        // keep joining until both sides are exhausted
    }

    return result;
}

TTable NestedLoopInnerJoin(const TTable& left, const TTable& right, TJoinFilter filter = {}) {
    TTable result;
    for (const auto& leftRow : left) {
        for (const auto& rightRow : right) {
            if (leftRow.Key == rightRow.Key) {
                TTableRow joinedRow;
                joinedRow.Key = leftRow.Key;
                joinedRow.Values.insert(leftRow.Values.begin(), leftRow.Values.end());
                joinedRow.Values.insert(rightRow.Values.begin(), rightRow.Values.end());
                // Residual filter is applied before emit, same as InnerJoin.
                if (filter && !filter(joinedRow)) {
                    continue;
                }
                result.push_back(joinedRow);
            }
        }
    }
    return result;
}

void CheckJoinMatchesNestedLoop(const TTable& left, const TTable& right, TJoinFilter filter = {}) {
    auto joinedTable = InnerJoin(left, right, filter);
    auto nestedLoopJoinedTable = NestedLoopInnerJoin(left, right, filter);
    EXPECT_EQ(joinedTable.size(), nestedLoopJoinedTable.size());
    for (const auto& row : joinedTable) {
        auto it = std::find_if(nestedLoopJoinedTable.begin(), nestedLoopJoinedTable.end(), [&](const TTableRow& r) {
            return r.Key == row.Key && r.Values == row.Values;
        });
        EXPECT_NE(it, nestedLoopJoinedTable.end());
    }
}

void PrintTable(const TTable& table, std::ostream& os) {
    for (const auto& row : table) {
        os << "Key: " << row.Key << ", Values: ";
        for (const auto& [col, val] : row.Values) {
            os << col << "=" << val << " ";
        }
        os << std::endl;
    }
}

} // namespace

TEST(JoinTest, BasicInnerJoin) {
    int seed = 42;
    const int numLeftKeys = 5;
    const int numRightKeys = 5;
    const int numLeftRows = 5;
    const int numRightRows = 6;
    std::set<std::string> leftColumns = {"col1", "col2"};
    std::set<std::string> rightColumns = {"col3", "col4"};
    auto leftTable = GenerateTable(&seed, numLeftKeys, numLeftRows, leftColumns);
    auto rightTable = GenerateTable(&seed, numRightKeys, numRightRows, rightColumns);
    auto joinedTable = InnerJoin(leftTable, rightTable);

    std::cout << "Left Table:" << std::endl;
    PrintTable(leftTable, std::cout);
    std::cout << "Right Table:" << std::endl;
    PrintTable(rightTable, std::cout);
    std::cout << "Joined Table:" << std::endl;
    PrintTable(joinedTable, std::cout);

    auto nestedLoopJoinedTable = NestedLoopInnerJoin(leftTable, rightTable);
    EXPECT_EQ(joinedTable.size(), nestedLoopJoinedTable.size());
    for (const auto& row : joinedTable) {
        auto it = std::find_if(nestedLoopJoinedTable.begin(), nestedLoopJoinedTable.end(), [&](const TTableRow& r) {
            return r.Key == row.Key && r.Values == row.Values;
        });
        EXPECT_NE(it, nestedLoopJoinedTable.end());
    }
}

TEST(JoinTest, LargeTablesManyKeys) {
    int seed = 123;
    const int numLeftKeys = 20;
    const int numRightKeys = 20;
    const int numLeftRows = 37;
    const int numRightRows = 42;
    std::set<std::string> leftColumns = {"col1", "col2"};
    std::set<std::string> rightColumns = {"col3", "col4"};
    auto leftTable = GenerateTable(&seed, numLeftKeys, numLeftRows, leftColumns);
    auto rightTable = GenerateTable(&seed, numRightKeys, numRightRows, rightColumns);

    CheckJoinMatchesNestedLoop(leftTable, rightTable);
}

TEST(JoinTest, FewKeysHighCardinality) {
    int seed = 7;
    const int numLeftKeys = 2;
    const int numRightKeys = 2;
    const int numLeftRows = 15;
    const int numRightRows = 18;
    std::set<std::string> leftColumns = {"col1"};
    std::set<std::string> rightColumns = {"col2"};
    auto leftTable = GenerateTable(&seed, numLeftKeys, numLeftRows, leftColumns);
    auto rightTable = GenerateTable(&seed, numRightKeys, numRightRows, rightColumns);

    CheckJoinMatchesNestedLoop(leftTable, rightTable);
}

TEST(JoinTest, SingleKeyFullCrossProduct) {
    int seed = 99;
    const int numLeftKeys = 1;
    const int numRightKeys = 1;
    const int numLeftRows = 8;
    const int numRightRows = 11;
    std::set<std::string> leftColumns = {"col1"};
    std::set<std::string> rightColumns = {"col2"};
    auto leftTable = GenerateTable(&seed, numLeftKeys, numLeftRows, leftColumns);
    auto rightTable = GenerateTable(&seed, numRightKeys, numRightRows, rightColumns);

    CheckJoinMatchesNestedLoop(leftTable, rightTable);
}

TEST(JoinTest, AsymmetricSidesAndKeyRanges) {
    int seed = 555;
    const int numLeftKeys = 3;
    const int numRightKeys = 15;
    const int numLeftRows = 5;
    const int numRightRows = 50;
    std::set<std::string> leftColumns = {"col1"};
    std::set<std::string> rightColumns = {"col2", "col3"};
    auto leftTable = GenerateTable(&seed, numLeftKeys, numLeftRows, leftColumns);
    auto rightTable = GenerateTable(&seed, numRightKeys, numRightRows, rightColumns);

    CheckJoinMatchesNestedLoop(leftTable, rightTable);
}

TEST(JoinTest, EmptySides) {
    int seed = 1;
    std::set<std::string> leftColumns = {"col1"};
    std::set<std::string> rightColumns = {"col2"};

    auto leftTable = GenerateTable(&seed, 5, 10, leftColumns);
    auto emptyRight = GenerateTable(&seed, 5, 0, rightColumns);
    CheckJoinMatchesNestedLoop(leftTable, emptyRight);

    auto emptyLeft = GenerateTable(&seed, 5, 0, leftColumns);
    auto rightTable = GenerateTable(&seed, 5, 10, rightColumns);
    CheckJoinMatchesNestedLoop(emptyLeft, rightTable);
}

TEST(JoinTest, ResidualFilterAppliedBeforeEmit) {
    int seed = 2024;
    const int numLeftKeys = 8;
    const int numRightKeys = 8;
    const int numLeftRows = 30;
    const int numRightRows = 35;
    std::set<std::string> leftColumns = {"l1", "l2"};
    std::set<std::string> rightColumns = {"r1", "r2"};
    auto leftTable = GenerateTable(&seed, numLeftKeys, numLeftRows, leftColumns);
    auto rightTable = GenerateTable(&seed, numRightKeys, numRightRows, rightColumns);

    // Residual theta over the joined row: keep only pairs where l1 < r1
    // lexicographically. The symmetric join must apply this at the match
    // point (before emit) and agree with the nested-loop oracle.
    TJoinFilter filter = [](const TTableRow& row) {
        auto l = row.Values.find("l1");
        auto r = row.Values.find("r1");
        return l != row.Values.end() && r != row.Values.end() && l->second < r->second;
    };

    auto joined = InnerJoin(leftTable, rightTable, filter);
    auto oracle = NestedLoopInnerJoin(leftTable, rightTable, filter);
    EXPECT_EQ(joined.size(), oracle.size());
    EXPECT_LT(joined.size(), InnerJoin(leftTable, rightTable).size()); // filter removed some pairs
    for (const auto& row : joined) {
        auto it = std::find_if(oracle.begin(), oracle.end(), [&](const TTableRow& r) {
            return r.Key == row.Key && r.Values == row.Values;
        });
        EXPECT_NE(it, oracle.end());
    }

    CheckJoinMatchesNestedLoop(leftTable, rightTable, filter);
}

// C++ prototypes of the two hash tables of QDB's equi-joins. Each follows its
// oz kernels step by step, so the algorithm and its memory accesses can be
// read, tested and measured without the JIT:
// - TSwissJoinTable: the table of the symmetric join (join_table.oz and
//   join_update.oz over the swiss HashTable of robin_hood_dual_key.oz);
// - TRowJoinTable: the row join table of a fixed build side (join_rows.oz).
// Both see a key only through HashJoinKey, operator== and CloneJoinKey, as the
// kernels see it only through the rh_hash, rh_key_equal and key_clone_owned
// overloads.

namespace {

// Owns the bytes of the keys a table stores. Blocks never move, so views into
// them stay valid while the table lives.
class TKeyArena {
public:
    std::string_view Copy(std::string_view bytes) {
        if (bytes.empty()) {
            return {};
        }
        if (Blocks_.empty() || Used_ + bytes.size() > BlockBytes_) {
            BlockBytes_ = std::max(BlockBytes, bytes.size());
            Blocks_.push_back(std::make_unique_for_overwrite<char[]>(BlockBytes_));
            Bytes_ += BlockBytes_;
            Used_ = 0;
        }
        char* dst = Blocks_.back().get() + Used_;
        std::memcpy(dst, bytes.data(), bytes.size());
        Used_ += bytes.size();
        return {dst, bytes.size()};
    }

    size_t Bytes() const {
        return Bytes_;
    }

private:
    static constexpr size_t BlockBytes = 64 * 1024;

    std::vector<std::unique_ptr<char[]>> Blocks_;
    size_t BlockBytes_ = 0;
    size_t Used_ = 0;
    size_t Bytes_ = 0;
};

// Both sides hash a key once per row. The tables need a well-mixed 64-bit hash
// and take different bits of it: the swiss table the low bits, the row join
// table the top bits.
uint64_t HashJoinKey(int64_t key) {
    uint64_t x = static_cast<uint64_t>(key);
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9;
    x ^= x >> 27;
    x *= 0x94D049BB133111EB;
    x ^= x >> 31;
    return x;
}

uint64_t HashJoinKey(std::string_view key) {
    return std::hash<std::string_view>{}(key);
}

int64_t CloneJoinKey(int64_t key, TKeyArena&) {
    return key;
}

std::string_view CloneJoinKey(std::string_view key, TKeyArena& arena) {
    return arena.Copy(key);
}

// A join key: a regular value type, the hash both sides compute, and a copy
// whose bytes the given arena owns.
template <class TKey>
concept CJoinKey = std::regular<TKey> && requires(const TKey& key, TKeyArena& arena) {
    { HashJoinKey(key) } -> std::same_as<uint64_t>;
    { CloneJoinKey(key, arena) } -> std::same_as<TKey>;
};

// A composite key, one struct over the key columns, as join_gen.cpp assembles
// the LookupKey of a join on several columns.
struct TOrderItemKey {
    int64_t OrderId;
    std::string_view Item;

    bool operator==(const TOrderItemKey&) const = default;
};

uint64_t HashJoinKey(const TOrderItemKey& key) {
    uint64_t h = HashJoinKey(key.OrderId);
    return h ^ (HashJoinKey(key.Item) + 0x9E3779B97F4A7C15 + (h << 6) + (h >> 2));
}

TOrderItemKey CloneJoinKey(const TOrderItemKey& key, TKeyArena& arena) {
    return {key.OrderId, arena.Copy(key.Item)};
}

static_assert(CJoinKey<int64_t>);
static_assert(CJoinKey<std::string_view>);
static_assert(CJoinKey<TOrderItemKey>);

// A row the symmetric join retains: the batch index in the high half, the row
// in the low half (TRowId in join_exec.h).
using TRowId = int64_t;

TRowId MakeRowId(size_t batch, size_t row) {
    return static_cast<TRowId>(batch << 32 | row);
}

size_t BatchOf(TRowId id) {
    return static_cast<size_t>(id >> 32);
}

size_t RowOf(TRowId id) {
    return static_cast<size_t>(id & 0xFFFFFFFF);
}

// The control bytes of a swiss table (swiss_group.oz), one per slot. The
// tables keep their keys and values in arrays indexed by the same slots.
class TSwissControl {
public:
    // A power of two, at least one group.
    void Assign(size_t capacity) {
        Ctrl_.assign(capacity, Empty);
    }

    size_t Capacity() const {
        return Ctrl_.size();
    }

    bool IsFull(size_t slot) const {
        return Ctrl_[slot] != Empty;
    }

    void Set(size_t slot, uint64_t hash) {
        Ctrl_[slot] = static_cast<uint8_t>(hash & 127);
    }

    // The slot isKey accepts, or -1 and the first empty slot of the probe
    // sequence. Groups are walked with triangular steps, which visit every
    // group once (rh_lookup_or_empty_physical).
    template <class TIsKey>
    int64_t FindSlot(uint64_t hash, TIsKey&& isKey, int64_t* emptySlot) const {
        size_t groupMask = (Capacity() >> GroupShift) - 1;
        size_t group = (hash >> 7) & groupMask;
        for (size_t step = 1; step <= groupMask + 1; ++step) {
            uint64_t word = Group(group);
            for (uint64_t m = Match(word, hash & 127); m != 0; m &= m - 1) {
                size_t slot = (group << GroupShift) + std::countr_zero(m) / 8;
                if (isKey(slot)) {
                    return static_cast<int64_t>(slot);
                }
            }
            if (uint64_t empty = MatchEmpty(word); empty != 0) {
                *emptySlot = static_cast<int64_t>((group << GroupShift) + std::countr_zero(empty) / 8);
                return -1;
            }
            group = (group + step) & groupMask;
        }
        return -1;
    }

    // The first empty slot of a probe sequence, for a key known to be absent
    // (rh_find_empty_slot): no key compares.
    int64_t FindEmptySlot(uint64_t hash) const {
        size_t groupMask = (Capacity() >> GroupShift) - 1;
        size_t group = (hash >> 7) & groupMask;
        for (size_t step = 1; step <= groupMask + 1; ++step) {
            if (uint64_t empty = MatchEmpty(Group(group)); empty != 0) {
                return static_cast<int64_t>((group << GroupShift) + std::countr_zero(empty) / 8);
            }
            group = (group + step) & groupMask;
        }
        return -1;
    }

private:
    // Groups of 8 slots matched by portable SWAR. The kernel matches the same
    // 8-slot groups with NEON on arm64, and 16-slot groups with SSE2 on x86.
    static constexpr size_t GroupShift = 3;
    static constexpr uint8_t Empty = 0x80;
    static constexpr uint64_t LowBits = 0x0101010101010101;
    static constexpr uint64_t HighBits = LowBits << 7;

    uint64_t Group(size_t group) const {
        uint64_t word;
        std::memcpy(&word, Ctrl_.data() + (group << GroupShift), sizeof(word));
        return word;
    }

    // One high bit per control byte equal to h2 (swiss_match, portable
    // backend). A byte after a borrow can match falsely; the key compare
    // that follows rejects it.
    static uint64_t Match(uint64_t group, uint64_t h2) {
        uint64_t x = group ^ (LowBits * h2);
        return (x - LowBits) & ~x & HighBits;
    }

    // There are no tombstones, so only an empty byte has its high bit set.
    static uint64_t MatchEmpty(uint64_t group) {
        return group & HighBits;
    }

    // Per slot: Empty or the low 7 bits of the hash.
    std::vector<uint8_t> Ctrl_;
};

// The table of one side of the symmetric join: a swiss table of the distinct
// keys, and for each key a bucket of the row ids stored under it. It is the
// aggregation HashTable with three dense int64 columns (join_table.oz): bucket
// length, capacity and pointer. The rows stay in their retained batches.
// Left out: NULL keys (jt_key_valid), the matched marks of outer joins and
// residuals, which do not change the table.
template <CJoinKey TKey>
class TSwissJoinTable {
public:
    TSwissJoinTable() {
        Allocate(InitialCapacity);
    }

    TSwissJoinTable(const TSwissJoinTable&) = delete;
    TSwissJoinTable& operator=(const TSwissJoinTable&) = delete;

    ~TSwissJoinTable() {
        for (size_t id = 0; id < Size_; ++id) {
            std::free(Buckets_[id]);
        }
    }

    // Stores a row under its key: aht_upsert_dual, then jb_append.
    void Insert(const TKey& key, uint64_t hash, TRowId row) {
        int64_t emptySlot = -1;
        int64_t slot = FindSlot(key, hash, &emptySlot);
        int64_t id = (slot >= 0)
            ? SlotId_[slot]
            : AddKey(key, hash, emptySlot);
        AppendRow(id, row);
    }

    // The rows of a key in insertion order (rh_lookup_dual and its bucket).
    std::span<const TRowId> Find(const TKey& key, uint64_t hash) const {
        int64_t emptySlot = -1;
        int64_t slot = FindSlot(key, hash, &emptySlot);
        if (slot < 0) {
            return {};
        }
        int64_t id = SlotId_[slot];
        return {Buckets_[id], static_cast<size_t>(Counts_[id])};
    }

    size_t Keys() const {
        return Size_;
    }

    size_t Capacity() const {
        return Ctrl_.Capacity();
    }

    size_t MemoryBytes() const {
        size_t bytes = Capacity() * (1 + 2 * sizeof(TKey) + 4 * sizeof(int64_t)) + Arena_.Bytes();
        for (size_t id = 0; id < Size_; ++id) {
            bytes += Capacities_[id] * sizeof(TRowId);
        }
        return bytes;
    }

private:
    // JoinInitialCapacity in join_exec.cpp.
    static constexpr size_t InitialCapacity = 256;

    // The physical slot of the key, or -1 and the first empty slot of its
    // probe sequence.
    int64_t FindSlot(const TKey& key, uint64_t hash, int64_t* emptySlot) const {
        auto isKey = [&](size_t slot) {
            return Keys_[slot] == key;
        };
        return Ctrl_.FindSlot(hash, isKey, emptySlot);
    }

    int64_t AddKey(const TKey& key, uint64_t hash, int64_t emptySlot) {
        // At 7/8 load the table doubles; the saved empty slot is then stale.
        if (Size_ + 1 > Capacity() - Capacity() / 8) {
            Grow();
            emptySlot = Ctrl_.FindEmptySlot(hash);
        }
        int64_t id = static_cast<int64_t>(Size_++);
        TKey stored = CloneJoinKey(key, Arena_);
        Ctrl_.Set(emptySlot, hash);
        Keys_[emptySlot] = stored;
        SlotId_[emptySlot] = id;
        GroupKeys_[id] = stored;
        return id;
    }

    // A bucket grows 1.5x from 4 row ids (jb_append).
    void AppendRow(int64_t id, TRowId row) {
        if (Counts_[id] == Capacities_[id]) {
            int64_t capacity = (Capacities_[id] == 0)
                ? 4
                : Capacities_[id] + Capacities_[id] / 2;
            void* bucket = std::realloc(Buckets_[id], capacity * sizeof(TRowId));
            if (bucket == nullptr) {
                throw std::bad_alloc();
            }
            Buckets_[id] = static_cast<TRowId*>(bucket);
            Capacities_[id] = capacity;
        }
        Buckets_[id][Counts_[id]++] = row;
    }

    void Allocate(size_t capacity) {
        Ctrl_.Assign(capacity);
        Keys_.assign(capacity, TKey{});
        SlotId_.assign(capacity, -1);
        GroupKeys_.resize(capacity);
        Counts_.resize(capacity);
        Capacities_.resize(capacity);
        Buckets_.resize(capacity);
    }

    // aht_rehash_dual_impl: every stored key moves to a table twice as large
    // under its recomputed hash, so a stored copy must hash like the key it
    // copies. The dense columns keep their ids and are only extended.
    void Grow() {
        TSwissControl ctrl = std::move(Ctrl_);
        std::vector<TKey> keys = std::move(Keys_);
        std::vector<int64_t> slotId = std::move(SlotId_);
        Allocate(ctrl.Capacity() * 2);
        for (size_t slot = 0; slot < ctrl.Capacity(); ++slot) {
            if (!ctrl.IsFull(slot)) {
                continue;
            }
            uint64_t hash = HashJoinKey(keys[slot]);
            int64_t emptySlot = Ctrl_.FindEmptySlot(hash);
            Ctrl_.Set(emptySlot, hash);
            Keys_[emptySlot] = keys[slot];
            SlotId_[emptySlot] = slotId[slot];
        }
    }

    TSwissControl Ctrl_;
    // Per physical slot: the stored key and its dense id.
    std::vector<TKey> Keys_;
    std::vector<int64_t> SlotId_;
    // Per dense id, sized like the slots. GroupKeys holds the output keys of
    // an aggregate; a join never reads them, but the shared HashTable keeps
    // them. Counts, Capacities and Buckets are AggBuffers[0..2].
    std::vector<TKey> GroupKeys_;
    std::vector<int64_t> Counts_;
    std::vector<int64_t> Capacities_;
    std::vector<TRowId*> Buckets_;
    size_t Size_ = 0;
    TKeyArena Arena_;
};

// The columns of one input batch of the symmetric join.
template <class TKey, class TPayload>
struct TBatch {
    std::vector<TKey> Keys;
    std::vector<TPayload> Payloads;
};

// One side of the symmetric join: every batch it received (TRowStore) and the
// table of its rows.
template <CJoinKey TKey, class TPayload>
struct TSymmetricSide {
    std::vector<TBatch<TKey, TPayload>> Batches;
    TSwissJoinTable<TKey> Table;

    const TPayload& Payload(TRowId id) const {
        return Batches[BatchOf(id)].Payloads[RowOf(id)];
    }
};

// Every row of a batch probes the opposite table, then joins its own table
// (jt_emit_and_insert), so a pair is found when its later row arrives,
// whichever side that is. Pairs are (own row, opposite row).
template <CJoinKey TKey, class TOwn, class TOpposite>
void PushBatch(
    TSymmetricSide<TKey, TOwn>& own,
    const TSymmetricSide<TKey, TOpposite>& opposite,
    TBatch<TKey, TOwn> batch,
    std::vector<std::pair<TRowId, TRowId>>& pairs)
{
    size_t batchIndex = own.Batches.size();
    own.Batches.push_back(std::move(batch));
    const std::vector<TKey>& keys = own.Batches.back().Keys;
    for (size_t row = 0; row < keys.size(); ++row) {
        uint64_t hash = HashJoinKey(keys[row]);
        TRowId id = MakeRowId(batchIndex, row);
        for (TRowId match : opposite.Table.Find(keys[row], hash)) {
            pairs.emplace_back(id, match);
        }
        own.Table.Insert(keys[row], hash, id);
    }
}

// A build row in the arena, the row layout of join_gen.cpp: one word that
// holds the key hash while the arena fills and then the next row with an
// equal key (null ends the chain), the key, the payload. Key and payload are
// stored as the probe sees them; string views point into the input batches,
// which stay retained.
template <CJoinKey TKey, class TPayload>
struct TBuildRow {
    uint64_t HashOrNext;
    TKey Key;
    TPayload Payload;
};

// Build rows in fixed blocks (ja_new_row). Rows never move, so table entries
// and chains can point at them.
template <class TRow>
class TRowArena {
public:
    TRow& Append() {
        if (CurRows_ == BlockRows) {
            Blocks_.push_back(std::make_unique_for_overwrite<TRow[]>(BlockRows));
            CurRows_ = 0;
        }
        ++Rows_;
        return Blocks_.back()[CurRows_++];
    }

    size_t Rows() const {
        return Rows_;
    }

    size_t MemoryBytes() const {
        return Blocks_.size() * BlockRows * sizeof(TRow);
    }

    // From the first row to the last.
    template <class TVisit>
    void Visit(TVisit&& visit) const {
        for (size_t block = 0; block < Blocks_.size(); ++block) {
            size_t rows = (block + 1 == Blocks_.size())
                ? CurRows_
                : BlockRows;
            for (size_t row = 0; row < rows; ++row) {
                visit(std::as_const(Blocks_[block][row]));
            }
        }
    }

    // From the last row to the first.
    template <class TVisit>
    void VisitBackward(TVisit&& visit) {
        for (size_t block = Blocks_.size(); block-- > 0;) {
            size_t rows = (block + 1 == Blocks_.size())
                ? CurRows_
                : BlockRows;
            for (size_t row = rows; row-- > 0;) {
                visit(Blocks_[block][row]);
            }
        }
    }

private:
    // The block size join_gen.cpp passes to ja_init.
    static constexpr size_t BlockRows = 16384;

    std::vector<std::unique_ptr<TRow[]>> Blocks_;
    size_t CurRows_ = BlockRows;
    size_t Rows_ = 0;
};

// jrt_mix multiplies by this odd constant, 2^64 divided by the golden ratio.
constexpr uint64_t FibonacciMultiplier = 0x9E3779B97F4A7C15;

// The entries of a row table (join_rows.oz): one 8-byte word per slot, 0 when
// empty, else a 16-bit salt above a 48-bit pointer to a row that lives
// elsewhere. Probing is linear from the home slot.
class TRowEntries {
public:
    // A power of two.
    void Assign(size_t capacity) {
        Entries_.assign(capacity, 0);
        Mask_ = capacity - 1;
        Shift_ = 64 - std::countr_zero(capacity);
    }

    size_t Capacity() const {
        return Entries_.size();
    }

    size_t MemoryBytes() const {
        return Entries_.size() * sizeof(uint64_t);
    }

    // The index is the top bits of the hash: the hash shuffle routes rows by
    // the low bits, which are thus nearly constant inside a partition. A key
    // hash can carry little entropy in its top bits, so a Fibonacci multiply
    // first spreads every bit over them (jrt_mix).
    static uint64_t Mix(uint64_t hash) {
        return hash * FibonacciMultiplier;
    }

    static uint64_t Salt(uint64_t mixed) {
        return (mixed >> 16) & 0xFFFF;
    }

    uint64_t Home(uint64_t mixed) const {
        return mixed >> Shift_;
    }

    uint64_t Next(uint64_t off) const {
        return (off + 1) & Mask_;
    }

    uint64_t operator[](uint64_t off) const {
        return Entries_[off];
    }

    static uint64_t SaltOf(uint64_t entry) {
        return entry >> 48;
    }

    template <class TRow>
    static TRow* Row(uint64_t entry) {
        return reinterpret_cast<TRow*>(entry & PointerMask);
    }

    // jrt_seek: from off to the first entry that is empty (0) or carries the
    // salt; off is left at that entry.
    uint64_t Seek(uint64_t& off, uint64_t salt) const {
        uint64_t entry = Entries_[off];
        while (entry != 0 && SaltOf(entry) != salt) {
            off = Next(off);
            entry = Entries_[off];
        }
        return entry;
    }

    // jrt_link: a user-space pointer fits in 48 bits.
    void Set(uint64_t off, uint64_t salt, const void* row) {
        Entries_[off] = salt << 48 | reinterpret_cast<uintptr_t>(row);
    }

private:
    static constexpr uint64_t PointerMask = (uint64_t{1} << 48) - 1;
    static_assert(sizeof(uintptr_t) == sizeof(uint64_t), "an entry holds a 64-bit pointer");

    std::vector<uint64_t> Entries_;
    uint64_t Mask_ = 0;
    int Shift_ = 64;
};

// The row join table of a fixed build side (join_rows.oz), after DuckDB's
// JoinHashTable: one 8-byte entry per slot, a 16-bit salt above a 48-bit row
// pointer, at most half full. A probe reads one entry and, on a salt match,
// one row, so a hit costs about two cache lines and a miss one.
template <CJoinKey TKey, class TPayload>
class TRowJoinTable {
public:
    using TRow = TBuildRow<TKey, TPayload>;

    // A pair of the probe output (PairBuffer): the probe row and its match.
    struct TMatch {
        size_t ProbeRow;
        const TRow* Row;
    };

    // The generated build append (jb_append): the hash waits in the first
    // word until Build.
    void Append(const TKey& key, const TPayload& payload) {
        TRow& row = Arena_.Append();
        row.HashOrNext = HashJoinKey(key);
        row.Key = key;
        row.Payload = payload;
    }

    // jr_build_rows: an empty entry starts a new key; a salt match with an
    // equal key chains the row in front of that key's rows; anything else
    // moves to the next entry. Linking from the last row to the first leaves
    // every chain in build order. It runs once: linking overwrites the hashes.
    void Build() {
        if (Entries_.Capacity() != 0) {
            throw std::logic_error("a row join table is built once");
        }
        size_t capacity = 1024;
        while (capacity < 2 * Arena_.Rows()) {
            capacity *= 2;
        }
        Entries_.Assign(capacity);
        Keys_ = 0;
        Arena_.VisitBackward([&](TRow& row) {
            uint64_t mixed = TRowEntries::Mix(row.HashOrNext);
            uint64_t salt = TRowEntries::Salt(mixed);
            uint64_t off = Entries_.Home(mixed);
            for (;;) {
                uint64_t entry = Entries_.Seek(off, salt);
                TRow* head = TRowEntries::Row<TRow>(entry);
                if (entry == 0 || head->Key == row.Key) {
                    Link(off, salt, row, head);
                    return;
                }
                off = Entries_.Next(off);
            }
        });
    }

    // Probes a batch of keys in three passes and appends one match per
    // matching build row. Each pass loops over independent rows, so the cache
    // misses of different rows overlap.
    void Probe(std::span<const TKey> keys, std::vector<TMatch>& matches) {
        if (Entries_.Capacity() == 0) {
            throw std::logic_error("a row join table is probed after Build");
        }
        size_t count = keys.size();
        Hashes_.resize(count);
        Salts_.resize(count);
        Offsets_.resize(count);
        Found_.resize(count);
        Work_.resize(count);
        // Pass 1, generated per join: the hash of every key. The kernel also
        // copies the keys into JoinScratch.
        for (size_t j = 0; j < count; ++j) {
            Hashes_[j] = HashJoinKey(keys[j]);
        }
        // Pass 2 (jr_seek_all): the first entry that is empty or carries the
        // row's salt.
        for (size_t j = 0; j < count; ++j) {
            uint64_t mixed = TRowEntries::Mix(Hashes_[j]);
            Salts_[j] = TRowEntries::Salt(mixed);
            Offsets_[j] = Entries_.Home(mixed);
            Found_[j] = Entries_.Seek(Offsets_[j], Salts_[j]);
        }
        // Pass 3 (jr_probe_candidates): compare the key of each candidate. A
        // match emits its whole chain; a salt collision moves past its entry
        // and seeks again (jr_seek_work), until every row reached an empty
        // entry or its key.
        size_t pending = count;
        for (bool first = true; pending > 0; first = false) {
            size_t collided = 0;
            for (size_t q = 0; q < pending; ++q) {
                size_t j = first
                    ? q
                    : Work_[q];
                const TRow* row = TRowEntries::Row<TRow>(Found_[j]);
                if (row == nullptr) {
                    continue;
                }
                if (row->Key == keys[j]) {
                    for (; row != nullptr; row = Next(row)) {
                        matches.push_back({j, row});
                    }
                } else {
                    Offsets_[j] = Entries_.Next(Offsets_[j]);
                    Work_[collided++] = j;
                }
            }
            pending = collided;
            for (size_t q = 0; q < pending; ++q) {
                size_t j = Work_[q];
                Found_[j] = Entries_.Seek(Offsets_[j], Salts_[j]);
            }
        }
    }

    size_t Keys() const {
        return Keys_;
    }

    size_t Capacity() const {
        return Entries_.Capacity();
    }

    size_t MemoryBytes() const {
        return Entries_.MemoryBytes() + Arena_.MemoryBytes();
    }

private:
    static const TRow* Next(const TRow* row) {
        return reinterpret_cast<const TRow*>(row->HashOrNext);
    }

    void Link(uint64_t off, uint64_t salt, TRow& row, TRow* head) {
        row.HashOrNext = reinterpret_cast<uintptr_t>(head);
        if (head == nullptr) {
            ++Keys_;
        }
        Entries_.Set(off, salt, &row);
    }

    TRowArena<TRow> Arena_;
    TRowEntries Entries_;
    size_t Keys_ = 0;
    // Probe scratch, one slot per probe row (JoinScratch).
    std::vector<uint64_t> Hashes_;
    std::vector<uint64_t> Salts_;
    std::vector<uint64_t> Offsets_;
    std::vector<uint64_t> Found_;
    std::vector<size_t> Work_;
};

// The multiplicative inverse of FibonacciMultiplier modulo 2^64. Each Newton
// step doubles the correct low bits; an odd number is its own inverse modulo 8.
constexpr uint64_t InverseFibonacciMultiplier() {
    uint64_t inverse = FibonacciMultiplier;
    for (int step = 0; step < 5; ++step) {
        inverse *= 2 - FibonacciMultiplier * inverse;
    }
    return inverse;
}

static_assert(FibonacciMultiplier * InverseFibonacciMultiplier() == 1);

// A key whose hash the row join table mixes to a chosen value: top bits 0, so
// every row has entry 0 as its home, and one of two salts. All rows then form
// one cluster in which both salts alternate, which exercises the collision
// walk of the row join table and the group walk of the swiss table.
struct TCollidingKey {
    int64_t Value;

    bool operator==(const TCollidingKey&) const = default;
};

uint64_t HashJoinKey(const TCollidingKey& key) {
    uint64_t mixed = static_cast<uint64_t>(key.Value & 1) << 16;
    return mixed * InverseFibonacciMultiplier();
}

TCollidingKey CloneJoinKey(const TCollidingKey& key, TKeyArena&) {
    return key;
}

std::vector<int64_t> RandomKeys(size_t count, int64_t range, uint64_t seed) {
    std::mt19937_64 random(seed);
    std::vector<int64_t> keys(count);
    for (int64_t& key : keys) {
        key = static_cast<int64_t>(random() % static_cast<uint64_t>(range));
    }
    return keys;
}

} // namespace

TEST(JoinTablePrototype, SwissTableKeepsRowsOfAKeyInInsertionOrder) {
    TSwissJoinTable<int64_t> table;
    // 3000 keys grow the table from 256 slots several times.
    for (int64_t row = 0; row < 9000; ++row) {
        int64_t key = row % 3000;
        table.Insert(key, HashJoinKey(key), MakeRowId(row / 1000, row % 1000));
    }
    EXPECT_EQ(table.Keys(), 3000u);
    EXPECT_EQ(table.Capacity(), 4096u);
    for (int64_t key = 0; key < 3000; ++key) {
        std::span<const TRowId> rows = table.Find(key, HashJoinKey(key));
        ASSERT_EQ(rows.size(), 3u);
        for (int64_t i = 0; i < 3; ++i) {
            int64_t row = key + i * 3000;
            EXPECT_EQ(rows[i], MakeRowId(row / 1000, row % 1000));
        }
    }
    EXPECT_TRUE(table.Find(3000, HashJoinKey(3000)).empty());
}

TEST(JoinTablePrototype, SwissTableOwnsCopiesOfStringKeys) {
    TSwissJoinTable<std::string_view> table;
    std::string input;
    for (int i = 0; i < 1000; ++i) {
        input = "customer#" + std::to_string(i);
        table.Insert(input, HashJoinKey(std::string_view(input)), i);
    }
    // The input buffer was overwritten after every insert.
    for (int i = 0; i < 1000; ++i) {
        std::string key = "customer#" + std::to_string(i);
        std::span<const TRowId> rows = table.Find(key, HashJoinKey(std::string_view(key)));
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows[0], i);
    }
}

TEST(JoinTablePrototype, SymmetricJoinMatchesNestedLoop) {
    using TSide = TSymmetricSide<int64_t, int64_t>;
    TSide left;
    TSide right;
    std::vector<std::pair<int64_t, int64_t>> expected;
    std::vector<std::pair<int64_t, int64_t>> actual;
    std::vector<std::pair<TRowId, TRowId>> leftPairs;
    std::vector<std::pair<TRowId, TRowId>> rightPairs;
    std::vector<int64_t> leftKeys;
    std::vector<int64_t> rightKeys;
    // Batches of both sides arrive interleaved, as the join pulls them.
    for (uint64_t batch = 0; batch < 8; ++batch) {
        TBatch<int64_t, int64_t> l{RandomKeys(300, 500, batch), {}};
        TBatch<int64_t, int64_t> r{RandomKeys(200, 500, batch + 100), {}};
        for (int64_t key : l.Keys) {
            l.Payloads.push_back(static_cast<int64_t>(leftKeys.size()));
            leftKeys.push_back(key);
        }
        for (int64_t key : r.Keys) {
            r.Payloads.push_back(static_cast<int64_t>(rightKeys.size()));
            rightKeys.push_back(key);
        }
        PushBatch(left, right, std::move(l), leftPairs);
        PushBatch(right, left, std::move(r), rightPairs);
    }
    for (auto [own, opposite] : leftPairs) {
        actual.emplace_back(left.Payload(own), right.Payload(opposite));
    }
    for (auto [own, opposite] : rightPairs) {
        actual.emplace_back(left.Payload(opposite), right.Payload(own));
    }
    for (size_t l = 0; l < leftKeys.size(); ++l) {
        for (size_t r = 0; r < rightKeys.size(); ++r) {
            if (leftKeys[l] == rightKeys[r]) {
                expected.emplace_back(l, r);
            }
        }
    }
    std::ranges::sort(actual);
    std::ranges::sort(expected);
    EXPECT_EQ(actual, expected);
}

TEST(JoinTablePrototype, RowTableChainsDuplicatesInBuildOrder) {
    TRowJoinTable<int64_t, int64_t> table;
    for (int64_t row = 0; row < 40000; ++row) {
        table.Append(row % 10000, row);
    }
    table.Build();
    EXPECT_EQ(table.Keys(), 10000u);
    EXPECT_EQ(table.Capacity(), 131072u);
    std::vector<int64_t> keys = {7, -1, 9999, 7, 10000};
    std::vector<TRowJoinTable<int64_t, int64_t>::TMatch> matches;
    table.Probe(keys, matches);
    std::vector<std::pair<size_t, int64_t>> actual;
    for (const auto& match : matches) {
        actual.emplace_back(match.ProbeRow, match.Row->Payload);
    }
    std::vector<std::pair<size_t, int64_t>> expected = {
        {0, 7},
        {0, 10007},
        {0, 20007},
        {0, 30007},
        {2, 9999},
        {2, 19999},
        {2, 29999},
        {2, 39999},
        {3, 7},
        {3, 10007},
        {3, 20007},
        {3, 30007},
    };
    EXPECT_EQ(actual, expected);
}

TEST(JoinTablePrototype, RowTableJoinsCompositeKeysOverRetainedInput) {
    // The build input stays alive while the table points into it: neither key
    // nor payload strings are copied.
    std::vector<std::string> items = {"bolt", "nut", "screw", "washer"};
    std::vector<std::string> suppliers = {"acme", "globex", "initech", "umbrella", "hooli"};
    TRowJoinTable<TOrderItemKey, std::string_view> table;
    for (size_t i = 0; i < 2000; ++i) {
        TOrderItemKey key{static_cast<int64_t>(i % 500), items[i % items.size()]};
        table.Append(key, suppliers[i % suppliers.size()]);
    }
    table.Build();
    std::vector<TOrderItemKey> probe;
    for (int64_t order = 0; order < 600; ++order) {
        for (const std::string& item : items) {
            probe.push_back({order, item});
        }
    }
    std::vector<TRowJoinTable<TOrderItemKey, std::string_view>::TMatch> matches;
    table.Probe(probe, matches);
    std::vector<std::pair<size_t, std::string_view>> actual;
    for (const auto& match : matches) {
        actual.emplace_back(match.ProbeRow, match.Row->Payload);
    }
    std::vector<std::pair<size_t, std::string_view>> expected;
    for (size_t j = 0; j < probe.size(); ++j) {
        for (size_t i = 0; i < 2000; ++i) {
            TOrderItemKey key{static_cast<int64_t>(i % 500), items[i % items.size()]};
            if (key == probe[j]) {
                expected.emplace_back(j, suppliers[i % suppliers.size()]);
            }
        }
    }
    // Salt collisions delay a row's matches to a later round.
    std::ranges::stable_sort(actual, {}, &std::pair<size_t, std::string_view>::first);
    EXPECT_FALSE(expected.empty());
    EXPECT_EQ(actual, expected);
}

TEST(JoinTablePrototype, BothTablesAgreeUnderSaltCollisions) {
    TSwissJoinTable<TCollidingKey> swiss;
    TRowJoinTable<TCollidingKey, int64_t> rows;
    std::vector<int64_t> buildKeys = RandomKeys(3000, 1000, 1);
    for (size_t row = 0; row < buildKeys.size(); ++row) {
        TCollidingKey key{buildKeys[row]};
        swiss.Insert(key, HashJoinKey(key), static_cast<TRowId>(row));
        rows.Append(key, static_cast<int64_t>(row));
    }
    rows.Build();
    EXPECT_EQ(rows.Keys(), swiss.Keys());
    std::vector<TCollidingKey> probe;
    for (int64_t key : RandomKeys(2000, 1200, 2)) {
        probe.push_back({key});
    }
    std::vector<TRowJoinTable<TCollidingKey, int64_t>::TMatch> matches;
    rows.Probe(probe, matches);
    std::vector<std::pair<size_t, int64_t>> actual;
    for (const auto& match : matches) {
        actual.emplace_back(match.ProbeRow, match.Row->Payload);
    }
    std::vector<std::pair<size_t, int64_t>> fromSwiss;
    for (size_t j = 0; j < probe.size(); ++j) {
        for (TRowId row : swiss.Find(probe[j], HashJoinKey(probe[j]))) {
            fromSwiss.emplace_back(j, row);
        }
    }
    std::vector<std::pair<size_t, int64_t>> expected;
    for (size_t j = 0; j < probe.size(); ++j) {
        for (size_t row = 0; row < buildKeys.size(); ++row) {
            if (buildKeys[row] == probe[j].Value) {
                expected.emplace_back(j, static_cast<int64_t>(row));
            }
        }
    }
    // A row that met a salt collision emits its matches in a later round, so
    // the row table orders pairs by round, not by probe row.
    std::ranges::stable_sort(actual, {}, &std::pair<size_t, int64_t>::first);
    EXPECT_FALSE(expected.empty());
    EXPECT_EQ(actual, expected);
    EXPECT_EQ(fromSwiss, expected);
}

// Build and probe cost per row of both tables on one thread, for growing
// tables, hit rates and duplicate counts. The swiss path reads its payload
// through the retained batch, as the symmetric join materializes a pair.
// Not run by ctest:
// test_join --gtest_also_run_disabled_tests --gtest_filter='*Microbenchmark*'
TEST(JoinTablePrototype, DISABLED_Microbenchmark) {
    using TClock = std::chrono::steady_clock;
    constexpr size_t probeRows = size_t{1} << 22;
    constexpr size_t probeBatch = 2048;
    constexpr size_t batchRows = 16384;
    auto nanosPerRow = [](TClock::time_point start, size_t rows) {
        return std::chrono::duration<double, std::nano>(TClock::now() - start).count() / rows;
    };
    std::cout << std::format(
        "{:>10} {:>4} {:>5} {:>12} {:>12} {:>12} {:>12} {:>8} {:>10} {:>10}\n",
        "rows",
        "dup",
        "hit%",
        "swiss_build",
        "row_build",
        "swiss_probe",
        "row_probe",
        "probe_x",
        "swiss_B",
        "row_B");
    for (int shift : {10, 14, 18, 22, 24}) {
        for (size_t duplicates : {1, 4}) {
            size_t buildRows = size_t{1} << shift;
            int64_t keys = static_cast<int64_t>(buildRows / duplicates);
            std::vector<int64_t> buildKeys(buildRows);
            for (size_t row = 0; row < buildRows; ++row) {
                buildKeys[row] = static_cast<int64_t>(row) % keys;
            }
            std::shuffle(buildKeys.begin(), buildKeys.end(), std::mt19937_64(shift));

            // The symmetric join retains its input batches as they arrive, so
            // only the table inserts count as its build.
            TSymmetricSide<int64_t, int64_t> swiss;
            for (size_t begin = 0; begin < buildRows; begin += batchRows) {
                auto& batch = swiss.Batches.emplace_back();
                for (size_t row = begin; row < std::min(buildRows, begin + batchRows); ++row) {
                    batch.Keys.push_back(buildKeys[row]);
                    batch.Payloads.push_back(buildKeys[row] * 3);
                }
            }
            auto start = TClock::now();
            for (size_t batch = 0; batch < swiss.Batches.size(); ++batch) {
                const std::vector<int64_t>& batchKeys = swiss.Batches[batch].Keys;
                for (size_t row = 0; row < batchKeys.size(); ++row) {
                    swiss.Table.Insert(batchKeys[row], HashJoinKey(batchKeys[row]), MakeRowId(batch, row));
                }
            }
            double swissBuild = nanosPerRow(start, buildRows);

            TRowJoinTable<int64_t, int64_t> rows;
            start = TClock::now();
            for (int64_t key : buildKeys) {
                rows.Append(key, key * 3);
            }
            rows.Build();
            double rowBuild = nanosPerRow(start, buildRows);

            // The swiss side retains its batches: two i64 columns per row.
            size_t swissBytes = swiss.Table.MemoryBytes() + buildRows * 2 * sizeof(int64_t);
            for (int hitPercent : {10, 50, 90}) {
                std::vector<int64_t> probe = RandomKeys(
                    probeRows,
                    keys * 100 / hitPercent,
                    shift + hitPercent);

                int64_t swissSum = 0;
                std::vector<std::pair<size_t, TRowId>> swissPairs;
                start = TClock::now();
                for (size_t begin = 0; begin < probeRows; begin += probeBatch) {
                    swissPairs.clear();
                    for (size_t j = begin; j < begin + probeBatch; ++j) {
                        for (TRowId row : swiss.Table.Find(probe[j], HashJoinKey(probe[j]))) {
                            swissPairs.emplace_back(j, row);
                        }
                    }
                    for (auto [j, row] : swissPairs) {
                        swissSum += swiss.Payload(row);
                    }
                }
                double swissProbe = nanosPerRow(start, probeRows);

                int64_t rowSum = 0;
                std::vector<TRowJoinTable<int64_t, int64_t>::TMatch> matches;
                start = TClock::now();
                for (size_t begin = 0; begin < probeRows; begin += probeBatch) {
                    matches.clear();
                    rows.Probe(std::span(probe).subspan(begin, probeBatch), matches);
                    for (const auto& match : matches) {
                        rowSum += match.Row->Payload;
                    }
                }
                double rowProbe = nanosPerRow(start, probeRows);

                EXPECT_EQ(swissSum, rowSum);
                std::cout << std::format(
                    "{:>10} {:>4} {:>5} {:>12.2f} {:>12.2f} {:>12.2f} {:>12.2f}"
                    " {:>8.2f} {:>10.1f} {:>10.1f}\n",
                    buildRows,
                    duplicates,
                    hitPercent,
                    swissBuild,
                    rowBuild,
                    swissProbe,
                    rowProbe,
                    swissProbe / rowProbe,
                    static_cast<double>(swissBytes) / buildRows,
                    static_cast<double>(rows.MemoryBytes()) / buildRows);
            }
        }
    }
}

// C++ prototypes of two aggregate hash tables (#352), tested and measured like
// the join tables above:
// - TSwissAggregateTable: the table of QDB's hash aggregate with fixed-width
//   states (aht_upsert_aggregate in robin_hood_dual_key.oz, and the update and
//   finalize loops gen.cpp generates around it);
// - TRowAggregateTable: the same aggregate on the layout of TRowJoinTable.
// Both compute the first 1 to 4 of sum, count, min and max over one int64
// argument.

namespace {

// The aggregates in the order a table of n aggregates takes the first n.
enum class EAggregate {
    Sum,
    Count,
    Min,
    Max,
};

constexpr std::array<EAggregate, 4> PrototypeAggregates = {
    EAggregate::Sum,
    EAggregate::Count,
    EAggregate::Min,
    EAggregate::Max,
};

// The scalar reducers of gen.cpp: the states of a new group are zero, and its
// first value replaces the zero of min and max.
int64_t Reduce(EAggregate aggregate, int64_t state, int64_t value, bool isNew) {
    switch (aggregate) {
        case EAggregate::Sum:
            return state + value;
        case EAggregate::Count:
            return state + 1;
        case EAggregate::Min:
            return (isNew || value < state)
                ? value
                : state;
        case EAggregate::Max:
            return (isNew || value > state)
                ? value
                : state;
    }
    std::unreachable();
}

// The output of finalization: a column of keys and one per aggregate. The
// caller sizes it to the group count, as the host sizes the output rowset
// before agg_finish_rowset.
template <class TKey, size_t AggregateCount>
struct TAggregateColumns {
    explicit TAggregateColumns(size_t groups)
        : Keys(groups)
    {
        for (std::vector<int64_t>& states : States) {
            states.resize(groups);
        }
    }

    std::vector<TKey> Keys;
    std::array<std::vector<int64_t>, AggregateCount> States;
};

// A composite key of two integer columns, as the GROUP BY ss_item_sk,
// ss_customer_sk of TPC-DS q78.
struct TItemCustomerKey {
    int64_t Item;
    int64_t Customer;

    bool operator==(const TItemCustomerKey&) const = default;
};

uint64_t HashJoinKey(const TItemCustomerKey& key) {
    uint64_t h = HashJoinKey(key.Item);
    return h ^ (HashJoinKey(key.Customer) + 0x9E3779B97F4A7C15 + (h << 6) + (h >> 2));
}

TItemCustomerKey CloneJoinKey(const TItemCustomerKey& key, TKeyArena&) {
    return key;
}

static_assert(CJoinKey<TItemCustomerKey>);

// The table of QDB's hash aggregate (aggregation_hashtable_generic.oz) with
// the states at the probe slot, where aht_upsert_aggregate keeps fixed-width
// states. Per slot: the control byte, the stored key, and one state in the
// array of each aggregate. Per group, in insertion order: the key (GroupKeys)
// and its slot (SlotId), which only growth reads. An update out of cache
// misses on the control group, the key and every state: 2 + n cache lines.
template <CJoinKey TKey, size_t AggregateCount>
class TSwissAggregateTable {
public:
    // From one group of 8 slots (aggregate_exec.cpp), or pre-sized for the
    // given groups as EstimateAggregateInitialCapacity sizes it for an
    // estimate.
    explicit TSwissAggregateTable(size_t groups = 0) {
        size_t capacity = InitialCapacity;
        while (groups > capacity - capacity / 8) {
            capacity *= 2;
        }
        Allocate(capacity);
    }

    // Row after row, as the update loop of agg_dispatch: find or insert the
    // group, then apply every reducer at its slot.
    void Update(std::span<const TKey> keys, std::span<const int64_t> values) {
        for (size_t j = 0; j < keys.size(); ++j) {
            uint64_t hash = HashJoinKey(keys[j]);
            auto isKey = [&](size_t slot) {
                return Keys_[slot] == keys[j];
            };
            int64_t emptySlot = -1;
            int64_t slot = Ctrl_.FindSlot(hash, isKey, &emptySlot);
            bool isNew = slot < 0;
            if (isNew) {
                slot = AddGroup(keys[j], hash, emptySlot);
            }
            for (size_t a = 0; a < AggregateCount; ++a) {
                States_[a][slot] = Reduce(PrototypeAggregates[a], States_[a][slot], values[j], isNew);
            }
        }
    }

    // agg_finish_rowset for fixed-width keys: the slots in order, so the keys
    // and the states are scanned, not gathered through SlotId.
    void Finalize(TAggregateColumns<TKey, AggregateCount>& output) const {
        size_t group = 0;
        for (size_t slot = 0; slot < Capacity(); ++slot) {
            if (!Ctrl_.IsFull(slot)) {
                continue;
            }
            output.Keys[group] = Keys_[slot];
            for (size_t a = 0; a < AggregateCount; ++a) {
                output.States[a][group] = States_[a][slot];
            }
            ++group;
        }
    }

    size_t Groups() const {
        return Size_;
    }

    size_t Capacity() const {
        return Ctrl_.Capacity();
    }

    size_t MemoryBytes() const {
        return Capacity() * (1 + 2 * sizeof(TKey) + (1 + AggregateCount) * sizeof(int64_t)) + Arena_.Bytes();
    }

private:
    static constexpr size_t InitialCapacity = 8;

    // aht_insert_dual for physical states: at 7/8 load the table doubles and
    // the saved empty slot is stale; the new group then zeroes its states.
    int64_t AddGroup(const TKey& key, uint64_t hash, int64_t emptySlot) {
        if (Size_ + 1 > Capacity() - Capacity() / 8) {
            Grow();
            emptySlot = Ctrl_.FindEmptySlot(hash);
        }
        TKey stored = CloneJoinKey(key, Arena_);
        Ctrl_.Set(emptySlot, hash);
        Keys_[emptySlot] = stored;
        SlotId_[Size_] = emptySlot;
        GroupKeys_[Size_] = stored;
        for (std::vector<int64_t>& states : States_) {
            states[emptySlot] = 0;
        }
        ++Size_;
        return emptySlot;
    }

    void Allocate(size_t capacity) {
        Ctrl_.Assign(capacity);
        Keys_.assign(capacity, TKey{});
        SlotId_.assign(capacity, -1);
        GroupKeys_.resize(capacity);
        for (std::vector<int64_t>& states : States_) {
            states.assign(capacity, 0);
        }
    }

    // rh_rehash_aggregate: every group key is hashed again into a table twice
    // as large, then every state moves from its old slot to its new one.
    void Grow() {
        std::vector<int64_t> slotId = std::move(SlotId_);
        std::array<std::vector<int64_t>, AggregateCount> states = std::move(States_);
        Allocate(2 * Capacity());
        for (size_t id = 0; id < Size_; ++id) {
            uint64_t hash = HashJoinKey(GroupKeys_[id]);
            int64_t slot = Ctrl_.FindEmptySlot(hash);
            Ctrl_.Set(slot, hash);
            Keys_[slot] = GroupKeys_[id];
            SlotId_[id] = slot;
        }
        for (size_t a = 0; a < AggregateCount; ++a) {
            for (size_t id = 0; id < Size_; ++id) {
                States_[a][SlotId_[id]] = states[a][slotId[id]];
            }
        }
    }

    TSwissControl Ctrl_;
    // Per physical slot.
    std::vector<TKey> Keys_;
    std::array<std::vector<int64_t>, AggregateCount> States_;
    // Per group, sized like the slots.
    std::vector<TKey> GroupKeys_;
    std::vector<int64_t> SlotId_;
    size_t Size_ = 0;
    TKeyArena Arena_;
};

// A group of the row aggregate table: the key hash, the key and every state
// in one arena row. Growth places the row by its hash without hashing the key
// again.
template <CJoinKey TKey, size_t AggregateCount>
struct TGroupRow {
    uint64_t Hash;
    TKey Key;
    std::array<int64_t, AggregateCount> States;
};

// The aggregate on the layout of TRowJoinTable: a group is a row of an arena,
// and the table holds entries that point at the rows, at most half full. An
// update reads one entry and, on a salt match, the row with the key and every
// state: about two cache lines whatever the number of aggregates. Rows never
// move, so growth rewrites only the entries.
template <CJoinKey TKey, size_t AggregateCount>
class TRowAggregateTable {
public:
    using TRow = TGroupRow<TKey, AggregateCount>;

    // From 8 entries, or pre-sized for the given groups.
    explicit TRowAggregateTable(size_t groups = 0) {
        Entries_.Assign(CapacityFor(groups));
    }

    // Finds or inserts the group of every row of a batch in three passes and
    // applies the row to its states. Each pass loops over independent rows,
    // so the cache misses of different rows overlap.
    void Update(std::span<const TKey> keys, std::span<const int64_t> values) {
        size_t count = keys.size();
        Hashes_.resize(count);
        Salts_.resize(count);
        Offsets_.resize(count);
        Work_.resize(count);
        // Pass 1: the hash of every key and its home entry.
        for (size_t j = 0; j < count; ++j) {
            Hashes_[j] = HashJoinKey(keys[j]);
            uint64_t mixed = TRowEntries::Mix(Hashes_[j]);
            Salts_[j] = TRowEntries::Salt(mixed);
            Offsets_[j] = Entries_.Home(mixed);
            Work_[j] = j;
        }
        size_t pending = count;
        while (pending > 0) {
            // Pass 2: the first entry that is empty or carries the row's salt.
            size_t empty = 0;
            for (size_t q = 0; q < pending; ++q) {
                size_t j = Work_[q];
                if (Entries_.Seek(Offsets_[j], Salts_[j]) == 0) {
                    ++empty;
                }
            }
            // Only a row at an empty entry inserts a group, at most one, so
            // growing here keeps the table at most half full. Growth moves
            // the home entries, and the pending rows seek again.
            if (Groups() + empty > Entries_.Capacity() / 2) {
                Grow(Groups() + empty);
                for (size_t q = 0; q < pending; ++q) {
                    size_t j = Work_[q];
                    Offsets_[j] = Entries_.Home(TRowEntries::Mix(Hashes_[j]));
                }
                continue;
            }
            // Pass 3: compare the key at each candidate entry, or insert at an
            // empty one. The entry is read again because an earlier row of the
            // batch may have inserted there since pass 2, with this row's key
            // or another. A salt collision moves past its entry and seeks again,
            // until every row found or inserted its group.
            size_t collided = 0;
            for (size_t q = 0; q < pending; ++q) {
                size_t j = Work_[q];
                uint64_t entry = Entries_[Offsets_[j]];
                TRow* row = TRowEntries::Row<TRow>(entry);
                if (entry == 0) {
                    Accumulate(Insert(Offsets_[j], Salts_[j], Hashes_[j], keys[j]), values[j], true);
                } else if (TRowEntries::SaltOf(entry) == Salts_[j] && row->Key == keys[j]) {
                    Accumulate(*row, values[j], false);
                } else {
                    Offsets_[j] = Entries_.Next(Offsets_[j]);
                    Work_[collided++] = j;
                }
            }
            pending = collided;
        }
    }

    // A scan of the arena: the groups in insertion order.
    void Finalize(TAggregateColumns<TKey, AggregateCount>& output) const {
        size_t group = 0;
        Arena_.Visit([&](const TRow& row) {
            output.Keys[group] = row.Key;
            for (size_t a = 0; a < AggregateCount; ++a) {
                output.States[a][group] = row.States[a];
            }
            ++group;
        });
    }

    // The row of a key, or null.
    const TRow* Find(const TKey& key) const {
        uint64_t mixed = TRowEntries::Mix(HashJoinKey(key));
        uint64_t salt = TRowEntries::Salt(mixed);
        for (uint64_t off = Entries_.Home(mixed);; off = Entries_.Next(off)) {
            const TRow* row = TRowEntries::Row<const TRow>(Entries_.Seek(off, salt));
            if (row == nullptr || row->Key == key) {
                return row;
            }
        }
    }

    size_t Groups() const {
        return Arena_.Rows();
    }

    size_t Capacity() const {
        return Entries_.Capacity();
    }

    size_t MemoryBytes() const {
        return Entries_.MemoryBytes() + Arena_.MemoryBytes() + KeyArena_.Bytes();
    }

private:
    static constexpr size_t InitialCapacity = 8;

    static size_t CapacityFor(size_t groups) {
        size_t capacity = InitialCapacity;
        while (capacity / 2 < groups) {
            capacity *= 2;
        }
        return capacity;
    }

    // A new group: its row with zero states, then the entry that points at it.
    TRow& Insert(uint64_t off, uint64_t salt, uint64_t hash, const TKey& key) {
        TRow& row = Arena_.Append();
        row.Hash = hash;
        row.Key = CloneJoinKey(key, KeyArena_);
        row.States.fill(0);
        Entries_.Set(off, salt, &row);
        return row;
    }

    static void Accumulate(TRow& row, int64_t value, bool isNew) {
        for (size_t a = 0; a < AggregateCount; ++a) {
            row.States[a] = Reduce(PrototypeAggregates[a], row.States[a], value, isNew);
        }
    }

    // Entries for the given groups, filled by a scan of the arena. Keys are
    // unique, so every row takes the first empty entry from its home.
    void Grow(size_t groups) {
        Entries_.Assign(CapacityFor(groups));
        Arena_.Visit([&](const TRow& row) {
            uint64_t mixed = TRowEntries::Mix(row.Hash);
            uint64_t off = Entries_.Home(mixed);
            while (Entries_[off] != 0) {
                off = Entries_.Next(off);
            }
            Entries_.Set(off, TRowEntries::Salt(mixed), &row);
        });
    }

    TRowArena<TRow> Arena_;
    TRowEntries Entries_;
    TKeyArena KeyArena_;
    // Update scratch, one slot per input row.
    std::vector<uint64_t> Hashes_;
    std::vector<uint64_t> Salts_;
    std::vector<uint64_t> Offsets_;
    std::vector<size_t> Work_;
};

// Input rows per Update call.
constexpr size_t AggregateBatchRows = 2048;

template <class TTable, class TKey>
void UpdateInBatches(TTable& table, std::span<const TKey> keys, std::span<const int64_t> values) {
    for (size_t begin = 0; begin < keys.size(); begin += AggregateBatchRows) {
        size_t rows = std::min(AggregateBatchRows, keys.size() - begin);
        table.Update(keys.subspan(begin, rows), values.subspan(begin, rows));
    }
}

struct TJoinKeyHasher {
    template <CJoinKey TKey>
    size_t operator()(const TKey& key) const {
        return HashJoinKey(key);
    }
};

// The states of every group.
template <class TKey, size_t AggregateCount>
using TGroupStates = std::unordered_map<TKey, std::array<int64_t, AggregateCount>, TJoinKeyHasher>;

// Aggregate arguments in [-1000, 1000], so min and max see both signs.
std::vector<int64_t> RandomValues(size_t count, uint64_t seed) {
    std::vector<int64_t> values = RandomKeys(count, 2001, seed);
    for (int64_t& value : values) {
        value -= 1000;
    }
    return values;
}

// Sum, count, min and max of every key, computed directly.
template <class TKey, size_t AggregateCount>
TGroupStates<TKey, AggregateCount> ReferenceAggregate(std::span<const TKey> keys, std::span<const int64_t> values) {
    constexpr int64_t minIdentity = std::numeric_limits<int64_t>::max();
    constexpr int64_t maxIdentity = std::numeric_limits<int64_t>::min();
    TGroupStates<TKey, 4> all;
    for (size_t j = 0; j < keys.size(); ++j) {
        std::array<int64_t, 4>& states = all.try_emplace(keys[j], std::array<int64_t, 4>{0, 0, minIdentity, maxIdentity})
            .first->second;
        states[0] += values[j];
        states[1] += 1;
        states[2] = std::min(states[2], values[j]);
        states[3] = std::max(states[3], values[j]);
    }
    TGroupStates<TKey, AggregateCount> groups;
    for (const auto& [key, states] : all) {
        std::copy_n(states.begin(), AggregateCount, groups[key].begin());
    }
    return groups;
}

// The finalized groups of a table. A group output twice would collapse in the
// map, so the map must keep every output row.
template <class TKey, size_t AggregateCount, class TTable>
TGroupStates<TKey, AggregateCount> FinalizedGroups(const TTable& table) {
    TAggregateColumns<TKey, AggregateCount> output(table.Groups());
    table.Finalize(output);
    TGroupStates<TKey, AggregateCount> groups;
    for (size_t group = 0; group < output.Keys.size(); ++group) {
        std::array<int64_t, AggregateCount>& states = groups[output.Keys[group]];
        for (size_t a = 0; a < AggregateCount; ++a) {
            states[a] = output.States[a][group];
        }
    }
    EXPECT_EQ(groups.size(), output.Keys.size());
    return groups;
}

// Both tables, grown from 8 slots and pre-sized for the exact group count.
template <class TKey, size_t AggregateCount>
void ExpectTablesMatchReference(std::span<const TKey> keys, std::span<const int64_t> values) {
    TGroupStates<TKey, AggregateCount> expected = ReferenceAggregate<TKey, AggregateCount>(keys, values);
    for (size_t groups : {size_t{0}, expected.size()}) {
        TSwissAggregateTable<TKey, AggregateCount> swiss(groups);
        UpdateInBatches(swiss, keys, values);
        EXPECT_EQ(swiss.Groups(), expected.size());
        EXPECT_EQ((FinalizedGroups<TKey, AggregateCount>(swiss)), expected);

        TRowAggregateTable<TKey, AggregateCount> rows(groups);
        UpdateInBatches(rows, keys, values);
        EXPECT_EQ(rows.Groups(), expected.size());
        EXPECT_EQ((FinalizedGroups<TKey, AggregateCount>(rows)), expected);
    }
}

// Keys 0 to range - 1, key k with a frequency proportional to 1 / (k + 1).
std::vector<int64_t> ZipfKeys(size_t count, int64_t range, uint64_t seed) {
    std::vector<double> weights(range);
    for (int64_t key = 0; key < range; ++key) {
        weights[key] = 1.0 / static_cast<double>(key + 1);
    }
    std::discrete_distribution<int64_t> zipf(weights.begin(), weights.end());
    std::mt19937_64 random(seed);
    std::vector<int64_t> keys(count);
    for (int64_t& key : keys) {
        key = zipf(random);
    }
    return keys;
}

// The two columns split the bits of the key, so distinct keys stay distinct.
std::vector<TItemCustomerKey> ItemCustomerKeys(std::span<const int64_t> keys) {
    std::vector<TItemCustomerKey> pairs;
    pairs.reserve(keys.size());
    for (int64_t key : keys) {
        pairs.push_back({key & 0xFFFF, key >> 16});
    }
    return pairs;
}

// One table over one input: a run that warms up, then the better of two.
struct TAggregateRun {
    double UpdateNanosPerRow = std::numeric_limits<double>::infinity();
    double FinalizeNanosPerGroup = std::numeric_limits<double>::infinity();
    double BytesPerGroup = 0;
    size_t Groups = 0;
    uint64_t Checksum = 0;
};

// Independent of the group order, so the two tables can be compared.
template <class TKey, size_t AggregateCount>
uint64_t Checksum(const TAggregateColumns<TKey, AggregateCount>& output) {
    uint64_t sum = 0;
    for (size_t group = 0; group < output.Keys.size(); ++group) {
        uint64_t h = HashJoinKey(output.Keys[group]);
        for (const std::vector<int64_t>& states : output.States) {
            h = HashJoinKey(static_cast<int64_t>(h ^ static_cast<uint64_t>(states[group])));
        }
        sum += h;
    }
    return sum;
}

// Every run builds a new table, grown from 8 slots or pre-sized for the given
// groups, so allocation and growth count as update time.
template <class TTable, class TKey, size_t AggregateCount>
TAggregateRun MeasureAggregateTable(
    std::span<const TKey> keys,
    std::span<const int64_t> values,
    size_t presizedGroups)
{
    using TClock = std::chrono::steady_clock;
    auto nanosSince = [](TClock::time_point start) {
        return std::chrono::duration<double, std::nano>(TClock::now() - start).count();
    };
    TAggregateRun best;
    for (int run = 0; run < 3; ++run) {
        auto start = TClock::now();
        TTable table(presizedGroups);
        UpdateInBatches(table, keys, values);
        double update = nanosSince(start) / keys.size();
        TAggregateColumns<TKey, AggregateCount> output(table.Groups());
        start = TClock::now();
        table.Finalize(output);
        double finalize = nanosSince(start) / table.Groups();
        if (run == 0) {
            continue;
        }
        best.UpdateNanosPerRow = std::min(best.UpdateNanosPerRow, update);
        best.FinalizeNanosPerGroup = std::min(best.FinalizeNanosPerGroup, finalize);
        best.BytesPerGroup = static_cast<double>(table.MemoryBytes()) / table.Groups();
        best.Groups = table.Groups();
        best.Checksum = Checksum(output);
    }
    return best;
}

// One line of the benchmark: both tables over one input, grown and pre-sized.
template <class TKey, size_t AggregateCount>
void PrintAggregateComparison(
    std::string_view distribution,
    std::string_view keyName,
    std::span<const TKey> keys,
    std::span<const int64_t> values)
{
    using TSwiss = TSwissAggregateTable<TKey, AggregateCount>;
    using TRows = TRowAggregateTable<TKey, AggregateCount>;
    TAggregateRun swiss = MeasureAggregateTable<TSwiss, TKey, AggregateCount>(keys, values, 0);
    TAggregateRun rows = MeasureAggregateTable<TRows, TKey, AggregateCount>(keys, values, 0);
    // The grown runs counted the groups that the pre-sized tables are sized for.
    TAggregateRun swissPresized = MeasureAggregateTable<TSwiss, TKey, AggregateCount>(keys, values, swiss.Groups);
    TAggregateRun rowsPresized = MeasureAggregateTable<TRows, TKey, AggregateCount>(keys, values, swiss.Groups);
    EXPECT_EQ(rows.Groups, swiss.Groups);
    EXPECT_EQ(rows.Checksum, swiss.Checksum);
    EXPECT_EQ(swissPresized.Checksum, swiss.Checksum);
    EXPECT_EQ(rowsPresized.Checksum, swiss.Checksum);
    std::cout << std::format(
        "{:>9} {:>9} {:>4} {:>4} {:>2} {:>7.2f} {:>7.2f} {:>5.2f} {:>7.2f} {:>7.2f} {:>5.2f}"
        " {:>7.2f} {:>7.2f} {:>5.2f} {:>6.1f} {:>6.1f}\n",
        keys.size(),
        swiss.Groups,
        distribution,
        keyName,
        AggregateCount,
        swiss.UpdateNanosPerRow,
        rows.UpdateNanosPerRow,
        swiss.UpdateNanosPerRow / rows.UpdateNanosPerRow,
        swissPresized.UpdateNanosPerRow,
        rowsPresized.UpdateNanosPerRow,
        swissPresized.UpdateNanosPerRow / rowsPresized.UpdateNanosPerRow,
        swiss.FinalizeNanosPerGroup,
        rows.FinalizeNanosPerGroup,
        swiss.FinalizeNanosPerGroup / rows.FinalizeNanosPerGroup,
        swiss.BytesPerGroup,
        rows.BytesPerGroup);
}

} // namespace

TEST(AggregateTablePrototype, BothTablesMatchReferenceOnRandomKeys) {
    std::vector<int64_t> keys = RandomKeys(100000, 20000, 1);
    std::vector<int64_t> values = RandomValues(keys.size(), 2);
    ExpectTablesMatchReference<int64_t, 4>(keys, values);
    ExpectTablesMatchReference<TItemCustomerKey, 4>(ItemCustomerKeys(keys), values);
}

TEST(AggregateTablePrototype, BothTablesMatchReferenceOnDistinctKeys) {
    // Every row starts a group.
    std::vector<int64_t> keys(50000);
    std::iota(keys.begin(), keys.end(), -25000);
    std::shuffle(keys.begin(), keys.end(), std::mt19937_64(3));
    ExpectTablesMatchReference<int64_t, 1>(keys, RandomValues(keys.size(), 4));
}

TEST(AggregateTablePrototype, BothTablesMatchReferenceOnHotKeys) {
    // Nineteen rows in twenty fall on three keys.
    std::vector<int64_t> keys = RandomKeys(100000, 20000, 5);
    for (int64_t& key : keys) {
        if (key % 20 != 0) {
            key %= 3;
        }
    }
    ExpectTablesMatchReference<int64_t, 3>(keys, RandomValues(keys.size(), 6));
}

TEST(AggregateTablePrototype, BothTablesMergeInBatchDuplicatesOfNewKeys) {
    // Every batch brings 64 keys no earlier batch had, each in 32 of its rows.
    std::vector<int64_t> keys;
    std::mt19937_64 random(7);
    for (int64_t batch = 0; batch < 16; ++batch) {
        size_t begin = keys.size();
        for (size_t row = 0; row < AggregateBatchRows; ++row) {
            keys.push_back(batch * 64 + static_cast<int64_t>(row % 64));
        }
        std::shuffle(keys.begin() + begin, keys.end(), random);
    }
    ExpectTablesMatchReference<int64_t, 4>(keys, RandomValues(keys.size(), 8));
    // The smallest case: one new key twice in a batch.
    std::vector<int64_t> twice = {42, 42};
    std::vector<int64_t> values = {5, -3};
    ExpectTablesMatchReference<int64_t, 4>(twice, values);
}

TEST(AggregateTablePrototype, BothTablesMatchReferenceUnderSaltCollisions) {
    std::vector<TCollidingKey> keys;
    for (int64_t key : RandomKeys(5000, 1000, 9)) {
        keys.push_back({key});
    }
    ExpectTablesMatchReference<TCollidingKey, 2>(keys, RandomValues(keys.size(), 10));
}

TEST(AggregateTablePrototype, BothTablesGrowAcrossSeveralDoublings) {
    // 100000 keys, each in two rows, grow both tables from 8 slots: the swiss
    // table to 131072 at 7/8 load, the row table to 262144 at 1/2.
    std::vector<int64_t> distinct(100000);
    std::iota(distinct.begin(), distinct.end(), 0);
    std::shuffle(distinct.begin(), distinct.end(), std::mt19937_64(11));
    std::vector<int64_t> keys = distinct;
    keys.insert(keys.end(), distinct.rbegin(), distinct.rend());
    std::vector<int64_t> values = RandomValues(keys.size(), 12);
    std::span<const int64_t> keySpan(keys);
    std::span<const int64_t> valueSpan(values);

    TSwissAggregateTable<int64_t, 4> swiss;
    UpdateInBatches(swiss, keySpan, valueSpan);
    EXPECT_EQ(swiss.Capacity(), 131072u);

    // The rows of the first batch keep their addresses while the row table
    // grows from 4096 entries: growth rewrites only the entries.
    TRowAggregateTable<int64_t, 4> rows;
    rows.Update(keySpan.first(AggregateBatchRows), valueSpan.first(AggregateBatchRows));
    EXPECT_EQ(rows.Capacity(), 4096u);
    std::vector<const TRowAggregateTable<int64_t, 4>::TRow*> firstRows;
    for (int64_t key : keySpan.first(AggregateBatchRows)) {
        firstRows.push_back(rows.Find(key));
        ASSERT_NE(firstRows.back(), nullptr);
    }
    UpdateInBatches(rows, keySpan.subspan(AggregateBatchRows), valueSpan.subspan(AggregateBatchRows));
    EXPECT_EQ(rows.Capacity(), 262144u);
    for (size_t row = 0; row < AggregateBatchRows; ++row) {
        EXPECT_EQ(rows.Find(keys[row]), firstRows[row]);
    }
    EXPECT_EQ(rows.Find(-1), nullptr);

    TGroupStates<int64_t, 4> expected = ReferenceAggregate<int64_t, 4>(keySpan, valueSpan);
    EXPECT_EQ((FinalizedGroups<int64_t, 4>(swiss)), expected);
    EXPECT_EQ((FinalizedGroups<int64_t, 4>(rows)), expected);
}

// Update cost per input row and finalize cost per group of both aggregate
// tables on one thread, and their memory per group, for 1K to 24M groups of
// uniform keys and one Zipf-skewed input; 1 and 4 aggregates; an int64 and a
// two-column key; tables grown from 8 slots or pre-sized for the exact group
// count (#342). Each configuration runs once to warm up and twice measured,
// and the better run counts. Not run by ctest; measure a Release build:
// test_join --gtest_also_run_disabled_tests --gtest_filter='*AggregateMicrobenchmark*'
TEST(AggregateTablePrototype, DISABLED_AggregateMicrobenchmark) {
#ifndef NDEBUG
    GTEST_SKIP() << "the aggregate microbenchmark measures a Release build";
#endif
    struct TInput {
        int64_t Keys;
        bool Zipf;
    };
    std::cout << std::format(
        "{:>9} {:>9} {:>4} {:>4} {:>2} {:>7} {:>7} {:>5} {:>7} {:>7} {:>5}"
        " {:>7} {:>7} {:>5} {:>6} {:>6}\n",
        "rows",
        "groups",
        "dist",
        "key",
        "n",
        "sw_grow",
        "row_grow",
        "x",
        "sw_pre",
        "row_pre",
        "x",
        "sw_fin",
        "row_fin",
        "x",
        "sw_B",
        "row_B");
    std::vector<TInput> inputs = {
        {int64_t{1} << 10, false},
        {int64_t{1} << 16, false},
        {int64_t{1} << 20, false},
        {int64_t{1} << 22, false},
        {int64_t{1} << 24, false},
        {int64_t{24} << 20, false},
        {int64_t{1} << 22, true},
    };
    for (const TInput& input : inputs) {
        size_t rows = std::max<size_t>(size_t{1} << 24, 4 * input.Keys);
        uint64_t seed = static_cast<uint64_t>(input.Keys) + input.Zipf;
        std::vector<int64_t> keys = input.Zipf
            ? ZipfKeys(rows, input.Keys, seed)
            : RandomKeys(rows, input.Keys, seed);
        std::vector<TItemCustomerKey> pairs = ItemCustomerKeys(keys);
        std::vector<int64_t> values = RandomKeys(rows, 1000, seed + 1);
        std::string_view distribution = input.Zipf
            ? "zipf"
            : "unif";
        PrintAggregateComparison<int64_t, 1>(distribution, "i64", keys, values);
        PrintAggregateComparison<int64_t, 4>(distribution, "i64", keys, values);
        PrintAggregateComparison<TItemCustomerKey, 1>(distribution, "pair", pairs, values);
        PrintAggregateComparison<TItemCustomerKey, 4>(distribution, "pair", pairs, values);
    }
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
