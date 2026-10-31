#include <gtest/gtest.h>
#include "mock_source.h"
#include "plan_runner.h"

#include <qdb/plan/ops/operator.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/ops/window.h>
#include <qdb/plan/passes/column_pruning.h>
#include <qdb/plan/passes/typing.h>
#include <qdb/plan/pipeline.h>
#include <qdb/plan/types/decimal.h>
#include <qdb/plan/types/nullable.h>
#include <qdb/sexp/parser.h>
#include <qdb/modules/qumirdb_runtime.h>

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>

#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace NQdb;

namespace {

TOperatorPtr ParsePlan(const std::string& sexp, ISource& source) {
    NSexp::TRelParserOptions opts;
    opts.SourceFactory = [&](std::string_view path, NQumir::TLocation) -> TOperatorPtr {
        return std::make_shared<TSourceOperator>(source, std::string(path));
    };
    opts.CteRegistry =
        std::make_shared<std::unordered_map<uint32_t, TCteDefinitionPtr>>();

    NQumir::NAst::NCore::TParser parser;
    for (auto& [name, fn] : NSexp::MakeRelParsers(std::move(opts))) {
        parser.NodeParsers[name] = std::move(fn);
    }

    std::istringstream in(sexp);
    NQumir::NAst::NCore::TTokenStream ts(in);
    auto result = parser.Parse(ts);
    if (!result.has_value()) {
        throw std::runtime_error(result.error().ToString());
    }

    auto root = std::static_pointer_cast<IOperator>(result.value());
    AnnotateTypes(root);
    ApplyColumnPruning(root);
    return root;
}

bool IsValid(const TColumn& column, int64_t row) {
    if (!column.Mask) {
        return true;
    }
    const int64_t bit = column.MaskBitOffset + row;
    return ((column.Mask[bit / 8] >> (bit % 8)) & 1) != 0;
}

} // namespace

TEST(WindowExec, PrefixSumI64ResetsPerPartition) {
    std::array<int64_t, 6> keys = {2, 1, 1, 2, 1, 2};
    std::array<int64_t, 6> order = {1, 2, 1, 3, 3, 2};
    std::array<int64_t, 6> values = {20, 7, 5, 4, 11, 6};

    std::vector<TColumn> columns = {
        TColumn{.Data = reinterpret_cast<char*>(keys.data())},
        TColumn{.Data = reinterpret_cast<char*>(order.data())},
        TColumn{.Data = reinterpret_cast<char*>(values.data())},
    };
    std::vector<TRowSet> batches = {TRowSet{
        .Columns = columns.data(),
        .ColumnCount = 3,
        .RowCount = static_cast<int64_t>(keys.size()),
        .Selection = nullptr,
        .RefCount = 1,
    }};
    TMockSource source({"k", "o", "v"}, std::move(batches));

    auto root = ParsePlan(
        "(rel window (rel source \"data.parquet\") "
        "(partition k) "
        "(order (o asc nulls-default)) "
        "(frame rows (start unbounded-preceding) (end current-row)) "
        "(fn running_sum sum v))",
        source);

    auto runtime = RunPlan(root);

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.ColumnCount, 4);
    ASSERT_EQ(result.RowCount, 6);

    auto* outKeys = reinterpret_cast<int64_t*>(result.Columns[0].Data);
    auto* outOrder = reinterpret_cast<int64_t*>(result.Columns[1].Data);
    auto* outValues = reinterpret_cast<int64_t*>(result.Columns[2].Data);
    auto* outSums = reinterpret_cast<int64_t*>(result.Columns[3].Data);

    const std::array<int64_t, 6> expectedKeys = {1, 1, 1, 2, 2, 2};
    const std::array<int64_t, 6> expectedOrder = {1, 2, 3, 1, 2, 3};
    const std::array<int64_t, 6> expectedValues = {5, 7, 11, 20, 6, 4};
    const std::array<int64_t, 6> expectedSums = {5, 12, 23, 20, 26, 30};

    for (int64_t row = 0; row < result.RowCount; ++row) {
        EXPECT_EQ(outKeys[row], expectedKeys[row]) << "row " << row;
        EXPECT_EQ(outOrder[row], expectedOrder[row]) << "row " << row;
        EXPECT_EQ(outValues[row], expectedValues[row]) << "row " << row;
        EXPECT_EQ(outSums[row], expectedSums[row]) << "row " << row;
    }

    Release(&result);

    TRowSet second{};
    EXPECT_FALSE(runtime->Next(second));
}

TEST(WindowExec, PrefixSumI32OutputsI64) {
    std::array<int64_t, 6> keys = {2, 1, 1, 2, 1, 2};
    std::array<int64_t, 6> order = {1, 2, 1, 3, 3, 2};
    std::array<int32_t, 6> values = {20, 7, 5, 4, 11, 6};

    std::vector<TColumn> columns = {
        TColumn{.Data = reinterpret_cast<char*>(keys.data())},
        TColumn{.Data = reinterpret_cast<char*>(order.data())},
        TColumn{.Data = reinterpret_cast<char*>(values.data())},
    };
    std::vector<TRowSet> batches = {TRowSet{
        .Columns = columns.data(),
        .ColumnCount = 3,
        .RowCount = static_cast<int64_t>(keys.size()),
        .Selection = nullptr,
        .RefCount = 1,
    }};
    TMockSource source(
        {"k", "o", "v"},
        std::move(batches),
        {std::make_shared<NQumir::NAst::TIntegerType>(),
         std::make_shared<NQumir::NAst::TIntegerType>(),
         std::make_shared<NQumir::NAst::TIntegerType>(
             NQumir::NAst::TIntegerType::I32)});

    auto root = ParsePlan(
        "(rel window (rel source \"data.parquet\") "
        "(partition k) "
        "(order (o asc nulls-default)) "
        "(frame rows (start unbounded-preceding) (end current-row)) "
        "(fn running_sum sum v))",
        source);

    auto runtime = RunPlan(root);

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.ColumnCount, 4);
    ASSERT_EQ(result.RowCount, 6);

    auto* outValues = reinterpret_cast<int32_t*>(result.Columns[2].Data);
    auto* outSums = reinterpret_cast<int64_t*>(result.Columns[3].Data);

    const std::array<int32_t, 6> expectedValues = {5, 7, 11, 20, 6, 4};
    const std::array<int64_t, 6> expectedSums = {5, 12, 23, 20, 26, 30};

    for (int64_t row = 0; row < result.RowCount; ++row) {
        EXPECT_EQ(outValues[row], expectedValues[row]) << "row " << row;
        EXPECT_EQ(outSums[row], expectedSums[row]) << "row " << row;
    }

    Release(&result);

    TRowSet second{};
    EXPECT_FALSE(runtime->Next(second));
}

TEST(WindowExec, RankI64WithPeersResetsPerPartition) {
    std::array<int64_t, 7> keys = {2, 1, 1, 2, 1, 2, 1};
    std::array<int64_t, 7> order = {5, 10, 10, 7, 20, 5, 30};
    std::array<int64_t, 7> payload = {100, 1, 2, 200, 3, 101, 4};

    std::vector<TColumn> columns = {
        TColumn{.Data = reinterpret_cast<char*>(keys.data())},
        TColumn{.Data = reinterpret_cast<char*>(order.data())},
        TColumn{.Data = reinterpret_cast<char*>(payload.data())},
    };
    std::vector<TRowSet> batches = {TRowSet{
        .Columns = columns.data(),
        .ColumnCount = 3,
        .RowCount = static_cast<int64_t>(keys.size()),
        .Selection = nullptr,
        .RefCount = 1,
    }};
    TMockSource source({"k", "o", "p"}, std::move(batches));

    auto root = ParsePlan(
        "(rel window (rel source \"data.parquet\") "
        "(partition k) "
        "(order (o asc nulls-default)) "
        "(fn r rank))",
        source);

    auto runtime = RunPlan(root);

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.ColumnCount, 4);
    ASSERT_EQ(result.RowCount, 7);

    auto* outKeys = reinterpret_cast<int64_t*>(result.Columns[0].Data);
    auto* outOrder = reinterpret_cast<int64_t*>(result.Columns[1].Data);
    auto* outRank = reinterpret_cast<int64_t*>(result.Columns[3].Data);

    const std::array<int64_t, 7> expectedKeys = {1, 1, 1, 1, 2, 2, 2};
    const std::array<int64_t, 7> expectedOrder = {10, 10, 20, 30, 5, 5, 7};
    const std::array<int64_t, 7> expectedRank = {1, 1, 3, 4, 1, 1, 3};

    for (int64_t row = 0; row < result.RowCount; ++row) {
        EXPECT_EQ(outKeys[row], expectedKeys[row]) << "row " << row;
        EXPECT_EQ(outOrder[row], expectedOrder[row]) << "row " << row;
        EXPECT_EQ(outRank[row], expectedRank[row]) << "row " << row;
    }

    Release(&result);

    TRowSet second{};
    EXPECT_FALSE(runtime->Next(second));
}

