#include <gtest/gtest.h>

#include <qdb/exec/join_exec.h>
#include <qdb/kernel/compiler.h>
#include <qdb/kernel/lib.h>
#include <qdb/plan/types/nullable.h>

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/runner/runner_llvm.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <numeric>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace {

const char* TestSortRowIdWrapperSource = R"(
(block
  (fun test_sort_row_ids
       ((var rows <ptr i64>)
        (var size i64)
        (var store <ptr TRowSet>))
    (block
      (call heap_sort rows size store))))
)";

struct TCompiledRowIdSort {
    using TSort = void (*)(int64_t*, int64_t, NQdb::TRowSet*);

    std::unique_ptr<NQumir::TLLVMRunner> Runner;
    TSort Sort = nullptr;
};

TCompiledRowIdSort
CompileRowIdSort(const std::vector<NQdb::TSortRadixKeyInput>& keys) {
    std::vector<NQumir::NAst::TExprPtr> statements;
    auto heap = NQdb::NKernel::ParseFunctionLibrary(
        NQdb::NKernel::ReadSortKernel("heap.oz"));
    if (!heap) {
        ADD_FAILURE() << heap.error().ToString();
        return {};
    }
    for (auto& statement : *heap) {
        statements.push_back(std::move(statement));
    }
    statements.push_back(NQdb::BuildSortRowIdLessAst(keys));

    auto wrapper =
        NQdb::NKernel::ParseFunctionLibrary(TestSortRowIdWrapperSource);
    if (!wrapper) {
        ADD_FAILURE() << wrapper.error().ToString();
        return {};
    }
    for (auto& statement : *wrapper) {
        statements.push_back(std::move(statement));
    }

    auto options = NQdb::KernelRunnerOptions();
    options.NativeCode = true;
    auto runner = std::make_unique<NQumir::TLLVMRunner>(options);
    auto program = std::make_shared<NQumir::NAst::TBlockExpr>(
        NQumir::TLocation{}, std::move(statements));
    std::string error;
    auto entries = NQdb::CompileKernelAst(*runner, std::move(program),
                                          {"test_sort_row_ids"}, &error);
    auto entry = entries.find("test_sort_row_ids");
    if (entry == entries.end() || !entry->second) {
        ADD_FAILURE() << error;
        return {.Runner = std::move(runner)};
    }
    return {
        .Runner = std::move(runner),
        .Sort = reinterpret_cast<TCompiledRowIdSort::TSort>(entry->second),
    };
}

NQdb::TKernelCompiler::THeapTopKDispatch
CompileHeapTopK(const std::vector<NQdb::TSortRadixKeyInput>& keys) {
    return NQdb::TKernelCompiler().CompileHeapTopK(keys);
}

std::vector<int64_t> MakeRowIds(std::array<NQdb::TRowSet, 2>& store) {
    std::vector<int64_t> rows;
    for (int32_t batch = 0; batch < static_cast<int32_t>(store.size());
         ++batch) {
        for (int32_t row = 0; row < store[batch].RowCount; ++row) {
            rows.push_back(NQdb::MakeRowId(batch, row));
        }
    }
    return rows;
}

