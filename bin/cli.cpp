#include "plan_export.h"

#include <qdb/catalog/external_module.h>
#include <qdb/exec/planner_helpers.h>
#include <qdb/io/parquet/source.h>
#include <qdb/io/text/sink.h>
#include <qdb/kernel/finalize.h>
#include <qdb/scheduler/plan_lowerer.h>
#include <qdb/plan/build.h>
#include <qdb/plan/pipeline.h>
#include <qdb/plan/plan_print.h>
#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/cte_consumer.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/passes/column_pruning.h>
#include <qdb/plan/passes/equijoin.h>
#include <qdb/plan/passes/join_order.h>
#include <qdb/plan/passes/qualify_columns.h>
#include <qdb/plan/passes/top_sort.h>
#include <qdb/plan/passes/typing.h>
#include <qdb/sexp/parser.h>
#include <qdb/sexp/printer.h>
#include <qdb/sql/parser.h>

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>

#include <readline/history.h>
#include <readline/readline.h>

#include <sys/resource.h>

#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

enum class ESyntax {
    Sexpr,
    Sql,
};

struct TFormatSpec {
    std::string Name = "console";
    char Separator = ',';
    bool NoEscape = false;
};

// Format spec grammar:
//   csv                       CSV, no escaping
//   console                   aligned table
//   null                      no output
//   <attrs>format             format with attributes
//
// Attributes (comma-separated inside <>):
//   separator=X               use X as CSV separator
//   escape                    enable CSV quoting/escaping
//
// Examples: <escape>csv   <separator=|>csv   <separator=|,escape>csv
TFormatSpec ParseFormat(const std::string& spec) {
    TFormatSpec result;
    bool explicitEscape = false;

    if (spec.starts_with('<')) {
        auto close = spec.find('>');
        if (close == std::string::npos) {
            throw std::invalid_argument("unclosed '<' in format spec");
        }
        std::string attrs = spec.substr(1, close - 1);
        result.Name = spec.substr(close + 1);

        std::istringstream ss(attrs);
        std::string attr;
        while (std::getline(ss, attr, ',')) {
            if (attr == "escape") {
                result.NoEscape = false;
                explicitEscape = true;
            } else if (auto eq = attr.find('='); eq != std::string::npos) {
                std::string key = attr.substr(0, eq);
                std::string val = attr.substr(eq + 1);
                if (key == "separator") {
                    if (val.size() != 1) {
                        throw std::invalid_argument("separator must be a single character");
                    }
                    result.Separator = val[0];
                } else {
                    throw std::invalid_argument("unknown format attribute: " + key);
                }
            } else {
                throw std::invalid_argument("unknown format attribute: " + attr);
            }
        }
    } else {
        result.Name = spec;
    }

    if (result.Name != "console" && result.Name != "csv" && result.Name != "null") {
        throw std::invalid_argument("unknown format: " + result.Name);
    }
    if (result.Name == "csv" && !explicitEscape) {
        result.NoEscape = true;
    }
    return result;
}

NQdb::NScheduler::EExecutionMode ParseSchedulerMode(const std::string& mode) {
    if (mode == "single") {
        return NQdb::NScheduler::EExecutionMode::SingleThreadedScheduler;
    }
    if (mode == "threaded") {
        return NQdb::NScheduler::EExecutionMode::ThreadedScheduler;
    }
    throw std::invalid_argument("unknown scheduler mode: " + mode);
}

NQdb::TSchema SchemaFromType(
    const NQumir::NAst::TTypePtr& type,
    std::vector<std::string>& names,
    std::vector<NQdb::TColumnSchema>& columns)
{
    auto* structType = static_cast<NQumir::NAst::TStructType*>(type.get());
    if (!structType) {
        throw std::runtime_error("executor output type must be TStructType");
    }
    names.clear();
    columns.clear();
    names.reserve(structType->Fields.size());
    columns.reserve(structType->Fields.size());
    for (const auto& [name, fieldType] : structType->Fields) {
        names.push_back(name);
    }
    for (size_t i = 0; i < structType->Fields.size(); ++i) {
        columns.push_back({.Name = names[i], .Type = structType->Fields[i].second});
    }
    return NQdb::TSchema{std::span<const NQdb::TColumnSchema>(columns)};
}

enum class EExplainMode { Text, Sexpr, Both };