TEST(WindowExec, RankResetsPerStringAndI64Partition) {
    const std::string groups = "baaabb";
    std::array<int64_t, 7> groupOffsets = {0, 1, 2, 3, 4, 5, 6};
    std::array<int64_t, 6> shards = {2, 1, 1, 1, 2, 1};
    std::array<int64_t, 6> order = {5, 10, 20, 10, 7, 5};
    std::array<int64_t, 6> payload = {200, 1, 3, 2, 300, 100};

    std::vector<TColumn> columns = {
        TColumn{
            .Data = const_cast<char*>(groups.data()),
            .Offsets = groupOffsets.data(),
            .OffsetWidth = 8,
        },
        TColumn{.Data = reinterpret_cast<char*>(shards.data())},
        TColumn{.Data = reinterpret_cast<char*>(order.data())},
        TColumn{.Data = reinterpret_cast<char*>(payload.data())},
    };
    std::vector<TRowSet> batches = {TRowSet{
        .Columns = columns.data(),
        .ColumnCount = 4,
        .RowCount = static_cast<int64_t>(shards.size()),
        .Selection = nullptr,
        .RefCount = 1,
    }};
    TMockSource source(
        {"g", "s", "o", "p"},
        std::move(batches),
        {
            std::make_shared<NQumir::NAst::TStringType>(),
            std::make_shared<NQumir::NAst::TIntegerType>(),
            std::make_shared<NQumir::NAst::TIntegerType>(),
            std::make_shared<NQumir::NAst::TIntegerType>(),
        });

    auto root = ParsePlan(
        "(rel window (rel source \"data.parquet\") "
        "(partition g s) "
        "(order (o asc nulls-default)) "
        "(fn r rank))",
        source);

    auto runtime = RunPlan(root);

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.ColumnCount, 5);
    ASSERT_EQ(result.RowCount, 6);

    auto* outShard = reinterpret_cast<int64_t*>(result.Columns[1].Data);
    auto* outOrder = reinterpret_cast<int64_t*>(result.Columns[2].Data);
    auto* outRank = reinterpret_cast<int64_t*>(result.Columns[4].Data);

    ASSERT_EQ(result.Columns[0].OffsetWidth, 8);
    const auto* outOffsets = static_cast<const int64_t*>(result.Columns[0].Offsets);
    const std::string_view outData(result.Columns[0].Data, outOffsets[result.RowCount]);

    const std::array<std::string_view, 6> expectedGroup = {"a", "a", "a", "b", "b", "b"};
    const std::array<int64_t, 6> expectedShard = {1, 1, 1, 1, 2, 2};
    const std::array<int64_t, 6> expectedOrder = {10, 10, 20, 5, 5, 7};
    const std::array<int64_t, 6> expectedRank = {1, 1, 3, 1, 1, 2};

    for (int64_t row = 0; row < result.RowCount; ++row) {
        const std::string_view gotGroup(
            outData.data() + outOffsets[row],
            static_cast<size_t>(outOffsets[row + 1] - outOffsets[row]));
        EXPECT_EQ(gotGroup, expectedGroup[row]) << "row " << row;
        EXPECT_EQ(outShard[row], expectedShard[row]) << "row " << row;
        EXPECT_EQ(outOrder[row], expectedOrder[row]) << "row " << row;
        EXPECT_EQ(outRank[row], expectedRank[row]) << "row " << row;
    }

    Release(&result);

    TRowSet second{};
    EXPECT_FALSE(runtime->Next(second));
}

TEST(WindowExec, AvgI64BroadcastsPerPartition) {
    std::array<int64_t, 5> keys = {2, 1, 1, 2, 1};
    std::array<int64_t, 5> values = {10, 3, 6, 30, 12};

    std::vector<TColumn> columns = {
        TColumn{.Data = reinterpret_cast<char*>(keys.data())},
        TColumn{.Data = reinterpret_cast<char*>(values.data())},
    };
    std::vector<TRowSet> batches = {TRowSet{
        .Columns = columns.data(),
        .ColumnCount = 2,
        .RowCount = static_cast<int64_t>(keys.size()),
        .Selection = nullptr,
        .RefCount = 1,
    }};
    TMockSource source({"k", "v"}, std::move(batches));

    auto root = ParsePlan(
        "(rel window (rel source \"data.parquet\") "
        "(partition k) "
        "(fn avg_v avg v))",
        source);

    auto runtime = RunPlan(root);

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.ColumnCount, 3);
    ASSERT_EQ(result.RowCount, 5);

    auto* outKeys = reinterpret_cast<int64_t*>(result.Columns[0].Data);
    auto* outValues = reinterpret_cast<int64_t*>(result.Columns[1].Data);
    auto* outAvg = reinterpret_cast<double*>(result.Columns[2].Data);

    const std::array<int64_t, 5> expectedKeys = {1, 1, 1, 2, 2};
    const std::array<int64_t, 5> expectedValues = {3, 6, 12, 10, 30};
    const std::array<double, 5> expectedAvg = {7.0, 7.0, 7.0, 20.0, 20.0};

    for (int64_t row = 0; row < result.RowCount; ++row) {
        EXPECT_EQ(outKeys[row], expectedKeys[row]) << "row " << row;
        EXPECT_EQ(outValues[row], expectedValues[row]) << "row " << row;
        EXPECT_TRUE(IsValid(result.Columns[2], row)) << "row " << row;
        EXPECT_DOUBLE_EQ(outAvg[row], expectedAvg[row]) << "row " << row;
    }

    Release(&result);

    TRowSet second{};
    EXPECT_FALSE(runtime->Next(second));
}

TEST(WindowExec, AvgI32BroadcastsF64PerPartition) {
    std::array<int64_t, 5> keys = {2, 1, 1, 2, 1};
    std::array<int32_t, 5> values = {10, 3, 6, 30, 12};

    std::vector<TColumn> columns = {
        TColumn{.Data = reinterpret_cast<char*>(keys.data())},
        TColumn{.Data = reinterpret_cast<char*>(values.data())},
    };
    std::vector<TRowSet> batches = {TRowSet{
        .Columns = columns.data(),
        .ColumnCount = 2,
        .RowCount = static_cast<int64_t>(keys.size()),
        .Selection = nullptr,
        .RefCount = 1,
    }};
    TMockSource source(
        {"k", "v"},
        std::move(batches),
        {std::make_shared<NQumir::NAst::TIntegerType>(),
         std::make_shared<NQumir::NAst::TIntegerType>(
             NQumir::NAst::TIntegerType::I32)});

    auto root = ParsePlan(
        "(rel window (rel source \"data.parquet\") "
        "(partition k) "
        "(fn avg_v avg v))",
        source);

    auto runtime = RunPlan(root);

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.ColumnCount, 3);
    ASSERT_EQ(result.RowCount, 5);

    auto* outValues = reinterpret_cast<int32_t*>(result.Columns[1].Data);
    auto* outAvg = reinterpret_cast<double*>(result.Columns[2].Data);

    const std::array<int32_t, 5> expectedValues = {3, 6, 12, 10, 30};
    const std::array<double, 5> expectedAvg = {7.0, 7.0, 7.0, 20.0, 20.0};

    for (int64_t row = 0; row < result.RowCount; ++row) {
        EXPECT_EQ(outValues[row], expectedValues[row]) << "row " << row;
        EXPECT_TRUE(IsValid(result.Columns[2], row)) << "row " << row;
        EXPECT_DOUBLE_EQ(outAvg[row], expectedAvg[row]) << "row " << row;
    }

    Release(&result);

    TRowSet second{};
    EXPECT_FALSE(runtime->Next(second));
}

