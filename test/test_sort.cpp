#include <gtest/gtest.h>

#include <stdint.h>

#include <qdb/kernel/lib.h>

#include <qumir/runner/runner_llvm.h>

#include <algorithm>
#include <bit>
#include <limits>
#include <numeric>
#include <random>
#include <type_traits>
#include <vector>

namespace {

/*
123
^
 ^
  ^
example: 123, 120, 341, 145

sort by 0
0 -> 1
1 -> 1 -> 2
3 -> 1 -> 3
5 -> 1 -> 4

*/

template<typename T>
requires (std::is_integral_v<T> && !std::is_same_v<T, bool>)
auto RadixKey(T x) {
    using U = std::make_unsigned_t<T>;
    U u = std::bit_cast<U>(x);

    if constexpr (std::is_signed_v<T>) {
        u ^= (U(1) << (sizeof(T) * 8 - 1));
    }

    return u;
}

uint32_t RadixKey(float x) {
    uint32_t u = std::bit_cast<uint32_t>(x);
    uint32_t sign = u >> 31;
    uint32_t mask = (-sign) | 0x80000000U;
    return u ^ mask;
}

uint64_t RadixKey(double x) {
    uint64_t u = std::bit_cast<uint64_t>(x);
    uint64_t sign = u >> 63;
    uint64_t mask = (-sign) | 0x8000000000000000ULL;
    return u ^ mask;
}

template<typename T>
void CountSortIndices(T* dest, uint32_t* indices, uint32_t* work, int n, int digit)
{
    // base-16, 8 bits
    uint32_t counts[256] = {0};
    // count digits
    for (int i = 0; i < n; i++) {
        counts[(RadixKey(dest[indices[i]]) >> digit) & 0xffU]++;
    }
    // counts -> places
    for (int i = 1; i < 256; i++) {
        counts[i] += counts[i - 1];
    }

    for (int i = n-1; i >= 0; --i) {
        auto index = (RadixKey(dest[indices[i]]) >> digit) & 0xffU;
        auto place = counts[index]-1;
        work[place] = indices[i];
        counts[index]--;
    }

    for (int i = 0; i < n; i++) {
        indices[i] = work[i];
    }
}

struct TStringKey {
    uint64_t Prefix;
    uint32_t RowIndex;
};

uint64_t RadixKey(const TStringKey& key) {
    return key.Prefix;
}

void CountSort(TStringKey* dest, TStringKey* work, int n, int digit)
{
    // base-16, 8 bits
    uint32_t counts[256] = {0};
    // count digits
    for (int i = 0; i < n; i++) {
        counts[(RadixKey(dest[i]) >> digit) & 0xffU]++;
    }
    // counts -> places
    for (int i = 1; i < 256; i++) {
        counts[i] += counts[i - 1];
    }

    for (int i = n-1; i >= 0; --i) {
        auto index = (RadixKey(dest[i]) >> digit) & 0xffU;
        auto place = counts[index]-1;
        work[place] = dest[i];
        counts[index]--;
    }

    for (int i = 0; i < n; i++) {
        dest[i] = work[i];
    }
}

uint64_t MakePrefix(std::string_view s) {
    uint64_t x = 0;
    const size_t n = std::min<size_t>(8, s.size());

    for (size_t i = 0; i < n; ++i) {
        x = (x << 8) | static_cast<unsigned char>(s[i]);
    }

    x <<= 8 * (8 - n); // padding zeroes
    return x;
}

void StableInsertionSort(TStringKey* keys, int n, std::vector<std::string_view>& strings) {
    for (int i = 1; i < n; ++i) {
        TStringKey key = keys[i];
        std::string_view str = strings[key.RowIndex];
        int j = i - 1;
        while (j >= 0 && strings[keys[j].RowIndex] > str) {
            keys[j + 1] = keys[j];
            --j;
        }
        keys[j + 1] = key;
    }
}

void MergeSort(TStringKey* keys, TStringKey* work, int n, std::vector<std::string_view>& strings) {
    static constexpr size_t SmallSortThreshold = 32;

    if (n <= 1) {
        return;
    }

    if (n <= SmallSortThreshold) {
        StableInsertionSort(keys, n, strings);
        return;
    }

    const size_t mid = n / 2;
    MergeSort(keys, work, mid, strings);
    MergeSort(keys + mid, work, n - mid, strings);

    size_t i = 0, j = mid, k = 0;
    while (i < mid && j < n) {
        if (strings[keys[i].RowIndex] <= strings[keys[j].RowIndex]) {
            work[k++] = keys[i++];
        } else {
            work[k++] = keys[j++];
        }
    }

    while (i < mid) {
        work[k++] = keys[i++];
    }

    while (j < n) {
        work[k++] = keys[j++];
    }

    for (size_t i = 0; i < n; ++i) {
        keys[i] = work[i];
    }
}

void SortStrings(std::vector<std::string_view>& strings, std::vector<uint32_t>& indices)
{
    int n = strings.size();
    std::vector<TStringKey> keys(n), work(n);
    for (int i = 0; i < n; i++) {
        keys[i].Prefix = MakePrefix(strings[i]);
        keys[i].RowIndex = i;
    }
    for (int i = 0; i < sizeof(uint64_t) * 8; i += 8) {
        CountSort(keys.data(), work.data(), n, i);
    }

    int i, j = 0;
    auto sort_range = [&](int start, int end) {
        MergeSort(keys.data() + start, work.data() + start, end - start, strings);
    };
    for (i = 0; i < n; i++) {
        if (keys[i].Prefix != keys[j].Prefix) {
            if (i - j > 1) {
                sort_range(j, i);
            }
            j = i;
        }
    }
    if (i - j > 1) {
        sort_range(j, i);
    }
    for (int i = 0; i < n; i++) {
        indices[i] = keys[i].RowIndex;
    }
}

template<typename T>
void RadixSortIndices(T* dest, uint32_t* indices, uint32_t* work, int n) {
    for (int i = 0; i < sizeof(T) * 8; i += 8) {
        CountSortIndices(dest, indices, work, n, i);
    }
}

void CountSort(uint32_t* dest, uint32_t* work, int n, int digit)
{
    // base-16, 8 bits
    uint32_t counts[256] = {0};
    // count digits
    for (int i = 0; i < n; i++) {
        counts[(dest[i] >> digit) & 0xffU]++;
    }
    // counts -> places
    for (int i = 1; i < 256; i++) {
        counts[i] += counts[i - 1];
    }

    for (int i = n-1; i >= 0; --i) {
        auto index = (dest[i] >> digit) & 0xffU;
        auto place = counts[index]-1;
        work[place] = dest[i];
        counts[index]--;
    }

    for (int i = 0; i < n; i++) {
        dest[i] = work[i];
    }
}

void RadixSort(uint32_t* dest, uint32_t* work, int n) {
    for (int i = 0; i < sizeof(uint32_t) * 8; i += 8) {
        CountSort(dest, work, n, i);
    }
}

int RandomInt(int* seed) {
    *seed = (*seed * 1103515245 + 12345) & 0x7fffffff;
    return *seed;
}

void GenerateColumn(uint32_t* column, int n, int* seed) {
    for (int i = 0; i < n; i++) {
        column[i] = RandomInt(seed) % 15;
    }
}

const char* TestRadixWrapperSource = R"(
(block
  (fun test_radix_sort_indices_u8
       ((var values <ptr u8>)
        (var indices <ptr u32>)
        (var work <ptr u32>)
        (var counts <ptr u32>)
        (var n i64)
        (var desc bool))
    (block
      (call radix_sort_indices values indices work counts n (: 8 i64) desc)))

  (fun test_radix_sort_indices_i8
       ((var values <ptr i8>)
        (var indices <ptr u32>)
        (var work <ptr u32>)
        (var counts <ptr u32>)
        (var n i64)
        (var desc bool))
    (block
      (call radix_sort_indices values indices work counts n (: 8 i64) desc)))

  (fun test_radix_sort_indices_u16
       ((var values <ptr u16>)
        (var indices <ptr u32>)
        (var work <ptr u32>)
        (var counts <ptr u32>)
        (var n i64)
        (var desc bool))
    (block
      (call radix_sort_indices values indices work counts n (: 16 i64) desc)))

  (fun test_radix_sort_indices_i16
       ((var values <ptr i16>)
        (var indices <ptr u32>)
        (var work <ptr u32>)
        (var counts <ptr u32>)
        (var n i64)
        (var desc bool))
    (block
      (call radix_sort_indices values indices work counts n (: 16 i64) desc)))

  (fun test_radix_sort_indices_u32
       ((var values <ptr u32>)
        (var indices <ptr u32>)
        (var work <ptr u32>)
        (var counts <ptr u32>)
        (var n i64)
        (var desc bool))
    (block
      (call radix_sort_indices values indices work counts n (: 32 i64) desc)))

  (fun test_radix_sort_indices_i32
       ((var values <ptr i32>)
        (var indices <ptr u32>)
        (var work <ptr u32>)
        (var counts <ptr u32>)
        (var n i64)
        (var desc bool))
    (block
      (call radix_sort_indices values indices work counts n (: 32 i64) desc)))

  (fun test_radix_key_i32 ((var value i32)) -> u64
    (block
      (return (call qumir_radix_key value))))

  (fun test_radix_key_f64 ((var value f64)) -> u64
    (block
      (return (call qumir_radix_key value))))

  (fun test_radix_key_f32 ((var value f32)) -> u64
    (block
      (return (call qumir_radix_key value)))))
)";

