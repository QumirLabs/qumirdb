#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <iostream>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <string_view>
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

// The table of one side of the symmetric join: a swiss table of the distinct
// keys, and for each key a bucket of the row ids stored under it. It is the
// aggregation HashTable with three dense int64 columns (join_table.oz): bucket
// length, capacity and pointer. The rows stay in their retained batches.
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
        return Ctrl_.size();
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
    // Groups of 8 slots, the NEON and portable width; SSE2 uses 16.
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

    // The physical slot of the key, or -1 and the first empty slot of its
    // probe sequence. Groups are walked with triangular steps, which visit
    // every group once (rh_lookup_or_empty_physical).
    int64_t FindSlot(const TKey& key, uint64_t hash, int64_t* emptySlot) const {
        size_t groupMask = (Capacity() >> GroupShift) - 1;
        size_t group = (hash >> 7) & groupMask;
        for (size_t step = 1; step <= groupMask + 1; ++step) {
            uint64_t word = Group(group);
            for (uint64_t m = Match(word, hash & 127); m != 0; m &= m - 1) {
                size_t slot = (group << GroupShift) + std::countr_zero(m) / 8;
                if (Keys_[slot] == key) {
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

    int64_t AddKey(const TKey& key, uint64_t hash, int64_t emptySlot) {
        // At 7/8 load the table doubles; the saved empty slot is then stale.
        if (Size_ + 1 > Capacity() - Capacity() / 8) {
            Grow();
            FindSlot(key, hash, &emptySlot);
        }
        int64_t id = static_cast<int64_t>(Size_++);
        TKey stored = CloneJoinKey(key, Arena_);
        Ctrl_[emptySlot] = static_cast<uint8_t>(hash & 127);
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
            Buckets_[id] = static_cast<TRowId*>(std::realloc(Buckets_[id], capacity * sizeof(TRowId)));
            Capacities_[id] = capacity;
        }
        Buckets_[id][Counts_[id]++] = row;
    }

    void Allocate(size_t capacity) {
        Ctrl_.assign(capacity, Empty);
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
        std::vector<uint8_t> ctrl = std::move(Ctrl_);
        std::vector<TKey> keys = std::move(Keys_);
        std::vector<int64_t> slotId = std::move(SlotId_);
        Allocate(ctrl.size() * 2);
        for (size_t slot = 0; slot < ctrl.size(); ++slot) {
            if (ctrl[slot] == Empty) {
                continue;
            }
            uint64_t hash = HashJoinKey(keys[slot]);
            int64_t emptySlot = -1;
            FindSlot(keys[slot], hash, &emptySlot);
            Ctrl_[emptySlot] = static_cast<uint8_t>(hash & 127);
            Keys_[emptySlot] = keys[slot];
            SlotId_[emptySlot] = slotId[slot];
        }
    }

    // Per physical slot: Empty or the low 7 bits of the hash.
    std::vector<uint8_t> Ctrl_;
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
    // every chain in build order.
    void Build() {
        size_t capacity = 1024;
        while (capacity < 2 * Arena_.Rows()) {
            capacity *= 2;
        }
        Entries_.assign(capacity, 0);
        Mask_ = capacity - 1;
        Shift_ = 64 - std::countr_zero(capacity);
        Keys_ = 0;
        Arena_.VisitBackward([&](TRow& row) {
            uint64_t mixed = Mix(row.HashOrNext);
            uint64_t salt = Salt(mixed);
            uint64_t off = mixed >> Shift_;
            for (;;) {
                uint64_t entry = Seek(off, salt);
                TRow* head = EntryRow(entry);
                if (entry == 0 || head->Key == row.Key) {
                    Link(off, salt, row, head);
                    return;
                }
                off = (off + 1) & Mask_;
            }
        });
    }

    // Probes a batch of keys in three passes and appends one match per
    // matching build row. Each pass loops over independent rows, so the cache
    // misses of different rows overlap.
    void Probe(std::span<const TKey> keys, std::vector<TMatch>& matches) {
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
            uint64_t mixed = Mix(Hashes_[j]);
            Salts_[j] = Salt(mixed);
            Offsets_[j] = mixed >> Shift_;
            Found_[j] = Seek(Offsets_[j], Salts_[j]);
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
                const TRow* row = EntryRow(Found_[j]);
                if (row == nullptr) {
                    continue;
                }
                if (row->Key == keys[j]) {
                    for (; row != nullptr; row = Next(row)) {
                        matches.push_back({j, row});
                    }
                } else {
                    Offsets_[j] = (Offsets_[j] + 1) & Mask_;
                    Work_[collided++] = j;
                }
            }
            pending = collided;
            for (size_t q = 0; q < pending; ++q) {
                size_t j = Work_[q];
                Found_[j] = Seek(Offsets_[j], Salts_[j]);
            }
        }
    }

    size_t Keys() const {
        return Keys_;
    }

    size_t Capacity() const {
        return Entries_.size();
    }

    size_t MemoryBytes() const {
        return Entries_.size() * sizeof(uint64_t) + Arena_.MemoryBytes();
    }

private:
    static constexpr uint64_t PointerMask = (uint64_t{1} << 48) - 1;

    // The index is the top bits of the hash: the hash shuffle routes rows by
    // the low bits, which are thus nearly constant inside a partition. A key
    // hash can carry little entropy in its top bits, so a Fibonacci multiply
    // first spreads every bit over them (jrt_mix).
    static uint64_t Mix(uint64_t hash) {
        return hash * 0x9E3779B97F4A7C15;
    }

    static uint64_t Salt(uint64_t mixed) {
        return (mixed >> 16) & 0xFFFF;
    }

    static TRow* EntryRow(uint64_t entry) {
        return reinterpret_cast<TRow*>(entry & PointerMask);
    }

    static const TRow* Next(const TRow* row) {
        return reinterpret_cast<const TRow*>(row->HashOrNext);
    }

    // jrt_seek: from off to the first entry that is empty (0) or carries the
    // salt; off is left at that entry.
    uint64_t Seek(uint64_t& off, uint64_t salt) const {
        uint64_t entry = Entries_[off];
        while (entry != 0 && (entry >> 48) != salt) {
            off = (off + 1) & Mask_;
            entry = Entries_[off];
        }
        return entry;
    }

    // jrt_link: as in the kernel and in DuckDB, a user-space pointer fits in
    // 48 bits.
    void Link(uint64_t off, uint64_t salt, TRow& row, TRow* head) {
        row.HashOrNext = reinterpret_cast<uintptr_t>(head);
        if (head == nullptr) {
            ++Keys_;
        }
        Entries_[off] = salt << 48 | reinterpret_cast<uintptr_t>(&row);
    }

    TRowArena<TRow> Arena_;
    std::vector<uint64_t> Entries_;
    uint64_t Mask_ = 0;
    int Shift_ = 64;
    size_t Keys_ = 0;
    // Probe scratch, one slot per probe row (JoinScratch).
    std::vector<uint64_t> Hashes_;
    std::vector<uint64_t> Salts_;
    std::vector<uint64_t> Offsets_;
    std::vector<uint64_t> Found_;
    std::vector<size_t> Work_;
};

// A key whose hash takes one of two values: every row then shares its salt
// with half of the others, which exercises the collision walk of the row
// join table and the group walk of the swiss table.
struct TCollidingKey {
    int64_t Value;

    bool operator==(const TCollidingKey&) const = default;
};

uint64_t HashJoinKey(const TCollidingKey& key) {
    return static_cast<uint64_t>(key.Value & 1);
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
    std::vector<std::pair<size_t, int64_t>> expected;
    for (size_t j = 0; j < probe.size(); ++j) {
        for (TRowId row : swiss.Find(probe[j], HashJoinKey(probe[j]))) {
            expected.emplace_back(j, row);
        }
    }
    // A row that met a salt collision emits its matches in a later round, so
    // the row table orders pairs by round, not by probe row.
    std::ranges::stable_sort(actual, {}, &std::pair<size_t, int64_t>::first);
    EXPECT_FALSE(expected.empty());
    EXPECT_EQ(actual, expected);
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
                    "{:>10} {:>4} {:>5} {:>12.2f} {:>12.2f} {:>12.2f} {:>12.2f} {:>8.2f} {:>10.1f} {:>10.1f}\n",
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

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