TEST(WindowExec, AvgDecimalBroadcastsPerPartition) {
    std::array<int64_t, 5> keys = {2, 1, 1, 2, 1};
    std::array<int64_t, 5> values = {
        1000,
        300,
        600,
        3000,
        1200,
    };

    std::vector<TColumn> columns = {
        TColumn{.Data = reinterpret_cast<char*>(keys.data())},
        TColumn{.Data = reinterpret_cast<char*>(values.data())},
    };
    std::vector<TRowSet> batches = {TRowSet{
        .Columns = columns.data(),
        .ColumnCount = 2,
        .RowCount = static_cast<int64_t>(keys.size()),
        .Selection = nullptr,
        .RefCount = 1,
    }};
    TMockSource source(
        {"k", "v"},
        std::move(batches),
        {std::make_shared<NQumir::NAst::TIntegerType>(), std::make_shared<TDecimal>(15, 2)});

    auto root = ParsePlan(
        "(rel window (rel source \"data.parquet\") "
        "(partition k) "
        "(fn avg_v avg v))",
        source);

    auto runtime = RunPlan(root);

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.ColumnCount, 3);
    ASSERT_EQ(result.RowCount, 5);

    auto* outKeys = reinterpret_cast<int64_t*>(result.Columns[0].Data);
    auto* outAvg = reinterpret_cast<qdb_bin_int*>(result.Columns[2].Data);

    auto* outType = static_cast<NQumir::NAst::TStructType*>(root->OutputColumns().get());
    ASSERT_NE(outType, nullptr);
    ASSERT_EQ(outType->Fields.size(), 3u);
    auto avgSpec = DecimalSpecOf(outType->Fields[2].second);
    ASSERT_TRUE(avgSpec.has_value());
    EXPECT_EQ(avgSpec->Precision, 19);
    EXPECT_EQ(avgSpec->Scale, 6);

    const std::array<int64_t, 5> expectedKeys = {1, 1, 1, 2, 2};
    // avg widens the decimal scale by WindowAvgExtraScale (2 -> 6), so values are
    // scaled by 10^4: 7.000000 and 20.000000 keep full fractional precision.
    const std::array<uint64_t, 5> expectedAvg = {
        7000000, 7000000, 7000000, 20000000, 20000000};

    for (int64_t row = 0; row < result.RowCount; ++row) {
        EXPECT_EQ(outKeys[row], expectedKeys[row]) << "row " << row;
        EXPECT_TRUE(IsValid(result.Columns[2], row)) << "row " << row;
        EXPECT_EQ(outAvg[row].Lo, expectedAvg[row]) << "row " << row;
        EXPECT_EQ(outAvg[row].Hi, 0u) << "row " << row;
    }

    Release(&result);

    TRowSet second{};
    EXPECT_FALSE(runtime->Next(second));
}

TEST(WindowExec, AvgDecimalTypeKeepsIntegralDigitsAtPrecisionCap) {
    auto whole = DecimalSpecOf(
        ComputeWindowAvgResultType(std::make_shared<TDecimal>(38, 0)));
    ASSERT_TRUE(whole.has_value());
    EXPECT_EQ(whole->Precision, 38);
    EXPECT_EQ(whole->Scale, 0);

    auto fractional = DecimalSpecOf(
        ComputeWindowAvgResultType(std::make_shared<TDecimal>(38, 37)));
    ASSERT_TRUE(fractional.has_value());
    EXPECT_EQ(fractional->Precision, 38);
    EXPECT_EQ(fractional->Scale, 37);
}

TEST(WindowExec, AvgDecimalKeepsFractionalPrecision) {
    // Non-exact average over one partition: 5.00 / 3. At the input scale (2) this
    // truncates to 1.66; widening the scale by 4 keeps 1.666666.
    std::array<int64_t, 3> keys = {1, 1, 1};
    std::array<int64_t, 3> values = {
        100, // 1.00
        200, // 2.00
        200, // 2.00
    };

    std::vector<TColumn> columns = {
        TColumn{.Data = reinterpret_cast<char*>(keys.data())},
        TColumn{.Data = reinterpret_cast<char*>(values.data())},
    };
    std::vector<TRowSet> batches = {TRowSet{
        .Columns = columns.data(),
        .ColumnCount = 2,
        .RowCount = 3,
        .Selection = nullptr,
        .RefCount = 1,
    }};
    TMockSource source({"k", "v"}, std::move(batches),
        {std::make_shared<NQumir::NAst::TIntegerType>(),
         std::make_shared<TDecimal>(15, 2)});

    auto root = ParsePlan(
        "(rel window (rel source \"data.parquet\") (partition k) (fn a avg v))",
        source);

    auto runtime = RunPlan(root);
    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.RowCount, 3);

    auto* outAvg = reinterpret_cast<qdb_bin_int*>(result.Columns[2].Data);
    for (int64_t row = 0; row < result.RowCount; ++row) {
        // 1.666666 at scale 6 (would be 1.66 = 166 at the truncating scale 2).
        EXPECT_EQ(outAvg[row].Lo, 1666666u) << "row " << row;
        EXPECT_EQ(outAvg[row].Hi, 0u) << "row " << row;
    }

    Release(&result);
}

TEST(WindowExec, Q47LikeStackedAvgThenRank) {
    std::array<int64_t, 4> cat = {1, 1, 1, 1};
    std::array<int64_t, 4> brand = {1, 1, 1, 1};
    std::array<int64_t, 4> store = {1, 1, 1, 1};
    std::array<int64_t, 4> year = {2000, 1999, 2001, 2000};
    std::array<int64_t, 4> month = {2, 12, 1, 1};
    std::array<int64_t, 4> sales = {30, 5, 50, 10};

    std::vector<TColumn> columns = {
        TColumn{.Data = reinterpret_cast<char*>(cat.data())},
        TColumn{.Data = reinterpret_cast<char*>(brand.data())},
        TColumn{.Data = reinterpret_cast<char*>(store.data())},
        TColumn{.Data = reinterpret_cast<char*>(year.data())},
        TColumn{.Data = reinterpret_cast<char*>(month.data())},
        TColumn{.Data = reinterpret_cast<char*>(sales.data())},
    };
    std::vector<TRowSet> batches = {TRowSet{
        .Columns = columns.data(),
        .ColumnCount = 6,
        .RowCount = static_cast<int64_t>(sales.size()),
        .Selection = nullptr,
        .RefCount = 1,
    }};
    TMockSource source({"c", "b", "s", "y", "m", "sales"}, std::move(batches));

    auto root = ParsePlan(R"qdb(
(rel window
  (rel window (rel source "data.parquet")
    (partition c b s y)
    (fn avg_sales avg sales))
  (partition c b s)
  (order (y asc nulls-default) (m asc nulls-default))
  (fn rn rank))
)qdb", source);

    auto runtime = RunPlan(root);

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.ColumnCount, 8);
    ASSERT_EQ(result.RowCount, 4);

    auto* outYear = reinterpret_cast<int64_t*>(result.Columns[3].Data);
    auto* outMonth = reinterpret_cast<int64_t*>(result.Columns[4].Data);
    auto* outSales = reinterpret_cast<int64_t*>(result.Columns[5].Data);
    auto* outAvg = reinterpret_cast<double*>(result.Columns[6].Data);
    auto* outRank = reinterpret_cast<int64_t*>(result.Columns[7].Data);

    const std::array<int64_t, 4> expectedYear = {1999, 2000, 2000, 2001};
    const std::array<int64_t, 4> expectedMonth = {12, 1, 2, 1};
    const std::array<int64_t, 4> expectedSales = {5, 10, 30, 50};
    const std::array<double, 4> expectedAvg = {5.0, 20.0, 20.0, 50.0};
    const std::array<int64_t, 4> expectedRank = {1, 2, 3, 4};

    for (int64_t row = 0; row < result.RowCount; ++row) {
        EXPECT_EQ(outYear[row], expectedYear[row]) << "row " << row;
        EXPECT_EQ(outMonth[row], expectedMonth[row]) << "row " << row;
        EXPECT_EQ(outSales[row], expectedSales[row]) << "row " << row;
        EXPECT_TRUE(IsValid(result.Columns[6], row)) << "row " << row;
        EXPECT_DOUBLE_EQ(outAvg[row], expectedAvg[row]) << "row " << row;
        EXPECT_EQ(outRank[row], expectedRank[row]) << "row " << row;
    }

    Release(&result);

    TRowSet second{};
    EXPECT_FALSE(runtime->Next(second));
}