std::unique_ptr<NQumir::TLLVMRunner> CompileRadixOperation(
    const std::string& entryName,
    void*& entry)
{
    std::vector<NQumir::NAst::TExprPtr> programStmts;
    auto addLibrary = [&](const std::string& name, bool skipUse) -> bool {
        auto library = NQdb::NKernel::ParseFunctionLibrary(
            NQdb::NKernel::ReadSortKernel(name));
        if (!library) {
            ADD_FAILURE() << library.error().ToString();
            return false;
        }
        for (auto& stmt : *library) {
            if (skipUse && NQumir::NAst::TMaybeNode<NQumir::NAst::TUseExpr>(stmt)) {
                continue;
            }
            programStmts.push_back(std::move(stmt));
        }
        return true;
    };
    if (!addLibrary("radix.oz", false)) {
        return {};
    }
    auto wrappers = NQdb::NKernel::ParseFunctionLibrary(TestRadixWrapperSource);
    if (!wrappers) {
        ADD_FAILURE() << wrappers.error().ToString();
        return {};
    }
    for (auto& stmt : *wrappers) {
        programStmts.push_back(std::move(stmt));
    }

    NQumir::TLLVMRunnerOptions options;
    options.CoreInput = true;
    options.NativeCode = true;
    options.AllowOverloads = true;
    auto runner = std::make_unique<NQumir::TLLVMRunner>(options);
    auto program = std::make_shared<NQumir::NAst::TBlockExpr>(
        NQumir::TLocation{}, std::move(programStmts));
    std::string error;
    entry = runner->CompileKernelAst(program, entryName, &error);
    EXPECT_NE(entry, nullptr) << error;
    return runner;
}