TEST(SortRowId, SortsSeveralBatchesByMixedNumericKeys) {
    using namespace NQumir::NAst;

    auto i64 = std::make_shared<TIntegerType>();
    auto i32 = std::make_shared<TIntegerType>(TIntegerType::I32);
    auto compiled = CompileRowIdSort({
        {.ColumnIndex = 0, .Type = i64, .Desc = true},
        {.ColumnIndex = 1, .Type = i32},
    });
    ASSERT_NE(compiled.Sort, nullptr);

    std::array<int64_t, 3> primary0 = {2, 1, 2};
    std::array<int32_t, 3> secondary0 = {1, 9, 1};
    std::array<int64_t, 3> primary1 = {2, 1, 2};
    std::array<int32_t, 3> secondary1 = {0, 9, 1};
    std::array<NQdb::TColumn, 2> columns0 = {
        NQdb::TColumn{.Data = reinterpret_cast<char*>(primary0.data())},
        NQdb::TColumn{.Data = reinterpret_cast<char*>(secondary0.data())},
    };
    std::array<NQdb::TColumn, 2> columns1 = {
        NQdb::TColumn{.Data = reinterpret_cast<char*>(primary1.data())},
        NQdb::TColumn{.Data = reinterpret_cast<char*>(secondary1.data())},
    };
    std::array<NQdb::TRowSet, 2> store = {
        NQdb::TRowSet{
            .Columns = columns0.data(),
            .ColumnCount = static_cast<int64_t>(columns0.size()),
            .RowCount = static_cast<int64_t>(primary0.size()),
        },
        NQdb::TRowSet{
            .Columns = columns1.data(),
            .ColumnCount = static_cast<int64_t>(columns1.size()),
            .RowCount = static_cast<int64_t>(primary1.size()),
        },
    };

    auto rows = MakeRowIds(store);
    compiled.Sort(rows.data(), static_cast<int64_t>(rows.size()),
                  store.data());

    EXPECT_EQ(rows, (std::vector<int64_t>{
                                NQdb::MakeRowId(1, 0),
                                NQdb::MakeRowId(0, 0),
                                NQdb::MakeRowId(0, 2),
                                NQdb::MakeRowId(1, 2),
                                NQdb::MakeRowId(0, 1),
                                NQdb::MakeRowId(1, 1),
                            }));
}

// Special values in two batches; ties (+-0) are decided by the second key.
template<typename TFloat>
std::vector<int64_t> SortFloatKeyRows(bool desc, int64_t topLimit) {
    NQdb::TThreadExecutionContext context;
    using namespace NQumir::NAst;
    const TFloat inf = std::numeric_limits<TFloat>::infinity();
    const TFloat nan = std::numeric_limits<TFloat>::quiet_NaN();
    auto type = std::make_shared<TFloatType>(
        sizeof(TFloat) == 4 ? TFloatType::F32 : TFloatType::F64);
    auto i32 = std::make_shared<TIntegerType>(TIntegerType::I32);
    std::vector<NQdb::TSortRadixKeyInput> keys = {
        {.ColumnIndex = 0, .Type = type, .Desc = desc},
        {.ColumnIndex = 1, .Type = i32},
    };

    std::array<TFloat, 4> primary0 = {nan, TFloat(-0.0), TFloat(0.1), -inf};
    std::array<int32_t, 4> secondary0 = {0, 3, 0, 0};
    std::array<TFloat, 4> primary1 = {TFloat(0.0), inf, TFloat(-1.5), nan};
    std::array<int32_t, 4> secondary1 = {1, 0, 0, 1};
    std::array<NQdb::TColumn, 2> columns0 = {
        NQdb::TColumn{.Data = reinterpret_cast<char*>(primary0.data())},
        NQdb::TColumn{.Data = reinterpret_cast<char*>(secondary0.data())},
    };
    std::array<NQdb::TColumn, 2> columns1 = {
        NQdb::TColumn{.Data = reinterpret_cast<char*>(primary1.data())},
        NQdb::TColumn{.Data = reinterpret_cast<char*>(secondary1.data())},
    };
    std::array<NQdb::TRowSet, 2> store = {
        NQdb::TRowSet{.Columns = columns0.data(), .ColumnCount = 2, .RowCount = 4},
        NQdb::TRowSet{.Columns = columns1.data(), .ColumnCount = 2, .RowCount = 4},
    };
    auto rows = MakeRowIds(store);
    if (topLimit == 0) {
        auto compiled = CompileRowIdSort(keys);
        EXPECT_NE(compiled.Sort, nullptr);
        if (!compiled.Sort) {
            return {};
        }
        compiled.Sort(rows.data(), static_cast<int64_t>(rows.size()), store.data());
        return rows;
    }
    auto select = CompileHeapTopK(keys);
    EXPECT_TRUE(select);
    const int64_t kept = select(context, store.data(), rows.data(), static_cast<int64_t>(rows.size()), topLimit);
    rows.resize(static_cast<size_t>(kept));
    return rows;
}

TEST(SortRowId, F32KeyOrdersLikeF64) {
    for (bool desc : {false, true}) {
        auto f32Rows = SortFloatKeyRows<float>(desc, 0);
        EXPECT_EQ(f32Rows.size(), 8u);
        EXPECT_EQ(f32Rows, SortFloatKeyRows<double>(desc, 0));
    }
}

