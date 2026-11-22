#include <gtest/gtest.h>
#include "test_types.h"

#include <set>

#include <qdb/kernel/lib.h>

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/runner/runner_llvm.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <numeric>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace {

const char* TestHeapWrapperSource = R"(
(block
  (pragma language overloads)

  (type TestHeapRow <struct
    (Primary <ptr i32>)
    (Secondary <ptr i32>)
    (PrimaryDesc i64)
    (RowId i64)>)

  (fun test_heap_row_lt
       ((var left <named TestHeapRow>)
        (var right <named TestHeapRow>)) -> bool (attrs (operator "<"))
    (block
      (var primary = (field left Primary))
      (var left_row = (field left RowId))
      (var right_row = (field right RowId))
      (var left_primary = (index primary left_row))
      (var right_primary = (index primary right_row))
      (if (!= left_primary right_primary)
        (block
          (if (!= (field left PrimaryDesc) (: 0 i64))
            (block
              (return (> left_primary right_primary))))
          (return (< left_primary right_primary))))

      (var secondary = (field left Secondary))
      (var left_secondary = (index secondary left_row))
      (var right_secondary = (index secondary right_row))
      (if (!= left_secondary right_secondary)
        (block
          (return (< left_secondary right_secondary))))

      ;; The row id makes equal keys deterministic for this test adapter.
      (return (< left_row right_row))))

  (fun test_heapify_i64
       ((var items <ptr i64>)
        (var size i64))
    (block
      (call heapify items size)))

  (fun test_heap_push_i64
       ((var items <ptr i64>)
        (var size i64)
        (var item i64)) -> i64
    (block
      (return (call heap_push items size item))))

  (fun test_heap_replace_top_i64
       ((var items <ptr i64>)
        (var size i64)
        (var item i64)) -> i64
    (block
      (return (call heap_replace_top items size item))))

  (fun test_heap_pop_i64
       ((var items <ptr i64>)
        (var size i64)) -> i64
    (block
      (return (call heap_pop items size))))

  (fun test_heap_sort_i64
       ((var items <ptr i64>)
        (var size i64))
    (block
      (call heap_sort items size)))

  (fun test_heap_top_k_i64
       ((var items <ptr i64>)
        (var size i64)
        (var limit i64)) -> i64
    (block
      (return (call heap_top_k items size limit))))

  (fun test_heap_sort_columns
       ((var rows <ptr <named TestHeapRow>>)
        (var size i64))
    (block
      (call heap_sort rows size)))
))";

struct THeapRow {
    int32_t* Primary;
    int32_t* Secondary;
    int64_t PrimaryDesc;
    int64_t RowId;
};

struct THeapFunctions {
    using THeapify = void (*)(int64_t*, int64_t);
    using TPush = int64_t (*)(int64_t*, int64_t, int64_t);
    using TReplaceTop = int64_t (*)(int64_t*, int64_t, int64_t);
    using TPop = int64_t (*)(int64_t*, int64_t);
    using TSort = void (*)(int64_t*, int64_t);
    using TTopK = int64_t (*)(int64_t*, int64_t, int64_t);
    using TSortColumns = void (*)(THeapRow*, int64_t);

    std::unique_ptr<NQumir::TLLVMRunner> Runner;
    THeapify Heapify = nullptr;
    TPush Push = nullptr;
    TReplaceTop ReplaceTop = nullptr;
    TPop Pop = nullptr;
    TSort Sort = nullptr;
    TTopK TopK = nullptr;
    TSortColumns SortColumns = nullptr;
};