std::unique_ptr<NQumir::TLLVMRunner> CompileTopSortOperation(
    const std::string& entryName,
    const std::string& entrySource,
    void*& entry)
{
    std::vector<NQumir::NAst::TExprPtr> programStmts;
    auto library = NQdb::NKernel::ParseFunctionLibrary(
        NQdb::NKernel::ReadSortKernel("top_sort.oz"));
    if (!library) {
        ADD_FAILURE() << library.error().ToString();
        return {};
    }
    for (auto& stmt : *library) {
        programStmts.push_back(std::move(stmt));
    }

    auto wrapper = NQdb::NKernel::ParseFunctionLibrary(entrySource);
    if (!wrapper) {
        ADD_FAILURE() << wrapper.error().ToString();
        return {};
    }
    for (auto& stmt : *wrapper) {
        programStmts.push_back(std::move(stmt));
    }

    NQumir::TLLVMRunnerOptions options;
    options.CoreInput = true;
    options.NativeCode = true;
    options.AllowOverloads = true;
    auto runner = std::make_unique<NQumir::TLLVMRunner>(options);
    auto program = std::make_shared<NQumir::NAst::TBlockExpr>(
        NQumir::TLocation{}, std::move(programStmts));
    std::string error;
    entry = runner->CompileKernelAst(program, entryName, &error);
    EXPECT_NE(entry, nullptr) << error;
    return runner;
}

template<typename T>
std::vector<uint32_t> StableSortedIndices(const std::vector<T>& values) {
    std::vector<uint32_t> expected(values.size());
    std::iota(expected.begin(), expected.end(), 0);
    std::stable_sort(expected.begin(), expected.end(), [&](uint32_t lhs, uint32_t rhs) {
        return values[lhs] < values[rhs];
    });
    return expected;
}

struct Pick {
    uint8_t src; // 0 = state, 1 = temp
    uint32_t idx;
};

template<typename T>
struct TTopSortScratch {
    std::vector<T> State;
    std::vector<T> NextState;
    std::vector<uint32_t> TempIndices;
    std::vector<uint32_t> Work;
    std::vector<Pick> Picks;

    void Prepare(size_t tempSize, size_t limit) {
        if (TempIndices.size() != tempSize) {
            TempIndices.resize(tempSize);
        }
        if (Work.size() != tempSize) {
            Work.resize(tempSize);
        }
        if (Picks.size() != limit) {
            Picks.resize(limit);
        }
        if (NextState.size() != limit) {
            NextState.resize(limit);
        }
    }
};

template<typename T>
size_t MergeTopSortPicks(const T* state, size_t stateSize,
    const T* temp, const uint32_t* tempIndices, size_t tempSize,
    Pick* picks, size_t limit)
{
    size_t left = 0;
    size_t right = 0;
    size_t out = 0;
    while (out < limit && (left < stateSize || right < tempSize)) {
        if (right == tempSize) {
            picks[out++] = Pick{0, static_cast<uint32_t>(left++)};
            continue;
        }
        if (left == stateSize) {
            picks[out++] = Pick{1, tempIndices[right++]};
            continue;
        }

        const uint32_t tempIndex = tempIndices[right];
        if (temp[tempIndex] < state[left]) {
            picks[out++] = Pick{1, tempIndex};
            ++right;
        } else {
            picks[out++] = Pick{0, static_cast<uint32_t>(left++)};
        }
    }

    return out;
}

template<typename T>
void GatherTopSortState(const T* state, const T* temp, const Pick* picks,
    size_t pickCount, T* nextState)
{
    for (size_t i = 0; i < pickCount; ++i) {
        const Pick& pick = picks[i];
        nextState[i] = pick.src == 0 ? state[pick.idx] : temp[pick.idx];
    }
}

struct TTopSortColumns {
    std::vector<std::vector<int32_t>> StateColumns;
    std::vector<std::vector<int32_t>> NextStateColumns;
    std::vector<std::vector<int32_t>> TempColumns;
};

void PrepareNextStateColumns(TTopSortColumns& columns, size_t columnCount, size_t limit) {
    if (columns.NextStateColumns.size() != columnCount) {
        columns.NextStateColumns.resize(columnCount);
    }
    for (auto& column : columns.NextStateColumns) {
        if (column.size() != limit) {
            column.resize(limit);
        }
    }
}

} // namespace

TEST(SortTest, Basic) {
    std::vector<uint32_t> ar = {2345, 123498, 123, 1, 2, 555, 10};
    std::vector<uint32_t> work; work.resize(ar.size());
    RadixSort(ar.data(), work.data(), ar.size());
    EXPECT_TRUE(std::is_sorted(ar.begin(), ar.end()));
}

TEST(SortTest, BasicIndices) {
    std::vector<uint32_t> ar = {2345, 123498, 123, 1, 2, 555, 10};
    std::vector<uint32_t> work; work.resize(ar.size());
    std::vector<uint32_t> indices; indices.resize(ar.size());
    std::iota(indices.begin(), indices.end(), 0);
    RadixSortIndices(ar.data(), indices.data(), work.data(), ar.size());
    EXPECT_EQ(indices, StableSortedIndices(ar));
}

