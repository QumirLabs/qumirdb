#include <gtest/gtest.h>

#include "mock_source.h"
#include "plan_runner.h"

#include <qdb/exec/plan_builder.h>
#include <qdb/io/parquet/source.h>
#include <qdb/plan/build.h>
#include <qdb/plan/passes/column_pruning.h>
#include <qdb/plan/passes/typing.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/late_materialize.h>
#include <qdb/plan/ops/limit.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/sort.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/pipeline.h>
#include <qdb/sexp/parser.h>
#include <qdb/sexp/printer.h>
#include <qdb/sql/parser.h>

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/writer.h>
#include <qumir/codegen/llvm/llvm_initializer.h>

#include <array>
#include <numeric>
#include <ranges>
#include <sstream>

using namespace NQdb;

namespace {

class TLookupMockSource final : public TMockSource, public IRowLookupSource {
public:
    TLookupMockSource()
        : TMockSource(TMockColumns{}, {
            {"URL", std::make_shared<NQumir::NAst::TStringType>()},
            {"EventTime", std::make_shared<NQumir::NAst::TIntegerType>()},
            {"Payload0", std::make_shared<NQumir::NAst::TIntegerType>()},
            {"Payload1", std::make_shared<NQumir::NAst::TIntegerType>()},
            {"Payload2", std::make_shared<NQumir::NAst::TIntegerType>()},
            {"Payload3", std::make_shared<NQumir::NAst::TIntegerType>()},
        })
    {}

    std::optional<TLateMaterializationCost> EstimateLookup(
        std::span<const std::string>,
        std::span<const std::string>,
        uint64_t) const override
    {
        return TLateMaterializationCost{
            .EagerBytes = 10000,
            .NarrowBytes = 500,
            .LookupBytes = 500,
        };
    }

    std::shared_ptr<const IPhysicalRowReader> CompileReader(
        std::span<const std::string>) const override
    {
        return {};
    }
};

std::expected<TOperatorPtr, NQumir::TError> BuildSqlPlan(
    std::string_view sql,
    ISource& source)
{
    std::istringstream input{std::string(sql)};
    NSql::TTokenStream tokens(input);
    NSql::TParser parser;
    auto parsed = parser.Parse(tokens);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    return BuildPlan(*parsed, [&](std::string_view table)
        -> std::expected<TOperatorPtr, NQumir::TError>
    {
        return std::make_shared<TSourceOperator>(source, std::string(table));
    });
}

TPlanPassOptions LateMaterializationOptions(
    TPlanPassDiagnostics* diagnostics = nullptr)
{
    return {
        .EnableCbo = false,
        .LateMaterialization = {.Enabled = true},
        .Diagnostics = diagnostics,
    };
}

std::shared_ptr<TSourceOperator> NarrowSource(const TOperatorPtr& plan) {
    auto late = TMaybeOp<TLateMaterializeOperator>(plan);
    if (!late) return {};
    TOperatorPtr current = late.Cast()->Input();
    if (auto top = TMaybeOp<TTopSortOperator>(current)) {
        current = top.Cast()->Input();
    } else if (auto limit = TMaybeOp<TLimitOperator>(current)) {
        current = limit.Cast()->Input();
    }
    if (auto project = TMaybeOp<TProjectOperator>(current)) {
        current = project.Cast()->Input();
    }
    if (auto filter = TMaybeOp<TFilterOperator>(current)) {
        current = filter.Cast()->Input();
    }
    auto source = TMaybeOp<TSourceOperator>(current);
    return source ? source.Cast() : nullptr;
}

std::shared_ptr<TLateMaterializeOperator> FindLateMaterialize(
    const TOperatorPtr& plan)
{
    if (auto late = TMaybeOp<TLateMaterializeOperator>(plan)) {
        return late.Cast();
    }
    for (const auto& child : plan->Children()) {
        if (auto op = NQumir::NAst::TMaybeNode<IOperator>(child)) {
            if (auto late = FindLateMaterialize(op.Cast())) {
                return late;
            }
        }
    }
    return {};
}

} // namespace

