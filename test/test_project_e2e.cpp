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

std::vector<int64_t> ReadAllI64(TTestRuntime& runtime) {
    std::vector<int64_t> values;
    TRowSet out{};
    while (runtime.Next(out)) {
        const auto* data = reinterpret_cast<const int64_t*>(out.Columns[0].Data);
        for (int64_t i = 0; i < out.RowCount; ++i) {
            values.push_back(data[i]);
        }
        Release(&out);
    }
    return values;
}

} // namespace

bool IsValid(const TColumn& column, int64_t row) {
    if (!column.Mask) {
        return true;
    }
    const int64_t bit = column.MaskBitOffset + row;
    return ((column.Mask[bit / 8] >> (bit % 8)) & 1) != 0;
}

TEST(SqlDateE2E, CastDateComparisons) {
    struct TCase {
        std::string Operator;
        int64_t Count;
        int64_t ReversedCount;
    };
    const std::array<TCase, 6> cases = {{
        {"=", 1, 1}, {"<>", 3, 3}, {"<", 1, 2},
        {"<=", 2, 3}, {">", 2, 1}, {">=", 3, 2},
    }};
    const std::string date = "CAST('1998-08-04' AS DATE)";
    for (bool nullable : {false, true}) {
        for (const auto& test : cases) {
            for (const auto& [expression, count] : {
                std::pair{"d " + test.Operator + " " + date, test.Count},
                std::pair{date + " " + test.Operator + " d", test.ReversedCount},
                std::pair{"CAST(d AS DATE) " + test.Operator + " " + date, test.Count},
            }) {
                SCOPED_TRACE(expression + (nullable ? " nullable" : ""));
                std::array<int32_t, 5> days = {10441, 10442, 10443, 10444, 10442};
                uint8_t mask = 0b00001111;
                std::array<TColumn, 1> columns = {
                    TColumn{
                        .Data = reinterpret_cast<char*>(days.data()),
                        .Mask = nullable ? &mask : nullptr,
                    },
                };
                TRowSet batch{
                    .Columns = columns.data(),
                    .ColumnCount = 1,
                    .RowCount = nullable ? 5 : 4,
                    .RefCount = 1,
                };
                TTypePtr type = std::make_shared<TIntegerType>(TIntegerType::I32);
                if (nullable) {
                    type = std::make_shared<TNullable>(type);
                }
                TMockSource source({"d"}, {type}, {batch});
                auto runtime = SqlPlan(
                    "SELECT count(*) FROM dates WHERE " + expression,
                    {{"dates", &source}});
                EXPECT_EQ(ReadAllI64(*runtime), (std::vector<int64_t>{count}));
            }
        }
    }
}

TEST(SqlDateE2E, BetweenCastDatesWithInterval) {
    for (bool nullable : {false, true}) {
        for (const std::string bounds : {
            "CAST('1998-08-04' AS DATE) AND "
                "CAST('1998-08-04' AS DATE) + INTERVAL '14' DAY",
            "CAST('1998-08-18' AS DATE) - INTERVAL '14' DAY AND "
                "CAST('1998-08-18' AS DATE)",
        }) {
            SCOPED_TRACE(bounds + (nullable ? " nullable" : ""));
            std::array<int32_t, 5> days = {10441, 10442, 10456, 10457, 10450};
            uint8_t mask = 0b00001111;
            std::array<TColumn, 1> columns = {
                TColumn{
                    .Data = reinterpret_cast<char*>(days.data()),
                    .Mask = nullable ? &mask : nullptr,
                },
            };
            TRowSet batch{
                .Columns = columns.data(),
                .ColumnCount = 1,
                .RowCount = nullable ? 5 : 4,
                .RefCount = 1,
            };
            TTypePtr type = std::make_shared<TIntegerType>(TIntegerType::I32);
            if (nullable) {
                type = std::make_shared<TNullable>(type);
            }
            TMockSource source({"d"}, {type}, {batch});
            auto runtime = SqlPlan(
                "SELECT count(*) FROM dates WHERE d BETWEEN " + bounds,
                {{"dates", &source}});
            EXPECT_EQ(ReadAllI64(*runtime), (std::vector<int64_t>{2}));
        }
    }
}