TEST(SortTest, BasicIndicesSigned) {
    std::vector<int32_t> ar = {-2345, 123498, -123, 1, -2, 555, 10};
    std::vector<uint32_t> work; work.resize(ar.size());
    std::vector<uint32_t> indices; indices.resize(ar.size());
    std::iota(indices.begin(), indices.end(), 0);
    RadixSortIndices(ar.data(), indices.data(), work.data(), ar.size());
    EXPECT_EQ(indices, StableSortedIndices(ar));
}

TEST(SortTest, BasicIndicesDouble) {
    std::vector<double> ar = {-2345.6, 123498.1, -123.5, 1.2, -2.5, 555.99, 10.12344};
    std::vector<uint32_t> work; work.resize(ar.size());
    std::vector<uint32_t> indices; indices.resize(ar.size());
    std::iota(indices.begin(), indices.end(), 0);
    RadixSortIndices(ar.data(), indices.data(), work.data(), ar.size());
    EXPECT_EQ(indices, StableSortedIndices(ar));
}

TEST(SortTest, RowSet) {
    int n = 16;
    int seed = 42;
    std::vector<uint32_t> col1(n);
    std::vector<uint32_t> col2(n);
    std::vector<uint32_t> col3(n);

    std::vector<uint32_t> work; work.resize(n);
    std::vector<uint32_t> indices; indices.resize(n);
    std::iota(indices.begin(), indices.end(), 0);

    GenerateColumn(col1.data(), n, &seed);
    GenerateColumn(col2.data(), n, &seed);
    GenerateColumn(col3.data(), n, &seed);

    RadixSortIndices(col3.data(), indices.data(), work.data(), n);
    RadixSortIndices(col2.data(), indices.data(), work.data(), n);
    RadixSortIndices(col1.data(), indices.data(), work.data(), n);

    std::vector<uint32_t> expected(n);
    std::iota(expected.begin(), expected.end(), 0);
    std::stable_sort(expected.begin(), expected.end(), [&](uint32_t lhs, uint32_t rhs) {
        return std::tuple(col1[lhs], col2[lhs], col3[lhs])
            < std::tuple(col1[rhs], col2[rhs], col3[rhs]);
    });
    EXPECT_EQ(indices, expected);
}

TEST(SortTest, TopSortMergesSortedStateAndRadixSortedBatch) {
    constexpr size_t limit = 5;
    TTopSortScratch<int32_t> scratch;
    scratch.State = {1, 4, 8, 10, 15};
    std::vector<int32_t> temp = {6, 3, 12, 0, 4, 9};
    scratch.Prepare(temp.size(), limit);
    std::iota(scratch.TempIndices.begin(), scratch.TempIndices.end(), 0);

    RadixSortIndices(temp.data(), scratch.TempIndices.data(), scratch.Work.data(), temp.size());
    const size_t pickCount = MergeTopSortPicks(
        scratch.State.data(), scratch.State.size(),
        temp.data(), scratch.TempIndices.data(), scratch.TempIndices.size(),
        scratch.Picks.data(), limit);
    GatherTopSortState(
        scratch.State.data(), temp.data(), scratch.Picks.data(), pickCount,
        scratch.NextState.data());

    ASSERT_EQ(pickCount, limit);
    EXPECT_EQ(scratch.NextState, (std::vector<int32_t>{0, 1, 3, 4, 4}));
    EXPECT_EQ(scratch.Picks[0].src, 1);
    EXPECT_EQ(scratch.Picks[0].idx, 3u);
    EXPECT_EQ(scratch.Picks[1].src, 0);
    EXPECT_EQ(scratch.Picks[1].idx, 0u);
    EXPECT_EQ(scratch.Picks[3].src, 0);
    EXPECT_EQ(scratch.Picks[3].idx, 1u);
    EXPECT_EQ(scratch.Picks[4].src, 1);
    EXPECT_EQ(scratch.Picks[4].idx, 4u);
}

TEST(SortTest, TopSortGathersMultipleColumnsByPickSelector) {
    constexpr size_t limit = 4;
    TTopSortScratch<int32_t> scratch;
    TTopSortColumns columns{
        .StateColumns = {
            {1, 4, 8, 10},
            {10, 40, 80, 100},
            {-1, -4, -8, -10},
        },
        .TempColumns = {
            {6, 3, 12, 0, 4},
            {60, 30, 120, 0, 41},
            {-6, -3, -12, 0, -41},
        },
    };
    scratch.State = columns.StateColumns[0];
    scratch.Prepare(columns.TempColumns[0].size(), limit);
    PrepareNextStateColumns(columns, columns.StateColumns.size(), limit);
    std::iota(scratch.TempIndices.begin(), scratch.TempIndices.end(), 0);

    RadixSortIndices(
        columns.TempColumns[0].data(),
        scratch.TempIndices.data(),
        scratch.Work.data(),
        columns.TempColumns[0].size());
    const size_t pickCount = MergeTopSortPicks(
        scratch.State.data(), scratch.State.size(),
        columns.TempColumns[0].data(), scratch.TempIndices.data(),
        scratch.TempIndices.size(), scratch.Picks.data(), limit);

    ASSERT_EQ(pickCount, limit);
    for (size_t column = 0; column < columns.StateColumns.size(); ++column) {
        GatherTopSortState(
            columns.StateColumns[column].data(),
            columns.TempColumns[column].data(),
            scratch.Picks.data(),
            pickCount,
            columns.NextStateColumns[column].data());
    }

    EXPECT_EQ(columns.NextStateColumns[0], (std::vector<int32_t>{0, 1, 3, 4}));
    EXPECT_EQ(columns.NextStateColumns[1], (std::vector<int32_t>{0, 10, 30, 40}));
    EXPECT_EQ(columns.NextStateColumns[2], (std::vector<int32_t>{0, -1, -3, -4}));
}