TEST(HeapTopK, F32KeyOrdersLikeF64) {
    for (bool desc : {false, true}) {
        auto f32Rows = SortFloatKeyRows<float>(desc, 5);
        EXPECT_EQ(f32Rows.size(), 5u);
        EXPECT_EQ(f32Rows, SortFloatKeyRows<double>(desc, 5));
    }
}

struct TStringColumn {
    std::string Data;
    std::vector<int32_t> Offsets;
    std::vector<uint8_t> Mask;
    NQdb::TColumn Column{};

    TStringColumn(const std::vector<std::string>& values,
                  std::vector<uint8_t> mask)
        : Offsets(values.size() + 1, 0), Mask(std::move(mask)) {
        for (size_t i = 0; i < values.size(); ++i) {
            Data += values[i];
            Offsets[i + 1] = static_cast<int32_t>(Data.size());
        }
        Column = {
            .Data = Data.data(),
            .Mask = Mask.data(),
            .Offsets = Offsets.data(),
            .OffsetWidth = 4,
        };
    }
};

std::vector<int64_t> MakeSingleBatchRowIds(const NQdb::TRowSet& batch) {
    std::vector<int64_t> rows;
    for (int32_t row = 0; row < batch.RowCount; ++row) {
        rows.push_back(NQdb::MakeRowId(0, row));
    }
    return rows;
}

TEST(SortRowId, SortsNullableStringsWithSqlNullOrder) {
    using namespace NQumir::NAst;

    TStringColumn strings({"12345678beta", "unused", "12345678alpha",
                           "12345678beta", "unused", "12345678alpha"},
                          {0b00101101});
    NQdb::TRowSet batch = {
        .Columns = &strings.Column,
        .ColumnCount = 1,
        .RowCount = 6,
    };
    auto stringType =
        std::make_shared<NQdb::TNullable>(std::make_shared<TStringType>());

    auto nullsFirst = CompileRowIdSort({{
        .ColumnIndex = 0,
        .Type = stringType,
        .NullsFirst = true,
    }});
    ASSERT_NE(nullsFirst.Sort, nullptr);
    auto rows = MakeSingleBatchRowIds(batch);
    nullsFirst.Sort(rows.data(), static_cast<int64_t>(rows.size()), &batch);
    EXPECT_EQ(rows, (std::vector<int64_t>{
                                NQdb::MakeRowId(0, 1),
                                NQdb::MakeRowId(0, 4),
                                NQdb::MakeRowId(0, 2),
                                NQdb::MakeRowId(0, 5),
                                NQdb::MakeRowId(0, 0),
                                NQdb::MakeRowId(0, 3),
                            }));

    auto descNullsLast = CompileRowIdSort({{
        .ColumnIndex = 0,
        .Type = stringType,
        .Desc = true,
        .NullsFirst = false,
    }});
    ASSERT_NE(descNullsLast.Sort, nullptr);
    rows = MakeSingleBatchRowIds(batch);
    descNullsLast.Sort(rows.data(), static_cast<int64_t>(rows.size()), &batch);
    EXPECT_EQ(rows, (std::vector<int64_t>{
                                NQdb::MakeRowId(0, 0),
                                NQdb::MakeRowId(0, 3),
                                NQdb::MakeRowId(0, 2),
                                NQdb::MakeRowId(0, 5),
                                NQdb::MakeRowId(0, 1),
                                NQdb::MakeRowId(0, 4),
                            }));
}