TEST(WindowExec, Q47LikeCteSelfJoinUsesWindowRankNeighbors) {
    std::array<int64_t, 4> cat = {1, 1, 1, 1};
    std::array<int64_t, 4> brand = {1, 1, 1, 1};
    std::array<int64_t, 4> store = {1, 1, 1, 1};
    std::array<int64_t, 4> year = {2000, 1999, 2001, 2000};
    std::array<int64_t, 4> month = {2, 12, 1, 1};
    std::array<int64_t, 4> sales = {30, 5, 50, 10};

    std::vector<TColumn> columns = {
        TColumn{.Data = reinterpret_cast<char*>(cat.data())},
        TColumn{.Data = reinterpret_cast<char*>(brand.data())},
        TColumn{.Data = reinterpret_cast<char*>(store.data())},
        TColumn{.Data = reinterpret_cast<char*>(year.data())},
        TColumn{.Data = reinterpret_cast<char*>(month.data())},
        TColumn{.Data = reinterpret_cast<char*>(sales.data())},
    };
    std::vector<TRowSet> batches = {TRowSet{
        .Columns = columns.data(),
        .ColumnCount = 6,
        .RowCount = static_cast<int64_t>(sales.size()),
        .Selection = nullptr,
        .RefCount = 1,
    }};
    TMockSource source({"c", "b", "s", "y", "m", "sales"}, std::move(batches));

    auto root = ParsePlan(R"qdb(
(query
  (cte 0
    (rel project
      (rel window
        (rel window (rel source "data.parquet")
          (partition c b s y)
          (fn avg_sales avg sales))
        (partition c b s)
        (order (y asc nulls-default) (m asc nulls-default))
        (fn rn rank))
      (c c) (b b) (s s) (y y) (m m)
      (sum_sales sales)
      (avg_monthly_sales avg_sales)
      (rn rn)))
  (main
    (rel project
      (rel filter
        (rel join
          (rel join
            (rel project (rel cte-ref 0)
              (|v1.c| c) (|v1.b| b) (|v1.s| s) (|v1.y| y)
              (|v1.m| m) (|v1.sum_sales| sum_sales)
              (|v1.avg_monthly_sales| avg_monthly_sales) (|v1.rn| rn))
            (rel project (rel cte-ref 0)
              (|lag.c| c) (|lag.b| b) (|lag.s| s) (|lag.y| y)
              (|lag.m| m) (|lag.sum_sales| sum_sales)
              (|lag.avg_monthly_sales| avg_monthly_sales) (|lag.rn| rn))
            () (inner))
          (rel project (rel cte-ref 0)
            (|lead.c| c) (|lead.b| b) (|lead.s| s) (|lead.y| y)
            (|lead.m| m) (|lead.sum_sales| sum_sales)
            (|lead.avg_monthly_sales| avg_monthly_sales) (|lead.rn| rn))
          () (inner))
        (&& (&& (&& (&& (&& (&& (&&
          (== |v1.c| |lag.c|)
          (== |v1.c| |lead.c|))
          (== |v1.b| |lag.b|))
          (== |v1.b| |lead.b|))
          (== |v1.s| |lag.s|))
          (== |v1.s| |lead.s|))
          (== |v1.rn| (+ |lag.rn| 1)))
          (== |v1.rn| (- |lead.rn| 1))))
      (y |v1.y|)
      (m |v1.m|)
      (sum_sales |v1.sum_sales|)
      (avg_monthly_sales |v1.avg_monthly_sales|)
      (psum |lag.sum_sales|)
      (nsum |lead.sum_sales|))))
)qdb", source);
    ApplyPlanPasses(root);

    auto runtime = RunPlan(root);

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.ColumnCount, 6);
    ASSERT_EQ(result.RowCount, 2);

    auto* outYear = reinterpret_cast<int64_t*>(result.Columns[0].Data);
    auto* outMonth = reinterpret_cast<int64_t*>(result.Columns[1].Data);
    auto* outSales = reinterpret_cast<int64_t*>(result.Columns[2].Data);
    auto* outAvg = reinterpret_cast<double*>(result.Columns[3].Data);
    auto* outPrev = reinterpret_cast<int64_t*>(result.Columns[4].Data);
    auto* outNext = reinterpret_cast<int64_t*>(result.Columns[5].Data);

    const std::array<int64_t, 2> expectedYear = {2000, 2000};
    const std::array<int64_t, 2> expectedMonth = {1, 2};
    const std::array<int64_t, 2> expectedSales = {10, 30};
    const std::array<double, 2> expectedAvg = {20.0, 20.0};
    const std::array<int64_t, 2> expectedPrev = {5, 10};
    const std::array<int64_t, 2> expectedNext = {30, 50};

    for (int64_t row = 0; row < result.RowCount; ++row) {
        EXPECT_EQ(outYear[row], expectedYear[row]) << "row " << row;
        EXPECT_EQ(outMonth[row], expectedMonth[row]) << "row " << row;
        EXPECT_EQ(outSales[row], expectedSales[row]) << "row " << row;
        EXPECT_DOUBLE_EQ(outAvg[row], expectedAvg[row]) << "row " << row;
        EXPECT_EQ(outPrev[row], expectedPrev[row]) << "row " << row;
        EXPECT_EQ(outNext[row], expectedNext[row]) << "row " << row;
    }

    Release(&result);

    TRowSet second{};
    EXPECT_FALSE(runtime->Next(second));
}

TEST(WindowExec, Q47LikeCteSelfJoinKeepsDecimalWindowAvg) {
    std::array<int64_t, 4> cat = {1, 1, 1, 1};
    std::array<int64_t, 4> brand = {1, 1, 1, 1};
    std::array<int64_t, 4> store = {1, 1, 1, 1};
    std::array<int64_t, 4> year = {2000, 1999, 2001, 2000};
    std::array<int64_t, 4> month = {2, 12, 1, 1};
    std::array<int64_t, 4> sales = {
        3000, // 30.00
        500, // 5.00
        5000, // 50.00
        1000, // 10.00
    };

    std::vector<TColumn> columns = {
        TColumn{.Data = reinterpret_cast<char*>(cat.data())},
        TColumn{.Data = reinterpret_cast<char*>(brand.data())},
        TColumn{.Data = reinterpret_cast<char*>(store.data())},
        TColumn{.Data = reinterpret_cast<char*>(year.data())},
        TColumn{.Data = reinterpret_cast<char*>(month.data())},
        TColumn{.Data = reinterpret_cast<char*>(sales.data())},
    };
    std::vector<TRowSet> batches = {TRowSet{
        .Columns = columns.data(),
        .ColumnCount = 6,
        .RowCount = static_cast<int64_t>(sales.size()),
        .Selection = nullptr,
        .RefCount = 1,
    }};
    TMockSource source(
        {"c", "b", "s", "y", "m", "sales"},
        std::move(batches),
        {
            std::make_shared<NQumir::NAst::TIntegerType>(),
            std::make_shared<NQumir::NAst::TIntegerType>(),
            std::make_shared<NQumir::NAst::TIntegerType>(),
            std::make_shared<NQumir::NAst::TIntegerType>(),
            std::make_shared<NQumir::NAst::TIntegerType>(),
            std::make_shared<TDecimal>(15, 2),
        });

    auto root = ParsePlan(R"qdb(
(query
  (cte 0
    (rel project
      (rel window
        (rel window (rel source "data.parquet")
          (partition c b s y)
          (fn avg_sales avg sales))
        (partition c b s)
        (order (y asc nulls-default) (m asc nulls-default))
        (fn rn rank))
      (c c) (b b) (s s) (y y) (m m)
      (sum_sales sales)
      (avg_monthly_sales avg_sales)
      (rn rn)))
  (main
    (rel project
      (rel filter
        (rel join
          (rel join
            (rel project (rel cte-ref 0)
              (|v1.c| c) (|v1.b| b) (|v1.s| s) (|v1.y| y)
              (|v1.m| m) (|v1.sum_sales| sum_sales)
              (|v1.avg_monthly_sales| avg_monthly_sales) (|v1.rn| rn))
            (rel project (rel cte-ref 0)
              (|lag.c| c) (|lag.b| b) (|lag.s| s) (|lag.y| y)
              (|lag.m| m) (|lag.sum_sales| sum_sales)
              (|lag.avg_monthly_sales| avg_monthly_sales) (|lag.rn| rn))
            () (inner))
          (rel project (rel cte-ref 0)
            (|lead.c| c) (|lead.b| b) (|lead.s| s) (|lead.y| y)
            (|lead.m| m) (|lead.sum_sales| sum_sales)
            (|lead.avg_monthly_sales| avg_monthly_sales) (|lead.rn| rn))
          () (inner))
        (&& (&& (&& (&& (&& (&& (&&
          (== |v1.c| |lag.c|)
          (== |v1.c| |lead.c|))
          (== |v1.b| |lag.b|))
          (== |v1.b| |lead.b|))
          (== |v1.s| |lag.s|))
          (== |v1.s| |lead.s|))
          (== |v1.rn| (+ |lag.rn| 1)))
          (== |v1.rn| (- |lead.rn| 1))))
      (y |v1.y|)
      (m |v1.m|)
      (sum_sales |v1.sum_sales|)
      (avg_monthly_sales |v1.avg_monthly_sales|)
      (psum |lag.sum_sales|)
      (nsum |lead.sum_sales|))))
)qdb", source);
    ApplyPlanPasses(root);

    auto runtime = RunPlan(root);

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.ColumnCount, 6);
    ASSERT_EQ(result.RowCount, 2);

    // DECIMAL(15,2) columns are i64; the DECIMAL(19,6) average is a BinInt.
    auto* outSales = reinterpret_cast<int64_t*>(result.Columns[2].Data);
    auto* outAvg = reinterpret_cast<qdb_bin_int*>(result.Columns[3].Data);
    auto* outPrev = reinterpret_cast<int64_t*>(result.Columns[4].Data);
    auto* outNext = reinterpret_cast<int64_t*>(result.Columns[5].Data);

    const std::array<int64_t, 2> expectedSales = {1000, 3000};
    const std::array<uint64_t, 2> expectedAvg = {20000000, 20000000};
    const std::array<int64_t, 2> expectedPrev = {500, 1000};
    const std::array<int64_t, 2> expectedNext = {3000, 5000};

    for (int64_t row = 0; row < result.RowCount; ++row) {
        EXPECT_EQ(outSales[row], expectedSales[row]) << "row " << row;
        EXPECT_EQ(outAvg[row].Lo, expectedAvg[row]) << "row " << row;
        EXPECT_EQ(outPrev[row], expectedPrev[row]) << "row " << row;
        EXPECT_EQ(outNext[row], expectedNext[row]) << "row " << row;
    }

    Release(&result);
}