TEST(SortTopSortOz, MergesAndGathersMultipleColumns) {
    const std::string entrySource = R"(
(block
  (fun qdb_top_sort_merge_and_gather_i32
       ((var state_key <ptr i32>)
        (var state_col1 <ptr i32>)
        (var state_col2 <ptr i32>)
        (var state_n i64)
        (var temp_key <ptr i32>)
        (var temp_col1 <ptr i32>)
        (var temp_col2 <ptr i32>)
        (var temp_indices <ptr u32>)
        (var temp_n i64)
        (var pick_src <ptr u8>)
        (var pick_idx <ptr u32>)
        (var out_key <ptr i32>)
        (var out_col1 <ptr i32>)
        (var out_col2 <ptr i32>)
        (var limit i64)
        (var desc bool)) -> i64
    (block
      (var n = (call top_sort_merge_picks
        state_key state_n temp_key temp_indices temp_n pick_src pick_idx limit desc))
      (call top_sort_gather_column state_key temp_key pick_src pick_idx out_key n)
      (call top_sort_gather_column state_col1 temp_col1 pick_src pick_idx out_col1 n)
      (call top_sort_gather_column state_col2 temp_col2 pick_src pick_idx out_col2 n)
      (return n))))
)";
    void* entry = nullptr;
    auto runner = CompileTopSortOperation(
        "qdb_top_sort_merge_and_gather_i32", entrySource, entry);
    ASSERT_NE(entry, nullptr);
    auto topSort = reinterpret_cast<int64_t(*)(
        int32_t*, int32_t*, int32_t*, int64_t,
        int32_t*, int32_t*, int32_t*, uint32_t*, int64_t,
        uint8_t*, uint32_t*, int32_t*, int32_t*, int32_t*, int64_t, bool)>(entry);

    constexpr int64_t limit = 4;
    std::vector<int32_t> stateKey = {1, 4, 8, 10};
    std::vector<int32_t> stateCol1 = {10, 40, 80, 100};
    std::vector<int32_t> stateCol2 = {-1, -4, -8, -10};
    std::vector<int32_t> tempKey = {6, 3, 12, 0, 4};
    std::vector<int32_t> tempCol1 = {60, 30, 120, 0, 41};
    std::vector<int32_t> tempCol2 = {-6, -3, -12, 0, -41};
    std::vector<uint32_t> tempIndices(tempKey.size());
    std::vector<uint32_t> work(tempKey.size());
    std::vector<uint8_t> pickSrc(limit);
    std::vector<uint32_t> pickIdx(limit);
    std::vector<int32_t> outKey(limit);
    std::vector<int32_t> outCol1(limit);
    std::vector<int32_t> outCol2(limit);
    std::iota(tempIndices.begin(), tempIndices.end(), 0);
    RadixSortIndices(tempKey.data(), tempIndices.data(), work.data(), tempKey.size());

    const int64_t n = topSort(
        stateKey.data(), stateCol1.data(), stateCol2.data(), stateKey.size(),
        tempKey.data(), tempCol1.data(), tempCol2.data(), tempIndices.data(), tempKey.size(),
        pickSrc.data(), pickIdx.data(), outKey.data(), outCol1.data(), outCol2.data(),
        limit, false);

    ASSERT_EQ(n, limit);
    EXPECT_EQ(outKey, (std::vector<int32_t>{0, 1, 3, 4}));
    EXPECT_EQ(outCol1, (std::vector<int32_t>{0, 10, 30, 40}));
    EXPECT_EQ(outCol2, (std::vector<int32_t>{0, -1, -3, -4}));
    EXPECT_EQ(pickSrc, (std::vector<uint8_t>{1, 0, 1, 0}));
    EXPECT_EQ(pickIdx, (std::vector<uint32_t>{3, 0, 1, 1}));
}

TEST(SortRadixOz, NumericKeysMatchPrototype) {
    void* i32Entry = nullptr;
    auto i32Runner = CompileRadixOperation("test_radix_key_i32", i32Entry);
    ASSERT_NE(i32Entry, nullptr);
    auto i32Key = reinterpret_cast<uint64_t(*)(int32_t)>(i32Entry);
    EXPECT_EQ(i32Key(std::numeric_limits<int32_t>::min()), RadixKey(std::numeric_limits<int32_t>::min()));
    EXPECT_EQ(i32Key(-1), RadixKey(-1));
    EXPECT_EQ(i32Key(0), RadixKey(0));
    EXPECT_EQ(i32Key(42), RadixKey(42));
    EXPECT_EQ(i32Key(std::numeric_limits<int32_t>::max()), RadixKey(std::numeric_limits<int32_t>::max()));

    void* f64Entry = nullptr;
    auto f64Runner = CompileRadixOperation("test_radix_key_f64", f64Entry);
    ASSERT_NE(f64Entry, nullptr);
    auto f64Key = reinterpret_cast<uint64_t(*)(double)>(f64Entry);
    for (double value : {-2345.6, -0.0, 0.0, 1.2, 555.99}) {
        EXPECT_EQ(f64Key(value), RadixKey(value));
    }
}

