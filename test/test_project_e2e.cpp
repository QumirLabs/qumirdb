#include <gtest/gtest.h>
#include "mock_source.h"

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>
#include <qumir/parser/type.h>

#include "plan_runner.h"
#include <qdb/io/io.h>
#include <qdb/modules/qumirdb_runtime.h>
#include <qdb/plan/build.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/passes/column_pruning.h>
#include <qdb/plan/passes/typing.h>
#include <qdb/plan/pipeline.h>
#include <qdb/plan/types/decimal.h>
#include <qdb/plan/types/nullable.h>
#include <qdb/sexp/parser.h>
#include <qdb/sql/parser.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace NQdb;
using namespace NQdb::NSexp;
using namespace NQumir::NAst::NCore;
using namespace NQumir::NAst;

namespace {


std::unique_ptr<TTestRuntime> Plan(
    const std::string& sexp,
    ISource& src,
    NScheduler::TSettings schedulerSettings = {})
{
    TRelParserOptions opts;
    opts.SourceFactory = [&](std::string_view path, NQumir::TLocation) -> TOperatorPtr {
        return std::make_shared<TSourceOperator>(src, std::string(path));
    };
    TParser parser;
    for (auto& [name, fn] : MakeRelParsers(std::move(opts))) {
        parser.NodeParsers[name] = std::move(fn);
    }
    std::istringstream in(sexp);
    TTokenStream ts(in);
    auto parsed = parser.Parse(ts);
    if (!parsed) throw std::runtime_error(parsed.error().ToString());
    auto root = std::static_pointer_cast<IOperator>(*parsed);
    AnnotateTypes(root);
    ApplyColumnPruning(root);
    return RunPlan(root, schedulerSettings);
}

std::unique_ptr<TTestRuntime> SqlPlan(
    const std::string& sql,
    const std::unordered_map<std::string, ISource*>& sources,
    NScheduler::TSettings settings = {},
    bool optimize = false)
{
    std::istringstream in(sql);
    NSql::TTokenStream ts(in);
    NSql::TParser parser;
    auto parsed = parser.Parse(ts);
    if (!parsed) {
        throw std::runtime_error(parsed.error().ToString());
    }
    auto root = BuildPlan(*parsed, [&](std::string_view table)
        -> std::expected<TOperatorPtr, NQumir::TError>
    {
        auto it = sources.find(std::string(table));
        if (it == sources.end()) {
            return std::unexpected(NQumir::TError(
                "unknown test table: " + std::string(table)));
        }
        return std::make_shared<TSourceOperator>(*it->second, std::string(table));
    });
    if (!root) {
        throw std::runtime_error(root.error().ToString());
    }
    if (optimize) {
        ApplyPlanPasses(*root);
    } else {
        AnnotateTypes(*root);
        ApplyColumnPruning(*root);
    }
    return RunPlan(*root, settings);
}


} // namespace

bool IsValid(const TColumn& column, int64_t row) {
    if (!column.Mask) {
        return true;
    }
    const int64_t bit = column.MaskBitOffset + row;
    return ((column.Mask[bit / 8] >> (bit % 8)) & 1) != 0;
}

TEST(ProjectE2E, NamedStructProjectionFreezesItsFlattenedLayout) {
    auto i64 = std::make_shared<TIntegerType>();
    auto structure = std::make_shared<TNamedType>("Pair", std::make_shared<TStructType>(
        std::vector<std::pair<std::string, TTypePtr>>{{"first", i64}, {"second", i64}}));
    TMockSource source(TMockColumns{}, {{"x", i64}, {"pair", structure}});
    auto input = std::make_shared<TSourceOperator>(source, "t");
    TOperatorPtr plan = std::make_shared<TProjectOperator>(input, std::vector<TProjectionSpec>{
        {.Name = "unused", .Expression = std::make_shared<TIdentExpr>(NQumir::TLocation{}, "x")},
        {.Expression = std::make_shared<TIdentExpr>(NQumir::TLocation{}, "pair"),
            .ImplicitName = true},
    });
    AnnotateTypes(plan);
    ASSERT_EQ(plan->OutputColumns()->Fields.size(), 3);

    // Pruning the leading column must not renumber the struct's colN outputs
    // that consumers already reference.
    auto project = TMaybeOp<TProjectOperator>(plan);
    ASSERT_TRUE(project);
    EXPECT_EQ(project.Cast()->Projections()[1].ColumnAliases,
        (std::vector<std::string>{"col1", "col2"}));
    TColumnSet demand{"col2"};
    ApplyColumnPruning(plan, &demand, nullptr);
    AnnotateTypes(plan);
    ASSERT_EQ(project.Cast()->Projections().size(), 1);
    EXPECT_FALSE(project.Cast()->Projections()[0].ImplicitName);
    ASSERT_EQ(plan->OutputColumns()->Fields.size(), 2);
    EXPECT_EQ(plan->OutputColumns()->Fields[0].first, "col1");
    EXPECT_EQ(plan->OutputColumns()->Fields[1].first, "col2");
}

