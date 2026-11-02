#include "plan_export.h"

#include <qdb/compiler/query.h>
#include <qdb/exec/planner_helpers.h>
#include <qdb/io/parquet/source.h>
#include <qdb/io/text/sink.h>
#include <qdb/kernel/finalize.h>
#include <qdb/scheduler/plan_lowerer.h>
#include <qdb/plan/ops/source.h>
#include <qdb/scheduler/engine_settings.h>
#include <qdb/sql/parser.h>

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/parser/core/printer.h>

#include <readline/history.h>
#include <readline/readline.h>

#include <sys/resource.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using NQdb::NCompiler::ESyntax;
using NQdb::NCompiler::EExplainMode;

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

std::optional<EExplainMode> ParseExplainMode(std::string_view value) {
    if (value == "text") return EExplainMode::Text;
    if (value == "sexpr" || value == "s-expr") return EExplainMode::Sexpr;
    if (value == "both") return EExplainMode::Both;
    return std::nullopt;
}

struct TCliOptions {
    ESyntax Syntax = ESyntax::Sexpr;
    std::string DataDir = ".";
    TFormatSpec Format;
    bool Verbose = false;
    bool Timing = false;
    EExplainMode ExplainMode = EExplainMode::Text;
    NQdb::TEngineSettings Engine;
};