TEST(SortRadixOz, F32KeyMatchesPrototypeAndKeepsFloatOrder) {
    void* entry = nullptr;
    auto runner = CompileRadixOperation("test_radix_key_f32", entry);
    ASSERT_NE(entry, nullptr);
    auto f32Key = reinterpret_cast<uint64_t(*)(float)>(entry);
    // Same bit-pattern order as the f64 key: -0.0 sorts before 0.0, NaN after +inf.
    const float inf = std::numeric_limits<float>::infinity();
    const std::vector<float> ascending = {
        -inf, -3.4e38f, -2345.6f, -1.0f, -1e-45f, -0.0f, 0.0f, 1e-45f, 1.2f, 555.99f,
        3.4e38f, inf, std::numeric_limits<float>::quiet_NaN()};
    for (size_t i = 0; i < ascending.size(); ++i) {
        EXPECT_EQ(f32Key(ascending[i]), RadixKey(ascending[i])) << i;
        EXPECT_LE(f32Key(ascending[i]), 0xffffffffULL) << i;
        if (i > 0) {
            EXPECT_LT(f32Key(ascending[i - 1]), f32Key(ascending[i])) << i;
        }
    }
    // A NaN with the sign bit set sorts before -inf, like f64.
    const float negNan = std::bit_cast<float>(0xffc00000U);
    EXPECT_EQ(f32Key(negNan), RadixKey(negNan));
    EXPECT_LT(f32Key(negNan), f32Key(-inf));
}

TEST(SortRadixOz, SortsU32IndicesAscending) {
    void* entry = nullptr;
    auto runner = CompileRadixOperation("test_radix_sort_indices_u32", entry);
    ASSERT_NE(entry, nullptr);
    auto sort = reinterpret_cast<void(*)(uint32_t*, uint32_t*, uint32_t*, uint32_t*, int64_t, bool)>(entry);

    std::vector<uint32_t> values = {7, 3, 7, 1, 9, 3, 0, 7};
    std::vector<uint32_t> indices(values.size());
    std::vector<uint32_t> work(values.size());
    std::vector<uint32_t> counts(256);
    std::iota(indices.begin(), indices.end(), 0);

    sort(values.data(), indices.data(), work.data(), counts.data(), values.size(), false);
    EXPECT_EQ(indices, StableSortedIndices(values));
}

TEST(SortRadixOz, SortsU8IndicesAscending) {
    void* entry = nullptr;
    auto runner = CompileRadixOperation("test_radix_sort_indices_u8", entry);
    ASSERT_NE(entry, nullptr);
    auto sort = reinterpret_cast<void(*)(uint8_t*, uint32_t*, uint32_t*, uint32_t*, int64_t, bool)>(entry);

    std::vector<uint8_t> values = {7, 3, 7, 1, 255, 3, 0, 7};
    std::vector<uint32_t> indices(values.size());
    std::vector<uint32_t> work(values.size());
    std::vector<uint32_t> counts(256);
    std::iota(indices.begin(), indices.end(), 0);

    sort(values.data(), indices.data(), work.data(), counts.data(), values.size(), false);
    EXPECT_EQ(indices, StableSortedIndices(values));
}

TEST(SortRadixOz, SortsI8IndicesAscending) {
    void* entry = nullptr;
    auto runner = CompileRadixOperation("test_radix_sort_indices_i8", entry);
    ASSERT_NE(entry, nullptr);
    auto sort = reinterpret_cast<void(*)(int8_t*, uint32_t*, uint32_t*, uint32_t*, int64_t, bool)>(entry);

    std::vector<int8_t> values = {-7, 3, -7, 1, 127, 3, 0, std::numeric_limits<int8_t>::min()};
    std::vector<uint32_t> indices(values.size());
    std::vector<uint32_t> work(values.size());
    std::vector<uint32_t> counts(256);
    std::iota(indices.begin(), indices.end(), 0);

    sort(values.data(), indices.data(), work.data(), counts.data(), values.size(), false);
    EXPECT_EQ(indices, StableSortedIndices(values));
}

TEST(SortRadixOz, SortsU16IndicesAscending) {
    void* entry = nullptr;
    auto runner = CompileRadixOperation("test_radix_sort_indices_u16", entry);
    ASSERT_NE(entry, nullptr);
    auto sort = reinterpret_cast<void(*)(uint16_t*, uint32_t*, uint32_t*, uint32_t*, int64_t, bool)>(entry);

    std::vector<uint16_t> values = {700, 3, 700, 1, 65535, 3, 0, 7};
    std::vector<uint32_t> indices(values.size());
    std::vector<uint32_t> work(values.size());
    std::vector<uint32_t> counts(256);
    std::iota(indices.begin(), indices.end(), 0);

    sort(values.data(), indices.data(), work.data(), counts.data(), values.size(), false);
    EXPECT_EQ(indices, StableSortedIndices(values));
}

TEST(SortRadixOz, SortsI16IndicesAscending) {
    void* entry = nullptr;
    auto runner = CompileRadixOperation("test_radix_sort_indices_i16", entry);
    ASSERT_NE(entry, nullptr);
    auto sort = reinterpret_cast<void(*)(int16_t*, uint32_t*, uint32_t*, uint32_t*, int64_t, bool)>(entry);

    std::vector<int16_t> values = {-700, 3, -700, 1, 32767, 3, 0, std::numeric_limits<int16_t>::min()};
    std::vector<uint32_t> indices(values.size());
    std::vector<uint32_t> work(values.size());
    std::vector<uint32_t> counts(256);
    std::iota(indices.begin(), indices.end(), 0);

    sort(values.data(), indices.data(), work.data(), counts.data(), values.size(), false);
    EXPECT_EQ(indices, StableSortedIndices(values));
}