TEST(WindowExec, Q47LikeCteConsumerAppliesDecimalOutlierFilter) {
    std::array<int64_t, 4> cat = {1, 1, 1, 1};
    std::array<int64_t, 4> brand = {1, 1, 1, 1};
    std::array<int64_t, 4> store = {1, 1, 1, 1};
    std::array<int64_t, 4> year = {2000, 1999, 2001, 2000};
    std::array<int64_t, 4> month = {2, 12, 1, 1};
    std::array<int64_t, 4> sales = {
        3000, // 30.00
        500, // 5.00
        5000, // 50.00
        1000, // 10.00
    };

    std::vector<TColumn> columns = {
        TColumn{.Data = reinterpret_cast<char*>(cat.data())},
        TColumn{.Data = reinterpret_cast<char*>(brand.data())},
        TColumn{.Data = reinterpret_cast<char*>(store.data())},
        TColumn{.Data = reinterpret_cast<char*>(year.data())},
        TColumn{.Data = reinterpret_cast<char*>(month.data())},
        TColumn{.Data = reinterpret_cast<char*>(sales.data())},
    };
    std::vector<TRowSet> batches = {TRowSet{
        .Columns = columns.data(),
        .ColumnCount = 6,
        .RowCount = static_cast<int64_t>(sales.size()),
        .Selection = nullptr,
        .RefCount = 1,
    }};
    TMockSource source(
        {"c", "b", "s", "y", "m", "sales"},
        std::move(batches),
        {
            std::make_shared<NQumir::NAst::TIntegerType>(),
            std::make_shared<NQumir::NAst::TIntegerType>(),
            std::make_shared<NQumir::NAst::TIntegerType>(),
            std::make_shared<NQumir::NAst::TIntegerType>(),
            std::make_shared<NQumir::NAst::TIntegerType>(),
            std::make_shared<TDecimal>(15, 2),
        });

    auto root = ParsePlan(R"qdb(
(query
  (cte 0
    (rel project
      (rel window
        (rel window (rel source "data.parquet")
          (partition c b s y)
          (fn avg_sales avg sales))
        (partition c b s)
        (order (y asc nulls-default) (m asc nulls-default))
        (fn rn rank))
      (c c) (b b) (s s) (y y) (m m)
      (sum_sales sales)
      (avg_monthly_sales avg_sales)
      (rn rn)))
  (main
    (rel project
      (rel filter (rel cte-ref 0)
        (&& (&&
          (== y 2000)
          (> avg_monthly_sales 0))
          (> (/ (call abs (- sum_sales avg_monthly_sales))
                avg_monthly_sales)
             0.10000000000000001)))
      (m m)
      (sum_sales sum_sales)
      (avg_monthly_sales avg_monthly_sales))))
)qdb", source);
    ApplyPlanPasses(root);

    auto runtime = RunPlan(root);

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.ColumnCount, 3);
    ASSERT_EQ(result.RowCount, 4);
    ASSERT_NE(result.Selection, nullptr);
    EXPECT_EQ(
        std::vector<uint8_t>(result.Selection, result.Selection + result.RowCount),
        (std::vector<uint8_t>{0, 0xff, 0xff, 0}));

    auto* outMonth = reinterpret_cast<int64_t*>(result.Columns[0].Data);
    auto* outSales = reinterpret_cast<int64_t*>(result.Columns[1].Data);
    auto* outAvg = reinterpret_cast<qdb_bin_int*>(result.Columns[2].Data);
    const std::array<int64_t, 4> expectedMonth = {12, 1, 2, 1};
    const std::array<int64_t, 4> expectedSales = {500, 1000, 3000, 5000};
    const std::array<uint64_t, 4> expectedAvg = {5000000, 20000000, 20000000, 50000000};
    for (int64_t row = 0; row < result.RowCount; ++row) {
        EXPECT_EQ(outMonth[row], expectedMonth[row]) << "row " << row;
        EXPECT_TRUE(IsValid(result.Columns[2], row)) << "row " << row;
        EXPECT_EQ(outSales[row], expectedSales[row]) << "row " << row;
        EXPECT_EQ(outAvg[row].Lo, expectedAvg[row]) << "row " << row;
    }

    Release(&result);
}

TEST(WindowExec, Q47LikeCteSelfJoinAppliesDecimalOutlierFilter) {
    std::array<int64_t, 4> cat = {1, 1, 1, 1};
    std::array<int64_t, 4> brand = {1, 1, 1, 1};
    std::array<int64_t, 4> store = {1, 1, 1, 1};
    std::array<int64_t, 4> year = {2000, 1999, 2001, 2000};
    std::array<int64_t, 4> month = {2, 12, 1, 1};
    std::array<int64_t, 4> sales = {
        3000, // 30.00
        500, // 5.00
        5000, // 50.00
        1000, // 10.00
    };

    std::vector<TColumn> columns = {
        TColumn{.Data = reinterpret_cast<char*>(cat.data())},
        TColumn{.Data = reinterpret_cast<char*>(brand.data())},
        TColumn{.Data = reinterpret_cast<char*>(store.data())},
        TColumn{.Data = reinterpret_cast<char*>(year.data())},
        TColumn{.Data = reinterpret_cast<char*>(month.data())},
        TColumn{.Data = reinterpret_cast<char*>(sales.data())},
    };
    std::vector<TRowSet> batches = {TRowSet{
        .Columns = columns.data(),
        .ColumnCount = 6,
        .RowCount = static_cast<int64_t>(sales.size()),
        .Selection = nullptr,
        .RefCount = 1,
    }};
    TMockSource source(
        {"c", "b", "s", "y", "m", "sales"},
        std::move(batches),
        {
            std::make_shared<NQumir::NAst::TIntegerType>(),
            std::make_shared<NQumir::NAst::TIntegerType>(),
            std::make_shared<NQumir::NAst::TIntegerType>(),
            std::make_shared<NQumir::NAst::TIntegerType>(),
            std::make_shared<NQumir::NAst::TIntegerType>(),
            std::make_shared<TDecimal>(15, 2),
        });

    auto root = ParsePlan(R"qdb(
(query
  (cte 0
    (rel project
      (rel window
        (rel window (rel source "data.parquet")
          (partition c b s y)
          (fn avg_sales avg sales))
        (partition c b s)
        (order (y asc nulls-default) (m asc nulls-default))
        (fn rn rank))
      (c c) (b b) (s s) (y y) (m m)
      (sum_sales sales)
      (avg_monthly_sales avg_sales)
      (rn rn)))
  (main
    (rel project
      (rel filter
        (rel filter
          (rel join
            (rel join
              (rel project (rel cte-ref 0)
                (|v1.c| c) (|v1.b| b) (|v1.s| s) (|v1.y| y)
                (|v1.m| m) (|v1.sum_sales| sum_sales)
                (|v1.avg_monthly_sales| avg_monthly_sales) (|v1.rn| rn))
              (rel project (rel cte-ref 0)
                (|lag.c| c) (|lag.b| b) (|lag.s| s) (|lag.y| y)
                (|lag.m| m) (|lag.sum_sales| sum_sales)
                (|lag.avg_monthly_sales| avg_monthly_sales) (|lag.rn| rn))
              () (inner))
            (rel project (rel cte-ref 0)
              (|lead.c| c) (|lead.b| b) (|lead.s| s) (|lead.y| y)
              (|lead.m| m) (|lead.sum_sales| sum_sales)
              (|lead.avg_monthly_sales| avg_monthly_sales) (|lead.rn| rn))
            () (inner))
          (&& (&& (&& (&& (&& (&& (&&
            (== |v1.c| |lag.c|)
            (== |v1.c| |lead.c|))
            (== |v1.b| |lag.b|))
            (== |v1.b| |lead.b|))
            (== |v1.s| |lag.s|))
            (== |v1.s| |lead.s|))
            (== |v1.rn| (+ |lag.rn| 1)))
            (== |v1.rn| (- |lead.rn| 1))))
        (&& (&&
          (== |v1.y| 2000)
          (> |v1.avg_monthly_sales| 0))
          (> (/ (call abs (- |v1.sum_sales| |v1.avg_monthly_sales|))
                |v1.avg_monthly_sales|)
             0.10000000000000001)))
      (m |v1.m|)
      (sum_sales |v1.sum_sales|)
      (avg_monthly_sales |v1.avg_monthly_sales|))))
)qdb", source);
    ApplyPlanPasses(root);

    auto runtime = RunPlan(root);

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.ColumnCount, 3);
    ASSERT_EQ(result.RowCount, 2);

    auto* outMonth = reinterpret_cast<int64_t*>(result.Columns[0].Data);
    auto* outSales = reinterpret_cast<int64_t*>(result.Columns[1].Data);
    auto* outAvg = reinterpret_cast<qdb_bin_int*>(result.Columns[2].Data);

    const std::array<int64_t, 2> expectedMonth = {1, 2};
    const std::array<int64_t, 2> expectedSales = {1000, 3000};
    const std::array<uint64_t, 2> expectedAvg = {20000000, 20000000};

    for (int64_t row = 0; row < result.RowCount; ++row) {
        EXPECT_EQ(outMonth[row], expectedMonth[row]) << "row " << row;
        EXPECT_EQ(outSales[row], expectedSales[row]) << "row " << row;
        EXPECT_EQ(outAvg[row].Lo, expectedAvg[row]) << "row " << row;
    }

    Release(&result);
}