std::optional<EExplainMode> ParseExplainMode(std::string_view value) {
    if (value == "text") return EExplainMode::Text;
    if (value == "sexpr" || value == "s-expr") return EExplainMode::Sexpr;
    if (value == "both") return EExplainMode::Both;
    return std::nullopt;
}

struct TConfig {
    ESyntax Syntax = ESyntax::Sexpr;
    std::string DataDir = ".";
    TFormatSpec Format;
    bool Verbose = false;
    bool EnableCbo = true;
    bool EnableRuntimeFilters = true;
    bool ForceRuntimeFilters = false;
    bool Timing = false;
    EExplainMode ExplainMode = EExplainMode::Text;
    NQdb::TLateMaterializationSettings LateMaterialization;
    NQdb::NScheduler::TSettings Scheduler;
};

std::string ResolveTablePath(const std::string& dataDir, std::string_view table) {
    if (table.ends_with(".parquet")) {
        return std::string(table);
    }
    return dataDir + "/" + std::string(table) + ".parquet";
}

std::expected<NQdb::TOperatorPtr, NQumir::TError> ParseSexpr(
    std::istream& in,
    const std::function<NQdb::TOperatorPtr(const std::string&)>& makeSource)
{
    NQdb::NSexp::TRelParserOptions opts;
    opts.SourceFactory = [&](std::string_view path, NQumir::TLocation) {
        return makeSource(std::string(path));
    };

    NQumir::NAst::NCore::TParser parser;
    for (auto& [key, value] : NQdb::NSexp::MakeRelParsers(std::move(opts))) {
        parser.NodeParsers[key] = std::move(value);
    }

    NQumir::NAst::NCore::TTokenStream tokens(in);
    auto parsed = parser.Parse(tokens);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    auto plan = std::dynamic_pointer_cast<NQdb::IOperator>(*parsed);
    if (!plan) {
        return std::unexpected(NQumir::TError("input must be a relational plan (rel ...)"));
    }
    return plan;
}

std::expected<std::vector<NQdb::NSql::TSqlNodePtr>, NQumir::TError>
ParseSqlStatements(std::istream& in)
{
    NQdb::NSql::TTokenStream tokens(in);
    NQdb::NSql::TParser parser;
    return parser.ParseAll(tokens);
}

// If `text` begins with the word "explain" (case-insensitive), strip it and
// return true.
bool StripExplain(std::string& text) {
    size_t start = text.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return false;
    }
    static const std::string keyword = "explain";
    if (text.size() - start < keyword.size()) {
        return false;
    }
    for (size_t i = 0; i < keyword.size(); ++i) {
        if (std::tolower(text[start + i]) != keyword[i]) {
            return false;
        }
    }
    size_t after = start + keyword.size();
    if (after < text.size() && !std::isspace(text[after])) {
        return false; // e.g. "explained"
    }
    text = text.substr(after);
    return true;
}

// Wraps the output sink to count the total number of result rows.
class TCountingSink : public NQdb::ISink {
public:
    explicit TCountingSink(NQdb::ISink& inner) : Inner_(inner) {}

    void Write(const NQdb::TRowSet& rowSet) override {
        if (rowSet.Selection) {
            for (int64_t i = 0; i < rowSet.RowCount; ++i) {
                Rows_ += rowSet.Selection[i] != 0;
            }
        } else {
            Rows_ += rowSet.RowCount;
        }
        Inner_.Write(rowSet);
    }
    void Flush() override { Inner_.Flush(); }
    int64_t Rows() const { return Rows_; }

private:
    NQdb::ISink& Inner_;
    int64_t Rows_ = 0;
};

// User + system CPU time consumed by the process across all threads.
std::chrono::duration<double> ProcessCpuTime() {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return std::chrono::duration<double>::zero();
    }
    auto seconds = [](const timeval& tv) {
        return static_cast<double>(tv.tv_sec) + static_cast<double>(tv.tv_usec) * 1e-6;
    };
    return std::chrono::duration<double>(seconds(usage.ru_utime) + seconds(usage.ru_stime));
}