TEST(SortRadixOz, SortsU32IndicesDescendingStably) {
    void* entry = nullptr;
    auto runner = CompileRadixOperation("test_radix_sort_indices_u32", entry);
    ASSERT_NE(entry, nullptr);
    auto sort = reinterpret_cast<void(*)(uint32_t*, uint32_t*, uint32_t*, uint32_t*, int64_t, bool)>(entry);

    std::vector<uint32_t> values = {7, 3, 7, 1, 9, 3, 0, 7};
    std::vector<uint32_t> indices(values.size());
    std::vector<uint32_t> work(values.size());
    std::vector<uint32_t> counts(256);
    std::iota(indices.begin(), indices.end(), 0);

    sort(values.data(), indices.data(), work.data(), counts.data(), values.size(), true);

    std::vector<uint32_t> expected(values.size());
    std::iota(expected.begin(), expected.end(), 0);
    std::stable_sort(expected.begin(), expected.end(), [&](uint32_t lhs, uint32_t rhs) {
        return values[lhs] > values[rhs];
    });
    EXPECT_EQ(indices, expected);
}

TEST(SortRadixOz, GenericSortUsesI32RadixKeyOverload) {
    void* entry = nullptr;
    auto runner = CompileRadixOperation("test_radix_sort_indices_i32", entry);
    ASSERT_NE(entry, nullptr);
    auto sort = reinterpret_cast<void(*)(int32_t*, uint32_t*, uint32_t*, uint32_t*, int64_t, bool)>(entry);

    std::vector<int32_t> values = {-7, 3, -7, 1, 9, 3, 0, std::numeric_limits<int32_t>::min()};
    std::vector<uint32_t> indices(values.size());
    std::vector<uint32_t> work(values.size());
    std::vector<uint32_t> counts(256);
    std::iota(indices.begin(), indices.end(), 0);

    sort(values.data(), indices.data(), work.data(), counts.data(), values.size(), false);
    EXPECT_EQ(indices, StableSortedIndices(values));
}

TEST(SortStrings, Basic) {
    std::vector<std::string_view> strings = {
        "banana", "apple", "cherry", "apple", "12345678y", "date", "fig", "grape", "elderberry", "12345678x",
    };
    std::vector<uint32_t> indices(strings.size());
    std::iota(indices.begin(), indices.end(), 0);

    SortStrings(strings, indices);

    std::vector<std::string_view> sortedStrings(strings.size());
    for (size_t i = 0; i < indices.size(); ++i) {
        sortedStrings[i] = strings[indices[i]];

        std::cerr << "Index: " << indices[i] << ", String: " << sortedStrings[i] << std::endl;
    }

    // EXPECT_TRUE(std::is_sorted(sortedStrings.begin(), sortedStrings.end()));
}

std::unique_ptr<NQumir::TLLVMRunner> CompileSortStringsOperation(
    const std::string& entryName,
    void*& entry)
{
    std::vector<NQumir::NAst::TExprPtr> programStmts;
    auto library = NQdb::NKernel::ParseFunctionLibrary(
        NQdb::NKernel::ReadSortKernel("sort_strings.oz"));
    if (!library) {
        ADD_FAILURE() << library.error().ToString();
        return {};
    }
    for (auto& stmt : *library) {
        programStmts.push_back(std::move(stmt));
    }

    NQumir::TLLVMRunnerOptions options;
    options.CoreInput = true;
    options.NativeCode = true;
    options.AllowOverloads = true;
    auto runner = std::make_unique<NQumir::TLLVMRunner>(options);
    auto program = std::make_shared<NQumir::NAst::TBlockExpr>(
        NQumir::TLocation{}, std::move(programStmts));
    std::string error;
    entry = runner->CompileKernelAst(program, entryName, &error);
    EXPECT_NE(entry, nullptr) << error;
    return runner;
}

// String column in kernel terms: concatenated bytes + (n+1) offsets + the
// 3-word { data, offsets, offset_width } descriptor the entries take.
struct TTestStringColumn {
    std::string Data;
    std::vector<int64_t> Offsets;
    std::array<int64_t, 3> View;

    explicit TTestStringColumn(
        const std::vector<std::string>& strings, int64_t offsetBase = 0)
    {
        Offsets.push_back(offsetBase);
        for (const auto& value : strings) {
            Data += value;
            Offsets.push_back(offsetBase + static_cast<int64_t>(Data.size()));
        }
        View = {
            reinterpret_cast<int64_t>(Data.data()),
            reinterpret_cast<int64_t>(Offsets.data()),
            8,
        };
    }
};

using TSortStringsFn =
    void(*)(int64_t*, uint32_t*, uint32_t*, uint32_t*, int64_t, bool);
using TSortStringsNullableFn =
    void(*)(int64_t*, uint8_t*, uint32_t*, uint32_t*, uint32_t*, int64_t, bool, bool);