TEST(ProjectE2E, IdentZeroCopyPlusComputed) {
    std::array<double, 3> x = {10.0, 20.0, 4.0};
    std::array<double, 3> z = {0.5, 0.25, 0.75};
    std::array<double, 3> w = {7.0, 8.0, 9.0}; // unused column -> pruned away
    std::array<TColumn, 3> cols = {
        TColumn{.Data = reinterpret_cast<char*>(x.data())},
        TColumn{.Data = reinterpret_cast<char*>(z.data())},
        TColumn{.Data = reinterpret_cast<char*>(w.data())},
    };
    TRowSet batch{.Columns = cols.data(), .ColumnCount = 3, .RowCount = 3, .RefCount = 1};
    NQdb::TMockSource src({"x", "z", "w"},
        {std::make_shared<TFloatType>(), std::make_shared<TFloatType>(),
         std::make_shared<TFloatType>()},
        {batch});

    // kept_x is a zero-copy ident; c is computed x * (2.0 - z).
    auto plan = Plan(
        "(rel project (rel source \"L\") (kept_x x) (c (* x (- (: 2.0 f64) z))))", src);

    // Output schema: kept_x (f64), c (f64).
    auto* outType = static_cast<TStructType*>(plan->OutputType().get());
    ASSERT_EQ(outType->Fields.size(), 2u);
    EXPECT_EQ(outType->Fields[0].first, "kept_x");
    EXPECT_EQ(outType->Fields[1].first, "c");
    EXPECT_TRUE(TMaybeType<TFloatType>(outType->Fields[1].second));

    TRowSet out{};
    ASSERT_TRUE(plan->Next(out));
    ASSERT_EQ(out.ColumnCount, 2);
    ASSERT_EQ(out.RowCount, 3);
    const auto* keptX = reinterpret_cast<const double*>(out.Columns[0].Data);
    const auto* c = reinterpret_cast<const double*>(out.Columns[1].Data);
    // kept_x is zero-copy: points into the input column.
    EXPECT_EQ(out.Columns[0].Data, reinterpret_cast<const char*>(x.data()));
    for (int i = 0; i < 3; ++i) {
        EXPECT_DOUBLE_EQ(keptX[i], x[i]);
        EXPECT_DOUBLE_EQ(c[i], x[i] * (2.0 - z[i]));
    }
    Release(&out);
    EXPECT_FALSE(plan->Next(out));
}

TEST(ProjectE2E, NormalizesBareNullIfBranchBeforeProjectKernel) {
    std::array<int64_t, 4> values = {1, 4, 2, 5};
    std::array<TColumn, 1> cols = {
        TColumn{.Data = reinterpret_cast<char*>(values.data())},
    };
    TRowSet batch{.Columns = cols.data(), .ColumnCount = 1, .RowCount = 4, .RefCount = 1};
    NQdb::TMockSource src({"value"}, {std::make_shared<TIntegerType>()}, {batch});

    auto plan = Plan(
        "(rel project (rel source \"L\") "
        "(maybe_value (if (< value (: 3 i64)) value (call qdb_sql_null))))",
        src);

    auto* outType = static_cast<TStructType*>(plan->OutputType().get());
    ASSERT_EQ(outType->Fields.size(), 1u);
    ASSERT_TRUE(IsNullableType(outType->Fields[0].second));
    EXPECT_TRUE(TMaybeType<TIntegerType>(UnwrapNullableType(outType->Fields[0].second)));

    TRowSet out{};
    ASSERT_TRUE(plan->Next(out));
    ASSERT_EQ(out.ColumnCount, 1);
    ASSERT_EQ(out.RowCount, 4);
    ASSERT_NE(out.Columns[0].Mask, nullptr);
    const auto* maybeValue = reinterpret_cast<const int64_t*>(out.Columns[0].Data);
    EXPECT_TRUE(IsValid(out.Columns[0], 0));
    EXPECT_FALSE(IsValid(out.Columns[0], 1));
    EXPECT_TRUE(IsValid(out.Columns[0], 2));
    EXPECT_FALSE(IsValid(out.Columns[0], 3));
    EXPECT_EQ(maybeValue[0], values[0]);
    EXPECT_EQ(maybeValue[2], values[2]);
    Release(&out);
    EXPECT_FALSE(plan->Next(out));
}

TEST(ProjectE2E, NormalizesBareNullStringIfBranchBeforeProjectKernel) {
    std::array<int64_t, 4> flags = {0, 1, 0, 1};
    const std::string labels = "abcd";
    std::array<int64_t, 5> offsets = {0, 1, 2, 3, 4};
    std::array<TColumn, 2> cols = {
        TColumn{.Data = reinterpret_cast<char*>(flags.data())},
        TColumn{
            .Data = const_cast<char*>(labels.data()),
            .Offsets = offsets.data(),
            .OffsetWidth = 8,
        },
    };
    TRowSet batch{.Columns = cols.data(), .ColumnCount = 2, .RowCount = 4, .RefCount = 1};
    NQdb::TMockSource src(
        {"flag", "label"},
        {std::make_shared<TIntegerType>(), std::make_shared<TStringType>()},
        {batch});

    auto plan = Plan(
        "(rel project (rel source \"L\") "
        "(maybe_label (if (call qdb_is_true (== flag (: 0 i64))) "
        "label (call qdb_sql_null))))",
        src);

    auto* outType = static_cast<TStructType*>(plan->OutputType().get());
    ASSERT_EQ(outType->Fields.size(), 1u);
    ASSERT_TRUE(IsNullableType(outType->Fields[0].second));
    EXPECT_TRUE(TMaybeType<TStringType>(UnwrapNullableType(outType->Fields[0].second)));

    TRowSet out{};
    ASSERT_TRUE(plan->Next(out));
    ASSERT_EQ(out.ColumnCount, 1);
    ASSERT_EQ(out.RowCount, 4);
    ASSERT_NE(out.Columns[0].Mask, nullptr);
    EXPECT_TRUE(IsValid(out.Columns[0], 0));
    EXPECT_FALSE(IsValid(out.Columns[0], 1));
    EXPECT_TRUE(IsValid(out.Columns[0], 2));
    EXPECT_FALSE(IsValid(out.Columns[0], 3));
    Release(&out);
    EXPECT_FALSE(plan->Next(out));
}