TEST(LateMaterialization, CanBeDisabledInPlanPipeline) {
    TLookupMockSource source;
    auto plan = BuildSqlPlan("SELECT * FROM hits LIMIT 10", source);
    ASSERT_TRUE(plan) << (plan ? "" : plan.error().ToString());

    ApplyPlanPasses(*plan, {
        .EnableCbo = false,
        .LateMaterialization = {.Enabled = false},
    });

    EXPECT_FALSE(TMaybeOp<TLateMaterializeOperator>(*plan));
}

TEST(LateMaterialization, RewritesWideFilteredTopK) {
    TLookupMockSource source;
    auto plan = BuildSqlPlan(
        "SELECT * FROM hits WHERE \"URL\" LIKE '%google%' "
        "ORDER BY \"EventTime\" LIMIT 10",
        source);
    ASSERT_TRUE(plan) << (plan ? "" : plan.error().ToString());

    TPlanPassDiagnostics diagnostics;
    ApplyPlanPasses(*plan, LateMaterializationOptions(&diagnostics));

    auto late = TMaybeOp<TLateMaterializeOperator>(*plan);
    ASSERT_TRUE(late);
    EXPECT_EQ(late.Cast()->Columns().size(), 6u);
    EXPECT_EQ(late.Cast()->LocatorColumn(), "__row_id__");
    auto top = TMaybeOp<TTopSortOperator>(late.Cast()->Input());
    ASSERT_TRUE(top);
    auto narrow = TMaybeOp<TProjectOperator>(top.Cast()->Input());
    ASSERT_TRUE(narrow);
    ASSERT_EQ(narrow.Cast()->Projections().size(), 2u);
    EXPECT_EQ(narrow.Cast()->Projections()[0].Name, "EventTime");
    EXPECT_EQ(narrow.Cast()->Projections()[1].Name, "__row_id__");
    auto sourceOp = NarrowSource(*plan);
    ASSERT_TRUE(sourceOp);
    EXPECT_TRUE(sourceOp->EmitsRowId());
    EXPECT_TRUE(diagnostics.LateMaterialization.Applied);
    EXPECT_EQ(diagnostics.LateMaterialization.Limit, 10u);

    auto second = ApplyLateMaterialization(*plan);
    EXPECT_EQ(second.get(), plan->get());
}

TEST(LateMaterialization, RewritesDirectAliasedColumnsWithPlainLimit) {
    TLookupMockSource source;
    auto plan = BuildSqlPlan(
        "SELECT \"URL\" AS u, \"Payload0\" AS p FROM hits LIMIT 10",
        source);
    ASSERT_TRUE(plan) << (plan ? "" : plan.error().ToString());
    ApplyPlanPasses(*plan, LateMaterializationOptions());

    auto late = TMaybeOp<TLateMaterializeOperator>(*plan);
    ASSERT_TRUE(late);
    ASSERT_EQ(late.Cast()->Columns().size(), 2u);
    EXPECT_EQ(late.Cast()->Columns()[0].PhysicalName, "URL");
    EXPECT_EQ(late.Cast()->Columns()[0].OutputName, "u");
    EXPECT_EQ(late.Cast()->Columns()[1].PhysicalName, "Payload0");
    EXPECT_EQ(late.Cast()->Columns()[1].OutputName, "p");
}