int ExecutePlan(
    NQdb::TOperatorPtr plan,
    const TConfig& config,
    std::shared_ptr<const NQdb::TExternalCatalogSnapshot> externalCatalog,
    bool explain,
    std::chrono::steady_clock::time_point planStart)
{
    NQdb::TPlanPassDiagnostics planDiagnostics;
    NQdb::ApplyPlanPasses(plan, {
        .EnableCbo = config.EnableCbo,
        .EnableRuntimeFilters = config.EnableRuntimeFilters,
        .ForceRuntimeFilters = config.ForceRuntimeFilters,
        .LateMaterialization = config.LateMaterialization,
        .Diagnostics = &planDiagnostics,
        .Annotation = {.ExternalCatalog = externalCatalog},
    });
    auto planElapsed = std::chrono::steady_clock::now() - planStart;
    const bool sexprAvailable = NQdb::CollectMaterializations(plan).empty();

    if (config.Verbose) {
        const auto& late = planDiagnostics.LateMaterialization;
        if (!late.Reason.empty()) {
            std::cerr << "late-materialization: " << late.Reason;
            if (late.Considered) {
                std::cerr << " (limit=" << late.Limit
                    << ", early-columns=" << late.EarlyColumnCount
                    << ", fetch-columns=" << late.FetchColumnCount
                    << ", eager-bytes=" << late.Cost.EagerBytes
                    << ", narrow-bytes=" << late.Cost.NarrowBytes
                    << ", lookup-bytes=" << late.Cost.LookupBytes << ")";
            }
            std::cerr << '\n';
        }
        std::cerr << "========== LOGICAL PLAN ==========\n";
        if (sexprAvailable) {
            NQdb::NSexp::PrintRelPlan(std::cerr, plan);
        } else {
            NQdb::PrintPlanTreeWithCtes(std::cerr, plan);
        }
        std::cerr << "\n==================================\n";
    }

    if (explain) {
        const bool wantSexpr =
            config.ExplainMode != EExplainMode::Text && sexprAvailable;
        const bool wantText =
            config.ExplainMode != EExplainMode::Sexpr || !sexprAvailable;
        if (wantSexpr) {
            NQdb::NSexp::PrintRelPlan(std::cout, plan);
            std::cout << (wantText ? "\n\n" : "\n");
        }
        if (wantText) {
            PrintPlanTreeWithCtes(std::cout, plan);
        }
        return 0;
    }

    auto* diagnostics = config.Verbose ? &std::cerr : nullptr;
    if (diagnostics) {
        NQdb::PrintRuntimePlan(*diagnostics, plan);
    }

    auto buildStart = std::chrono::steady_clock::now();
    auto lowered = NQdb::NScheduler::LowerPlanToGraph(
        plan, config.Scheduler, diagnostics, externalCatalog);
    auto buildElapsed = std::chrono::steady_clock::now() - buildStart;
    auto llvmStart = std::chrono::steady_clock::now();
    NQdb::JitFinalizeKernels(
        lowered.Kernels, diagnostics, externalCatalog);
    auto llvmElapsed = std::chrono::steady_clock::now() - llvmStart;

    std::vector<std::string> outputNames;
    std::vector<NQdb::TColumnSchema> outputColumns;
    auto schema = SchemaFromType(lowered.OutputType, outputNames, outputColumns);

    std::unique_ptr<NQdb::ISink> sink;
    if (config.Format.Name == "csv") {
        sink = std::make_unique<NQdb::TCsvSink>(
            schema, std::cout, config.Format.Separator, config.Format.NoEscape);
    } else if (config.Format.Name == "null") {
        sink = std::make_unique<NQdb::TNullSink>(schema);
    } else {
        sink = std::make_unique<NQdb::TConsoleSink>(schema, std::cout);
    }

    TCountingSink counting(*sink);
    auto start = std::chrono::steady_clock::now();
    auto cpuStart = ProcessCpuTime();
    std::string error;
    if (!NQdb::NScheduler::RunPlanIntoSink(
            std::move(lowered), counting, config.Scheduler, diagnostics, &error)) {
        std::cerr << error << "\n";
        return 1;
    }
    counting.Flush();
    auto elapsed = std::chrono::steady_clock::now() - start;
    auto cpuElapsed = ProcessCpuTime() - cpuStart;
    if (config.Timing) {
        std::cerr << "Planning: "
                  << std::chrono::duration<double>(planElapsed).count() << " seconds\n"
                  << "KernelBuild: "
                  << std::chrono::duration<double>(buildElapsed).count() << " seconds\n"
                  << "JitLLVM: "
                  << std::chrono::duration<double>(llvmElapsed).count() << " seconds\n"
                  << "Cpu: " << cpuElapsed.count() << " seconds\n";
    }
    std::cerr << "Returned " << counting.Rows() << " rows in "
              << std::chrono::duration<double>(elapsed).count() << " seconds\n";
    return 0;
}

