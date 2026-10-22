#include "mock_source.h"
#include "plan_runner.h"

#include <qdb/plan/build.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/pipeline.h>
#include <qdb/plan/types/nullable.h>
#include <qdb/sql/parser.h>

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/parser/type.h>

#include <cstdint>
#include <expected>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using namespace NQdb;
using namespace NQumir::NAst;

struct TFixtureTable {
    std::vector<std::string> Names;
    std::vector<std::vector<int64_t>> Values;
    std::vector<TColumn> Columns;
    std::vector<std::vector<uint8_t>> BooleanData;
    std::vector<uint8_t> ValidityData;
    TRowSet Batch{};
    std::unique_ptr<TMockSource> Source;

    TFixtureTable(
        std::vector<std::string> names,
        std::vector<std::vector<int64_t>> values)
        : Names(std::move(names))
        , Values(std::move(values))
    {
        if (Names.size() != Values.size()) {
            throw std::runtime_error("fixture column count mismatch");
        }
        const size_t rowCount = Values.empty() ? 0 : Values.front().size();
        Columns.reserve(Values.size());
        for (auto& column : Values) {
            if (column.size() != rowCount) {
                throw std::runtime_error("fixture row count mismatch");
            }
            Columns.push_back({
                .Data = reinterpret_cast<char*>(column.data()),
                .Mask = nullptr,
                .Offsets = nullptr,
                .OffsetWidth = 0,
            });
        }
        Batch = {
            .Columns = Columns.data(),
            .ColumnCount = static_cast<int64_t>(Columns.size()),
            .RowCount = static_cast<int64_t>(rowCount),
            .Selection = nullptr,
            .Destroy = nullptr,
            .Private = nullptr,
            .RefCount = 1,
        };
        Source = std::make_unique<TMockSource>(
            Names, std::vector<TRowSet>{Batch});
    }
};

struct TFixture {
    std::string Sql;
    std::unordered_map<std::string, std::unique_ptr<TFixtureTable>> Tables;
};