TEST(LateMaterialization, FetchesOnlyOutputWhenSortKeyIsHidden) {
    TLookupMockSource source;
    auto plan = BuildSqlPlan(
        "SELECT \"Payload0\" AS p FROM hits "
        "ORDER BY \"EventTime\" LIMIT 10",
        source);
    ASSERT_TRUE(plan) << (plan ? "" : plan.error().ToString());
    ApplyPlanPasses(*plan, LateMaterializationOptions());

    auto late = TMaybeOp<TLateMaterializeOperator>(*plan);
    ASSERT_TRUE(late);
    ASSERT_EQ(late.Cast()->Columns().size(), 1u);
    EXPECT_EQ(late.Cast()->Columns()[0].PhysicalName, "Payload0");
    EXPECT_EQ(late.Cast()->Columns()[0].OutputName, "p");
    auto top = TMaybeOp<TTopSortOperator>(late.Cast()->Input());
    ASSERT_TRUE(top);
    auto narrow = TMaybeOp<TProjectOperator>(top.Cast()->Input());
    ASSERT_TRUE(narrow);
    ASSERT_EQ(narrow.Cast()->Projections().size(), 2u);
    EXPECT_EQ(narrow.Cast()->Projections()[0].Name, "__sort_0");
    auto sortIdent = NQumir::NAst::TMaybeNode<NQumir::NAst::TIdentExpr>(
        narrow.Cast()->Projections()[0].Expression);
    ASSERT_TRUE(sortIdent);
    EXPECT_EQ(sortIdent.Cast()->Name, "hits.EventTime");
    EXPECT_EQ(narrow.Cast()->Projections()[1].Name, "__row_id__");

    TLookupMockSource offsetSource;
    auto offsetPlan = BuildSqlPlan(
        "SELECT \"Payload0\" AS p FROM hits "
        "ORDER BY \"EventTime\" DESC LIMIT 10 OFFSET 5",
        offsetSource);
    ASSERT_TRUE(offsetPlan) << (offsetPlan ? "" : offsetPlan.error().ToString());
    ApplyPlanPasses(*offsetPlan, LateMaterializationOptions());
    auto offsetLate = TMaybeOp<TLateMaterializeOperator>(*offsetPlan);
    ASSERT_TRUE(offsetLate);
    auto offsetLimit = TMaybeOp<TLimitOperator>(offsetLate.Cast()->Input());
    ASSERT_TRUE(offsetLimit);
    EXPECT_EQ(offsetLimit.Cast()->Limit(), 10);
    EXPECT_EQ(offsetLimit.Cast()->Offset(), 5);
}

TEST(LateMaterialization, RejectsLargeLimitAndComputedOutput) {
    TLookupMockSource source;
    auto large = BuildSqlPlan("SELECT * FROM hits LIMIT 101", source);
    ASSERT_TRUE(large);
    ApplyPlanPasses(*large, LateMaterializationOptions());
    EXPECT_FALSE(TMaybeOp<TLateMaterializeOperator>(*large));

    TLookupMockSource computedSource;
    auto computed = BuildSqlPlan(
        "SELECT \"Payload0\" + 1 AS p FROM hits LIMIT 10",
        computedSource);
    ASSERT_TRUE(computed);
    ApplyPlanPasses(*computed, LateMaterializationOptions());
    EXPECT_FALSE(TMaybeOp<TLateMaterializeOperator>(*computed));
}

TEST(LateMaterialization, BindsRepeatedLabelsBeforeLookupRewrite) {
    TLookupMockSource source;
    auto plan = BuildSqlPlan(
        "SELECT \"EventTime\" AS a, \"Payload0\" AS a FROM hits ORDER BY \"Payload0\" LIMIT 10",
        source);
    ASSERT_TRUE(plan) << plan.error().ToString();
    TPlanPassDiagnostics diagnostics;
    ApplyPlanPasses(*plan, LateMaterializationOptions(&diagnostics));
    auto late = TMaybeOp<TLateMaterializeOperator>(*plan);
    ASSERT_TRUE(late);
    ASSERT_EQ(late.Cast()->Columns().size(), 2);
    EXPECT_EQ(late.Cast()->Columns()[0].OutputName, "a");
    EXPECT_EQ(late.Cast()->Columns()[1].OutputName, "a");
    EXPECT_EQ(late.Cast()->Columns()[0].PhysicalName, "EventTime");
    EXPECT_EQ(late.Cast()->Columns()[1].PhysicalName, "Payload0");
    auto sort = TMaybeOp<TTopSortOperator>(late.Cast()->Input());
    ASSERT_TRUE(sort);
    EXPECT_EQ(sort.Cast()->Keys()[0].Column,
        sort.Cast()->Input()->OutputColumns()->Fields[0].first);
    EXPECT_TRUE(diagnostics.LateMaterialization.Applied);
}