std::vector<uint32_t> RunSortStringsKernel(
    const std::vector<std::string>& strings, bool desc, int64_t offsetBase = 0)
{
    void* entry = nullptr;
    auto runner = CompileSortStringsOperation("sort_strings_indices", entry);
    if (!entry) {
        return {};
    }
    TTestStringColumn column(strings, offsetBase);
    const int64_t n = static_cast<int64_t>(strings.size());
    std::vector<uint32_t> indices(n);
    std::iota(indices.begin(), indices.end(), 0);
    // Pair scratch: two arrays of 16-byte {prefix, row} pairs.
    std::vector<uint32_t> work(8 * n);
    std::vector<uint32_t> counts(257);
    reinterpret_cast<TSortStringsFn>(entry)(
        column.View.data(), indices.data(), work.data(), counts.data(), n, desc);
    return indices;
}

std::vector<uint32_t> StableSortedStringIndices(
    const std::vector<std::string>& strings, bool desc)
{
    std::vector<uint32_t> expected(strings.size());
    std::iota(expected.begin(), expected.end(), 0);
    std::stable_sort(expected.begin(), expected.end(), [&](uint32_t lhs, uint32_t rhs) {
        return desc ? strings[rhs] < strings[lhs] : strings[lhs] < strings[rhs];
    });
    return expected;
}

TEST(SortStringsOz, MatchesStableSortWithPrefixCollisions) {
    // Long shared prefixes force the merge tie-break past the 8-byte radix.
    const std::vector<std::string> strings = {
        "banana", "apple", "cherry", "apple", "12345678y", "date", "fig",
        "grape", "elderberry", "12345678x", "12345678", "1234567",
        "same-prefix-aaaa", "same-prefix-aaab", "same-prefix-aa",
        "", "z", "same-prefix-aaaa",
    };
    EXPECT_EQ(RunSortStringsKernel(strings, false),
              StableSortedStringIndices(strings, false));
    EXPECT_EQ(RunSortStringsKernel(strings, true),
              StableSortedStringIndices(strings, true));
}

TEST(SortStringsOz, StableOnDuplicates) {
    const std::vector<std::string> strings = {
        "dup", "dup", "aaa", "dup", "aaa", "dup",
    };
    const auto indices = RunSortStringsKernel(strings, false);
    EXPECT_EQ(indices, StableSortedStringIndices(strings, false));
    // Explicit stability check: equal keys keep source order.
    EXPECT_EQ(indices, (std::vector<uint32_t>{2, 4, 0, 1, 3, 5}));
}

TEST(SortStringsOz, HonorsOffsetBase) {
    const std::vector<std::string> strings = {"beta", "alpha", "gamma"};
    EXPECT_EQ(RunSortStringsKernel(strings, false, /*offsetBase=*/1000),
              StableSortedStringIndices(strings, false));
}

TEST(SortStringsOz, MatchesPrototypeOnRandomStrings) {
    std::mt19937 rng(42);
    std::vector<std::string> strings;
    for (int i = 0; i < 500; ++i) {
        // Bias toward shared prefixes to stress tie-break runs.
        std::string value = (i % 3 == 0) ? "shared-prefix-longer-than-8-" : "";
        const int len = static_cast<int>(rng() % 12);
        for (int j = 0; j < len; ++j) {
            value += static_cast<char>('a' + rng() % 4);
        }
        strings.push_back(std::move(value));
    }
    EXPECT_EQ(RunSortStringsKernel(strings, false),
              StableSortedStringIndices(strings, false));
    EXPECT_EQ(RunSortStringsKernel(strings, true),
              StableSortedStringIndices(strings, true));

    // Cross-check against the C++ prototype (SortStrings above).
    std::vector<std::string_view> views(strings.begin(), strings.end());
    std::vector<uint32_t> prototype(strings.size());
    SortStrings(views, prototype);
    EXPECT_EQ(RunSortStringsKernel(strings, false), prototype);
}

TEST(SortStringsOz, NullableGroupsNullsAndSortsValid) {
    void* entry = nullptr;
    auto runner = CompileSortStringsOperation("sort_strings_indices_nullable", entry);
    ASSERT_NE(entry, nullptr);
    auto sort = reinterpret_cast<TSortStringsNullableFn>(entry);

    const std::vector<std::string> strings = {
        "banana", "", "apple", "", "cherry", "apple",
    };
    const std::vector<uint8_t> valid = {1, 0, 1, 0, 1, 1};
    TTestStringColumn column(strings);
    const int64_t n = static_cast<int64_t>(strings.size());
    std::vector<uint32_t> work(8 * n);
    std::vector<uint32_t> counts(257);

    for (bool nullsFirst : {true, false}) {
        for (bool desc : {false, true}) {
            std::vector<uint32_t> indices(n);
            std::iota(indices.begin(), indices.end(), 0);
            sort(column.View.data(), const_cast<uint8_t*>(valid.data()),
                 indices.data(), work.data(), counts.data(), n, desc, nullsFirst);

            std::vector<uint32_t> expected;
            std::vector<uint32_t> validIdx = {0, 2, 4, 5};
            std::stable_sort(validIdx.begin(), validIdx.end(), [&](uint32_t a, uint32_t b) {
                return desc ? strings[b] < strings[a] : strings[a] < strings[b];
            });
            if (nullsFirst) {
                expected = {1, 3};
                expected.insert(expected.end(), validIdx.begin(), validIdx.end());
            } else {
                expected = validIdx;
                expected.push_back(1);
                expected.push_back(3);
            }
            EXPECT_EQ(indices, expected)
                << "nullsFirst=" << nullsFirst << " desc=" << desc;
        }
    }
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