TEST(ProjectE2E, StringLiteralIsNotFlattenedAsStringViewStruct) {
    NQdb::TMockSource src({"value"});
    auto plan = SqlPlan("SELECT 's' AS sale_type FROM t", {{"t", &src}});

    auto* outType = static_cast<TStructType*>(plan->OutputType().get());
    ASSERT_EQ(outType->Fields.size(), 1u);
    EXPECT_EQ(outType->Fields[0].first, "sale_type");
    EXPECT_TRUE(TMaybeType<TStringType>(outType->Fields[0].second));
}

TEST(ProjectE2E, DecimalProjectComputesWithBinIntStorage) {
    std::array<qdb_bin_int, 3> amount = {{
        {.Lo = 1000, .Hi = 0}, // 10.00
        {.Lo = 1250, .Hi = 0}, // 12.50
        {.Lo = 2500, .Hi = 0}, // 25.00
    }};
    std::array<TColumn, 1> cols = {
        TColumn{.Data = reinterpret_cast<char*>(amount.data())},
    };
    TRowSet batch{.Columns = cols.data(), .ColumnCount = 1, .RowCount = 3, .RefCount = 1};
    NQdb::TMockSource src({"amount"}, {std::make_shared<NQdb::TDecimal>(7, 2)}, {batch});

    auto plan = Plan(
        "(rel project (rel source \"L\") "
        "(plus_one (+ amount 1)) "
        "(twice (* amount 2)) "
        "(half (/ amount 2)))",
        src);

    auto* outType = static_cast<TStructType*>(plan->OutputType().get());
    ASSERT_EQ(outType->Fields.size(), 3u);
    EXPECT_TRUE(NQdb::IsDecimalType(outType->Fields[0].second));
    EXPECT_TRUE(NQdb::IsDecimalType(outType->Fields[1].second));
    EXPECT_TRUE(NQdb::IsDecimalType(outType->Fields[2].second));

    TRowSet out{};
    ASSERT_TRUE(plan->Next(out));
    ASSERT_EQ(out.ColumnCount, 3);
    ASSERT_EQ(out.RowCount, 3);
    const auto* plusOne = reinterpret_cast<const qdb_bin_int*>(out.Columns[0].Data);
    const auto* twice = reinterpret_cast<const qdb_bin_int*>(out.Columns[1].Data);
    const auto* half = reinterpret_cast<const qdb_bin_int*>(out.Columns[2].Data);
    EXPECT_EQ(plusOne[0].Lo, 1100u);
    EXPECT_EQ(plusOne[1].Lo, 1350u);
    EXPECT_EQ(plusOne[2].Lo, 2600u);
    EXPECT_EQ(twice[0].Lo, 2000u);
    EXPECT_EQ(twice[1].Lo, 2500u);
    EXPECT_EQ(twice[2].Lo, 5000u);
    EXPECT_EQ(half[0].Lo, 500u);
    EXPECT_EQ(half[1].Lo, 625u);
    EXPECT_EQ(half[2].Lo, 1250u);
    Release(&out);
    EXPECT_FALSE(plan->Next(out));
}

TEST(ProjectE2E, DecimalDecimalDivisionComputesWithScaledBinIntStorage) {
    std::array<qdb_bin_int, 3> amount = {{
        {.Lo = 1000, .Hi = 0}, // 10.00
        {.Lo = 1250, .Hi = 0}, // 12.50
        {.Lo = 2500, .Hi = 0}, // 25.00
    }};
    std::array<qdb_bin_int, 3> divisor = {{
        {.Lo = 200, .Hi = 0}, // 2.00
        {.Lo = 250, .Hi = 0}, // 2.50
        {.Lo = 500, .Hi = 0}, // 5.00
    }};
    std::array<TColumn, 2> cols = {
        TColumn{.Data = reinterpret_cast<char*>(amount.data())},
        TColumn{.Data = reinterpret_cast<char*>(divisor.data())},
    };
    TRowSet batch{.Columns = cols.data(), .ColumnCount = 2, .RowCount = 3, .RefCount = 1};
    NQdb::TMockSource src(
        {"amount", "divisor"},
        {std::make_shared<NQdb::TDecimal>(7, 2),
         std::make_shared<NQdb::TDecimal>(7, 2)},
        {batch});

    auto plan = Plan(
        "(rel project (rel source \"L\") (ratio (/ amount divisor)))",
        src);

    auto* outType = static_cast<TStructType*>(plan->OutputType().get());
    ASSERT_EQ(outType->Fields.size(), 1u);
    auto spec = NQdb::DecimalSpecOf(outType->Fields[0].second);
    ASSERT_TRUE(spec.has_value());
    EXPECT_EQ(spec->Precision, 17);
    EXPECT_EQ(spec->Scale, 10);

    TRowSet out{};
    ASSERT_TRUE(plan->Next(out));
    ASSERT_EQ(out.ColumnCount, 1);
    ASSERT_EQ(out.RowCount, 3);
    const auto* ratio = reinterpret_cast<const qdb_bin_int*>(out.Columns[0].Data);
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(ratio[i].Lo, 50000000000u);
        EXPECT_EQ(ratio[i].Hi, 0u);
    }
    Release(&out);
    EXPECT_FALSE(plan->Next(out));
}