TEST(SqlUnionE2E, UnionAllPreservesDuplicates) {
    std::array<int64_t, 2> left = {1, 1};
    std::array<int64_t, 1> right = {1};
    std::array<TColumn, 1> leftCols = {
        TColumn{.Data = reinterpret_cast<char*>(left.data())},
    };
    std::array<TColumn, 1> rightCols = {
        TColumn{.Data = reinterpret_cast<char*>(right.data())},
    };
    TRowSet leftBatch{.Columns = leftCols.data(), .ColumnCount = 1, .RowCount = 2, .RefCount = 1};
    TRowSet rightBatch{.Columns = rightCols.data(), .ColumnCount = 1, .RowCount = 1, .RefCount = 1};
    TMockSource l({"a"}, {leftBatch});
    TMockSource r({"a"}, {rightBatch});

    auto plan = SqlPlan(
        "select a from l union all select a from r;",
        {{"l", &l}, {"r", &r}});
    auto values = ReadAllI64(*plan);
    std::sort(values.begin(), values.end());
    EXPECT_EQ(values, (std::vector<int64_t>{1, 1, 1}));
}

TEST(SqlUnionE2E, BareUnionDeduplicatesRows) {
    std::array<int64_t, 3> left = {1, 2, 2};
    std::array<int64_t, 3> right = {2, 3, 3};
    std::array<TColumn, 1> leftCols = {
        TColumn{.Data = reinterpret_cast<char*>(left.data())},
    };
    std::array<TColumn, 1> rightCols = {
        TColumn{.Data = reinterpret_cast<char*>(right.data())},
    };
    TRowSet leftBatch{.Columns = leftCols.data(), .ColumnCount = 1, .RowCount = 3, .RefCount = 1};
    TRowSet rightBatch{.Columns = rightCols.data(), .ColumnCount = 1, .RowCount = 3, .RefCount = 1};
    TMockSource l({"a"}, {leftBatch});
    TMockSource r({"a"}, {rightBatch});

    auto plan = SqlPlan(
        "select a from l union select a from r;",
        {{"l", &l}, {"r", &r}});
    auto values = ReadAllI64(*plan);
    std::sort(values.begin(), values.end());
    EXPECT_EQ(values, (std::vector<int64_t>{1, 2, 3}));
}

TEST(SqlUnionE2E, BareUnionDeduplicatesNullRows) {
    std::array<int64_t, 3> left = {1, 2, 0};
    std::array<int64_t, 3> right = {2, 3, 0};
    std::array<uint8_t, 1> leftMask = {0b00000011};
    std::array<uint8_t, 1> rightMask = {0b00000011};
    std::array<TColumn, 1> leftCols = {
        TColumn{.Data = reinterpret_cast<char*>(left.data()), .Mask = leftMask.data()},
    };
    std::array<TColumn, 1> rightCols = {
        TColumn{.Data = reinterpret_cast<char*>(right.data()), .Mask = rightMask.data()},
    };
    TRowSet leftBatch{.Columns = leftCols.data(), .ColumnCount = 1, .RowCount = 3, .RefCount = 1};
    TRowSet rightBatch{.Columns = rightCols.data(), .ColumnCount = 1, .RowCount = 3, .RefCount = 1};
    auto nullableI64 = std::make_shared<TNullable>(std::make_shared<TIntegerType>());
    TMockSource l({"a"}, {nullableI64}, {leftBatch});
    TMockSource r({"a"}, {nullableI64}, {rightBatch});

    auto plan = SqlPlan(
        "select a from l union select a from r;",
        {{"l", &l}, {"r", &r}});

    std::vector<int64_t> validValues;
    int nullCount = 0;
    TRowSet out{};
    while (plan->Next(out)) {
        const auto* data = reinterpret_cast<const int64_t*>(out.Columns[0].Data);
        for (int64_t i = 0; i < out.RowCount; ++i) {
            if (IsValid(out.Columns[0], i)) {
                validValues.push_back(data[i]);
            } else {
                ++nullCount;
            }
        }
        Release(&out);
    }
    std::sort(validValues.begin(), validValues.end());
    EXPECT_EQ(validValues, (std::vector<int64_t>{1, 2, 3}));
    EXPECT_EQ(nullCount, 1);
}