TFixture MakeFixture(std::string_view name) {
    TFixture fixture;
    if (name == "boolean_columns") {
        fixture.Sql = "SELECT x, b, n, b = FALSE AS inv, n = FALSE AS ni "
            "FROM t WHERE x % 3 <> 1 ORDER BY x";
        std::vector<int64_t> x(145);
        for (int64_t row = 0; row < 145; ++row) {
            x[row] = row;
        }
        auto table = std::make_unique<TFixtureTable>(
            std::vector<std::string>{"x", "b", "n"},
            std::vector<std::vector<int64_t>>{x, x, x});
        table->BooleanData.assign(2, std::vector<uint8_t>(19, 0));
        table->ValidityData.assign(19, 0);
        for (int64_t row = 0; row < 145; ++row) {
            if (row % 2 == 1) {
                table->BooleanData[0][row / 8] |= uint8_t{1} << (row % 8);
            } else {
                table->BooleanData[1][row / 8] |= uint8_t{1} << (row % 8);
            }
            if (row % 5 != 0) {
                table->ValidityData[row / 8] |= uint8_t{1} << (row % 8);
            }
        }
        table->Columns[1].Data = reinterpret_cast<char*>(table->BooleanData[0].data());
        table->Columns[2].Data = reinterpret_cast<char*>(table->BooleanData[1].data());
        table->Columns[2].Mask = table->ValidityData.data();
        auto boolType = std::make_shared<TBoolType>();
        table->Source = std::make_unique<TMockSource>(table->Names,
            std::vector<TTypePtr>{std::make_shared<TIntegerType>(), boolType,
                std::make_shared<TNullable>(boolType)},
            std::vector<TRowSet>{table->Batch});
        fixture.Tables.emplace("t", std::move(table));
        return fixture;
    }
    if (name == "filter_project") {
        fixture.Sql =
            "SELECT k AS k, v + 1 AS x FROM t WHERE v >= 20";
        fixture.Tables.emplace("t", std::make_unique<TFixtureTable>(
            std::vector<std::string>{"k", "v"},
            std::vector<std::vector<int64_t>>{
                {1, 2, 3, 4}, {10, 20, 30, 40}}));
        return fixture;
    }
    if (name == "aggregate") {
        fixture.Sql = "SELECT k AS k, sum(v) AS s FROM t GROUP BY k";
        fixture.Tables.emplace("t", std::make_unique<TFixtureTable>(
            std::vector<std::string>{"k", "v"},
            std::vector<std::vector<int64_t>>{
                {1, 2, 3, 4, 4}, {10, 20, 3, 4, 5}}));
        return fixture;
    }
    if (name == "join") {
        fixture.Sql =
            "SELECT l.k AS k, l.v + r.w AS total "
            "FROM l JOIN r ON l.k = r.rk";
        fixture.Tables.emplace("l", std::make_unique<TFixtureTable>(
            std::vector<std::string>{"k", "v"},
            std::vector<std::vector<int64_t>>{
                {1, 2, 3, 4}, {10, 20, 30, 40}}));
        fixture.Tables.emplace("r", std::make_unique<TFixtureTable>(
            std::vector<std::string>{"rk", "w"},
            std::vector<std::vector<int64_t>>{
                {2, 4, 5}, {200, 400, 500}}));
        return fixture;
    }
    if (name == "cross_join") {
        fixture.Sql = "SELECT l.k AS k, r.w AS w FROM l, r WHERE l.v < r.w";
        fixture.Tables.emplace("l", std::make_unique<TFixtureTable>(
            std::vector<std::string>{"k", "v"},
            std::vector<std::vector<int64_t>>{
                {1, 2, 3, 4}, {10, 20, 30, 40}}));
        fixture.Tables.emplace("r", std::make_unique<TFixtureTable>(
            std::vector<std::string>{"w"},
            std::vector<std::vector<int64_t>>{{15, 35}}));
        return fixture;
    }
    if (name == "left_outer_join" || name == "left_outer_residual" ||
        name == "right_outer_join" || name == "right_outer_residual")
    {
        const bool rightJoin = name.starts_with("right_");
        const bool residual = name.ends_with("_residual");
        fixture.Sql =
            "SELECT l.k AS k, l.v AS v, r.rk AS rk, r.w AS w FROM l ";
        fixture.Sql += rightJoin ? "RIGHT" : "LEFT";
        fixture.Sql += " JOIN r ON l.k = r.rk";
        if (residual) {
            fixture.Sql += " AND l.v < r.w";
        }
        const std::vector<std::vector<int64_t>> shortSide = {
            {1, 1, 2, 3, 4}, {10, 30, 20, 40, 50}};
        const std::vector<std::vector<int64_t>> longSide = {
            {1, 1, 2, 4, 5, 6}, {5, 15, 25, 1, 100, 200}};
        const bool mirror = name == "right_outer_join";
        fixture.Tables.emplace("l", std::make_unique<TFixtureTable>(
            std::vector<std::string>{"k", "v"}, mirror ? longSide : shortSide));
        fixture.Tables.emplace("r", std::make_unique<TFixtureTable>(
            std::vector<std::string>{"rk", "w"}, mirror ? shortSide : longSide));
        return fixture;
    }
    if (name == "semi_join") {
        fixture.Sql =
            "SELECT k AS k, v AS v FROM l WHERE k IN (SELECT rk FROM r)";
        fixture.Tables.emplace("l", std::make_unique<TFixtureTable>(
            std::vector<std::string>{"k", "v"},
            std::vector<std::vector<int64_t>>{
                {1, 2, 3, 4, 5, 6, 1, 2, 3, 4, 5, 6, 1, 2, 3, 4, 5, 6, 1, 2, 3, 4, 5, 6, 1, 2, 3, 4, 5, 6, 1, 2, 3, 4, 5, 6, 1, 2, 3, 4},
                {10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120, 130, 140, 150, 160, 170, 180, 190, 200, 210, 220, 230, 240, 250, 260, 270, 280, 290, 300, 310, 320, 330, 340, 350, 360, 370, 380, 390, 400}}));
        fixture.Tables.emplace("r", std::make_unique<TFixtureTable>(
            std::vector<std::string>{"rk"},
            std::vector<std::vector<int64_t>>{
                {2, 4, 5}}));
        return fixture;
    }
    if (name == "nested_limit") {
        fixture.Sql =
            "SELECT count(*) AS n FROM "
            "(SELECT k FROM t WHERE k >= 2 LIMIT 2 OFFSET 1) u";
        fixture.Tables.emplace("t", std::make_unique<TFixtureTable>(
            std::vector<std::string>{"k"},
            std::vector<std::vector<int64_t>>{
                {1, 2, 3, 4, 5}}));
        return fixture;
    }
    if (name == "nested_limit_zero") {
        fixture.Sql =
            "SELECT count(*) AS n FROM (SELECT k FROM t LIMIT 0) u";
        fixture.Tables.emplace("t", std::make_unique<TFixtureTable>(
            std::vector<std::string>{"k"},
            std::vector<std::vector<int64_t>>{
                {1, 2, 3}}));
        return fixture;
    }
    if (name == "source_limit") {
        fixture.Sql = "SELECT * FROM t LIMIT 10";
        fixture.Tables.emplace("t", std::make_unique<TFixtureTable>(
            std::vector<std::string>{"k"},
            std::vector<std::vector<int64_t>>{
                {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}}));
        return fixture;
    }
    if (name == "top_sort") {
        fixture.Sql = "SELECT k AS k, v AS v FROM t ORDER BY v DESC, k ASC LIMIT 3";
        fixture.Tables.emplace("t", std::make_unique<TFixtureTable>(
            std::vector<std::string>{"k", "v"},
            std::vector<std::vector<int64_t>>{
                {1, 2, 3, 4, 5, 6, 7, 8},
                {30, 10, 30, 40, 20, 40, 10, 20}}));
        return fixture;
    }
    throw std::runtime_error("unknown fixture: " + std::string(name));
}