TEST(WindowExec, RankI64WithoutPartition) {
    std::array<int64_t, 5> order = {30, 10, 20, 20, 10};
    std::array<int64_t, 5> payload = {3, 1, 2, 4, 5};

    std::vector<TColumn> columns = {
        TColumn{.Data = reinterpret_cast<char*>(order.data())},
        TColumn{.Data = reinterpret_cast<char*>(payload.data())},
    };
    std::vector<TRowSet> batches = {TRowSet{
        .Columns = columns.data(),
        .ColumnCount = 2,
        .RowCount = static_cast<int64_t>(order.size()),
        .Selection = nullptr,
        .RefCount = 1,
    }};
    TMockSource source({"o", "p"}, std::move(batches));

    auto root = ParsePlan(
        "(rel window (rel source \"data.parquet\") "
        "(order (o asc nulls-default)) "
        "(fn r rank))",
        source);

    auto runtime = RunPlan(root);

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.ColumnCount, 3);
    ASSERT_EQ(result.RowCount, 5);

    auto* outOrder = reinterpret_cast<int64_t*>(result.Columns[0].Data);
    auto* outRank = reinterpret_cast<int64_t*>(result.Columns[2].Data);

    const std::array<int64_t, 5> expectedOrder = {10, 10, 20, 20, 30};
    const std::array<int64_t, 5> expectedRank = {1, 1, 3, 3, 5};

    for (int64_t row = 0; row < result.RowCount; ++row) {
        EXPECT_EQ(outOrder[row], expectedOrder[row]) << "row " << row;
        EXPECT_EQ(outRank[row], expectedRank[row]) << "row " << row;
    }

    Release(&result);

    TRowSet second{};
    EXPECT_FALSE(runtime->Next(second));
}

TEST(WindowExec, RankDecimalWithoutPartition) {
    std::array<int64_t, 5> order = {
        3000,
        1000,
        2000,
        2000,
        1000,
    };
    std::array<int64_t, 5> payload = {3, 1, 2, 4, 5};

    std::vector<TColumn> columns = {
        TColumn{.Data = reinterpret_cast<char*>(order.data())},
        TColumn{.Data = reinterpret_cast<char*>(payload.data())},
    };
    std::vector<TRowSet> batches = {TRowSet{
        .Columns = columns.data(),
        .ColumnCount = 2,
        .RowCount = static_cast<int64_t>(order.size()),
        .Selection = nullptr,
        .RefCount = 1,
    }};
    TMockSource source(
        {"d", "p"},
        std::move(batches),
        {std::make_shared<TDecimal>(15, 4), std::make_shared<NQumir::NAst::TIntegerType>()});

    auto root = ParsePlan(
        "(rel window (rel source \"data.parquet\") "
        "(order (d asc nulls-default)) "
        "(fn r rank))",
        source);

    auto runtime = RunPlan(root);

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.ColumnCount, 3);
    ASSERT_EQ(result.RowCount, 5);

    auto* outOrder = reinterpret_cast<int64_t*>(result.Columns[0].Data);
    auto* outRank = reinterpret_cast<int64_t*>(result.Columns[2].Data);

    const std::array<int64_t, 5> expectedOrder = {1000, 1000, 2000, 2000, 3000};
    const std::array<int64_t, 5> expectedRank = {1, 1, 3, 3, 5};

    for (int64_t row = 0; row < result.RowCount; ++row) {
        EXPECT_EQ(outOrder[row], expectedOrder[row]) << "row " << row;
        EXPECT_EQ(outRank[row], expectedRank[row]) << "row " << row;
    }

    Release(&result);

    TRowSet second{};
    EXPECT_FALSE(runtime->Next(second));
}

TEST(WindowExec, SumDecimalBroadcastsNullForAllNullPartition) {
    std::array<int64_t, 5> keys = {2, 1, 2, 1, 2};
    std::array<int64_t, 5> values = {
        100,
        10,
        300,
        20,
        400,
    };
    std::array<uint8_t, 1> valueMask = {0};
    valueMask[0] = static_cast<uint8_t>((1u << 0) | (1u << 2) | (1u << 4));

    std::vector<TColumn> columns = {
        TColumn{.Data = reinterpret_cast<char*>(keys.data())},
        TColumn{
            .Data = reinterpret_cast<char*>(values.data()),
            .Mask = valueMask.data(),
        },
    };
    std::vector<TRowSet> batches = {TRowSet{
        .Columns = columns.data(),
        .ColumnCount = 2,
        .RowCount = static_cast<int64_t>(keys.size()),
        .Selection = nullptr,
        .RefCount = 1,
    }};
    TMockSource source(
        {"k", "v"},
        std::move(batches),
        {
            std::make_shared<NQumir::NAst::TIntegerType>(),
            std::make_shared<TNullable>(std::make_shared<TDecimal>(15, 2)),
        });

    auto root = ParsePlan(
        "(rel window (rel source \"data.parquet\") "
        "(partition k) "
        "(frame range (start unbounded-preceding) (end unbounded-following)) "
        "(fn total sum v))",
        source);

    auto runtime = RunPlan(root);

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.ColumnCount, 3);
    ASSERT_EQ(result.RowCount, 5);

    auto* outKeys = reinterpret_cast<int64_t*>(result.Columns[0].Data);
    auto* outSum = reinterpret_cast<qdb_bin_int*>(result.Columns[2].Data);

    const std::array<int64_t, 5> expectedKeys = {1, 1, 2, 2, 2};
    const std::array<bool, 5> expectedValid = {false, false, true, true, true};
    const std::array<uint64_t, 5> expectedSum = {0, 0, 800, 800, 800};

    for (int64_t row = 0; row < result.RowCount; ++row) {
        EXPECT_EQ(outKeys[row], expectedKeys[row]) << "row " << row;
        EXPECT_EQ(IsValid(result.Columns[2], row), expectedValid[row]) << "row " << row;
        if (expectedValid[row]) {
            EXPECT_EQ(outSum[row].Lo, expectedSum[row]) << "row " << row;
            EXPECT_EQ(outSum[row].Hi, 0u) << "row " << row;
        }
    }

    Release(&result);

    TRowSet second{};
    EXPECT_FALSE(runtime->Next(second));
}