TEST(SqlIntersectE2E, BareIntersectDeduplicatesRows) {
    std::array<int64_t, 4> left = {1, 2, 2, 4};
    std::array<int64_t, 4> right = {2, 2, 3, 4};
    std::array<TColumn, 1> leftCols = {
        TColumn{.Data = reinterpret_cast<char*>(left.data())},
    };
    std::array<TColumn, 1> rightCols = {
        TColumn{.Data = reinterpret_cast<char*>(right.data())},
    };
    TRowSet leftBatch{.Columns = leftCols.data(), .ColumnCount = 1, .RowCount = 4, .RefCount = 1};
    TRowSet rightBatch{.Columns = rightCols.data(), .ColumnCount = 1, .RowCount = 4, .RefCount = 1};
    TMockSource l({"a"}, {leftBatch});
    TMockSource r({"a"}, {rightBatch});

    auto plan = SqlPlan(
        "select a from l intersect select a from r;",
        {{"l", &l}, {"r", &r}});
    auto values = ReadAllI64(*plan);
    std::sort(values.begin(), values.end());
    EXPECT_EQ(values, (std::vector<int64_t>{2, 4}));
}

TEST(SqlIntersectE2E, BareIntersectDeduplicatesNullRows) {
    std::array<int64_t, 4> left = {1, 2, 0, 0};
    std::array<int64_t, 4> right = {2, 3, 0, 0};
    std::array<uint8_t, 1> leftMask = {0b00000011};
    std::array<uint8_t, 1> rightMask = {0b00000011};
    std::array<TColumn, 1> leftCols = {
        TColumn{.Data = reinterpret_cast<char*>(left.data()), .Mask = leftMask.data()},
    };
    std::array<TColumn, 1> rightCols = {
        TColumn{.Data = reinterpret_cast<char*>(right.data()), .Mask = rightMask.data()},
    };
    TRowSet leftBatch{.Columns = leftCols.data(), .ColumnCount = 1, .RowCount = 4, .RefCount = 1};
    TRowSet rightBatch{.Columns = rightCols.data(), .ColumnCount = 1, .RowCount = 4, .RefCount = 1};
    auto nullableI64 = std::make_shared<TNullable>(std::make_shared<TIntegerType>());
    TMockSource l({"a"}, {nullableI64}, {leftBatch});
    TMockSource r({"a"}, {nullableI64}, {rightBatch});

    auto plan = SqlPlan(
        "select a from l intersect select a from r;",
        {{"l", &l}, {"r", &r}});

    std::vector<int64_t> validValues;
    int nullCount = 0;
    TRowSet out{};
    while (plan->Next(out)) {
        const auto* data = reinterpret_cast<const int64_t*>(out.Columns[0].Data);
        for (int64_t i = 0; i < out.RowCount; ++i) {
            if (IsValid(out.Columns[0], i)) {
                validValues.push_back(data[i]);
            } else {
                ++nullCount;
            }
        }
        Release(&out);
    }
    std::sort(validValues.begin(), validValues.end());
    EXPECT_EQ(validValues, (std::vector<int64_t>{2}));
    EXPECT_EQ(nullCount, 1);
}