TEST(ProjectE2E, DecimalAverageShapeDividesSumByCount) {
    std::array<int64_t, 3> keys = {1, 1, 1};
    std::array<qdb_bin_int, 3> amount = {{
        {.Lo = 1000, .Hi = 0}, // 10.00
        {.Lo = 2000, .Hi = 0}, // 20.00
        {.Lo = 3000, .Hi = 0}, // 30.00
    }};
    std::array<TColumn, 2> cols = {
        TColumn{.Data = reinterpret_cast<char*>(keys.data())},
        TColumn{.Data = reinterpret_cast<char*>(amount.data())},
    };
    TRowSet batch{.Columns = cols.data(), .ColumnCount = 2, .RowCount = 3, .RefCount = 1};
    NQdb::TMockSource src(
        {"k", "amount"},
        {std::make_shared<TIntegerType>(), std::make_shared<NQdb::TDecimal>(7, 2)},
        {batch});

    auto plan = Plan(
        "(rel project "
        "  (rel aggregate (rel source \"L\") (keys k) "
        "    (agg s sum amount) (agg c count)) "
        "  (k k) (avg (/ s c)))",
        src);

    auto* outType = static_cast<TStructType*>(plan->OutputType().get());
    ASSERT_EQ(outType->Fields.size(), 2u);
    EXPECT_TRUE(NQdb::IsDecimalType(outType->Fields[1].second));

    TRowSet out{};
    ASSERT_TRUE(plan->Next(out));
    ASSERT_EQ(out.ColumnCount, 2);
    ASSERT_EQ(out.RowCount, 1);
    EXPECT_EQ(reinterpret_cast<const int64_t*>(out.Columns[0].Data)[0], 1);
    const auto* avg = reinterpret_cast<const qdb_bin_int*>(out.Columns[1].Data);
    EXPECT_EQ(avg[0].Lo, 2000u);
    EXPECT_EQ(avg[0].Hi, 0u);
    Release(&out);
    EXPECT_FALSE(plan->Next(out));
}

TEST(ProjectE2E, NullableDecimalProjectPropagatesValidity) {
    std::array<qdb_bin_int, 3> amount = {{
        {.Lo = 1000, .Hi = 0}, // 10.00
        {.Lo = 1250, .Hi = 0}, // NULL by mask
        {.Lo = 2500, .Hi = 0}, // 25.00
    }};
    std::array<uint8_t, 1> mask = {0b00000101};
    std::array<TColumn, 1> cols = {
        TColumn{
            .Data = reinterpret_cast<char*>(amount.data()),
            .Mask = mask.data(),
        },
    };
    TRowSet batch{.Columns = cols.data(), .ColumnCount = 1, .RowCount = 3, .RefCount = 1};
    NQdb::TMockSource src(
        {"amount"},
        {std::make_shared<NQdb::TNullable>(std::make_shared<NQdb::TDecimal>(7, 2))},
        {batch});

    auto plan = Plan(
        "(rel project (rel source \"L\") (plus_one (+ amount 1)))",
        src);

    auto* outType = static_cast<TStructType*>(plan->OutputType().get());
    ASSERT_EQ(outType->Fields.size(), 1u);
    ASSERT_TRUE(NQdb::IsNullableType(outType->Fields[0].second));
    EXPECT_TRUE(NQdb::IsDecimalType(NQdb::UnwrapNullableType(outType->Fields[0].second)));

    TRowSet out{};
    ASSERT_TRUE(plan->Next(out));
    ASSERT_EQ(out.ColumnCount, 1);
    ASSERT_EQ(out.RowCount, 3);
    ASSERT_NE(out.Columns[0].Mask, nullptr);
    const auto* plusOne = reinterpret_cast<const qdb_bin_int*>(out.Columns[0].Data);
    EXPECT_TRUE(IsValid(out.Columns[0], 0));
    EXPECT_FALSE(IsValid(out.Columns[0], 1));
    EXPECT_TRUE(IsValid(out.Columns[0], 2));
    EXPECT_EQ(plusOne[0].Lo, 1100u);
    EXPECT_EQ(plusOne[2].Lo, 2600u);
    Release(&out);
    EXPECT_FALSE(plan->Next(out));
}