TEST(WindowExec, PrefixSumDecimalSkipsNullsAndKeepsNullUntilFirstValue) {
    std::array<int64_t, 5> keys = {1, 1, 2, 1, 2};
    std::array<int64_t, 5> order = {1, 2, 1, 3, 2};
    std::array<int64_t, 5> values = {
        10,
        500,
        70,
        30,
        700,
    };
    std::array<uint8_t, 1> valueMask = {0};
    valueMask[0] = static_cast<uint8_t>((1u << 1) | (1u << 4));

    std::vector<TColumn> columns = {
        TColumn{.Data = reinterpret_cast<char*>(keys.data())},
        TColumn{.Data = reinterpret_cast<char*>(order.data())},
        TColumn{
            .Data = reinterpret_cast<char*>(values.data()),
            .Mask = valueMask.data(),
        },
    };
    std::vector<TRowSet> batches = {TRowSet{
        .Columns = columns.data(),
        .ColumnCount = 3,
        .RowCount = static_cast<int64_t>(keys.size()),
        .Selection = nullptr,
        .RefCount = 1,
    }};
    TMockSource source(
        {"k", "o", "v"},
        std::move(batches),
        {
            std::make_shared<NQumir::NAst::TIntegerType>(),
            std::make_shared<NQumir::NAst::TIntegerType>(),
            std::make_shared<TNullable>(std::make_shared<TDecimal>(15, 2)),
        });

    auto root = ParsePlan(
        "(rel window (rel source \"data.parquet\") "
        "(partition k) "
        "(order (o asc nulls-default)) "
        "(frame rows (start unbounded-preceding) (end current-row)) "
        "(fn running_sum sum v))",
        source);

    auto runtime = RunPlan(root);

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.ColumnCount, 4);
    ASSERT_EQ(result.RowCount, 5);

    auto* outKeys = reinterpret_cast<int64_t*>(result.Columns[0].Data);
    auto* outOrder = reinterpret_cast<int64_t*>(result.Columns[1].Data);
    auto* outSum = reinterpret_cast<qdb_bin_int*>(result.Columns[3].Data);

    const std::array<int64_t, 5> expectedKeys = {1, 1, 1, 2, 2};
    const std::array<int64_t, 5> expectedOrder = {1, 2, 3, 1, 2};
    const std::array<bool, 5> expectedValid = {false, true, true, false, true};
    const std::array<uint64_t, 5> expectedSum = {0, 500, 500, 0, 700};

    for (int64_t row = 0; row < result.RowCount; ++row) {
        EXPECT_EQ(outKeys[row], expectedKeys[row]) << "row " << row;
        EXPECT_EQ(outOrder[row], expectedOrder[row]) << "row " << row;
        EXPECT_EQ(IsValid(result.Columns[3], row), expectedValid[row]) << "row " << row;
        if (expectedValid[row]) {
            EXPECT_EQ(outSum[row].Lo, expectedSum[row]) << "row " << row;
            EXPECT_EQ(outSum[row].Hi, 0u) << "row " << row;
        }
    }

    Release(&result);

    TRowSet second{};
    EXPECT_FALSE(runtime->Next(second));
}

TEST(WindowExec, PrefixMaxDecimalSkipsNullsAndKeepsNullUntilFirstValue) {
    std::array<int64_t, 5> keys = {1, 1, 2, 1, 2};
    std::array<int64_t, 5> order = {1, 2, 1, 3, 2};
    std::array<int64_t, 5> values = {
        10,
        300,
        70,
        200,
        700,
    };
    std::array<uint8_t, 1> valueMask = {0};
    valueMask[0] = static_cast<uint8_t>((1u << 1) | (1u << 3) | (1u << 4));

    std::vector<TColumn> columns = {
        TColumn{.Data = reinterpret_cast<char*>(keys.data())},
        TColumn{.Data = reinterpret_cast<char*>(order.data())},
        TColumn{
            .Data = reinterpret_cast<char*>(values.data()),
            .Mask = valueMask.data(),
        },
    };
    std::vector<TRowSet> batches = {TRowSet{
        .Columns = columns.data(),
        .ColumnCount = 3,
        .RowCount = static_cast<int64_t>(keys.size()),
        .Selection = nullptr,
        .RefCount = 1,
    }};
    TMockSource source(
        {"k", "o", "v"},
        std::move(batches),
        {
            std::make_shared<NQumir::NAst::TIntegerType>(),
            std::make_shared<NQumir::NAst::TIntegerType>(),
            std::make_shared<TNullable>(std::make_shared<TDecimal>(15, 2)),
        });

    auto root = ParsePlan(
        "(rel window (rel source \"data.parquet\") "
        "(partition k) "
        "(order (o asc nulls-default)) "
        "(frame rows (start unbounded-preceding) (end current-row)) "
        "(fn running_max max v))",
        source);

    auto runtime = RunPlan(root);

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.ColumnCount, 4);
    ASSERT_EQ(result.RowCount, 5);

    auto* outKeys = reinterpret_cast<int64_t*>(result.Columns[0].Data);
    auto* outOrder = reinterpret_cast<int64_t*>(result.Columns[1].Data);
    auto* outMax = reinterpret_cast<int64_t*>(result.Columns[3].Data);

    const std::array<int64_t, 5> expectedKeys = {1, 1, 1, 2, 2};
    const std::array<int64_t, 5> expectedOrder = {1, 2, 3, 1, 2};
    const std::array<bool, 5> expectedValid = {false, true, true, false, true};
    const std::array<int64_t, 5> expectedMax = {0, 300, 300, 0, 700};

    for (int64_t row = 0; row < result.RowCount; ++row) {
        EXPECT_EQ(outKeys[row], expectedKeys[row]) << "row " << row;
        EXPECT_EQ(outOrder[row], expectedOrder[row]) << "row " << row;
        EXPECT_EQ(IsValid(result.Columns[3], row), expectedValid[row]) << "row " << row;
        if (expectedValid[row]) {
            EXPECT_EQ(outMax[row], expectedMax[row]) << "row " << row;
        }
    }

    Release(&result);

    TRowSet second{};
    EXPECT_FALSE(runtime->Next(second));
}

TEST(WindowExec, PrefixSumGroupsNullPartitionKey) {
    // Partition key k is nullable; rows 0 and 2 are NULL but hold different
    // underlying value bytes (5 and 2), so a non-nullable radix would scatter
    // them by raw bytes and split the NULL partition / misorder it. The nullable
    // radix must segregate NULLs into one contiguous partition.
    std::array<int64_t, 4> k = {5, 1, 2, 1};
    std::array<uint8_t, 1> Mask = {0x0A}; // rows 1,3 valid; rows 0,2 NULL
    std::array<int64_t, 4> o = {1, 1, 2, 2};
    std::array<int64_t, 4> v = {10, 20, 30, 40};

    std::vector<TColumn> columns = {
        TColumn{.Data = reinterpret_cast<char*>(k.data()), .Mask = Mask.data()},
        TColumn{.Data = reinterpret_cast<char*>(o.data())},
        TColumn{.Data = reinterpret_cast<char*>(v.data())},
    };
    std::vector<TRowSet> batches = {TRowSet{
        .Columns = columns.data(),
        .ColumnCount = 3,
        .RowCount = 4,
        .Selection = nullptr,
        .RefCount = 1,
    }};
    auto nullableI64 = std::make_shared<NQdb::TNullable>(
        std::make_shared<NQumir::NAst::TIntegerType>());
    auto i64 = std::make_shared<NQumir::NAst::TIntegerType>();
    TMockSource source({"k", "o", "v"}, std::move(batches),
        {nullableI64, i64, i64});

    auto root = ParsePlan(
        "(rel window (rel source \"data.parquet\") "
        "(partition k) "
        "(order (o asc nulls-default)) "
        "(frame rows (start unbounded-preceding) (end current-row)) "
        "(fn running_sum sum v))",
        source);

    auto runtime = RunPlan(root);

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.RowCount, 4);

    auto* outOrder = reinterpret_cast<int64_t*>(result.Columns[1].Data);
    auto* outValues = reinterpret_cast<int64_t*>(result.Columns[2].Data);
    auto* outSums = reinterpret_cast<int64_t*>(result.Columns[3].Data);

    // k=1 partition first (NULLS LAST for ASC), then the NULL partition; ORDER BY
    // o within each. Prefix sums reset at the partition boundary.
    const std::array<bool, 4> expectedKeyValid = {true, true, false, false};
    const std::array<int64_t, 4> expectedOrder = {1, 2, 1, 2};
    const std::array<int64_t, 4> expectedValues = {20, 40, 10, 30};
    const std::array<int64_t, 4> expectedSums = {20, 60, 10, 40};

    for (int64_t row = 0; row < result.RowCount; ++row) {
        EXPECT_EQ(IsValid(result.Columns[0], row), expectedKeyValid[row]) << "row " << row;
        EXPECT_EQ(outOrder[row], expectedOrder[row]) << "row " << row;
        EXPECT_EQ(outValues[row], expectedValues[row]) << "row " << row;
        EXPECT_EQ(outSums[row], expectedSums[row]) << "row " << row;
    }

    Release(&result);
}