int RunSqlStatement(
    const NQdb::NSql::TSqlNodePtr& statement,
    const TConfig& config,
    NQdb::TExternalModuleCatalog& catalog,
    bool explain)
{
    if (auto module = NQdb::NSql::TMaybeNode<NQdb::NSql::TSqlExternalModule>(statement)) {
        if (explain) {
            std::cerr << "EXPLAIN requires a SELECT statement\n";
            return 1;
        }
        auto applied = catalog.Apply(*module.Cast());
        if (!applied) {
            std::cerr << applied.error().ToString() << "\n";
            return 1;
        }
        return 0;
    }
    if (auto function = NQdb::NSql::TMaybeNode<NQdb::NSql::TSqlExternalFunction>(statement)) {
        if (explain) {
            std::cerr << "EXPLAIN requires a SELECT statement\n";
            return 1;
        }
        auto applied = catalog.Apply(*function.Cast());
        if (!applied) {
            std::cerr << applied.error().ToString() << "\n";
            return 1;
        }
        return 0;
    }

    std::vector<std::unique_ptr<NQdb::TParquetSource>> sources;
    auto makeSource = [&](const std::string& path) -> NQdb::TOperatorPtr {
        auto source = std::make_unique<NQdb::TParquetSource>(path);
        auto op = std::make_shared<NQdb::TSourceOperator>(*source, path);
        sources.push_back(std::move(source));
        return op;
    };

    auto planStart = std::chrono::steady_clock::now();
    auto factory = [&](std::string_view table)
        -> std::expected<NQdb::TOperatorPtr, NQumir::TError>
    {
        try {
            return makeSource(ResolveTablePath(config.DataDir, table));
        } catch (const std::exception& e) {
            return std::unexpected(NQumir::TError(
                "cannot open table '" + std::string(table) + "': " + e.what()));
        }
    };
    auto plan = NQdb::BuildPlan(statement, factory);

    if (!plan) {
        std::cerr << plan.error().ToString() << "\n";
        return 1;
    }
    auto snapshot = catalog.Snapshot();
    return ExecutePlan(
        std::move(*plan), config, std::move(snapshot), explain, planStart);
}

int RunQuery(
    ESyntax syntax,
    std::istream& in,
    const TConfig& config,
    NQdb::TExternalModuleCatalog& catalog)
{
    std::string text{
        std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    bool explain = StripExplain(text);
    std::istringstream stream(text);
    if (syntax == ESyntax::Sql) {
        auto statements = ParseSqlStatements(stream);
        if (!statements) {
            std::cerr << statements.error().ToString() << "\n";
            return 1;
        }
        if (explain && statements->size() != 1) {
            std::cerr << "EXPLAIN requires exactly one SELECT statement\n";
            return 1;
        }
        for (const auto& statement : *statements) {
            if (int status = RunSqlStatement(statement, config, catalog, explain)) {
                return status;
            }
        }
        return 0;
    }

    std::vector<std::unique_ptr<NQdb::TParquetSource>> sources;
    auto makeSource = [&](const std::string& path) -> NQdb::TOperatorPtr {
        auto source = std::make_unique<NQdb::TParquetSource>(path);
        auto op = std::make_shared<NQdb::TSourceOperator>(*source, path);
        sources.push_back(std::move(source));
        return op;
    };
    auto planStart = std::chrono::steady_clock::now();
    auto plan = ParseSexpr(stream, makeSource);
    if (!plan) {
        std::cerr << plan.error().ToString() << "\n";
        return 1;
    }
    return ExecutePlan(std::move(*plan), config, nullptr, explain, planStart);
}

std::string HistoryPath() {
    const char* home = std::getenv("HOME");
    return home ? std::string(home) + "/.qdb_history" : std::string();
}

// Multi-line statements are kept multi-line in the in-memory history (for proper
// recall), but persisted one escaped line per entry: '\n' -> "\\n", '\' -> "\\\\".
std::string EscapeHistory(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\\') {
            out += "\\\\";
        } else if (c == '\n') {
            out += "\\n";
        } else {
            out += c;
        }
    }
    return out;
}

std::string UnescapeHistory(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char next = s[++i];
            out += (next == 'n') ? '\n' : next;
        } else {
            out += s[i];
        }
    }
    return out;
}

void LoadHistory(const std::string& path) {
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty()) {
            add_history(UnescapeHistory(line).c_str());
        }
    }
}