TEST(ProjectE2E, DecimalDivisionByZeroIsNull) {
    std::array<qdb_bin_int, 3> amount = {{
        {.Lo = 1000, .Hi = 0}, // 10.00
        {.Lo = 1250, .Hi = 0}, // 12.50
        {.Lo = 2500, .Hi = 0}, // 25.00
    }};
    std::array<qdb_bin_int, 3> divisor = {{
        {.Lo = 0, .Hi = 0}, // 0.00
        {.Lo = 250, .Hi = 0}, // 2.50
        {.Lo = 500, .Hi = 0}, // 5.00
    }};
    std::array<int64_t, 3> count = {2, 0, 5};
    std::array<TColumn, 3> cols = {
        TColumn{.Data = reinterpret_cast<char*>(amount.data())},
        TColumn{.Data = reinterpret_cast<char*>(divisor.data())},
        TColumn{.Data = reinterpret_cast<char*>(count.data())},
    };
    TRowSet batch{.Columns = cols.data(), .ColumnCount = 3, .RowCount = 3, .RefCount = 1};
    NQdb::TMockSource src(
        {"amount", "divisor", "cnt"},
        {std::make_shared<NQdb::TDecimal>(7, 2),
         std::make_shared<NQdb::TDecimal>(7, 2),
         std::make_shared<TIntegerType>()},
        {batch});

    auto plan = Plan(
        "(rel project (rel source \"L\") "
        "(ratio (/ amount divisor)) "
        "(share (/ amount cnt)))",
        src);

    auto* outType = static_cast<TStructType*>(plan->OutputType().get());
    ASSERT_EQ(outType->Fields.size(), 2u);
    EXPECT_TRUE(NQdb::IsNullableType(outType->Fields[0].second));
    EXPECT_TRUE(NQdb::IsNullableType(outType->Fields[1].second));

    TRowSet out{};
    ASSERT_TRUE(plan->Next(out));
    ASSERT_EQ(out.ColumnCount, 2);
    ASSERT_EQ(out.RowCount, 3);
    const auto* ratio = reinterpret_cast<const qdb_bin_int*>(out.Columns[0].Data);
    const auto* share = reinterpret_cast<const qdb_bin_int*>(out.Columns[1].Data);
    EXPECT_FALSE(IsValid(out.Columns[0], 0));
    EXPECT_TRUE(IsValid(out.Columns[0], 1));
    EXPECT_TRUE(IsValid(out.Columns[0], 2));
    EXPECT_EQ(ratio[1].Lo, 50000000000u); // 5.0000000000
    EXPECT_EQ(ratio[2].Lo, 50000000000u);
    EXPECT_TRUE(IsValid(out.Columns[1], 0));
    EXPECT_FALSE(IsValid(out.Columns[1], 1));
    EXPECT_TRUE(IsValid(out.Columns[1], 2));
    EXPECT_EQ(share[0].Lo, 500u); // 5.00
    EXPECT_EQ(share[2].Lo, 500u);
    Release(&out);
    EXPECT_FALSE(plan->Next(out));
}

TEST(FilterE2E, DecimalFilterAlignsIntegerLiteralScale) {
    std::array<qdb_bin_int, 3> amount = {{
        {.Lo = 1000, .Hi = 0}, // 10.00
        {.Lo = 1250, .Hi = 0}, // 12.50
        {.Lo = 2500, .Hi = 0}, // 25.00
    }};
    std::array<TColumn, 1> cols = {
        TColumn{.Data = reinterpret_cast<char*>(amount.data())},
    };
    TRowSet batch{.Columns = cols.data(), .ColumnCount = 1, .RowCount = 3, .RefCount = 1};
    NQdb::TMockSource src({"amount"}, {std::make_shared<NQdb::TDecimal>(7, 2)}, {batch});

    auto plan = Plan("(rel filter (rel source \"L\") (> amount 12))", src);

    TRowSet out{};
    ASSERT_TRUE(plan->Next(out));
    ASSERT_NE(out.Selection, nullptr);
    EXPECT_EQ(
        std::vector<uint8_t>(out.Selection, out.Selection + out.RowCount),
        (std::vector<uint8_t>{0, 0xff, 0xff}));
    Release(&out);
    EXPECT_FALSE(plan->Next(out));
}

TEST(FilterE2E, NullableDecimalDivisionComparesWithFloatLiteral) {
    std::array<int64_t, 3> a = {8, 10, 1};
    std::array<int64_t, 3> b = {10, 10, 2};
    std::array<uint8_t, 1> bMask = {0b00000011};
    std::array<TColumn, 2> cols = {
        TColumn{.Data = reinterpret_cast<char*>(a.data())},
        TColumn{.Data = reinterpret_cast<char*>(b.data()), .Mask = bMask.data()},
    };
    TRowSet batch{.Columns = cols.data(), .ColumnCount = 2, .RowCount = 3, .RefCount = 1};
    auto nullableI64 = std::make_shared<NQdb::TNullable>(std::make_shared<TIntegerType>());
    NQdb::TMockSource src({"a", "b"}, {nullableI64, nullableI64}, {batch});

    auto plan = Plan(
        "(rel filter (rel source \"L\") "
        "  (< (/ (cast a <named DECIMAL [17 2]>) "
        "        (cast b <named DECIMAL [17 2]>)) 0.9))",
        src);

    TRowSet out{};
    ASSERT_TRUE(plan->Next(out));
    ASSERT_NE(out.Selection, nullptr);
    EXPECT_EQ(
        std::vector<uint8_t>(out.Selection, out.Selection + out.RowCount),
        (std::vector<uint8_t>{0xff, 0, 0}));
    Release(&out);
    EXPECT_FALSE(plan->Next(out));
}