THeapFunctions CompileHeapFunctions() {
    std::vector<NQumir::NAst::TExprPtr> programStmts;
    auto library = NQdb::NKernel::ParseFunctionLibrary(NQdb::TestTypes(),
        NQdb::NKernel::ReadSortKernel("heap.oz"));
    if (!library) {
        ADD_FAILURE() << library.error().ToString();
        return {};
    }
    for (auto& stmt : *library) {
        programStmts.push_back(std::move(stmt));
    }

    auto wrappers = NQdb::NKernel::ParseFunctionLibrary(NQdb::TestTypes(), TestHeapWrapperSource);
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
    auto runner = std::make_unique<NQumir::TLLVMRunner>(options, nullptr, &NQdb::TestTypes());
    auto program = NQumir::MakeIntrusive<NQumir::NAst::TBlockExpr>(
        NQumir::TLocation{}, std::move(programStmts));
    std::string error;
    auto entries = runner->CompileKernelAst(program,
                                            {
                                                "test_heapify_i64",
                                                "test_heap_push_i64",
                                                "test_heap_replace_top_i64",
                                                "test_heap_pop_i64",
                                                "test_heap_sort_i64",
                                                "test_heap_top_k_i64",
                                                "test_heap_sort_columns",
                                            },
                                            &error);
    EXPECT_FALSE(entries.empty()) << error;

    THeapFunctions result;
    result.Runner = std::move(runner);
    result.Heapify =
        reinterpret_cast<THeapFunctions::THeapify>(entries["test_heapify_i64"]);
    result.Push =
        reinterpret_cast<THeapFunctions::TPush>(entries["test_heap_push_i64"]);
    result.ReplaceTop = reinterpret_cast<THeapFunctions::TReplaceTop>(
        entries["test_heap_replace_top_i64"]);
    result.Pop =
        reinterpret_cast<THeapFunctions::TPop>(entries["test_heap_pop_i64"]);
    result.Sort =
        reinterpret_cast<THeapFunctions::TSort>(entries["test_heap_sort_i64"]);
    result.TopK = reinterpret_cast<THeapFunctions::TTopK>(
        entries["test_heap_top_k_i64"]);
    result.SortColumns = reinterpret_cast<THeapFunctions::TSortColumns>(
        entries["test_heap_sort_columns"]);
    return result;
}

bool IsHeap(const std::vector<int64_t>& values, int64_t size) {
    for (int64_t child = 1; child < size; ++child) {
        const int64_t parent = (child - 1) / 2;
        if (values[parent] < values[child]) {
            return false;
        }
    }
    return true;
}

TEST(Heap, HeapifyBuildsHeap) {
    auto heap = CompileHeapFunctions();
    ASSERT_NE(heap.Heapify, nullptr);

    std::vector<int64_t> values = {4, -2, 9, 1, 9, 0, 7, -8};
    heap.Heapify(values.data(), values.size());
    EXPECT_TRUE(IsHeap(values, values.size()));
    EXPECT_EQ(values.front(), 9);
}

TEST(Heap, PushReplaceAndPopPreserveHeap) {
    auto heap = CompileHeapFunctions();
    ASSERT_NE(heap.Push, nullptr);
    ASSERT_NE(heap.ReplaceTop, nullptr);
    ASSERT_NE(heap.Pop, nullptr);

    std::vector<int64_t> values(8);
    int64_t size = 0;
    for (int64_t value : {4, -2, 9, 1, 7}) {
        size = heap.Push(values.data(), size, value);
        EXPECT_TRUE(IsHeap(values, size));
    }

    EXPECT_EQ(heap.ReplaceTop(values.data(), size, 3), size);
    EXPECT_TRUE(IsHeap(values, size));
    EXPECT_EQ(values.front(), 7);

    // An empty heap keeps nothing, and says so instead of dropping the item.
    std::vector<int64_t> untouched = {11, 22};
    EXPECT_EQ(heap.ReplaceTop(untouched.data(), 0, 99), 0);
    EXPECT_EQ(untouched, (std::vector<int64_t>{11, 22}));

    std::vector<int64_t> removed;
    while (size > 0) {
        size = heap.Pop(values.data(), size);
        removed.push_back(values[size]);
        EXPECT_TRUE(IsHeap(values, size));
    }
    EXPECT_EQ(removed, (std::vector<int64_t>{7, 4, 3, 1, -2}));
}

TEST(Heap, SortHandlesEdgeCasesAndRandomValues) {
    auto heap = CompileHeapFunctions();
    ASSERT_NE(heap.Sort, nullptr);

    std::vector<int64_t> empty;
    heap.Sort(empty.data(), 0);

    std::vector<int64_t> one = {7};
    heap.Sort(one.data(), one.size());
    EXPECT_EQ(one, (std::vector<int64_t>{7}));

    std::mt19937_64 random(42);
    std::uniform_int_distribution<int64_t> distribution(-100, 100);
    std::vector<int64_t> values(1000);
    std::generate(values.begin(), values.end(),
                  [&] { return distribution(random); });

    auto expected = values;
    std::sort(expected.begin(), expected.end());
    heap.Sort(values.data(), values.size());
    EXPECT_EQ(values, expected);
}