void SaveHistory(const std::string& path) {
    std::ofstream out(path);
    if (HIST_ENTRY** list = history_list()) {
        for (size_t i = 0; list[i]; ++i) {
            out << EscapeHistory(list[i]->line) << '\n';
        }
    }
}

std::string_view Trim(std::string_view s) {
    auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == ';'; };
    while (!s.empty() && isSpace(s.front())) {
        s.remove_prefix(1);
    }
    while (!s.empty() && isSpace(s.back())) {
        s.remove_suffix(1);
    }
    return s;
}

// `\d <table>`: open the table and print its column names and types.
void DescribeTable(std::string_view table, const TConfig& config) {
    if (table.empty()) {
        std::cerr << "usage: \\d <table>\n";
        return;
    }
    const std::string path = ResolveTablePath(config.DataDir, table);
    try {
        NQdb::TParquetSource source(path);
        const auto& schema = source.Schema();
        std::cout << "Table \"" << table << "\" (" << path << ")\n";
        for (const auto& column : schema.Columns) {
            std::cout << "  " << column.Name << "  "
                      << NQumir::NAst::NCore::PrintType(column.Type) << "\n";
        }
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
    }
}

bool HasCompleteSqlStatement(const std::string& text) {
    try {
        std::istringstream in(text);
        NQdb::NSql::TTokenStream tokens(in);
        NQumir::NAst::TToken last{};
        bool any = false;
        while (true) {
            auto token = tokens.Next();
            if (token.IsEof()) {
                break;
            }
            last = std::move(token);
            any = true;
        }
        return any && last.Type == NQumir::NAst::TToken::Operator
            && last.Value.i64 == ';';
    } catch (const std::runtime_error& e) {
        const std::string_view message(e.what());
        if (message.starts_with("unterminated")) {
            return false;
        }
        return text.find(';') != std::string::npos;
    }
}

// Reads SQL statements from readline and runs complete batches terminated by ';'.
int RunInteractive(
    TConfig config,
    NQdb::TExternalModuleCatalog& catalog)
{
    const std::string historyPath = HistoryPath();
    if (!historyPath.empty()) {
        LoadHistory(historyPath);
    }

    // Strip trailing newlines but keep the statement multi-line for recall.
    auto trim = [](std::string buffer) {
        while (!buffer.empty() && (buffer.back() == '\n' || buffer.back() == '\r')) {
            buffer.pop_back();
        }
        return buffer;
    };

    std::string buffer;
    while (true) {
        char* line = readline(buffer.empty() ? "qdb> " : "  ..> ");
        if (!line) {
            std::cout << "\n";
            break;
        }
        std::string input(line);
        std::free(line);

        if (buffer.empty()) {
            if (input == "exit" || input == "quit" || input == ".quit") {
                break;
            }
            if (input.empty()) {
                continue;
            }
            if (input.starts_with("\\d")) {
                DescribeTable(Trim(std::string_view(input).substr(2)), config);
                continue;
            }
            if (input.starts_with("\\explain")) {
                auto arg = Trim(std::string_view(input).substr(8));
                if (arg.empty()) {
                    std::cerr << "usage: \\explain <text|sexpr|both>\n";
                } else if (auto mode = ParseExplainMode(arg)) {
                    config.ExplainMode = *mode;
                } else {
                    std::cerr << "unknown explain mode '" << arg
                              << "' (text|sexpr|both)\n";
                }
                continue;
            }
        }

        buffer += input;
        buffer += '\n';
        if (!HasCompleteSqlStatement(buffer)) {
            continue;
        }

        add_history(trim(buffer).c_str());
        if (!historyPath.empty()) {
            SaveHistory(historyPath);
        }
        try {
            std::istringstream in(buffer);
            RunQuery(ESyntax::Sql, in, config, catalog);
        } catch (const std::exception& e) {
            std::cerr << e.what() << "\n";
        }
        buffer.clear();
    }
    return 0;
}

std::expected<NQdb::NScheduler::TInstrumentationSettings, std::string>
ParseInstrumentation(std::string_view components) {
    NQdb::NScheduler::TInstrumentationSettings settings;
    while (true) {
        const auto comma = components.find(',');
        const auto component = components.substr(0, comma);
        if (component == "operators") {
            settings.Operators = true;
        } else if (component == "group_predicate") {
            settings.RowGroupPredicate = true;
        } else {
            return std::unexpected("Unknown instrumentation component '" + std::string(component) +
                "' (expected operators or group_predicate)");
        }
        if (comma == std::string_view::npos) {
            return settings;
        }
        components.remove_prefix(comma + 1);
    }
}