TEST(WindowExec, RangeSumSharesValueAcrossOrderPeers) {
    // Default frame RANGE UNBOUNDED PRECEDING .. CURRENT ROW: all rows equal on
    // the order key (peers) must share the running sum through the last peer.
    std::array<int64_t, 5> o = {1, 1, 2, 3, 3};
    std::array<int64_t, 5> v = {10, 20, 5, 4, 11};

    std::vector<TColumn> columns = {
        TColumn{.Data = reinterpret_cast<char*>(o.data())},
        TColumn{.Data = reinterpret_cast<char*>(v.data())},
    };
    std::vector<TRowSet> batches = {TRowSet{
        .Columns = columns.data(),
        .ColumnCount = 2,
        .RowCount = 5,
        .Selection = nullptr,
        .RefCount = 1,
    }};
    TMockSource source({"o", "v"}, std::move(batches));

    auto root = ParsePlan(
        "(rel window (rel source \"data.parquet\") "
        "(order (o asc nulls-default)) "
        "(frame range (start unbounded-preceding) (end current-row)) "
        "(fn s sum v))",
        source);

    auto runtime = RunPlan(root);

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.RowCount, 5);

    auto* outOrder = reinterpret_cast<int64_t*>(result.Columns[0].Data);
    auto* outSums = reinterpret_cast<int64_t*>(result.Columns[2].Data);

    // Peers (o=1 -> 10+20; o=3 -> +4+11) share the cumulative value; contrast the
    // ROWS result 10,30,35,39,50.
    const std::array<int64_t, 5> expectedOrder = {1, 1, 2, 3, 3};
    const std::array<int64_t, 5> expectedSums = {30, 30, 35, 50, 50};

    for (int64_t row = 0; row < result.RowCount; ++row) {
        EXPECT_EQ(outOrder[row], expectedOrder[row]) << "row " << row;
        EXPECT_EQ(outSums[row], expectedSums[row]) << "row " << row;
    }

    Release(&result);
}

int main(int argc, char** argv) {
    NQumir::NCodeGen::TLLVMInitializer initializer;
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

namespace {

// Owns the f32 input and the runtime of a window over (k, o, nullable f32 v).
struct TF32WindowRun {
    std::vector<TColumn> Columns;
    std::unique_ptr<TMockSource> Source;
    std::unique_ptr<TTestRuntime> Runtime;

    TF32WindowRun(
        const std::string& tail,
        std::array<int64_t, 6>& keys,
        std::array<int64_t, 6>& order,
        std::array<float, 6>& values,
        std::array<uint8_t, 1>& valueMask)
    {
        Columns = {
            TColumn{.Data = reinterpret_cast<char*>(keys.data())},
            TColumn{.Data = reinterpret_cast<char*>(order.data())},
            TColumn{.Data = reinterpret_cast<char*>(values.data()), .Mask = valueMask.data()},
        };
        std::vector<TRowSet> batches = {TRowSet{
            .Columns = Columns.data(),
            .ColumnCount = 3,
            .RowCount = static_cast<int64_t>(keys.size()),
            .Selection = nullptr,
            .RefCount = 1,
        }};
        Source = std::make_unique<TMockSource>(
            std::vector<std::string>{"k", "o", "v"},
            std::move(batches),
            std::vector<NQumir::NAst::TTypePtr>{
                std::make_shared<NQumir::NAst::TIntegerType>(),
                std::make_shared<NQumir::NAst::TIntegerType>(),
                std::make_shared<TNullable>(std::make_shared<NQumir::NAst::TFloatType>(
                    NQumir::NAst::TFloatType::F32)),
            });
        Runtime = RunPlan(ParsePlan(
            "(rel window (rel source \"data.parquet\") (partition k) " + tail, *Source));
    }
};

} // namespace

TEST(WindowExec, PrefixSumAndMaxOfF32WidenSumAndKeepMax) {
    const float inf = std::numeric_limits<float>::infinity();
    std::array<int64_t, 6> keys = {1, 1, 1, 1, 2, 2};
    std::array<int64_t, 6> order = {1, 2, 3, 4, 1, 2};
    std::array<float, 6> values = {0.1f, 5.0f, 0.2f, inf, 3.0f, -0.0f};
    std::array<uint8_t, 1> valueMask = {0b00111101}; // row 1 is NULL

    TF32WindowRun run(
        "(order (o asc nulls-default)) "
        "(frame rows (start unbounded-preceding) (end current-row)) "
        "(fn running_sum sum v) (fn running_max max v))",
        keys, order, values, valueMask);
    auto& runtime = run.Runtime;

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.ColumnCount, 5);
    ASSERT_EQ(result.RowCount, 6);

    auto sumType = NQumir::NAst::TMaybeType<NQumir::NAst::TFloatType>(
        UnwrapNullableType(static_cast<NQumir::NAst::TStructType*>(
            runtime->OutputType().get())->Fields[3].second));
    auto maxType = NQumir::NAst::TMaybeType<NQumir::NAst::TFloatType>(
        UnwrapNullableType(static_cast<NQumir::NAst::TStructType*>(
            runtime->OutputType().get())->Fields[4].second));
    ASSERT_TRUE(sumType);
    ASSERT_TRUE(maxType);
    EXPECT_EQ(sumType.Cast()->BitWidth(), 64);
    EXPECT_EQ(maxType.Cast()->BitWidth(), 32);

    auto* sums = reinterpret_cast<double*>(result.Columns[3].Data);
    auto* maxes = reinterpret_cast<float*>(result.Columns[4].Data);
    const std::array<bool, 6> expectedValid = {true, true, true, true, true, true};
    const std::array<double, 6> expectedSums = {
        static_cast<double>(0.1f), static_cast<double>(0.1f),
        static_cast<double>(0.1f) + static_cast<double>(0.2f),
        std::numeric_limits<double>::infinity(), 3.0, 3.0};
    const std::array<float, 6> expectedMax = {0.1f, 0.1f, 0.2f, inf, 3.0f, 3.0f};
    for (int64_t row = 0; row < result.RowCount; ++row) {
        EXPECT_EQ(IsValid(result.Columns[3], row), expectedValid[row]) << "row " << row;
        EXPECT_EQ(sums[row], expectedSums[row]) << "row " << row;
        EXPECT_EQ(maxes[row], expectedMax[row]) << "row " << row;
    }
    Release(&result);
}

TEST(WindowExec, PartitionSumAndAvgOfF32AccumulateInF64) {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    std::array<int64_t, 6> keys = {1, 1, 2, 2, 3, 3};
    std::array<int64_t, 6> order = {1, 2, 1, 2, 1, 2};
    std::array<float, 6> values = {0.1f, 0.2f, nan, 1.0f, 4.0f, 5.0f};
    std::array<uint8_t, 1> valueMask = {0b00001111}; // partition 3 is all NULL

    TF32WindowRun run(
        "(fn total sum v) (fn mean avg v))", keys, order, values, valueMask);
    auto& runtime = run.Runtime;

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.ColumnCount, 5);
    ASSERT_EQ(result.RowCount, 6);
    auto* sums = reinterpret_cast<double*>(result.Columns[3].Data);
    auto* avgs = reinterpret_cast<double*>(result.Columns[4].Data);

    const double sum1 = static_cast<double>(0.1f) + static_cast<double>(0.2f);
    for (int64_t row : {0, 1}) {
        EXPECT_EQ(sums[row], sum1) << "row " << row;
        EXPECT_EQ(avgs[row], sum1 / 2.0) << "row " << row;
    }
    for (int64_t row : {2, 3}) {
        EXPECT_TRUE(std::isnan(sums[row])) << "row " << row;
        EXPECT_TRUE(std::isnan(avgs[row])) << "row " << row;
    }
    for (int64_t row : {4, 5}) {
        EXPECT_FALSE(IsValid(result.Columns[3], row)) << "row " << row;
        EXPECT_FALSE(IsValid(result.Columns[4], row)) << "row " << row;
    }
    Release(&result);
}