TEST(Heap, TopKKeepsSortedPrefix) {
    auto heap = CompileHeapFunctions();
    ASSERT_NE(heap.TopK, nullptr);

    std::vector<int64_t> values = {12, -4, 7, 3, 19, 0, 7, -8, 5};
    auto expected = values;
    std::sort(expected.begin(), expected.end());

    const int64_t kept = heap.TopK(values.data(), values.size(), 4);
    ASSERT_EQ(kept, 4);
    EXPECT_TRUE(std::equal(
        values.begin(), values.begin() + kept, expected.begin()));
}

TEST(Heap, SortsColumnarDataThroughRowIds) {
    auto heap = CompileHeapFunctions();
    ASSERT_NE(heap.SortColumns, nullptr);

    std::vector<int32_t> primary = {2, 1, 2, 1, 2, 1, 2, 1};
    std::vector<int32_t> secondary = {3, 4, 1, 4, 1, 2, 1, 2};
    std::vector<THeapRow> rows(primary.size());
    for (size_t i = 0; i < rows.size(); ++i) {
        rows[i] = {
            .Primary = primary.data(),
            .Secondary = secondary.data(),
            .PrimaryDesc = 0,
            .RowId = static_cast<int64_t>(i),
        };
    }
    heap.SortColumns(rows.data(), rows.size());

    std::vector<int64_t> expected(primary.size());
    std::iota(expected.begin(), expected.end(), 0);
    std::sort(expected.begin(), expected.end(),
              [&](int64_t left, int64_t right) {
                  if (primary[left] != primary[right]) {
                      return primary[left] < primary[right];
                  }
                  if (secondary[left] != secondary[right]) {
                      return secondary[left] < secondary[right];
                  }
                  return left < right;
              });
    std::vector<int64_t> rowIds(rows.size());
    std::transform(rows.begin(), rows.end(), rowIds.begin(),
                   [](const THeapRow& row) { return row.RowId; });
    EXPECT_EQ(rowIds, expected);

    for (size_t i = 0; i < rows.size(); ++i) {
        rows[i] = {
            .Primary = primary.data(),
            .Secondary = secondary.data(),
            .PrimaryDesc = 1,
            .RowId = static_cast<int64_t>(i),
        };
    }
    heap.SortColumns(rows.data(), rows.size());
    std::sort(expected.begin(), expected.end(),
              [&](int64_t left, int64_t right) {
                  if (primary[left] != primary[right]) {
                      return primary[left] > primary[right];
                  }
                  if (secondary[left] != secondary[right]) {
                      return secondary[left] < secondary[right];
                  }
                  return left < right;
              });
    std::transform(rows.begin(), rows.end(), rowIds.begin(),
                   [](const THeapRow& row) { return row.RowId; });
    EXPECT_EQ(rowIds, expected);
}

TEST(Heap, HandlesOddAndEvenSizes) {
    auto heap = CompileHeapFunctions();
    ASSERT_NE(heap.Heapify, nullptr);
    ASSERT_NE(heap.Push, nullptr);
    ASSERT_NE(heap.Sort, nullptr);

    // Parent and last-parent indexes need integer division. An odd size leaves
    // (size - 2) / 2 inexact, which is where a real divide goes wrong.
    for (int64_t size = 0; size <= 17; ++size) {
        std::vector<int64_t> input(static_cast<size_t>(size));
        for (int64_t i = 0; i < size; ++i) {
            input[static_cast<size_t>(i)] = size - i;
        }
        const std::multiset<int64_t> items(input.begin(), input.end());

        auto heapified = input;
        heap.Heapify(heapified.data(), size);
        EXPECT_TRUE(IsHeap(heapified, size)) << "heapify at size " << size;
        EXPECT_EQ(std::multiset<int64_t>(heapified.begin(), heapified.end()),
                  items)
            << "heapify changed the items at size " << size;

        // Pushing one by one walks the parent index in sift_up.
        std::vector<int64_t> pushed(static_cast<size_t>(size) + 1);
        int64_t pushedSize = 0;
        for (int64_t i = 0; i < size; ++i) {
            pushedSize = heap.Push(
                pushed.data(), pushedSize, input[static_cast<size_t>(i)]);
            EXPECT_TRUE(IsHeap(pushed, pushedSize)) << "push at size " << size;
        }
        EXPECT_EQ(pushedSize, size);

        auto sorted = input;
        auto expected = input;
        std::sort(expected.begin(), expected.end());
        heap.Sort(sorted.data(), size);
        EXPECT_EQ(sorted, expected) << "sort at size " << size;
    }
}

} // namespace

int main(int argc, char** argv) {
    NQumir::NCodeGen::TLLVMInitializer initializer;
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