TEST(FilterE2E, DecimalAbsDivisionComparesWithFloatLiteral) {
    std::array<qdb_bin_int, 3> sumSales = {{
        {.Lo = 1000, .Hi = 0},     // 10.00
        {.Lo = 1950, .Hi = 0},     // 19.50
        {.Lo = 3000, .Hi = 0},     // 30.00
    }};
    std::array<qdb_bin_int, 3> avgSales = {{
        {.Lo = 20000000, .Hi = 0}, // 20.000000
        {.Lo = 20000000, .Hi = 0}, // 20.000000
        {.Lo = 20000000, .Hi = 0}, // 20.000000
    }};
    std::array<TColumn, 2> cols = {
        TColumn{.Data = reinterpret_cast<char*>(sumSales.data())},
        TColumn{.Data = reinterpret_cast<char*>(avgSales.data())},
    };
    TRowSet batch{.Columns = cols.data(), .ColumnCount = 2, .RowCount = 3, .RefCount = 1};
    NQdb::TMockSource src(
        {"sum_sales", "avg_sales"},
        {std::make_shared<NQdb::TDecimal>(15, 2),
         std::make_shared<NQdb::TDecimal>(19, 6)},
        {batch});

    auto plan = Plan(R"qdb(
(rel filter (rel source "L")
  (> (/ (call abs (- sum_sales avg_sales)) avg_sales)
     0.10000000000000001))
)qdb", src);

    TRowSet out{};
    ASSERT_TRUE(plan->Next(out));
    ASSERT_NE(out.Selection, nullptr);
    EXPECT_EQ(
        std::vector<uint8_t>(out.Selection, out.Selection + out.RowCount),
        (std::vector<uint8_t>{0xff, 0, 0xff}));
    Release(&out);
    EXPECT_FALSE(plan->Next(out));
}

TEST(FilterE2E, FloatAbsDivisionComparesWithFloatLiteral) {
    std::array<double, 3> sumSales = {
        10.0,
        19.5,
        30.0,
    };
    std::array<double, 3> avgSales = {
        20.0,
        20.0,
        20.0,
    };
    std::array<TColumn, 2> cols = {
        TColumn{.Data = reinterpret_cast<char*>(sumSales.data())},
        TColumn{.Data = reinterpret_cast<char*>(avgSales.data())},
    };
    TRowSet batch{.Columns = cols.data(), .ColumnCount = 2, .RowCount = 3, .RefCount = 1};
    NQdb::TMockSource src(
        {"sum_sales", "avg_sales"},
        {std::make_shared<NQumir::NAst::TFloatType>(),
         std::make_shared<NQumir::NAst::TFloatType>()},
        {batch});

    auto plan = Plan(R"qdb(
(rel filter (rel source "L")
  (> (/ (call abs (- sum_sales avg_sales)) avg_sales)
     0.10000000000000001))
)qdb", src);

    TRowSet out{};
    ASSERT_TRUE(plan->Next(out));
    ASSERT_NE(out.Selection, nullptr);
    EXPECT_EQ(
        std::vector<uint8_t>(out.Selection, out.Selection + out.RowCount),
        (std::vector<uint8_t>{0xff, 0, 0xff}));
    Release(&out);
    EXPECT_FALSE(plan->Next(out));
}

TEST(FilterE2E, NullableFloatAbsDivisionComparesWithFloatLiteral) {
    std::array<double, 4> sumSales = {
        10.0,
        19.5,
        30.0,
        50.0,
    };
    std::array<double, 4> avgSales = {
        20.0,
        20.0,
        20.0,
        50.0,
    };
    std::array<uint8_t, 1> sumMask = {0b00000111};
    std::array<uint8_t, 1> avgMask = {0b00001111};
    std::array<TColumn, 2> cols = {
        TColumn{.Data = reinterpret_cast<char*>(sumSales.data()), .Mask = sumMask.data()},
        TColumn{.Data = reinterpret_cast<char*>(avgSales.data()), .Mask = avgMask.data()},
    };
    TRowSet batch{.Columns = cols.data(), .ColumnCount = 2, .RowCount = 4, .RefCount = 1};
    auto nullableF64 = std::make_shared<NQdb::TNullable>(
        std::make_shared<NQumir::NAst::TFloatType>());
    NQdb::TMockSource src(
        {"sum_sales", "avg_sales"},
        {nullableF64, nullableF64},
        {batch});

    auto plan = Plan(R"qdb(
(rel filter (rel source "L")
  (> (/ (call abs (- sum_sales avg_sales)) avg_sales)
     0.10000000000000001))
)qdb", src);

    TRowSet out{};
    ASSERT_TRUE(plan->Next(out));
    ASSERT_NE(out.Selection, nullptr);
    EXPECT_EQ(
        std::vector<uint8_t>(out.Selection, out.Selection + out.RowCount),
        (std::vector<uint8_t>{0xff, 0, 0xff, 0}));
    Release(&out);
    EXPECT_FALSE(plan->Next(out));
}

TEST(ProjectE2E, FloatAbsClearsNegativeZero) {
    std::array<double, 2> values = {
        -0.0,
        -2.5,
    };
    std::array<TColumn, 1> cols = {
        TColumn{.Data = reinterpret_cast<char*>(values.data())},
    };
    TRowSet batch{.Columns = cols.data(), .ColumnCount = 1, .RowCount = 2, .RefCount = 1};
    NQdb::TMockSource src(
        {"v"},
        {std::make_shared<NQumir::NAst::TFloatType>()},
        {batch});

    auto plan = Plan(R"qdb(
(rel project (rel source "L")
  (a (call abs v)))
)qdb", src);

    TRowSet out{};
    ASSERT_TRUE(plan->Next(out));
    ASSERT_EQ(out.RowCount, 2);
    const auto* absValues = reinterpret_cast<const double*>(out.Columns[0].Data);
    EXPECT_FALSE(std::signbit(absValues[0]));
    EXPECT_EQ(absValues[0], 0.0);
    EXPECT_EQ(absValues[1], 2.5);
    Release(&out);
    EXPECT_FALSE(plan->Next(out));
}

TEST(RuntimeAbs, IntegerMinThrows) {
    EXPECT_EQ(qdb_abs_i32(-7), 7);
    EXPECT_EQ(qdb_abs_i64(-7), 7);
    EXPECT_THROW(
        qdb_abs_i32(std::numeric_limits<int32_t>::min()),
        std::overflow_error);
    EXPECT_THROW(
        qdb_abs_i64(std::numeric_limits<int64_t>::min()),
        std::overflow_error);
}