TEST(SqlExceptE2E, BareExceptDeduplicatesRows) {
    std::array<int64_t, 4> left = {1, 1, 2, 3};
    std::array<int64_t, 2> right = {2, 4};
    std::array<TColumn, 1> leftCols = {
        TColumn{.Data = reinterpret_cast<char*>(left.data())},
    };
    std::array<TColumn, 1> rightCols = {
        TColumn{.Data = reinterpret_cast<char*>(right.data())},
    };
    TRowSet leftBatch{.Columns = leftCols.data(), .ColumnCount = 1, .RowCount = 4, .RefCount = 1};
    TRowSet rightBatch{.Columns = rightCols.data(), .ColumnCount = 1, .RowCount = 2, .RefCount = 1};
    TMockSource l({"a"}, {leftBatch});
    TMockSource r({"a"}, {rightBatch});

    auto plan = SqlPlan(
        "select a from l except select a from r;",
        {{"l", &l}, {"r", &r}});
    auto values = ReadAllI64(*plan);
    std::sort(values.begin(), values.end());
    EXPECT_EQ(values, (std::vector<int64_t>{1, 3}));
}

TEST(SqlExceptE2E, BareExceptDeduplicatesNullRows) {
    std::array<int64_t, 4> left = {1, 2, 0, 0};
    std::array<int64_t, 3> right = {2, 3, 0};
    std::array<uint8_t, 1> leftMask = {0b00000011};
    std::array<uint8_t, 1> rightMask = {0b00000011};
    std::array<TColumn, 1> leftCols = {
        TColumn{.Data = reinterpret_cast<char*>(left.data()), .Mask = leftMask.data()},
    };
    std::array<TColumn, 1> rightCols = {
        TColumn{.Data = reinterpret_cast<char*>(right.data()), .Mask = rightMask.data()},
    };
    TRowSet leftBatch{.Columns = leftCols.data(), .ColumnCount = 1, .RowCount = 4, .RefCount = 1};
    TRowSet rightBatch{.Columns = rightCols.data(), .ColumnCount = 1, .RowCount = 3, .RefCount = 1};
    auto nullableI64 = std::make_shared<TNullable>(std::make_shared<TIntegerType>());
    TMockSource l({"a"}, {nullableI64}, {leftBatch});
    TMockSource r({"a"}, {nullableI64}, {rightBatch});

    auto plan = SqlPlan(
        "select a from l except select a from r;",
        {{"l", &l}, {"r", &r}});

    std::vector<int64_t> validValues;
    int nullCount = 0;
    TRowSet out{};
    while (plan->Next(out)) {
        const auto* data = reinterpret_cast<const int64_t*>(out.Columns[0].Data);
        for (int64_t i = 0; i < out.RowCount; ++i) {
            if (IsValid(out.Columns[0], i)) {
                validValues.push_back(data[i]);
            } else {
                ++nullCount;
            }
        }
        Release(&out);
    }
    std::sort(validValues.begin(), validValues.end());
    EXPECT_EQ(validValues, (std::vector<int64_t>{1}));
    EXPECT_EQ(nullCount, 0);
}

class SqlNullableSetOps
    : public testing::TestWithParam<std::pair<NScheduler::EExecutionMode, bool>> {};