void PrintHelp() {
    std::cout <<
        "qdb [options]\n"
        "Options:\n"
        "  -i <file>                    Input query file; without it, interactive mode\n"
        "  --sexpr                      Input file is an s-expression plan (default)\n"
        "  --sql                        Input file is SQL (built into a plan)\n"
        "  --data <dir>                 Directory of <table>.parquet files for SQL (default: .)\n"
        "  --format <spec>              Output format (default: console)\n"
        "    console                    Aligned table (PostgreSQL style)\n"
        "    csv                        CSV without escaping\n"
        "    <escape>csv                CSV with quoting/escaping\n"
        "    <separator=X>csv           CSV with custom separator X\n"
        "    null                       Consume rows without output\n"
        "  --scheduler <mode>           Execution mode: single, threaded (default: threaded)\n"
        "  --scheduler-workers <n>      Worker count (default: logical CPU count, at least 1)\n"
        "  --scan-tasks <n>             Maximum parquet scan tasks per source (default: auto)\n"
        "  --instrumentation [list]     Enable instrumentation (default: operators)\n"
        "                                Components: operators,group_predicate\n"
        "  --scheduler-counters         Print scheduler and connection counters with --verbose\n"
        "  --cascade-aggregates         Parallelize global aggregates (partial->gather->combine)\n"
        "  --no-partial-aggregates      Disable statistics-selected local grouped aggregation\n"
        "  --shuffle-partitions <n>     Hash shuffle partition count\n"
        "  --queue-depth <n>            Rowset queue capacity per connection lane\n"
        "  --late-materialization=auto|off\n"
        "                                Enable/disable cost-based late materialization\n"
        "  --late-materialization-max-rows <n>\n"
        "                                Maximum statically known output rows (default: 100)\n"
        "  --late-materialization-min-savings <factor>\n"
        "                                Required byte saving factor (default: 2.0)\n"
        "  --shuffle-queue <n>          Rowset queue capacity per shuffle lane\n"
        "  --shuffle-target-rows <n>    Target rows per materialized shuffle batch\n"
        "  --shuffle-max-rows <n>       Maximum rows per materialized shuffle batch\n"
        "  --shuffle-target-bytes <n>   Target bytes per materialized shuffle batch\n"
        "  --nocbo                      Disable cost-based join reordering\n"
        "  --no-runtime-filters         Disable join runtime filters (for A/B runs)\n"
        "  --force-runtime-filters      Emit a join runtime filter wherever a build\n"
        "                               side exists, ignoring selectivity estimates\n"
        "  --explain-mode <mode>        explain output: text (default), sexpr, both\n"
        "  --verbose                    Print the logical and runtime plans\n"
        "  --timing                     Print per-phase timings (planning, kernel build, JIT LLVM, CPU)\n"
        "  --help|-h                    Show this help message\n"
        "  --version|-v                 Show version information\n"
        "  --plan-export                Plan/runtime bundle export mode for the web UI\n"
        "                               (--stdin-json --stdout-json [--cache <dir>])\n"
        "\n"
        "Without -i, qdb starts an interactive SQL prompt (statements end with ';').\n"
        "  \\d [table]                   Describe a table\n"
        "  \\explain <text|sexpr|both>   Set the explain output format\n";
}

} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--plan-export")) {
            return NQdb::RunPlanExport(argc, argv);
        }
    }

    NQumir::NCodeGen::TLLVMInitializer llvmInit;

    TConfig config;
    NQdb::TExternalModuleCatalog catalog;
    std::string queryFile;

    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "-i")) {
            if (i + 1 >= argc) {
                std::cerr << "-i requires a filename argument\n";
                return 1;
            }
            queryFile = argv[++i];
        } else if (!std::strcmp(argv[i], "--sexpr")) {
            config.Syntax = ESyntax::Sexpr;
        } else if (!std::strcmp(argv[i], "--sql")) {
            config.Syntax = ESyntax::Sql;
        } else if (!std::strcmp(argv[i], "--explain-mode")) {
            if (i + 1 >= argc) {
                std::cerr << "--explain-mode requires an argument\n";
                return 1;
            }
            auto mode = ParseExplainMode(argv[++i]);
            if (!mode) {
                std::cerr << "Invalid explain mode (text|sexpr|both)\n";
                return 1;
            }
            config.ExplainMode = *mode;
        } else if (!std::strcmp(argv[i], "--data")) {
            if (i + 1 >= argc) {
                std::cerr << "--data requires a directory argument\n";
                return 1;
            }
            config.DataDir = argv[++i];
        } else if (!std::strcmp(argv[i], "--format")) {
            if (i + 1 >= argc) {
                std::cerr << "--format requires an argument\n";
                return 1;
            }
            try {
                config.Format = ParseFormat(argv[++i]);
            } catch (const std::invalid_argument& e) {
                std::cerr << "Invalid format spec: " << e.what() << "\n";
                return 1;
            }
        } else if (!std::strcmp(argv[i], "--scheduler")) {
            if (i + 1 >= argc) {
                std::cerr << "--scheduler requires an argument\n";
                return 1;
            }
            try {
                config.Scheduler.Scheduler.Mode = ParseSchedulerMode(argv[++i]);
            } catch (const std::invalid_argument& e) {
                std::cerr << e.what() << "\n";
                return 1;
            }
        } else if (!std::strcmp(argv[i], "--scheduler-workers")) {
            if (i + 1 >= argc) {
                std::cerr << "--scheduler-workers requires an argument\n";
                return 1;
            }
            auto workers = std::atoi(argv[++i]);
            if (workers <= 0) {
                std::cerr << "--scheduler-workers requires a positive integer\n";
                return 1;
            }
            config.Scheduler.Scheduler.WorkerCount = static_cast<size_t>(workers);
        } else if (!std::strcmp(argv[i], "--scan-tasks")) {
            if (i + 1 >= argc) {
                std::cerr << "--scan-tasks requires an argument\n";
                return 1;
            }
            auto tasks = std::atoi(argv[++i]);
            if (tasks <= 0) {
                std::cerr << "--scan-tasks requires a positive integer\n";
                return 1;
            }
            config.Scheduler.ScanSplit.MaxScanTasks = static_cast<size_t>(tasks);
        } else if (!std::strcmp(argv[i], "--instrumentation") ||
            std::string_view(argv[i]).starts_with("--instrumentation="))
        {
            std::string_view components = "operators";
            if (const auto argument = std::string_view(argv[i]); argument.starts_with("--instrumentation=")) {
                components = argument.substr(std::string_view("--instrumentation=").size());
            } else if (i + 1 < argc && !std::string_view(argv[i + 1]).starts_with('-')) {
                components = argv[++i];
            }
            auto settings = ParseInstrumentation(components);
            if (!settings) {
                std::cerr << settings.error() << '\n';
                return 1;
            }
            config.Scheduler.Instrumentation = *settings;
            config.Scheduler.Instrumentation.Output = &std::cerr;
        } else if (!std::strcmp(argv[i], "--scheduler-counters")) {
            config.Scheduler.Queue.EnableDebugCounters = true;
        } else if (!std::strcmp(argv[i], "--cascade-aggregates")) {
            config.Scheduler.Aggregate.CascadeGlobal = true;
        } else if (!std::strcmp(argv[i], "--no-partial-aggregates")) {
            config.Scheduler.Aggregate.EnablePartial = false;
        } else if (!std::strcmp(argv[i], "--shuffle-partitions")) {
            if (i + 1 >= argc) {
                std::cerr << "--shuffle-partitions requires an argument\n";
                return 1;
            }
            auto partitions = std::atoi(argv[++i]);
            if (partitions <= 0) {
                std::cerr << "--shuffle-partitions requires a positive integer\n";
                return 1;
            }
            config.Scheduler.HashShuffle.PartitionCount =
                static_cast<size_t>(partitions);
            config.Scheduler.HashShuffle.MaxPartitionCount =
                static_cast<size_t>(partitions);
        } else if (!std::strcmp(argv[i], "--shuffle-queue")) {
            if (i + 1 >= argc) {
                std::cerr << "--shuffle-queue requires an argument\n";
                return 1;
            }
            auto capacity = std::atoi(argv[++i]);
            if (capacity <= 0) {
                std::cerr << "--shuffle-queue requires a positive integer\n";
                return 1;
            }
            config.Scheduler.HashShuffle.MaxQueuedRowsetsPerLane =
                static_cast<size_t>(capacity);
        } else if (!std::strcmp(argv[i], "--queue-depth")) {
            if (i + 1 >= argc) {
                std::cerr << "--queue-depth requires an argument\n";
                return 1;
            }
            auto capacity = std::atoi(argv[++i]);
            if (capacity <= 0) {
                std::cerr << "--queue-depth requires a positive integer\n";
                return 1;
            }
            config.Scheduler.Queue.RowsetCapacityPerLane =
                static_cast<size_t>(capacity);
        } else if (!std::strncmp(
                       argv[i], "--late-materialization=", 23)) {
            const std::string_view mode(argv[i] + 23);
            if (mode == "auto") {
                config.LateMaterialization.Enabled = true;
            } else if (mode == "off") {
                config.LateMaterialization.Enabled = false;
            } else {
                std::cerr << "--late-materialization expects auto or off\n";
                return 1;
            }
        } else if (!std::strcmp(argv[i], "--late-materialization-max-rows")) {
            if (i + 1 >= argc) {
                std::cerr << "--late-materialization-max-rows requires an argument\n";
                return 1;
            }
            auto rows = std::atoll(argv[++i]);
            if (rows <= 0) {
                std::cerr << "--late-materialization-max-rows requires a positive integer\n";
                return 1;
            }
            config.LateMaterialization.MaxOutputRows = static_cast<uint64_t>(rows);
        } else if (!std::strcmp(argv[i], "--late-materialization-min-savings")) {
            if (i + 1 >= argc) {
                std::cerr << "--late-materialization-min-savings requires an argument\n";
                return 1;
            }
            const double factor = std::atof(argv[++i]);
            if (!std::isfinite(factor) || factor < 1.0) {
                std::cerr << "--late-materialization-min-savings requires a number >= 1\n";
                return 1;
            }
            config.LateMaterialization.MinSavingsFactor = factor;
        } else if (!std::strcmp(argv[i], "--shuffle-target-rows")) {
            if (i + 1 >= argc) {
                std::cerr << "--shuffle-target-rows requires an argument\n";
                return 1;
            }
            auto rows = std::atoi(argv[++i]);
            if (rows <= 0) {
                std::cerr << "--shuffle-target-rows requires a positive integer\n";
                return 1;
            }
            config.Scheduler.HashShuffle.TargetOutputBatchRows =
                static_cast<size_t>(rows);
        } else if (!std::strcmp(argv[i], "--shuffle-max-rows")) {
            if (i + 1 >= argc) {
                std::cerr << "--shuffle-max-rows requires an argument\n";
                return 1;
            }
            auto rows = std::atoi(argv[++i]);
            if (rows <= 0) {
                std::cerr << "--shuffle-max-rows requires a positive integer\n";
                return 1;
            }
            config.Scheduler.HashShuffle.MaxOutputBatchRows =
                static_cast<size_t>(rows);
        } else if (!std::strcmp(argv[i], "--shuffle-target-bytes")) {
            if (i + 1 >= argc) {
                std::cerr << "--shuffle-target-bytes requires an argument\n";
                return 1;
            }
            auto bytes = std::atoi(argv[++i]);
            if (bytes <= 0) {
                std::cerr << "--shuffle-target-bytes requires a positive integer\n";
                return 1;
            }
            config.Scheduler.HashShuffle.TargetOutputBatchBytes =
                static_cast<size_t>(bytes);
        } else if (!std::strcmp(argv[i], "--nocbo")) {
            config.EnableCbo = false;
        } else if (!std::strcmp(argv[i], "--no-runtime-filters")) {
            config.EnableRuntimeFilters = false;
        } else if (!std::strcmp(argv[i], "--force-runtime-filters")) {
            config.ForceRuntimeFilters = true;
        } else if (!std::strcmp(argv[i], "--verbose")) {
            config.Verbose = true;
        } else if (!std::strcmp(argv[i], "--timing")) {
            config.Timing = true;
        } else if (!std::strcmp(argv[i], "--help") || !std::strcmp(argv[i], "-h")) {
            PrintHelp();
            return 0;
        } else if (!std::strcmp(argv[i], "--version") || !std::strcmp(argv[i], "-v")) {
            std::cout << QDB_VERSION_STRING << "\n";
            return 0;
        } else {
            std::cerr << "Unknown option: " << argv[i] << "\n";
            return 1;
        }
    }

    try {
        if (queryFile.empty()) {
            return RunInteractive(config, catalog);
        }

        std::ifstream ifs(queryFile);
        if (!ifs) {
            std::cerr << "Cannot open query file: " << queryFile << "\n";
            return 1;
        }
        return RunQuery(config.Syntax, ifs, config, catalog);
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