TEST(RuntimeAbs, DecimalMinThrows) {
    qdb_bin_int minValue{.Lo = 0, .Hi = uint64_t{1} << 63};
    EXPECT_THROW(qdb_decimal_neg(minValue), std::overflow_error);
}

TEST(FilterE2E, DecimalOutlierPredicateCombinesWithIntegerConjuncts) {
    std::array<int64_t, 4> year = {1999, 2000, 2000, 2001};
    std::array<qdb_bin_int, 4> sumSales = {{
        {.Lo = 500, .Hi = 0},      // 5.00
        {.Lo = 1000, .Hi = 0},     // 10.00
        {.Lo = 3000, .Hi = 0},     // 30.00
        {.Lo = 5000, .Hi = 0},     // 50.00
    }};
    std::array<qdb_bin_int, 4> avgSales = {{
        {.Lo = 5000000, .Hi = 0},  // 5.000000
        {.Lo = 20000000, .Hi = 0}, // 20.000000
        {.Lo = 20000000, .Hi = 0}, // 20.000000
        {.Lo = 50000000, .Hi = 0}, // 50.000000
    }};
    std::array<TColumn, 3> cols = {
        TColumn{.Data = reinterpret_cast<char*>(year.data())},
        TColumn{.Data = reinterpret_cast<char*>(sumSales.data())},
        TColumn{.Data = reinterpret_cast<char*>(avgSales.data())},
    };
    TRowSet batch{.Columns = cols.data(), .ColumnCount = 3, .RowCount = 4, .RefCount = 1};
    NQdb::TMockSource src(
        {"y", "sum_sales", "avg_sales"},
        {std::make_shared<NQumir::NAst::TIntegerType>(),
         std::make_shared<NQdb::TDecimal>(15, 2),
         std::make_shared<NQdb::TDecimal>(19, 6)},
        {batch});

    auto plan = Plan(R"qdb(
(rel filter (rel source "L")
  (&& (&&
    (> (/ (call abs (- sum_sales avg_sales)) avg_sales)
       0.10000000000000001)
    (== y 2000))
    (> avg_sales 0)))
)qdb", src);

    TRowSet out{};
    ASSERT_TRUE(plan->Next(out));
    ASSERT_NE(out.Selection, nullptr);
    EXPECT_EQ(
        std::vector<uint8_t>(out.Selection, out.Selection + out.RowCount),
        (std::vector<uint8_t>{0, 0xff, 0xff, 0}));
    Release(&out);
    EXPECT_FALSE(plan->Next(out));
}

TEST(FilterE2E, DecimalOutlierPredicateWithCaseNull) {
    std::array<int64_t, 4> year = {1999, 2000, 2000, 2001};
    std::array<qdb_bin_int, 4> sumSales = {{
        {.Lo = 500, .Hi = 0},      // 5.00
        {.Lo = 1000, .Hi = 0},     // 10.00
        {.Lo = 3000, .Hi = 0},     // 30.00
        {.Lo = 5000, .Hi = 0},     // 50.00
    }};
    std::array<qdb_bin_int, 4> avgSales = {{
        {.Lo = 5000000, .Hi = 0},  // 5.000000
        {.Lo = 20000000, .Hi = 0}, // 20.000000
        {.Lo = 20000000, .Hi = 0}, // 20.000000
        {.Lo = 50000000, .Hi = 0}, // 50.000000
    }};
    std::array<TColumn, 3> cols = {
        TColumn{.Data = reinterpret_cast<char*>(year.data())},
        TColumn{.Data = reinterpret_cast<char*>(sumSales.data())},
        TColumn{.Data = reinterpret_cast<char*>(avgSales.data())},
    };
    TRowSet batch{.Columns = cols.data(), .ColumnCount = 3, .RowCount = 4, .RefCount = 1};
    NQdb::TMockSource src(
        {"y", "sum_sales", "avg_sales"},
        {std::make_shared<NQumir::NAst::TIntegerType>(),
         std::make_shared<NQdb::TDecimal>(15, 2),
         std::make_shared<NQdb::TDecimal>(19, 6)},
        {batch});

    auto plan = Plan(R"qdb(
(rel filter (rel source "L")
  (&& (&&
    (== y 2000)
    (> avg_sales 0))
    (> (if
        (call qdb_is_true (> avg_sales 0))
        (/ (call abs (- sum_sales avg_sales)) avg_sales)
        (call qdb_sql_null))
      0.10000000000000001)))
)qdb", src);

    TRowSet out{};
    ASSERT_TRUE(plan->Next(out));
    ASSERT_NE(out.Selection, nullptr);
    EXPECT_EQ(
        std::vector<uint8_t>(out.Selection, out.Selection + out.RowCount),
        (std::vector<uint8_t>{0, 0xff, 0xff, 0}));
    Release(&out);
    EXPECT_FALSE(plan->Next(out));
}