TEST(LateMaterialization, RejectsReservedUserOutputNames) {
    TLookupMockSource source;
    auto selectAlias = BuildSqlPlan(
        "SELECT \"EventTime\" AS \"__row_id__\" FROM hits", source);
    ASSERT_FALSE(selectAlias);
    EXPECT_NE(
        selectAlias.error().ToString().find("is reserved"),
        std::string::npos);

    auto cteAlias = BuildSqlPlan(
        "WITH x(\"__row_id__\") AS "
        "(SELECT \"EventTime\" FROM hits) SELECT * FROM x",
        source);
    ASSERT_FALSE(cteAlias);
    EXPECT_NE(
        cteAlias.error().ToString().find("is reserved"),
        std::string::npos);
}

TEST(LateMaterialization, AppliesAfterCteOutputPruning) {
    TLookupMockSource source;
    auto plan = BuildSqlPlan(
        "WITH x AS (SELECT * FROM hits ORDER BY \"EventTime\" LIMIT 10) "
        "SELECT \"EventTime\" FROM x",
        source);
    ASSERT_TRUE(plan) << (plan ? "" : plan.error().ToString());

    ApplyPlanPasses(*plan, LateMaterializationOptions());
    auto late = FindLateMaterialize(*plan);
    ASSERT_TRUE(late);
    ASSERT_EQ(late->Columns().size(), 1u);
    EXPECT_EQ(late->Columns()[0].PhysicalName, "EventTime");
    EXPECT_EQ(late->Columns()[0].OutputName, "EventTime");
}

TEST(LateMaterialization, SExpressionRoundTripBindsOutsideParser) {
    TLookupMockSource source;
    auto plan = BuildSqlPlan(
        "SELECT * FROM hits WHERE \"URL\" LIKE '%google%' "
        "ORDER BY \"EventTime\" LIMIT 10",
        source);
    ASSERT_TRUE(plan) << (plan ? "" : plan.error().ToString());
    ApplyPlanPasses(*plan, LateMaterializationOptions());
    ASSERT_TRUE(TMaybeOp<TLateMaterializeOperator>(*plan));

    std::ostringstream serialized;
    NSexp::PrintRelPlan(serialized, *plan);
    EXPECT_NE(
        serialized.str().find("(rel late-materialize"),
        std::string::npos);

    NSexp::TRelParserOptions options;
    options.SourceFactory = [&](std::string_view path, NQumir::TLocation)
        -> TOperatorPtr
    {
        return std::make_shared<TSourceOperator>(source, std::string(path));
    };
    NQumir::NAst::NCore::TParser parser;
    for (auto& [name, parse] : NSexp::MakeRelParsers(std::move(options))) {
        parser.NodeParsers[name] = std::move(parse);
    }
    std::istringstream input(serialized.str());
    NQumir::NAst::NCore::TTokenStream tokens(input);
    auto parsed = parser.Parse(tokens);
    ASSERT_TRUE(parsed) << (parsed ? "" : parsed.error().ToString());
    auto roundTrip = std::dynamic_pointer_cast<IOperator>(*parsed);
    ASSERT_TRUE(roundTrip);

    auto parsedLate = TMaybeOp<TLateMaterializeOperator>(roundTrip);
    ASSERT_TRUE(parsedLate);
    auto parsedSource = NarrowSource(roundTrip);
    ASSERT_TRUE(parsedSource);
    EXPECT_FALSE(parsedSource->EmitsRowId());

    std::ostringstream serializedAgain;
    NSexp::PrintRelPlan(serializedAgain, roundTrip);
    EXPECT_EQ(serializedAgain.str(), serialized.str());

    ApplyPlanPasses(roundTrip, LateMaterializationOptions());
    EXPECT_TRUE(TMaybeOp<TLateMaterializeOperator>(roundTrip));
    EXPECT_TRUE(parsedSource->EmitsRowId());
}