TEST_P(SqlNullableSetOps, CompositeKeysCompareNullsBySetSemantics) {
    for (const std::string operation : {"INTERSECT", "EXCEPT"}) {
        // Invalid cells deliberately contain different bytes on either side.
        std::array<int64_t, 7> leftA = {1, 1, 111, 222, 0, 3, 333};
        std::array<int64_t, 7> leftB = {111, 222, 2, 333, 2, 3, 7};
        std::array<int64_t, 5> rightA = {1, 444, 555, 666, 2};
        std::array<int64_t, 5> rightB = {444, 2, 555, 0, 2};
        uint8_t leftAMask = 0b00110011, leftBMask = 0b01110100;
        uint8_t rightAMask = 0b00010001, rightBMask = 0b00011010;
        std::array<TColumn, 2> leftCols = {
            TColumn{.Data = reinterpret_cast<char*>(leftA.data()), .Mask = &leftAMask},
            TColumn{.Data = reinterpret_cast<char*>(leftB.data()), .Mask = &leftBMask},
        };
        std::array<TColumn, 2> rightCols = {
            TColumn{.Data = reinterpret_cast<char*>(rightA.data()), .Mask = &rightAMask},
            TColumn{.Data = reinterpret_cast<char*>(rightB.data()), .Mask = &rightBMask},
        };
        auto nullableI64 = std::make_shared<TNullable>(std::make_shared<TIntegerType>());
        TMockSource left({"a", "b"}, {nullableI64, nullableI64}, {TRowSet{
            .Columns = leftCols.data(), .ColumnCount = 2, .RowCount = 7, .RefCount = 1}});
        TMockSource right({"a", "b"}, {nullableI64, nullableI64}, {TRowSet{
            .Columns = rightCols.data(), .ColumnCount = 2, .RowCount = 5, .RefCount = 1}});
        NScheduler::TSettings settings;
        settings.Scheduler.Mode = GetParam().first;
        settings.Scheduler.WorkerCount = 2;
        settings.HashShuffle.PartitionCount = 2;
        auto runtime = SqlPlan("SELECT a, b FROM l " + operation + " SELECT a, b FROM r",
            {{"l", &left}, {"r", &right}}, settings, GetParam().second);

        using TCell = std::optional<int64_t>;
        using TRow = std::pair<TCell, TCell>;
        std::vector<TRow> rows;
        TRowSet out{};
        while (runtime->Next(out)) {
            for (int64_t i = 0; i < out.RowCount; ++i) {
                if (out.Selection && !out.Selection[i]) {
                    continue;
                }
                auto cell = [&](size_t column) -> TCell {
                    return IsValid(out.Columns[column], i)
                        ? TCell{reinterpret_cast<const int64_t*>(out.Columns[column].Data)[i]}
                        : std::nullopt;
                };
                rows.emplace_back(cell(0), cell(1));
            }
            Release(&out);
        }
        auto expected = operation == "INTERSECT"
            ? std::vector<TRow>{{1, std::nullopt}, {std::nullopt, 2}, {std::nullopt, std::nullopt}}
            : std::vector<TRow>{{0, 2}, {3, 3}, {std::nullopt, 7}};
        std::ranges::sort(rows);
        std::ranges::sort(expected);
        EXPECT_EQ(rows, expected) << operation;
    }
}

INSTANTIATE_TEST_SUITE_P(SchedulersAndOptimizer, SqlNullableSetOps,
    testing::Values(
        std::pair{NScheduler::EExecutionMode::SingleThreadedScheduler, false},
        std::pair{NScheduler::EExecutionMode::SingleThreadedScheduler, true},
        std::pair{NScheduler::EExecutionMode::ThreadedScheduler, false},
        std::pair{NScheduler::EExecutionMode::ThreadedScheduler, true}));

TEST(SqlGroupingE2E, RollupGroupingColumns) {
    std::array<int64_t, 3> a = {1, 1, 2};
    std::array<int64_t, 3> b = {10, 20, 10};
    std::array<TColumn, 2> cols = {
        TColumn{.Data = reinterpret_cast<char*>(a.data())},
        TColumn{.Data = reinterpret_cast<char*>(b.data())},
    };
    TRowSet batch{.Columns = cols.data(), .ColumnCount = 2, .RowCount = 3, .RefCount = 1};
    TMockSource t({"a", "b"}, {batch});

    auto plan = SqlPlan(
        "select grouping(a) ga, grouping(b) gb, count(*) c "
        "from t group by rollup(a, b);",
        {{"t", &t}});

    std::vector<std::array<int64_t, 3>> rows;
    TRowSet out{};
    while (plan->Next(out)) {
        const auto* ga = reinterpret_cast<const int64_t*>(out.Columns[0].Data);
        const auto* gb = reinterpret_cast<const int64_t*>(out.Columns[1].Data);
        const auto* c = reinterpret_cast<const int64_t*>(out.Columns[2].Data);
        for (int64_t i = 0; i < out.RowCount; ++i) {
            rows.push_back({ga[i], gb[i], c[i]});
        }
        Release(&out);
    }
    std::sort(rows.begin(), rows.end());
    EXPECT_EQ(rows, (std::vector<std::array<int64_t, 3>>{
        {0, 0, 1},
        {0, 0, 1},
        {0, 0, 1},
        {0, 1, 1},
        {0, 1, 2},
        {1, 1, 3},
    }));
}