std::string ResolveTablePath(const std::string& dataDir, std::string_view table) {
    if (table.ends_with(".parquet")) {
        return std::string(table);
    }
    return dataDir + "/" + std::string(table) + ".parquet";
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
    const NQdb::NCompiler::TPreparedQuery& prepared,
    const TCliOptions& config,
    bool explain,
    std::chrono::steady_clock::time_point planStart)
{
    const auto& plan = prepared.Plan;
    const auto& annotation = prepared.Annotation;
    const auto& planDiagnostics = prepared.Diagnostics;
    auto planElapsed = std::chrono::steady_clock::now() - planStart;

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
        NQdb::NCompiler::Explain(std::cerr, plan, EExplainMode::Sexpr);
        std::cerr << "\n==================================\n";
    }

    if (explain) {
        NQdb::NCompiler::Explain(std::cout, plan, config.ExplainMode);
        return 0;
    }

    auto* diagnostics = config.Verbose ? &std::cerr : nullptr;
    if (diagnostics) {
        NQdb::PrintRuntimePlan(*diagnostics, plan, config.Engine.Scheduler.Join.Table);
    }

    auto buildStart = std::chrono::steady_clock::now();
    auto lowered = NQdb::NScheduler::LowerPlanToGraph(
        plan, config.Engine.Scheduler, diagnostics, annotation.ExternalCatalog);
    auto buildElapsed = std::chrono::steady_clock::now() - buildStart;
    auto llvmStart = std::chrono::steady_clock::now();
    NQdb::JitFinalizeKernels(
        lowered.Kernels, diagnostics, annotation.ExternalCatalog,
        config.Engine.Scheduler.Jit.CacheDir);
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
            std::move(lowered), counting, config.Engine.Scheduler, diagnostics, &error)) {
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

int RunQuery(
    ESyntax syntax,
    std::istream& in,
    const TCliOptions& config,
    NQdb::TExternalModuleCatalog& catalog)
{
    std::vector<std::unique_ptr<NQdb::TParquetSource>> sources;
    auto factory = [&](std::string_view table)
        -> std::expected<NQdb::TOperatorPtr, NQumir::TError>
    {
        const auto path = syntax == ESyntax::Sql
            ? ResolveTablePath(config.DataDir, table)
            : std::string(table);
        try {
            auto source = std::make_unique<NQdb::TParquetSource>(path);
            auto op = std::make_shared<NQdb::TSourceOperator>(*source, path);
            sources.push_back(std::move(source));
            return op;
        } catch (const std::exception& e) {
            return std::unexpected(NQumir::TError(
                "cannot open table '" + std::string(table) + "': " + e.what()));
        }
    };
    NQdb::NCompiler::TQueryCompiler compiler(catalog, factory, config.Engine.Optimizer);
    const auto parseStart = std::chrono::steady_clock::now();
    auto query = compiler.Parse(in, syntax);
    if (!query) {
        std::cerr << query.error().ToString() << "\n";
        return 1;
    }
    for (const auto& statement : query->Statements) {
        if (syntax == ESyntax::Sql) {
            sources.clear();
        }
        auto planStart = syntax == ESyntax::Sql
            ? std::chrono::steady_clock::now()
            : parseStart;
        auto prepared = compiler.Prepare(statement, query->Explain);
        if (!prepared) {
            std::cerr << prepared.error().ToString() << "\n";
            return 1;
        }
        if (*prepared) {
            if (int status = ExecutePlan(**prepared, config, query->Explain, planStart)) {
                return status;
            }
        }
    }
    return 0;
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
void DescribeTable(std::string_view table, const TCliOptions& config) {
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
    TCliOptions config,
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

void PrintHelp() {
    std::cout <<
        "qdb [options] [--section:key=value ...]\n"
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
        "  --explain-mode <mode>        explain output: text (default), sexpr, both\n"
        "  --verbose                    Print the logical and runtime plans and the effective settings\n"
        "  --timing                     Print per-phase timings (planning, kernel build, JIT LLVM, CPU)\n"
        "  --config <path>              Ini file with engine settings\n"
        "  --help|-h                    Show this help message\n"
        "  --version|-v                 Show version information\n"
        "  --plan-export                Plan/runtime bundle export mode for the web UI\n"
        "                               (--stdin-json --stdout-json [--cache <dir>])\n"
        "\n"
        "Without -i, qdb starts an interactive SQL prompt (statements end with ';').\n"
        "  \\d [table]                   Describe a table\n"
        "  \\explain <text|sexpr|both>   Set the explain output format\n"
        "\n"
        "Engine settings come from, in order, the built-in defaults, the file\n"
        "$XDG_CONFIG_HOME/qdb/qdb.ini (else ~/.config/qdb/qdb.ini) if it exists,\n"
        "--config <path>, and --section:key=value arguments. An ini file lists the keys\n"
        "under [section] headers as 'key = value'; '#' and ';' start comments. An unknown\n"
        "section or key and a bad value are errors.\n"
        "\n"
        "Settings (--section:key=<value>):\n"
        << NQdb::EngineConfigSchema().Help();
}

} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--plan-export")) {
            return NQdb::RunPlanExport(argc, argv);
        }
    }

    NQumir::NCodeGen::TLLVMInitializer llvmInit;

    TCliOptions config;
    NQdb::TExternalModuleCatalog catalog;
    std::string queryFile;
    std::optional<std::string> configPath;
    std::vector<std::string> overrides;

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
        } else if (!std::strcmp(argv[i], "--config")) {
            if (i + 1 >= argc) {
                std::cerr << "--config requires a path\n";
                return 1;
            }
            configPath = argv[++i];
        } else if (NQdb::NUtils::IsConfigOverride(argv[i])) {
            overrides.emplace_back(argv[i] + 2);
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

    NQdb::NUtils::TConfig engineConfig(NQdb::EngineConfigSchema());
    if (auto loaded = NQdb::LoadEngineConfig(engineConfig,  NQdb::NUtils::DefaultConfigPath(), configPath, overrides); !loaded) {
        std::cerr << loaded.error() << "\n";
        return 1;
    }
    NQdb::ApplyEngineConfig(engineConfig, config.Engine);
    NQdb::SetArrowMemoryPool(config.Engine.ArrowMemoryPool);
    if (config.Verbose) {
        std::cerr << "========== SETTINGS ==========\n" << engineConfig.Dump()
            << "==============================\n";
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