TOperatorPtr BuildSqlPlan(TFixture& fixture) {
    std::istringstream input(fixture.Sql);
    NSql::TTokenStream tokens(input);
    NSql::TParser parser;
    auto parsed = parser.Parse(tokens);
    if (!parsed) {
        throw std::runtime_error(parsed.error().ToString());
    }

    auto plan = BuildPlan(*parsed, [&](std::string_view table)
        -> std::expected<TOperatorPtr, NQumir::TError>
    {
        auto it = fixture.Tables.find(std::string(table));
        if (it == fixture.Tables.end()) {
            return std::unexpected(
                NQumir::TError("unknown table: " + std::string(table)));
        }
        return std::make_shared<TSourceOperator>(
            *it->second->Source, std::string(table));
    });
    if (!plan) {
        throw std::runtime_error(plan.error().ToString());
    }
    auto root = *plan;
    ApplyPlanPasses(root, {.EnableCbo = false});
    return root;
}

NScheduler::TSettings SchedulerSettings(std::string_view mode) {
    NScheduler::TSettings settings;
    if (mode == "single") {
        settings.Scheduler.Mode =
            NScheduler::EExecutionMode::SingleThreadedScheduler;
        settings.Scheduler.WorkerCount = 1;
        settings.ScanSplit.Strategy =
            NScheduler::EScanSplitStrategy::SerialRead;
        settings.ScanSplit.MaxScanTasks = 1;
        settings.HashShuffle.PartitionCount = 1;
        settings.HashShuffle.MaxPartitionCount = 1;
        return settings;
    }
    if (mode == "threaded") {
        settings.Scheduler.Mode =
            NScheduler::EExecutionMode::ThreadedScheduler;
        settings.Scheduler.WorkerCount = 4;
        settings.ScanSplit.Strategy =
            NScheduler::EScanSplitStrategy::RowGroupRange;
        settings.ScanSplit.MaxScanTasks = 4;
        settings.HashShuffle.PartitionCount = 2;
        settings.HashShuffle.MaxPartitionCount = 2;
        return settings;
    }
    throw std::runtime_error("unknown mode: " + std::string(mode));
}

std::string BareName(std::string_view name) {
    const auto dot = name.rfind('.');
    return std::string(dot == std::string_view::npos
        ? name
        : name.substr(dot + 1));
}

bool IsValid(const TColumn& column, int64_t row) {
    if (!column.Mask) {
        return true;
    }
    const int64_t bit = column.MaskBitOffset + row;
    return (column.Mask[bit / 8] & (uint8_t{1} << (bit % 8))) != 0;
}

void PrintJsonString(std::string_view value) {
    std::cout << std::quoted(std::string(value));
}

void PrintResult(TTestRuntime& runtime) {
    auto output = TMaybeType<TStructType>(runtime.OutputType());
    if (!output) {
        throw std::runtime_error("native result is not a struct");
    }

    std::cout << "{\"columns\":[";
    for (size_t i = 0; i < output.Cast()->Fields.size(); ++i) {
        if (i) {
            std::cout << ',';
        }
        PrintJsonString(BareName(output.Cast()->Fields[i].first));
    }
    std::cout << "],\"rows\":[";

    bool firstRow = true;
    TRowSet batch{};
    while (runtime.Next(batch)) {
        for (int64_t row = 0; row < batch.RowCount; ++row) {
            if (batch.Selection && batch.Selection[row] == 0) {
                continue;
            }
            if (!firstRow) {
                std::cout << ',';
            }
            firstRow = false;
            std::cout << '[';
            for (int64_t columnIndex = 0;
                 columnIndex < batch.ColumnCount;
                 ++columnIndex)
            {
                if (columnIndex) {
                    std::cout << ',';
                }
                const auto& column = batch.Columns[columnIndex];
                if (!IsValid(column, row)) {
                    PrintJsonString("");
                    continue;
                }
                const auto type = UnwrapNamedType(UnwrapNullableType(
                    output.Cast()->Fields[columnIndex].second));
                if (TMaybeType<TBoolType>(type)) {
                    const auto* bits = reinterpret_cast<const uint8_t*>(column.Data);
                    const int64_t bit = row + column.DataBitOffset;
                    PrintJsonString(std::to_string((bits[bit / 8] >> (bit % 8)) & 1));
                } else {
                    const auto* values = reinterpret_cast<const int64_t*>(column.Data);
                    PrintJsonString(std::to_string(values[row]));
                }
            }
            std::cout << ']';
        }
        Release(&batch);
    }
    std::cout << "]}\n";
}

} // namespace

int main(int argc, char** argv) {
    NQumir::NCodeGen::TLLVMInitializer llvmInit;
    try {
        if (argc != 3) {
            throw std::runtime_error(
                "usage: plan_export_native_runner <fixture> <single|threaded>");
        }
        auto fixture = MakeFixture(argv[1]);
        auto root = BuildSqlPlan(fixture);
        auto runtime = RunPlan(root, SchedulerSettings(argv[2]));
        PrintResult(*runtime);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "plan_export_native_runner: " << error.what() << '\n';
        return 1;
    }
}