TEST(SqlGroupingE2E, RollupKeepsComputedAggregateArgumentsIndependentOfMaskedKeys) {
    std::array<int64_t, 3> keys = {1, 1, 2};
    std::array<TColumn, 1> cols = {
        TColumn{.Data = reinterpret_cast<char*>(keys.data())},
    };
    TRowSet batch{
        .Columns = cols.data(), .ColumnCount = 1, .RowCount = 3, .RefCount = 1};
    TMockSource t({"k"}, {batch});

    auto plan = SqlPlan(
        "select grouping(k) g, sum(k + 1) s, avg(k + 1) av, "
        "min(k + 1) mn, max(k + 1) mx, count(k + 1) c "
        "from t group by rollup(k);",
        {{"t", &t}});

    struct TRow {
        int64_t Grouping;
        int64_t Sum;
        double Average;
        int64_t Min;
        int64_t Max;
        int64_t Count;
    };
    std::vector<TRow> rows;
    TRowSet out{};
    while (plan->Next(out)) {
        ASSERT_EQ(out.ColumnCount, 6);
        const auto* grouping = reinterpret_cast<const int64_t*>(out.Columns[0].Data);
        const auto* sum = reinterpret_cast<const int64_t*>(out.Columns[1].Data);
        const auto* average = reinterpret_cast<const double*>(out.Columns[2].Data);
        const auto* min = reinterpret_cast<const int64_t*>(out.Columns[3].Data);
        const auto* max = reinterpret_cast<const int64_t*>(out.Columns[4].Data);
        const auto* count = reinterpret_cast<const int64_t*>(out.Columns[5].Data);
        for (int64_t i = 0; i < out.RowCount; ++i) {
            for (int column = 0; column < 6; ++column) {
                EXPECT_TRUE(IsValid(out.Columns[column], i));
            }
            rows.push_back({
                grouping[i], sum[i], average[i], min[i], max[i], count[i]});
        }
        Release(&out);
    }
    std::ranges::sort(rows, {}, &TRow::Sum);
    ASSERT_EQ(rows.size(), 3u);

    EXPECT_EQ(rows[0].Grouping, 0);
    EXPECT_EQ(rows[0].Sum, 3);
    EXPECT_DOUBLE_EQ(rows[0].Average, 3.0);
    EXPECT_EQ(rows[0].Min, 3);
    EXPECT_EQ(rows[0].Max, 3);
    EXPECT_EQ(rows[0].Count, 1);

    EXPECT_EQ(rows[1].Grouping, 0);
    EXPECT_EQ(rows[1].Sum, 4);
    EXPECT_DOUBLE_EQ(rows[1].Average, 2.0);
    EXPECT_EQ(rows[1].Min, 2);
    EXPECT_EQ(rows[1].Max, 2);
    EXPECT_EQ(rows[1].Count, 2);

    EXPECT_EQ(rows[2].Grouping, 1);
    EXPECT_EQ(rows[2].Sum, 7);
    EXPECT_DOUBLE_EQ(rows[2].Average, 7.0 / 3.0);
    EXPECT_EQ(rows[2].Min, 2);
    EXPECT_EQ(rows[2].Max, 3);
    EXPECT_EQ(rows[2].Count, 3);
}