TEST(HeapTopK, SelectsSortedPrefixFromRandomRows) {
    NQdb::TThreadExecutionContext context;
    using namespace NQumir::NAst;

    auto i64 = std::make_shared<TIntegerType>();
    auto i32 = std::make_shared<TIntegerType>(TIntegerType::I32);
    auto select = CompileHeapTopK({
        {.ColumnIndex = 0, .Type = i64, .Desc = true},
        {.ColumnIndex = 1, .Type = i32},
    });
    ASSERT_TRUE(select);

    constexpr int32_t batchRows = 2048;
    constexpr int32_t rowCount = batchRows * 2;
    std::mt19937 random(42);
    std::uniform_int_distribution<int64_t> primaryDistribution(-20, 20);
    std::uniform_int_distribution<int32_t> secondaryDistribution(-10, 10);
    std::vector<int64_t> primary(rowCount);
    std::vector<int32_t> secondary(rowCount);
    for (int32_t row = 0; row < rowCount; ++row) {
        primary[row] = primaryDistribution(random);
        secondary[row] = secondaryDistribution(random);
    }

    std::array<NQdb::TColumn, 2> columns0 = {
        NQdb::TColumn{.Data = reinterpret_cast<char*>(primary.data())},
        NQdb::TColumn{.Data = reinterpret_cast<char*>(secondary.data())},
    };
    std::array<NQdb::TColumn, 2> columns1 = {
        NQdb::TColumn{.Data = reinterpret_cast<char*>(
            primary.data() + batchRows)},
        NQdb::TColumn{.Data = reinterpret_cast<char*>(
            secondary.data() + batchRows)},
    };
    std::array<NQdb::TRowSet, 2> store = {
        NQdb::TRowSet{
            .Columns = columns0.data(),
            .ColumnCount = static_cast<int64_t>(columns0.size()),
            .RowCount = batchRows,
        },
        NQdb::TRowSet{
            .Columns = columns1.data(),
            .ColumnCount = static_cast<int64_t>(columns1.size()),
            .RowCount = batchRows,
        },
    };

    auto input = MakeRowIds(store);
    std::shuffle(input.begin(), input.end(), random);
    auto expected = input;
    std::sort(expected.begin(), expected.end(), [&](int64_t left, int64_t right) {
        const int32_t leftRow =
            NQdb::BatchIndex(left) * batchRows + NQdb::RowIndex(left);
        const int32_t rightRow =
            NQdb::BatchIndex(right) * batchRows + NQdb::RowIndex(right);
        if (primary[leftRow] != primary[rightRow]) {
            return primary[leftRow] > primary[rightRow];
        }
        if (secondary[leftRow] != secondary[rightRow]) {
            return secondary[leftRow] < secondary[rightRow];
        }
        return left < right;
    });

    const std::array<int64_t, 6> limits = {
        0, 1, 7, 37, rowCount, rowCount + 100,
    };
    for (int64_t limit : limits) {
        auto rows = input;
        const int64_t kept = select(
            context,
            store.data(), rows.data(), static_cast<int64_t>(rows.size()), limit);
        const int64_t expectedCount = std::min<int64_t>(rowCount, limit);
        EXPECT_EQ(kept, expectedCount);
        EXPECT_TRUE(std::equal(
            rows.begin(), rows.begin() + kept, expected.begin()));
    }

    EXPECT_EQ(select(context, nullptr, nullptr, 0, 10), 0);
}

TEST(HeapTopK, AppliesNullableStringOrder) {
    NQdb::TThreadExecutionContext context;
    using namespace NQumir::NAst;

    TStringColumn strings({"12345678beta", "unused", "12345678alpha",
                           "12345678beta", "unused", "12345678alpha"},
                          {0b00101101});
    NQdb::TRowSet batch = {
        .Columns = &strings.Column,
        .ColumnCount = 1,
        .RowCount = 6,
    };
    auto stringType =
        std::make_shared<NQdb::TNullable>(std::make_shared<TStringType>());
    auto select = CompileHeapTopK({{
        .ColumnIndex = 0,
        .Type = stringType,
        .NullsFirst = true,
    }});
    ASSERT_TRUE(select);

    auto rows = MakeSingleBatchRowIds(batch);
    std::reverse(rows.begin(), rows.end());
    ASSERT_EQ(select(
        context,
        &batch, rows.data(), static_cast<int64_t>(rows.size()), 4), 4);
    const std::vector<int64_t> expected = {
        NQdb::MakeRowId(0, 1),
        NQdb::MakeRowId(0, 4),
        NQdb::MakeRowId(0, 2),
        NQdb::MakeRowId(0, 5),
    };
    EXPECT_TRUE(std::equal(
        rows.begin(), rows.begin() + 4, expected.begin()));
}

} // namespace

int main(int argc, char** argv) {
    NQumir::NCodeGen::TLLVMInitializer initializer;
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