TEST(LateMaterialization, ExecutesParquetLookupAfterGlobalTopK) {
    const std::string path = "/tmp/test_late_materialization.parquet";
    constexpr int64_t rowCount = 1000;

    arrow::StringBuilder urls;
    arrow::Int64Builder eventTimes;
    std::array<arrow::Int64Builder, 8> payloads;
    for (int64_t row = 0; row < rowCount; ++row) {
        (void)urls.Append(row % 7 == 0
            ? "https://google.example/" + std::to_string(row)
            : "https://example.test/" + std::to_string(row));
        (void)eventTimes.Append(row);
        for (size_t column = 0; column < payloads.size(); ++column) {
            (void)payloads[column].Append(row * 10 + static_cast<int64_t>(column));
        }
    }
    std::vector<std::shared_ptr<arrow::Field>> fields{
        arrow::field("URL", arrow::utf8(), false),
        arrow::field("EventTime", arrow::int64(), false),
    };
    std::vector<std::shared_ptr<arrow::Array>> arrays{
        urls.Finish().ValueOrDie(),
        eventTimes.Finish().ValueOrDie(),
    };
    for (size_t column = 0; column < payloads.size(); ++column) {
        fields.push_back(arrow::field(
            "Payload" + std::to_string(column), arrow::int64(), false));
        arrays.push_back(payloads[column].Finish().ValueOrDie());
    }
    auto batch = arrow::RecordBatch::Make(
        arrow::schema(std::move(fields)), rowCount, std::move(arrays));
    auto outfile = arrow::io::FileOutputStream::Open(path).ValueOrDie();
    auto table = arrow::Table::FromRecordBatches({batch}).ValueOrDie();
    ASSERT_TRUE(parquet::arrow::WriteTable(
        *table, arrow::default_memory_pool(), outfile, 10).ok());

    bool checkedExecPlan = false;
    bool checkedSexprExecution = false;
    bool checkedSingleWorkerGraph = false;
    auto execute = [&](std::string_view sql,
                       NScheduler::EExecutionMode mode,
                       bool enabled,
                       size_t workerCount)
    {
        TParquetSource source(path);
        auto plan = BuildSqlPlan(sql, source);
        EXPECT_TRUE(plan) << (plan ? "" : plan.error().ToString());
        if (!plan) return std::vector<int64_t>{};
        TPlanPassDiagnostics diagnostics;
        ApplyPlanPasses(*plan, {
            .EnableCbo = false,
            .LateMaterialization = {.Enabled = enabled},
            .Diagnostics = &diagnostics,
        });
        if (enabled) {
            EXPECT_TRUE(TMaybeOp<TLateMaterializeOperator>(*plan))
                << diagnostics.LateMaterialization.Reason
                << " eager=" << diagnostics.LateMaterialization.Cost.EagerBytes
                << " narrow=" << diagnostics.LateMaterialization.Cost.NarrowBytes
                << " lookup=" << diagnostics.LateMaterialization.Cost.LookupBytes;
        } else {
            EXPECT_FALSE(TMaybeOp<TLateMaterializeOperator>(*plan));
        }

        if (enabled &&
            mode == NScheduler::EExecutionMode::ThreadedScheduler &&
            !checkedSexprExecution)
        {
            std::ostringstream serialized;
            NSexp::PrintRelPlan(serialized, *plan);
            NSexp::TRelParserOptions options;
            options.SourceFactory =
                [&](std::string_view sourcePath, NQumir::TLocation)
                    -> TOperatorPtr
                {
                    return std::make_shared<TSourceOperator>(
                        source, std::string(sourcePath));
                };
            NQumir::NAst::NCore::TParser parser;
            for (auto& [name, parse] :
                 NSexp::MakeRelParsers(std::move(options)))
            {
                parser.NodeParsers[name] = std::move(parse);
            }
            std::istringstream input(serialized.str());
            NQumir::NAst::NCore::TTokenStream tokens(input);
            auto parsed = parser.Parse(tokens);
            if (!parsed) {
                ADD_FAILURE() << parsed.error().ToString();
                return std::vector<int64_t>{};
            }
            *plan = std::dynamic_pointer_cast<IOperator>(*parsed);
            if (!*plan) {
                ADD_FAILURE() << "round-tripped late plan is not relational";
                return std::vector<int64_t>{};
            }
            ApplyPlanPasses(*plan, {
                .EnableCbo = false,
                .LateMaterialization = {.Enabled = true},
            });
            checkedSexprExecution = true;
        }

        NScheduler::TSettings settings;
        settings.Scheduler.Mode = mode;
        settings.Scheduler.WorkerCount = workerCount;
        settings.ScanSplit.MaxScanTasks =
            mode == NScheduler::EExecutionMode::ThreadedScheduler ? 4 : 1;
        if (enabled &&
            mode == NScheduler::EExecutionMode::ThreadedScheduler &&
            !checkedExecPlan)
        {
            auto lowered = NScheduler::LowerPlanToGraph(
                *plan, settings, nullptr);
            const auto materializeTasks = std::ranges::count_if(
                lowered.Graph->Nodes(),
                [](const auto& node) {
                    return node->DebugKind == "late-materialize";
                });
            EXPECT_EQ(materializeTasks, 1);
            EXPECT_EQ(std::ranges::count_if(
                lowered.Graph->Nodes(),
                [](const auto& node) {
                    return node->DebugKind == "late-lookup";
                }), 10);
            EXPECT_EQ(std::ranges::count_if(
                lowered.Graph->Nodes(),
                [](const auto& node) {
                    return node->DebugKind == "late-lookup-broadcast";
                }), 1);
            auto execPlan = BuildExecPlan(lowered);
            if (!execPlan) {
                ADD_FAILURE() << execPlan.error();
                return std::vector<int64_t>{};
            }
            const auto lateNode = std::ranges::find_if(
                execPlan->Nodes,
                [](const TExecPlanNode& node) {
                    return node.Kind == EExecPlanNodeKind::LateMaterialize;
                });
            if (lateNode == execPlan->Nodes.end()) {
                ADD_FAILURE() << "exec plan has no late-materialize stage";
                return std::vector<int64_t>{};
            }
            checkedExecPlan = true;
        }
        if (enabled &&
            mode == NScheduler::EExecutionMode::ThreadedScheduler &&
            workerCount == 1 &&
            !checkedSingleWorkerGraph)
        {
            auto lowered = NScheduler::LowerPlanToGraph(
                *plan, settings, nullptr);
            EXPECT_EQ(std::ranges::count_if(
                lowered.Graph->Nodes(),
                [](const auto& node) {
                    return node->DebugKind == "late-lookup";
                }), 1);
            EXPECT_EQ(std::ranges::count_if(
                lowered.Graph->Nodes(),
                [](const auto& node) {
                    return node->DebugKind == "late-lookup-broadcast";
                }), 0);
            checkedSingleWorkerGraph = true;
        }
        auto runtime = RunPlan(*plan, settings);
        const int64_t expectedColumnCount = static_cast<int64_t>(
            (*plan)->OutputColumns()->Fields.size());
        std::vector<int64_t> result;
        TRowSet output{};
        while (runtime->Next(output)) {
            EXPECT_EQ(output.ColumnCount, expectedColumnCount);
            const auto* eventTime = reinterpret_cast<const int64_t*>(
                output.Columns[1].Data);
            for (int64_t row = 0; row < output.RowCount; ++row) {
                if (!output.Selection || output.Selection[row] != 0) {
                    result.push_back(eventTime[row]);
                }
            }
            Release(&output);
        }
        return result;
    };

    constexpr std::string_view filteredTopK =
        "SELECT * FROM hits WHERE \"URL\" LIKE '%google%' "
        "ORDER BY \"EventTime\" LIMIT 10";
    const std::vector<int64_t> expected{0, 7, 14, 21, 28, 35, 42, 49, 56, 63};
    EXPECT_EQ(execute(
        filteredTopK,
        NScheduler::EExecutionMode::SingleThreadedScheduler,
        true,
        1), expected);
    EXPECT_EQ(execute(
        filteredTopK,
        NScheduler::EExecutionMode::ThreadedScheduler,
        true,
        4), expected);
    EXPECT_EQ(execute(
        filteredTopK,
        NScheduler::EExecutionMode::ThreadedScheduler,
        true,
        1), expected);
    EXPECT_TRUE(execute(
        "SELECT * FROM hits WHERE \"URL\" LIKE '%missing%' "
        "ORDER BY \"EventTime\" LIMIT 10",
        NScheduler::EExecutionMode::ThreadedScheduler,
        true,
        4).empty());
    EXPECT_EQ(execute(
        "SELECT \"URL\", \"EventTime\", \"Payload0\", \"Payload1\", "
        "\"Payload2\", \"Payload3\", \"Payload4\", \"Payload5\", "
        "\"Payload6\", \"Payload7\", \"EventTime\" AS EventTimeCopy "
        "FROM hits WHERE \"URL\" LIKE '%google%' "
        "ORDER BY \"EventTime\" LIMIT 10",
        NScheduler::EExecutionMode::ThreadedScheduler,
        true,
        4), expected);
    EXPECT_EQ(execute(
        filteredTopK,
        NScheduler::EExecutionMode::SingleThreadedScheduler,
        false,
        1), expected);

    const std::vector<int64_t> expectedOffset{
        959, 952, 945, 938, 931, 924, 917, 910, 903, 896};
    EXPECT_EQ(execute(
        "SELECT * FROM hits WHERE \"URL\" LIKE '%google%' "
        "ORDER BY \"EventTime\" DESC LIMIT 10 OFFSET 5",
        NScheduler::EExecutionMode::SingleThreadedScheduler,
        true,
        1), expectedOffset);

    std::vector<int64_t> firstTen(10);
    std::iota(firstTen.begin(), firstTen.end(), int64_t{0});
    EXPECT_EQ(execute(
        "SELECT * FROM hits LIMIT 10",
        NScheduler::EExecutionMode::SingleThreadedScheduler,
        true,
        1), firstTen);

    TParquetSource multiLaneSource(path);
    auto sourcePlan = std::make_shared<TSourceOperator>(
        multiLaneSource, path);
    sourcePlan->EnableRowId();
    auto integerType = std::make_shared<NQumir::NAst::TIntegerType>();
    auto multiLanePlan = std::make_shared<TLateMaterializeOperator>(
        sourcePlan,
        std::string(InternalRowIdColumnName),
        std::vector<TLateMaterializeColumn>{
            {"Payload0", "Payload", integerType},
            {"Payload0", "PayloadCopy", integerType},
            {"EventTime", "EventTime", integerType},
        });
    // Lowering expects a pruned plan: the physical locator column is only
    // emitted once column pruning has put it in the source's required set.
    AnnotateTypes(multiLanePlan, {});
    ApplyColumnPruning(multiLanePlan);

    NScheduler::TSettings multiLaneSettings;
    multiLaneSettings.Scheduler.Mode =
        NScheduler::EExecutionMode::ThreadedScheduler;
    multiLaneSettings.Scheduler.WorkerCount = 4;
    multiLaneSettings.ScanSplit.MaxScanTasks = 4;
    auto lowered = NScheduler::LowerPlanToGraph(
        multiLanePlan, multiLaneSettings, nullptr);
    lowered.Graph->Build();
    auto broadcast = std::ranges::find_if(
        lowered.Graph->Nodes(),
        [](const auto& node) {
            return node->DebugKind == "late-lookup-broadcast";
        });
    ASSERT_NE(broadcast, lowered.Graph->Nodes().end());
    EXPECT_EQ((*broadcast)->Inbound.size(), 4u);

    auto runtime = RunPlan(multiLanePlan, multiLaneSettings);
    int64_t materializedRows = 0;
    TRowSet output{};
    while (runtime->Next(output)) {
        ASSERT_EQ(output.ColumnCount, 3);
        const auto* payload = reinterpret_cast<const int64_t*>(
            output.Columns[0].Data);
        const auto* payloadCopy = reinterpret_cast<const int64_t*>(
            output.Columns[1].Data);
        const auto* eventTime = reinterpret_cast<const int64_t*>(
            output.Columns[2].Data);
        for (int64_t row = 0; row < output.RowCount; ++row) {
            EXPECT_EQ(payload[row], payloadCopy[row]);
            EXPECT_EQ(payload[row], eventTime[row] * 10);
            ++materializedRows;
        }
        Release(&output);
    }
    EXPECT_EQ(materializedRows, rowCount);
}

int main(int argc, char** argv) {
    NQumir::NCodeGen::TLLVMInitializer initializer;
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