// Duplicate grouping sets must produce duplicate rows: the aggregate keeps them
// distinct by the ordinal grouping-set id (not a column bitmask), which the
// parallel combine also keys on, so the parallelized path preserves this too.
TEST(SqlGroupingE2E, DuplicateGroupingSetsAreNotCollapsed) {
    std::array<int64_t, 3> a = {1, 1, 2};
    std::array<TColumn, 1> cols = {
        TColumn{.Data = reinterpret_cast<char*>(a.data())},
    };
    TRowSet batch{.Columns = cols.data(), .ColumnCount = 1, .RowCount = 3, .RefCount = 1};
    TMockSource t({"a"}, {batch});

    auto plan = SqlPlan(
        "select a, count(*) c from t group by grouping sets ((a), (a));",
        {{"t", &t}});

    std::vector<std::array<int64_t, 2>> rows;
    TRowSet out{};
    while (plan->Next(out)) {
        const auto* a = reinterpret_cast<const int64_t*>(out.Columns[0].Data);
        const auto* c = reinterpret_cast<const int64_t*>(out.Columns[1].Data);
        for (int64_t i = 0; i < out.RowCount; ++i) {
            rows.push_back({a[i], c[i]});
        }
        Release(&out);
    }
    std::sort(rows.begin(), rows.end());
    EXPECT_EQ(rows, (std::vector<std::array<int64_t, 2>>{
        {1, 2},
        {1, 2},
        {2, 1},
        {2, 1},
    }));
}

TEST(SqlGroupingE2E, GroupingExpressionSurvivesWindowExtraction) {
    const std::string categories = "aabb";
    std::array<int64_t, 5> categoryOffsets = {0, 1, 2, 3, 4};
    std::array<int64_t, 4> classes = {10, 20, 10, 20};
    std::array<int64_t, 4> values = {5, 7, 11, 13};
    std::array<TColumn, 3> cols = {
        TColumn{
            .Data = const_cast<char*>(categories.data()),
            .Offsets = categoryOffsets.data(),
            .OffsetWidth = 8,
        },
        TColumn{.Data = reinterpret_cast<char*>(classes.data())},
        TColumn{.Data = reinterpret_cast<char*>(values.data())},
    };
    TRowSet batch{.Columns = cols.data(), .ColumnCount = 3, .RowCount = 4, .RefCount = 1};
    TMockSource t(
        {"category", "class", "value"},
        {std::make_shared<TStringType>(),
         std::make_shared<TIntegerType>(),
         std::make_shared<TIntegerType>()},
        {batch});

    auto plan = SqlPlan(
        "select grouping(category)+grouping(class) as lochierarchy, "
        "rank() over (partition by grouping(category)+grouping(class), "
        "case when grouping(class) = 0 then category end "
        "order by sum(value) desc) as rank_within_parent "
        "from t group by rollup(category, class);",
        {{"t", &t}});

    int64_t rows = 0;
    TRowSet out{};
    while (plan->Next(out)) {
        rows += out.RowCount;
        Release(&out);
    }
    EXPECT_EQ(rows, 7);
}

TEST(SqlStddevSampE2E, GlobalFloat) {
    std::array<double, 3> v = {1.0, 2.0, 3.0};
    std::array<TColumn, 1> cols = {
        TColumn{.Data = reinterpret_cast<char*>(v.data())},
    };
    TRowSet batch{.Columns = cols.data(), .ColumnCount = 1, .RowCount = 3, .RefCount = 1};
    TMockSource t({"v"}, {std::make_shared<TFloatType>()}, {batch});

    auto plan = SqlPlan("select stddev_samp(v) as s from t;", {{"t", &t}});

    auto* outType = static_cast<TStructType*>(plan->OutputType().get());
    ASSERT_EQ(outType->Fields.size(), 1u);
    ASSERT_TRUE(IsNullableType(outType->Fields[0].second));
    EXPECT_TRUE(TMaybeType<TFloatType>(UnwrapNullableType(outType->Fields[0].second)));

    TRowSet out{};
    ASSERT_TRUE(plan->Next(out));
    ASSERT_EQ(out.ColumnCount, 1);
    ASSERT_EQ(out.RowCount, 1);
    ASSERT_TRUE(IsValid(out.Columns[0], 0));
    const auto* data = reinterpret_cast<const double*>(out.Columns[0].Data);
    EXPECT_NEAR(data[0], 1.0, 1e-12);
    Release(&out);
    EXPECT_FALSE(plan->Next(out));
}