TEST(FilterE2E, PublishesSelectionFromUnaryStreamingShell) {
    std::array<int64_t, 4> values = {0, 1, 2, 3};
    std::array<TColumn, 1> cols = {
        TColumn{.Data = reinterpret_cast<char*>(values.data())},
    };
    TRowSet batch{.Columns = cols.data(), .ColumnCount = 1, .RowCount = 4, .RefCount = 1};
    NQdb::TMockSource src({"value"}, {std::make_shared<TIntegerType>()}, {batch});

    auto plan = Plan("(rel filter (rel source \"L\") (> value 1))", src);

    TRowSet out{};
    ASSERT_TRUE(plan->Next(out));
    ASSERT_NE(out.Selection, nullptr);
    EXPECT_EQ(
        std::vector<uint8_t>(out.Selection, out.Selection + out.RowCount),
        (std::vector<uint8_t>{0, 0, 0xff, 0xff}));
    Release(&out);
    EXPECT_FALSE(plan->Next(out));
}

TEST(FilterE2E, NormalizesBareNullIfBranchBeforeFilterKernel) {
    std::array<int64_t, 4> values = {1, 4, 2, 5};
    std::array<TColumn, 1> cols = {
        TColumn{.Data = reinterpret_cast<char*>(values.data())},
    };
    TRowSet batch{.Columns = cols.data(), .ColumnCount = 1, .RowCount = 4, .RefCount = 1};
    NQdb::TMockSource src({"value"}, {std::make_shared<TIntegerType>()}, {batch});

    auto plan = Plan(
        "(rel filter (rel source \"L\") "
        "(if (< value (: 3 i64)) #t (call qdb_sql_null)))",
        src);

    TRowSet out{};
    ASSERT_TRUE(plan->Next(out));
    ASSERT_NE(out.Selection, nullptr);
    EXPECT_EQ(
        std::vector<uint8_t>(out.Selection, out.Selection + out.RowCount),
        (std::vector<uint8_t>{0xff, 0, 0xff, 0}));
    Release(&out);
    EXPECT_FALSE(plan->Next(out));
}

TEST(ProjectE2E, SchedulerRuntimeRunsUnaryPipeline) {
    std::array<int64_t, 4> values = {0, 1, 2, 3};
    std::array<int64_t, 4> payload = {10, 11, 12, 13};
    std::array<TColumn, 2> cols = {
        TColumn{.Data = reinterpret_cast<char*>(values.data())},
        TColumn{.Data = reinterpret_cast<char*>(payload.data())},
    };
    TRowSet batch{
        .Columns = cols.data(),
        .ColumnCount = 2,
        .RowCount = 4,
        .RefCount = 1,
    };
    NQdb::TMockSource src(
        {"value", "payload"},
        {std::make_shared<TIntegerType>(), std::make_shared<TIntegerType>()},
        {batch});
    NScheduler::TSettings settings;
    settings.Scheduler.Mode = NScheduler::EExecutionMode::ThreadedScheduler;
    settings.Scheduler.WorkerCount = 2;

    auto plan = Plan(
        "(rel project (rel filter (rel source \"L\") (> value 1)) "
        "(value value) (shifted (+ payload 100)))",
        src,
        settings);

    TRowSet out{};
    ASSERT_TRUE(plan->Next(out));
    ASSERT_EQ(out.ColumnCount, 2);
    ASSERT_EQ(out.RowCount, 4);
    ASSERT_NE(out.Selection, nullptr);
    EXPECT_EQ(
        std::vector<uint8_t>(out.Selection, out.Selection + out.RowCount),
        (std::vector<uint8_t>{0, 0, 0xff, 0xff}));
    const auto* outValue = reinterpret_cast<const int64_t*>(out.Columns[0].Data);
    const auto* shifted = reinterpret_cast<const int64_t*>(out.Columns[1].Data);
    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(outValue[i], values[i]);
        EXPECT_EQ(shifted[i], payload[i] + 100);
    }
    Release(&out);
    EXPECT_FALSE(plan->Next(out));
}

TEST(ProjectE2E, SchedulerRuntimeRunsLimitTail) {
    std::array<int64_t, 3> first = {0, 1, 2};
    std::array<int64_t, 3> second = {3, 4, 5};
    std::array<TColumn, 1> firstCols = {
        TColumn{.Data = reinterpret_cast<char*>(first.data())},
    };
    std::array<TColumn, 1> secondCols = {
        TColumn{.Data = reinterpret_cast<char*>(second.data())},
    };
    TRowSet firstBatch{
        .Columns = firstCols.data(),
        .ColumnCount = 1,
        .RowCount = 3,
        .RefCount = 1,
    };
    TRowSet secondBatch{
        .Columns = secondCols.data(),
        .ColumnCount = 1,
        .RowCount = 3,
        .RefCount = 1,
    };
    NQdb::TMockSource src(
        {"value"},
        {std::make_shared<TIntegerType>()},
        {firstBatch, secondBatch});
    NScheduler::TSettings settings;
    settings.Scheduler.Mode = NScheduler::EExecutionMode::ThreadedScheduler;
    settings.Scheduler.WorkerCount = 2;

    auto plan = Plan(
        "(rel limit (rel project (rel source \"L\") (value value)) "
        "(limit 3) (offset 1))",
        src,
        settings);

    std::vector<int64_t> got;
    TRowSet out{};
    while (plan->Next(out)) {
        const auto* values = reinterpret_cast<const int64_t*>(out.Columns[0].Data);
        for (int64_t row = 0; row < out.RowCount; ++row) {
            if (!out.Selection || out.Selection[row]) {
                got.push_back(values[row]);
            }
        }
        Release(&out);
    }
    EXPECT_EQ(got, (std::vector<int64_t>{1, 2, 3}));
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    NQumir::NCodeGen::TLLVMInitializer initializer;
    return RUN_ALL_TESTS();
}