TEST(SqlStddevSampE2E, GlobalIntegerCastsToFloat) {
    std::array<int64_t, 3> v = {1, 2, 3};
    std::array<TColumn, 1> cols = {
        TColumn{.Data = reinterpret_cast<char*>(v.data())},
    };
    TRowSet batch{.Columns = cols.data(), .ColumnCount = 1, .RowCount = 3, .RefCount = 1};
    TMockSource t({"v"}, {std::make_shared<TIntegerType>()}, {batch});

    auto plan = SqlPlan("select stddev_samp(v) as s from t;", {{"t", &t}});

    TRowSet out{};
    ASSERT_TRUE(plan->Next(out));
    ASSERT_EQ(out.ColumnCount, 1);
    ASSERT_EQ(out.RowCount, 1);
    ASSERT_TRUE(IsValid(out.Columns[0], 0));
    const auto* data = reinterpret_cast<const double*>(out.Columns[0].Data);
    EXPECT_NEAR(data[0], 1.0, 1e-12);
    Release(&out);
    EXPECT_FALSE(plan->Next(out));
}

TEST(SqlStddevSampE2E, NullableFloatGroupsReturnNullForSmallSamples) {
    std::array<int64_t, 4> k = {1, 2, 2, 3};
    std::array<double, 4> v = {42.0, 1.0, 3.0, 99.0};
    std::array<uint8_t, 1> mask = {0b00000111};
    std::array<TColumn, 2> cols = {
        TColumn{.Data = reinterpret_cast<char*>(k.data())},
        TColumn{.Data = reinterpret_cast<char*>(v.data()), .Mask = mask.data()},
    };
    TRowSet batch{.Columns = cols.data(), .ColumnCount = 2, .RowCount = 4, .RefCount = 1};
    TMockSource t(
        {"k", "v"},
        {std::make_shared<TIntegerType>(),
         std::make_shared<TNullable>(std::make_shared<TFloatType>())},
        {batch});

    auto plan = SqlPlan("select k, stddev_samp(v) as s from t group by k;", {{"t", &t}});

    struct TRow {
        int64_t K = 0;
        bool Valid = false;
        double Value = 0;
    };
    std::vector<TRow> rows;
    TRowSet out{};
    while (plan->Next(out)) {
        const auto* keys = reinterpret_cast<const int64_t*>(out.Columns[0].Data);
        const auto* values = reinterpret_cast<const double*>(out.Columns[1].Data);
        for (int64_t i = 0; i < out.RowCount; ++i) {
            rows.push_back({keys[i], IsValid(out.Columns[1], i), values[i]});
        }
        Release(&out);
    }
    std::sort(rows.begin(), rows.end(), [](const TRow& lhs, const TRow& rhs) {
        return lhs.K < rhs.K;
    });

    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(rows[0].K, 1);
    EXPECT_FALSE(rows[0].Valid);
    EXPECT_EQ(rows[1].K, 2);
    ASSERT_TRUE(rows[1].Valid);
    EXPECT_NEAR(rows[1].Value, std::sqrt(2.0), 1e-12);
    EXPECT_EQ(rows[2].K, 3);
    EXPECT_FALSE(rows[2].Valid);
}

TEST(SqlStddevSampE2E, DecimalInputIsRejected) {
    TMockSource t({"v"}, {std::make_shared<TDecimal>(12, 2)}, {});

    try {
        (void)SqlPlan("select stddev_samp(v) as s from t;", {{"t", &t}});
        FAIL() << "stddev_samp(decimal) unexpectedly planned";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(
            std::string(error.what()).find("stddev_samp(decimal) is not supported in qdb v1"),
            std::string::npos);
    }
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
