#include "plan_export.h"
#include <qdb/exec/instrumentation/operator.h>

#include <qdb/catalog/external_module.h>
#include <qdb/exec/plan_builder.h>
#include <qdb/exec/planner_helpers.h>
#include <qdb/exec/runtime_context.h>
#include <qdb/io/io.h>
#include <qdb/plan/build.h>
#include <qdb/plan/pipeline.h>
#include <qdb/plan/plan_print.h>
#include <qdb/plan/stats_codec.h>
#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/cte_consumer.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/limit.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/sort.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/ops/union.h>
#include <qdb/plan/ops/window.h>
#include <qdb/plan/passes/column_pruning.h>
#include <qdb/plan/passes/equijoin.h>
#include <qdb/plan/passes/join_order.h>
#include <qdb/plan/passes/qualify_columns.h>
#include <qdb/plan/passes/top_sort.h>
#include <qdb/plan/passes/typing.h>
#include <qdb/plan/passes/unbound_vars.h>
#include <qdb/plan/types/decimal.h>
#include <qdb/plan/types/nullable.h>
#include <qdb/kernel/compiler.h>
#include <qdb/kernel/finalize_fused.h>
#include <qdb/kernel/join_key.h>
#include <qdb/kernel/annotate_type.h>
#include <qdb/scheduler/plan_lowerer.h>
#include <qdb/scheduler/scan_split.h>
#include <qdb/sexp/printer.h>
#include <qdb/sql/parser.h>

#include <qumir/codegen/llvm/llvm_codegen.h>
#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/frontend/compose.h>
#include <qumir/frontend/source_module_loader.h>
#include <qumir/ir/builder.h>
#include <qumir/ir/lowering/lower_ast.h>
#include <qumir/ir/passes/transforms/pipeline.h>
#include <qumir/parser/core/parser.h>
#include <qumir/parser/core/printer.h>
#include <qumir/parser/pragma.h>
#include <qumir/parser/type.h>
#include <qumir/runner/runner_llvm.h>
#include <qumir/semantics/name_resolution/name_resolver.h>
#include <qumir/semantics/transform/transform.h>

#include <llvm/Support/Base64.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/JSON.h>
#include <llvm/Object/Wasm.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <expected>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using namespace NQdb;

constexpr int64_t WasmPageSize = 65536;
constexpr int64_t WasmSlot0 = 1 << 20;
constexpr int64_t WasmStackSize = 262144;
constexpr int64_t WasmInitialSlackPages = 256;

struct TTableStats {
    struct TScalar {
        enum class EKind {
            Integer,
            Number,
        };

        EKind Kind = EKind::Integer;
        int64_t Integer = 0;
        double Number = 0.0;
    };

    struct TColumn {
        std::string Name;
        std::optional<uint64_t> Ndv;
        bool NdvIsExact = false;
        std::optional<uint64_t> NullCount;
        std::optional<TScalar> Min;
        std::optional<TScalar> Max;
        std::vector<TScalar> Histogram;
    };

    int64_t Rows = 0;
    int64_t Bytes = 0;
    size_t RowGroups = 0;
    std::vector<TColumn> Columns;
};

std::optional<uint64_t> EncodeStatsScalar(
    const TTableStats::TScalar& value,
    const NQumir::NAst::TTypePtr& type)
{
    switch (NQdb::StatsScalarKind(type)) {
        case NQdb::EStatsScalarKind::None:
            return std::nullopt;
        case NQdb::EStatsScalarKind::Float:
            return std::bit_cast<uint64_t>(
                value.Kind == TTableStats::TScalar::EKind::Number
                    ? value.Number
                    : static_cast<double>(value.Integer));
        case NQdb::EStatsScalarKind::Int:
            return std::bit_cast<uint64_t>(
                value.Kind == TTableStats::TScalar::EKind::Integer
                    ? value.Integer
                    : static_cast<int64_t>(value.Number));
    }
    return std::nullopt;
}

class TSchemaOnlySource final
    : public NQdb::ISource
    , public NQdb::NScheduler::IScanMetadataSource
{
public:
    explicit TSchemaOnlySource(
        std::vector<std::pair<std::string, NQumir::NAst::TTypePtr>> fields,
        TTableStats stats)
        : Stats_(stats)
    {
        Names_.reserve(fields.size());
        Columns_.reserve(fields.size());
        AllColumns_.reserve(fields.size());
        for (auto& [name, type] : fields) {
            Names_.push_back(std::move(name));
            AllColumns_.push_back({
                .Name = Names_.back(),
                .Type = std::move(type),
            });
        }
        Columns_ = AllColumns_;
        Schema_ = NQdb::TSchema{std::span<const NQdb::TColumnSchema>(Columns_)};
    }

    const NQdb::TSchema& Schema() const override {
        return Schema_;
    }

    const NQdb::TStatsPtr Stats() const override {
        if (Stats_.Rows <= 0) {
            return nullptr;
        }
        auto stats = std::make_shared<NQdb::TStats>();
        stats->RowCount = static_cast<uint64_t>(Stats_.Rows);
        for (const auto& column : Stats_.Columns) {
            auto schemaIt = std::find_if(
                AllColumns_.begin(),
                AllColumns_.end(),
                [&](const NQdb::TColumnSchema& schemaColumn) {
                    return schemaColumn.Name == column.Name;
                });
            if (schemaIt == AllColumns_.end()) {
                continue;
            }

            auto columnStats = std::make_shared<NQdb::TStats::TColumnStats>();
            bool hasStats = false;
            if (column.Ndv) {
                columnStats->Ndv = *column.Ndv;
                hasStats = true;
            }
            columnStats->NdvIsExact = column.NdvIsExact;
            if (column.NullCount) {
                columnStats->NullCount = *column.NullCount;
                hasStats = true;
            }
            if (column.Min) {
                if (auto encoded = EncodeStatsScalar(*column.Min, schemaIt->Type)) {
                    columnStats->MinValue = *encoded;
                    hasStats = true;
                }
            }
            if (column.Max) {
                if (auto encoded = EncodeStatsScalar(*column.Max, schemaIt->Type)) {
                    columnStats->MaxValue = *encoded;
                    hasStats = true;
                }
            }
            for (const auto& bucket : column.Histogram) {
                if (auto encoded = EncodeStatsScalar(bucket, schemaIt->Type)) {
                    columnStats->Histogram.push_back(*encoded);
                }
            }
            hasStats = hasStats || !columnStats->Histogram.empty();
            if (hasStats) {
                stats->ColumnStats[column.Name] = std::move(columnStats);
            }
        }
        return stats;
    }

    bool Next(NQdb::TRowSet&) override {
        return false;
    }

    void RestrictColumns(const std::unordered_set<std::string>& names) override {
        if (names.empty()) {
            return;
        }

        Columns_.clear();
        for (const auto& column : AllColumns_) {
            if (names.contains(std::string(column.Name))) {
                Columns_.push_back(column);
            }
        }
        Schema_ = NQdb::TSchema{std::span<const NQdb::TColumnSchema>(Columns_)};
    }

    std::vector<NQdb::NScheduler::TScanRowGroup> ScanRowGroups() const override {
        if (Stats_.Rows <= 0 || Stats_.RowGroups == 0) {
            return {};
        }

        std::vector<NQdb::NScheduler::TScanRowGroup> out;
        out.reserve(Stats_.RowGroups);
        const auto groups = static_cast<int64_t>(Stats_.RowGroups);
        for (size_t i = 0; i < Stats_.RowGroups; ++i) {
            const auto begin =
                Stats_.Rows * static_cast<int64_t>(i) / groups;
            const auto end =
                Stats_.Rows * static_cast<int64_t>(i + 1) / groups;
            const auto byteBegin =
                Stats_.Bytes * static_cast<int64_t>(i) / groups;
            const auto byteEnd =
                Stats_.Bytes * static_cast<int64_t>(i + 1) / groups;
            out.push_back({
                .RowGroup = i,
                .RowCount = end - begin,
                .ByteSize = byteEnd - byteBegin,
            });
        }
        return out;
    }

private:
    TTableStats Stats_;
    std::vector<std::string> Names_;
    std::vector<NQdb::TColumnSchema> AllColumns_;
    std::vector<NQdb::TColumnSchema> Columns_;
    NQdb::TSchema Schema_;
};

struct TCatalog {
    std::unordered_map<std::string, std::unique_ptr<TSchemaOnlySource>> Sources;
};

struct TExportRequest {
    std::string Sql;
    TCatalog Catalog;
    NQdb::NScheduler::TSettings Scheduler;
    bool EmbedWasm = false;
    // Defaults to 64 so old clients that omit this field keep current behavior.
    int WasmBits = 64;
};

struct TParsedSql {
    NQdb::TOperatorPtr Plan;
    std::shared_ptr<const NQdb::TExternalCatalogSnapshot> ExternalCatalog;
};

struct TKernelArtifacts {
    NQdb::TExecStageId ExecStageId = NQdb::InvalidExecStageId;
    std::string Name;
    std::string Ast;
    std::string Ir;
    std::string Llvm;
    std::string Wasm;
    std::unordered_map<std::string, std::string> Entrypoints;
};

struct TWasmFinalizeResult {
    std::vector<TKernelArtifacts> Kernels;
    std::string QueryWasm;
    std::string ErrorStage;
    std::string Error;
    int64_t HeapBase = 0;
};

class TArtifactStore {
public:
    std::string Add(
        std::string kind,
        std::string text,
        std::string label = {},
        std::string stage = {})
    {
        auto id = "a" + std::to_string(NextId_++);
        llvm::json::Object item{
            {"kind", std::move(kind)},
            {"encoding", "utf-8"},
            {"text", std::move(text)},
        };
        if (!label.empty()) {
            item["label"] = std::move(label);
        }
        if (!stage.empty()) {
            item["stage"] = std::move(stage);
        }
        Items_[id] = std::move(item);
        return id;
    }

    std::string AddBinary(
        std::string kind,
        std::string bytes,
        std::string label = {},
        std::string stage = {})
    {
        auto id = "a" + std::to_string(NextId_++);
        llvm::json::Object item{
            {"kind", std::move(kind)},
            {"encoding", "base64"},
            {"data", llvm::encodeBase64(bytes)},
            {"byteLength", static_cast<int64_t>(bytes.size())},
        };
        if (!label.empty()) {
            item["label"] = std::move(label);
        }
        if (!stage.empty()) {
            item["stage"] = std::move(stage);
        }
        Items_[id] = std::move(item);
        return id;
    }

    llvm::json::Object Take() {
        return std::move(Items_);
    }

private:
    size_t NextId_ = 0;
    llvm::json::Object Items_;
};

class TOutputSilencer {
public:
    TOutputSilencer() {
        std::fflush(stdout);
        std::fflush(stderr);
        SavedStdoutFd_ = dup(STDOUT_FILENO);
        SavedStderrFd_ = dup(STDERR_FILENO);
        int nullFd = open("/dev/null", O_WRONLY);
        if (SavedStdoutFd_ >= 0 && nullFd >= 0) {
            dup2(nullFd, STDOUT_FILENO);
        }
        if (SavedStderrFd_ >= 0 && nullFd >= 0) {
            dup2(nullFd, STDERR_FILENO);
        }
        if (nullFd >= 0) {
            close(nullFd);
        }
    }

    ~TOutputSilencer() {
        std::fflush(stdout);
        std::fflush(stderr);
        if (SavedStdoutFd_ >= 0) {
            dup2(SavedStdoutFd_, STDOUT_FILENO);
            close(SavedStdoutFd_);
        }
        if (SavedStderrFd_ >= 0) {
            dup2(SavedStderrFd_, STDERR_FILENO);
            close(SavedStderrFd_);
        }
    }

private:
    int SavedStdoutFd_ = -1;
    int SavedStderrFd_ = -1;
};

std::string ReadAllStdin() {
    std::ostringstream out;
    out << std::cin.rdbuf();
    return out.str();
}

std::string ToJsonString(llvm::json::Value value) {
    std::string out;
    llvm::raw_string_ostream os(out);
    os << value;
    return out;
}

llvm::json::Object ErrorObject(std::string stage, std::string message) {
    return llvm::json::Object{
        {"ok", false},
        {"error", llvm::json::Object{
            {"stage", std::move(stage)},
            {"message", std::move(message)},
        }},
    };
}

int PrintError(std::string stage, std::string message) {
    std::cout << ToJsonString(ErrorObject(std::move(stage), std::move(message)))
              << "\n";
    return 0;
}

std::filesystem::path TempPath(std::string_view suffix) {
    static uint64_t nextId = 0;
    auto name = "qdb-plan-export-" + std::to_string(::getpid()) + "-" +
        std::to_string(nextId++) + std::string(suffix);
    return std::filesystem::temp_directory_path() / name;
}

std::string ReadBinaryFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(
        std::istreambuf_iterator<char>(in),
        std::istreambuf_iterator<char>());
}

void WriteResponseArgument(std::ostream& out, std::string_view argument) {
    out.put('"');
    for (char ch : argument) {
        if (ch == '\\' || ch == '"') {
            out.put('\\');
        }
        out.put(ch);
    }
    out << "\"\n";
}

std::expected<std::monostate, std::string> RunWasmLd(
    const std::vector<std::filesystem::path>& objectPaths,
    const std::filesystem::path& wasmPath,
    int64_t globalBase,
    int wasmBits)
{
    std::vector<std::string> linkArgs{
        "--no-entry",
        "--export-all",
        "--allow-undefined",
        "--import-memory",
    };
    if (wasmBits == 64) {
        linkArgs.push_back("-mwasm64");
    }
    linkArgs.push_back("--global-base=" + std::to_string(globalBase));
    linkArgs.push_back("-z");
    linkArgs.push_back("stack-size=" + std::to_string(WasmStackSize));
    linkArgs.push_back("-o");
    linkArgs.push_back(wasmPath.string());
    linkArgs.reserve(linkArgs.size() + objectPaths.size());
    for (const auto& path : objectPaths) {
        linkArgs.push_back(path.string());
    }

    const auto responsePath = TempPath(".rsp");
    {
        std::ofstream response(responsePath, std::ios::binary);
        if (!response) {
            return std::unexpected("cannot create temporary wasm-ld response file");
        }
        for (const auto& arg : linkArgs) {
            WriteResponseArgument(response, arg);
        }
        if (!response) {
            std::error_code ec;
            std::filesystem::remove(responsePath, ec);
            return std::unexpected("cannot write temporary wasm-ld response file");
        }
    }

    std::vector<std::string> args{
        "wasm-ld",
        "--rsp-quoting=posix",
        "@" + responsePath.string(),
    };
    std::vector<char*> argv{
        args[0].data(),
        args[1].data(),
        args[2].data(),
        nullptr,
    };

    const pid_t pid = fork();
    if (pid < 0) {
        std::error_code ec;
        std::filesystem::remove(responsePath, ec);
        return std::unexpected("fork failed");
    }
    if (pid == 0) {
        int nullFd = open("/dev/null", O_WRONLY);
        if (nullFd >= 0) {
            dup2(nullFd, STDOUT_FILENO);
            dup2(nullFd, STDERR_FILENO);
            close(nullFd);
        }
        execvp(argv[0], argv.data());
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        std::error_code ec;
        std::filesystem::remove(responsePath, ec);
        return std::unexpected("waitpid failed");
    }
    std::error_code ec;
    std::filesystem::remove(responsePath, ec);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return std::unexpected(
            "wasm-ld failed with code " +
            std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : -1));
    }
    return std::monostate{};
}

std::expected<int64_t, std::string> ExtractWasmHeapBase(const std::string& wasm) {
    llvm::Error parseError = llvm::Error::success();
    llvm::object::WasmObjectFile object(
        llvm::MemoryBufferRef(
            llvm::StringRef(wasm.data(), wasm.size()), "qdb-fused.wasm"),
        parseError);
    if (parseError) {
        return std::unexpected(llvm::toString(std::move(parseError)));
    }

    uint32_t heapBaseGlobalIndex = 0;
    bool foundExport = false;
    for (const auto& exportEntry : object.exports()) {
        if (exportEntry.Name != "__heap_base") {
            continue;
        }
        if (exportEntry.Kind != llvm::wasm::WASM_EXTERNAL_GLOBAL) {
            return std::unexpected("__heap_base is not a wasm global export");
        }
        heapBaseGlobalIndex = exportEntry.Index;
        foundExport = true;
        break;
    }
    if (!foundExport) {
        return std::unexpected("linked wasm does not export __heap_base");
    }

    for (const auto& global : object.globals()) {
        if (global.Index != heapBaseGlobalIndex) {
            continue;
        }
        if (global.InitExpr.Extended) {
            return std::unexpected("__heap_base uses an unsupported init expr");
        }
        switch (global.InitExpr.Inst.Opcode) {
            case llvm::wasm::WASM_OPCODE_I32_CONST:
                return static_cast<int64_t>(global.InitExpr.Inst.Value.Int32);
            case llvm::wasm::WASM_OPCODE_I64_CONST:
                return global.InitExpr.Inst.Value.Int64;
            default:
                return std::unexpected("__heap_base is not a constant global");
        }
    }

    return std::unexpected("__heap_base global definition was not found");
}

struct TCompiledWasm64Module {
    std::string Bytes;
    int64_t HeapBase = 0;
};

// Compiles a kernel AST to wasm32 or wasm64 (wasmBits: 32 | 64). wasm64
// (Memory64) is preferred; wasm32 is a fallback for browsers without
// Memory64, with linear memory capped at 4GiB.
std::expected<TCompiledWasm64Module, std::string> CompileKernelAstToWasm(
    NQumir::NAst::TExprPtr ast,
    int64_t globalBase,
    const std::vector<std::string>& entryNames,
    const std::string& cacheDir,
    const std::shared_ptr<const NQdb::TExternalCatalogSnapshot>& externalCatalog,
    int wasmBits)
{
    const std::string targetTriple =
        wasmBits == 32 ? "wasm32-unknown-unknown" : "wasm64-unknown-unknown";
    auto opts = NQdb::KernelRunnerOptions();
    opts.CoreInput = !(externalCatalog && externalCatalog->HasKumirModules());
    opts.NativeCode = false;
    opts.TargetTriple = targetTriple;
    NQumir::TLLVMRunner runner(std::move(opts));

    const auto wasmPath = TempPath(".wasm");
    std::vector<std::filesystem::path> objectPaths;
    std::vector<std::filesystem::path> temporaryObjects;
    std::vector<std::string> objectBlobs;

    std::string err;
    std::optional<NQumir::NFrontend::TComposeResult> externalProgram;
    if (externalCatalog) {
        auto composed = externalCatalog->ComposeReferenced(
            ast, targetTriple, cacheDir);
        if (!composed) {
            return std::unexpected(composed.error().ToString());
        }
        externalProgram = std::move(*composed);
    }
    if (externalProgram) {
        auto object = NQdb::CompileKernelAstToObject(
            runner, std::move(*externalProgram), entryNames, &err);
        if (!object) {
            return std::unexpected(
                err.empty() ? "wasm kernel compilation failed" : err);
        }
        objectBlobs.push_back(std::move(*object));
    } else if (cacheDir.empty()) {
        auto object = NQdb::CompileKernelAstToObject(
            runner, std::move(ast), entryNames, &err);
        if (!object) {
            return std::unexpected(
                err.empty() ? "wasm kernel compilation failed" : err);
        }
        objectBlobs.push_back(std::move(*object));
    } else {
        auto compiled = NQdb::CompileKernelAstToObjectsCached(
            runner, std::move(ast), entryNames, cacheDir, &err);
        if (!compiled) {
            return std::unexpected(
                err.empty() ? "cached wasm kernel compilation failed" : err);
        }
        objectPaths.reserve(
            compiled->ObjectFiles.size() + compiled->ObjectBlobs.size() + 1);
        for (auto& path : compiled->ObjectFiles) {
            objectPaths.emplace_back(std::move(path));
        }
        objectBlobs = std::move(compiled->ObjectBlobs);
        objectBlobs.push_back(std::move(compiled->KernelObject));
    }

    auto cleanup = [&]() {
        std::error_code ec;
        for (const auto& path : temporaryObjects) {
            std::filesystem::remove(path, ec);
            ec.clear();
        }
        std::filesystem::remove(wasmPath, ec);
    };

    for (const auto& bytes : objectBlobs) {
        const auto objPath = TempPath(".o");
        std::ofstream obj(objPath, std::ios::binary);
        if (!obj) {
            cleanup();
            return std::unexpected("cannot create temporary wasm object");
        }
        temporaryObjects.push_back(objPath);
        obj.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!obj) {
            cleanup();
            return std::unexpected("cannot write temporary wasm object");
        }
        objectPaths.push_back(objPath);
    }

    if (auto linked = RunWasmLd(objectPaths, wasmPath, globalBase, wasmBits); !linked) {
        cleanup();
        return std::unexpected(linked.error());
    }

    auto wasm = ReadBinaryFile(wasmPath);
    cleanup();
    if (wasm.empty()) {
        return std::unexpected("empty wasm output");
    }
    auto heapBase = ExtractWasmHeapBase(wasm);
    if (!heapBase) {
        return std::unexpected(heapBase.error());
    }
    return TCompiledWasm64Module{
        .Bytes = std::move(wasm),
        .HeapBase = *heapBase,
    };
}

NQumir::NAst::TTypePtr TranslateSchemaNamedTypes(
    NQumir::NAst::TTypePtr type)
{
    using namespace NQumir::NAst;

    if (auto named = TMaybeType<TNamedType>(type)) {
        auto value = named.Cast();
        if (value->Name == "Nullable") {
            if (value->UnderlyingType || value->TypeArgs.size() != 1 ||
                value->TypeArgs[0].Kind != TGenericArg::EKind::Type ||
                !value->TypeArgs[0].Type)
            {
                throw std::runtime_error(
                    "schema type Nullable expects one type argument");
            }
            return std::make_shared<TNullable>(
                TranslateSchemaNamedTypes(value->TypeArgs[0].Type));
        }
        if (value->Name == "Decimal") {
            if (value->UnderlyingType || value->TypeArgs.size() != 2 ||
                value->TypeArgs[0].Kind != TGenericArg::EKind::Value ||
                value->TypeArgs[1].Kind != TGenericArg::EKind::Value)
            {
                throw std::runtime_error(
                    "schema type Decimal expects precision and scale value arguments");
            }
            auto precision = ParseDecimalArg(value->TypeArgs[0].Value);
            auto scale = ParseDecimalArg(value->TypeArgs[1].Value);
            if (!precision || !scale) {
                throw std::runtime_error(
                    "schema type Decimal precision and scale must be integers");
            }
            return std::make_shared<TDecimal>(*precision, *scale);
        }
        for (auto& arg : value->TypeArgs) {
            if (arg.Kind == TGenericArg::EKind::Type) {
                arg.Type = TranslateSchemaNamedTypes(std::move(arg.Type));
            }
        }
        value->UnderlyingType =
            TranslateSchemaNamedTypes(std::move(value->UnderlyingType));
        return type;
    }
    if (auto function = TMaybeType<TFunctionType>(type)) {
        auto value = function.Cast();
        for (auto& param : value->ParamTypes) {
            param = TranslateSchemaNamedTypes(std::move(param));
        }
        value->ReturnType = TranslateSchemaNamedTypes(std::move(value->ReturnType));
    } else if (auto future = TMaybeType<TFutureType>(type)) {
        auto value = future.Cast();
        value->ResultType = TranslateSchemaNamedTypes(std::move(value->ResultType));
    } else if (auto array = TMaybeType<TArrayType>(type)) {
        auto value = array.Cast();
        value->ElementType = TranslateSchemaNamedTypes(std::move(value->ElementType));
    } else if (auto pointer = TMaybeType<TPointerType>(type)) {
        auto value = pointer.Cast();
        value->PointeeType = TranslateSchemaNamedTypes(std::move(value->PointeeType));
    } else if (auto reference = TMaybeType<TReferenceType>(type)) {
        auto value = reference.Cast();
        value->ReferencedType = TranslateSchemaNamedTypes(std::move(value->ReferencedType));
    } else if (auto structure = TMaybeType<TStructType>(type)) {
        for (auto& [name, fieldType] : structure.Cast()->Fields) {
            fieldType = TranslateSchemaNamedTypes(std::move(fieldType));
        }
    }
    return type;
}

NQumir::NAst::TTypePtr ParseSchemaType(std::string_view typeName) {
    std::istringstream input{std::string(typeName)};
    NQumir::NAst::NCore::TTokenStream tokens(input);
    auto parsed = NQumir::NAst::NCore::ParseType(tokens);
    if (!parsed) {
        throw std::runtime_error(
            "invalid schema type '" + std::string(typeName) + "': " +
            parsed.error().ToString());
    }
    if (!tokens.Next().IsEof()) {
        throw std::runtime_error(
            "invalid schema type '" + std::string(typeName) +
            "': unexpected trailing input");
    }
    return TranslateSchemaNamedTypes(std::move(*parsed));
}

std::optional<uint64_t> ParseUnsignedStatsInteger(
    const llvm::json::Object& object,
    const char* key)
{
    auto value = object.getInteger(key);
    if (!value || *value < 0) {
        return std::nullopt;
    }
    return static_cast<uint64_t>(*value);
}

std::optional<TTableStats::TScalar> ParseStatsScalar(const llvm::json::Value& value) {
    if (auto integer = value.getAsInteger()) {
        return TTableStats::TScalar{
            .Kind = TTableStats::TScalar::EKind::Integer,
            .Integer = *integer,
        };
    }
    if (auto number = value.getAsNumber()) {
        return TTableStats::TScalar{
            .Kind = TTableStats::TScalar::EKind::Number,
            .Number = *number,
        };
    }
    if (auto boolean = value.getAsBoolean()) {
        return TTableStats::TScalar{
            .Kind = TTableStats::TScalar::EKind::Integer,
            .Integer = *boolean ? 1 : 0,
        };
    }
    return std::nullopt;
}

std::optional<TTableStats::TColumn> ParseColumnStats(const llvm::json::Object& object) {
    auto name = object.getString("name");
    if (!name) {
        return std::nullopt;
    }

    TTableStats::TColumn out;
    out.Name = std::string(*name);
    out.Ndv = ParseUnsignedStatsInteger(object, "ndv");
    out.NdvIsExact = object.getBoolean("ndv_exact").value_or(false);
    out.NullCount = ParseUnsignedStatsInteger(object, "null_count");
    if (const auto* minValue = object.get("min")) {
        out.Min = ParseStatsScalar(*minValue);
    }
    if (const auto* maxValue = object.get("max")) {
        out.Max = ParseStatsScalar(*maxValue);
    }
    if (const auto* histogram = object.getArray("histogram")) {
        out.Histogram.reserve(histogram->size());
        for (const auto& bucket : *histogram) {
            if (auto scalar = ParseStatsScalar(bucket)) {
                out.Histogram.push_back(*scalar);
            }
        }
    }
    return out;
}

std::vector<std::pair<std::string, NQumir::NAst::TTypePtr>> ParseTableColumns(
    const llvm::json::Object& table)
{
    auto* columns = table.getArray("columns");
    if (!columns) {
        throw std::runtime_error("dataset table is missing columns array");
    }

    std::vector<std::pair<std::string, NQumir::NAst::TTypePtr>> out;
    out.reserve(columns->size());
    for (const auto& columnValue : *columns) {
        const auto* column = columnValue.getAsObject();
        if (!column) {
            throw std::runtime_error("dataset column must be an object");
        }
        auto name = column->getString("name");
        auto type = column->getString("type");
        if (!name || !type) {
            throw std::runtime_error("dataset column must have name and type");
        }
        out.emplace_back(std::string(*name), ParseSchemaType(*type));
    }
    return out;
}

TTableStats ParseTableStats(const llvm::json::Object& table) {
    TTableStats stats;
    const auto* value = table.get("stats");
    const auto* object = value ? value->getAsObject() : nullptr;
    if (!object) {
        return stats;
    }

    if (auto rows = object->getInteger("rows")) {
        stats.Rows = std::max<int64_t>(*rows, 0);
    }
    if (auto bytes = object->getInteger("bytes")) {
        stats.Bytes = std::max<int64_t>(*bytes, 0);
    }
    if (auto rowGroups = object->getInteger("rowGroups")) {
        if (*rowGroups > 0) {
            stats.RowGroups = static_cast<size_t>(*rowGroups);
        }
    }
    if (const auto* columns = object->getArray("columns")) {
        stats.Columns.reserve(columns->size());
        for (const auto& columnValue : *columns) {
            const auto* column = columnValue.getAsObject();
            if (!column) {
                continue;
            }
            if (auto parsed = ParseColumnStats(*column)) {
                stats.Columns.push_back(std::move(*parsed));
            }
        }
    }
    if (stats.Bytes == 0 && stats.Rows > 0) {
        stats.Bytes = stats.Rows * 128;
    }
    if (stats.RowGroups == 0 && stats.Rows > 0) {
        stats.RowGroups = 1;
    }
    return stats;
}

TCatalog ParseCatalog(const llvm::json::Object& root) {
    const auto* datasetValue = root.get("dataset");
    const auto* dataset = datasetValue ? datasetValue->getAsObject() : nullptr;
    if (!dataset) {
        throw std::runtime_error("request is missing dataset object");
    }

    auto* tables = dataset->getArray("tables");
    if (!tables) {
        throw std::runtime_error("dataset is missing tables array");
    }

    TCatalog catalog;
    for (const auto& tableValue : *tables) {
        const auto* table = tableValue.getAsObject();
        if (!table) {
            throw std::runtime_error("dataset table must be an object");
        }
        auto name = table->getString("name");
        if (!name) {
            throw std::runtime_error("dataset table must have name");
        }
        catalog.Sources.emplace(
            std::string(*name),
            std::make_unique<TSchemaOnlySource>(
                ParseTableColumns(*table),
                ParseTableStats(*table)));
    }
    return catalog;
}

NQdb::NScheduler::EExecutionMode ParseSchedulerMode(std::string_view mode) {
    if (mode == "single") {
        return NQdb::NScheduler::EExecutionMode::SingleThreadedScheduler;
    }
    if (mode == "threaded") {
        return NQdb::NScheduler::EExecutionMode::ThreadedScheduler;
    }
    throw std::runtime_error("unknown scheduler mode: " + std::string(mode));
}

std::string SchedulerModeName(NQdb::NScheduler::EExecutionMode mode) {
    using NQdb::NScheduler::EExecutionMode;
    switch (mode) {
        case EExecutionMode::SingleThreadedScheduler:
            return "single";
        case EExecutionMode::ThreadedScheduler:
            return "threaded";
    }
    return "unknown";
}

NQdb::NScheduler::TSettings ParseSchedulerSettings(const llvm::json::Object& root) {
    NQdb::NScheduler::TSettings settings;

    const auto* optionsValue = root.get("options");
    const auto* options = optionsValue ? optionsValue->getAsObject() : nullptr;
    if (!options) {
        return settings;
    }

    if (auto scheduler = options->getString("scheduler")) {
        settings.Scheduler.Mode = ParseSchedulerMode(*scheduler);
    }
    if (auto workers = options->getInteger("schedulerWorkers")) {
        if (*workers > 0) {
            settings.Scheduler.WorkerCount = static_cast<size_t>(*workers);
        }
    }
    if (auto scanTasks = options->getInteger("scanTasks")) {
        if (*scanTasks > 0) {
            settings.ScanSplit.MaxScanTasks = static_cast<size_t>(*scanTasks);
        }
    }
    if (auto shuffleParts = options->getInteger("shufflePartitions")) {
        if (*shuffleParts > 0) {
            settings.HashShuffle.PartitionCount = static_cast<size_t>(*shuffleParts);
            settings.HashShuffle.MaxPartitionCount = static_cast<size_t>(*shuffleParts);
        }
    }
    settings.Instrumentation.Operators = options->getBoolean("operatorInstrumentation").value_or(false);
    return settings;
}

bool ParseEmbedWasm(const llvm::json::Object& root) {
    const auto* optionsValue = root.get("options");
    const auto* options = optionsValue ? optionsValue->getAsObject() : nullptr;
    if (!options) {
        return false;
    }
    return options->getBoolean("embedWasm").value_or(false);
}

int ParseWasmBits(const llvm::json::Object& root) {
    const auto* optionsValue = root.get("options");
    const auto* options = optionsValue ? optionsValue->getAsObject() : nullptr;
    if (!options) {
        return 64;
    }
    int64_t bits = options->getInteger("wasmBits").value_or(64);
    return bits == 32 ? 32 : 64;
}

std::expected<TExportRequest, std::string> ParseRequest(std::string_view text) {
    auto parsed = llvm::json::parse(text);
    if (!parsed) {
        return std::unexpected(llvm::toString(parsed.takeError()));
    }

    const auto* root = parsed->getAsObject();
    if (!root) {
        return std::unexpected("request root must be a JSON object");
    }

    auto sql = root->getString("sql");
    if (!sql || sql->trim().empty()) {
        return std::unexpected("missing sql");
    }

    try {
        return TExportRequest{
            .Sql = std::string(*sql),
            .Catalog = ParseCatalog(*root),
            .Scheduler = ParseSchedulerSettings(*root),
            .EmbedWasm = ParseEmbedWasm(*root),
            .WasmBits = ParseWasmBits(*root),
        };
    } catch (const std::exception& e) {
        return std::unexpected(std::string(e.what()));
    }
}

std::expected<TParsedSql, NQumir::TError> ParseSql(
    std::string_view sql,
    TCatalog& catalog)
{
    std::istringstream in{std::string(sql)};
    NQdb::NSql::TTokenStream tokens(in);
    NQdb::NSql::TParser parser;
    auto statements = parser.ParseAll(tokens);
    if (!statements) {
        return std::unexpected(statements.error());
    }

    NQdb::TExternalModuleCatalog externalCatalog;
    NQdb::NSql::TSqlNodePtr query;
    bool hasExternalDefinitions = false;
    for (const auto& statement : *statements) {
        if (query) {
            const bool isDefinition =
                NQdb::NSql::TMaybeNode<NQdb::NSql::TSqlExternalModule>(statement) ||
                NQdb::NSql::TMaybeNode<NQdb::NSql::TSqlExternalFunction>(statement);
            return std::unexpected(NQumir::TError(
                isDefinition
                    ? "plan export requires CREATE definitions before the query"
                    : "plan export supports only a single query statement"));
        }
        if (auto module = NQdb::NSql::TMaybeNode<NQdb::NSql::TSqlExternalModule>(statement)) {
            auto applied = externalCatalog.Apply(*module.Cast());
            if (!applied) {
                return std::unexpected(applied.error());
            }
            hasExternalDefinitions = true;
        } else if (auto function =
                NQdb::NSql::TMaybeNode<NQdb::NSql::TSqlExternalFunction>(statement))
        {
            auto applied = externalCatalog.Apply(*function.Cast());
            if (!applied) {
                return std::unexpected(applied.error());
            }
            hasExternalDefinitions = true;
        } else {
            query = statement;
        }
    }
    if (!query) {
        return std::unexpected(NQumir::TError(
            "plan export requires a query after CREATE definitions"));
    }

    NQdb::TTableSourceFactory sources = [&](std::string_view table)
        -> std::expected<NQdb::TOperatorPtr, NQumir::TError>
    {
        auto it = catalog.Sources.find(std::string(table));
        if (it == catalog.Sources.end()) {
            return std::unexpected(NQumir::TError(
                "unknown browser dataset table: " + std::string(table)));
        }
        return std::make_shared<NQdb::TSourceOperator>(*it->second, std::string(table));
    };

    auto plan = NQdb::BuildPlan(query, sources);
    if (!plan) {
        return std::unexpected(plan.error());
    }
    return TParsedSql{
        .Plan = std::move(*plan),
        .ExternalCatalog = hasExternalDefinitions
            ? externalCatalog.Snapshot()
            : nullptr,
    };
}

std::string LogicalPlanTreeText(const NQdb::TOperatorPtr& plan) {
    std::ostringstream out;
    try {
        PrintPlanTreeWithCtes(out, plan);
    } catch (const std::exception& e) {
        out << "<unprintable logical plan: " << e.what() << ">\n";
    }
    return out.str();
}

std::string LogicalPlanAstText(const NQdb::TOperatorPtr& plan) {
    std::ostringstream out;
    try {
        NQdb::NSexp::PrintRelPlan(out, plan);
    } catch (const std::exception& e) {
        out << "<unprintable logical ast: " << e.what() << ">";
    }
    return out.str();
}

llvm::json::Object JoinReorderDiagnosticsJson(
    const NQdb::TJoinReorderDiagnostics& diagnostics)
{
    llvm::json::Array chains;
    for (const auto& chain : diagnostics.Chains) {
        chains.push_back(llvm::json::Object{
            {"leafCount", static_cast<int64_t>(chain.LeafCount)},
            {"edgeCount", static_cast<int64_t>(chain.EdgeCount)},
            {"enableCbo", chain.EnableCbo},
            {"usedCbo", chain.UsedCbo},
            {"strategy", chain.Strategy},
            {"reason", chain.Reason},
        });
    }
    return llvm::json::Object{
        {"enableCbo", diagnostics.EnableCbo},
        {"usedCbo", diagnostics.UsedCbo},
        {"chains", std::move(chains)},
    };
}

llvm::json::Object PlanPassDiagnosticsJson(
    const NQdb::TPlanPassDiagnostics& diagnostics)
{
    const auto& late = diagnostics.LateMaterialization;
    return llvm::json::Object{
        {"joinReorder", JoinReorderDiagnosticsJson(diagnostics.JoinReorder)},
        {"lateMaterialization", llvm::json::Object{
            {"considered", late.Considered},
            {"applied", late.Applied},
            {"reason", late.Reason},
            {"limit", static_cast<int64_t>(late.Limit)},
            {"earlyColumnCount", static_cast<int64_t>(late.EarlyColumnCount)},
            {"fetchColumnCount", static_cast<int64_t>(late.FetchColumnCount)},
            {"eagerBytes", static_cast<int64_t>(late.Cost.EagerBytes)},
            {"narrowBytes", static_cast<int64_t>(late.Cost.NarrowBytes)},
            {"lookupBytes", static_cast<int64_t>(late.Cost.LookupBytes)},
        }},
    };
}

std::string LogicalNodeAstText(const NQdb::TOperatorPtr& op) {
    std::ostringstream out;
    try {
        NQumir::NAst::NCore::PrintAst(
            out,
            op,
            NQumir::NAst::NCore::TPrintOptions{
                .NodePrinters = NQdb::NSexp::MakeRelPrinters(),
            });
    } catch (const std::exception& e) {
        out << "<unprintable logical node ast: " << e.what() << ">";
    }
    return out.str();
}

std::string Trim(std::string_view text) {
    while (!text.empty() &&
           (text.front() == '\n' || text.front() == '\r' ||
            text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() &&
           (text.back() == '\n' || text.back() == '\r' ||
            text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return std::string(text);
}

std::string SafeExprLine(const NQumir::NAst::TExprPtr& expr) {
    try {
        return NQdb::ExprLine(expr);
    } catch (const std::exception& e) {
        return "<unprintable expr: " + std::string(e.what()) + ">";
    }
}

void MaybeAddArtifactRef(
    llvm::json::Object& refs,
    std::string name,
    std::string id)
{
    if (id.empty()) {
        return;
    }
    refs[std::move(name)] = std::move(id);
}

void AddArrayArtifactRef(
    llvm::json::Object& refs,
    std::string name,
    std::string id)
{
    if (id.empty()) {
        return;
    }
    auto* value = refs.get(name);
    if (!value) {
        llvm::json::Array array;
        array.push_back(std::move(id));
        refs[std::move(name)] = std::move(array);
        return;
    }
    if (auto* array = value->getAsArray()) {
        array->push_back(std::move(id));
    }
}


std::string ConnectionKindName(NQdb::NScheduler::EConnectionKind kind) {
    using NQdb::NScheduler::EConnectionKind;
    switch (kind) {
        case EConnectionKind::OneToOne:
            return "one-to-one";
        case EConnectionKind::Gather:
            return "gather";
        case EConnectionKind::HashShuffle:
            return "hash-shuffle";
        case EConnectionKind::Broadcast:
            return "broadcast";
    }
    return "unknown";
}

llvm::json::Array StringArray(const std::vector<std::string>& values) {
    llvm::json::Array out;
    for (const auto& value : values) {
        out.push_back(value);
    }
    return out;
}

llvm::json::Array SortKeysJson(const std::vector<NQdb::TSortKey>& keys) {
    llvm::json::Array out;
    for (const auto& key : keys) {
        out.push_back(llvm::json::Object{
            {"column", key.Column},
            {"direction", std::string(NQdb::SortDirectionName(key.Direction))},
            {"nulls", std::string(NQdb::SortNullsName(key.Nulls))},
        });
    }
    return out;
}

std::string LogicalNodeKind(const NQdb::TOperatorPtr& op) {
    if (auto ref = TMaybeOp<TCteRef>(op)) {
        return "cte-ref #" + std::to_string(ref.Cast()->Def()->Id);
    }
    if (auto consumer = TMaybeOp<TCteConsumer>(op)) {
        return "cte-consumer #" + std::to_string(consumer.Cast()->Def()->Id);
    }
    return std::string(op->RelName());
}

llvm::json::Object LogicalNodeDetails(const NQdb::TOperatorPtr& op) {
    using namespace NQdb;

    if (auto source = TMaybeOp<TSourceOperator>(op)) {
        auto src = source.Cast();
        return llvm::json::Object{
            {"path", src->SourcePath()},
            {"alias", src->GetAlias()},
        };
    }
    if (auto filter = TMaybeOp<TFilterOperator>(op)) {
        return llvm::json::Object{
            {"predicate", SafeExprLine(filter.Cast()->Predicate())},
        };
    }
    if (auto project = TMaybeOp<TProjectOperator>(op)) {
        llvm::json::Array projections;
        for (const auto& spec : project.Cast()->Projections()) {
            projections.push_back(llvm::json::Object{
                {"name", spec.Name},
                {"expr", SafeExprLine(spec.Expression)},
            });
        }
        return llvm::json::Object{
            {"projections", std::move(projections)},
        };
    }
    if (auto aggregate = TMaybeOp<TAggregateOperator>(op)) {
        auto agg = aggregate.Cast();
        llvm::json::Array aggs;
        for (const auto& spec : agg->Aggs()) {
            aggs.push_back(llvm::json::Object{
                {"name", spec.Name},
                {"func", spec.Func},
                {"arg", SafeExprLine(spec.Arg)},
            });
        }
        return llvm::json::Object{
            {"groupKeys", StringArray(agg->GroupKeys())},
            {"aggregates", std::move(aggs)},
        };
    }
    if (auto join = TMaybeOp<TJoinOperator>(op)) {
        auto j = join.Cast();
        llvm::json::Array keys;
        for (const auto& key : j->Keys()) {
            keys.push_back(llvm::json::Object{
                {"left", key.Left},
                {"right", key.Right},
            });
        }
        return llvm::json::Object{
            {"joinType", std::string(JoinTypeName(j->JoinType()))},
            {"buildSide", std::string(JoinBuildSideName(ChooseJoinBuildSide(*j)))},
            {"keys", std::move(keys)},
            {"residual", SafeExprLine(j->Filter())},
        };
    }
    if (auto sort = TMaybeOp<TSortOperator>(op)) {
        return llvm::json::Object{
            {"keys", SortKeysJson(sort.Cast()->Keys())},
        };
    }
    if (auto sort = TMaybeOp<TTopSortOperator>(op)) {
        return llvm::json::Object{
            {"keys", SortKeysJson(sort.Cast()->Keys())},
            {"limit", sort.Cast()->Limit()},
        };
    }
    if (auto limit = TMaybeOp<TLimitOperator>(op)) {
        return llvm::json::Object{
            {"limit", limit.Cast()->Limit()},
            {"offset", limit.Cast()->Offset()},
        };
    }
    if (auto ref = TMaybeOp<TCteRef>(op)) {
        return llvm::json::Object{
            {"cteId", static_cast<int64_t>(ref.Cast()->Def()->Id)},
        };
    }
    if (auto consumer = TMaybeOp<TCteConsumer>(op)) {
        return llvm::json::Object{
            {"cteId", static_cast<int64_t>(consumer.Cast()->Def()->Id)},
            {"refCount", static_cast<int64_t>(consumer.Cast()->Materialization()->RefCount)},
        };
    }
    return llvm::json::Object{};
}

llvm::json::Object GraphJson(
    const NQdb::NScheduler::TLoweredPlan& lowered,
    const std::vector<TKernelArtifacts>& kernelArtifacts)
{
    struct TGroup {
        std::string Id;
        std::string Kind;
        std::string Label;
        std::string Group;
        NQdb::TExecStageId ExecStageId = NQdb::InvalidExecStageId;
        size_t TaskCount = 0;
        size_t InputCount = 0;
        size_t OutputCount = 0;
        std::vector<std::string> TaskIds;
    };
    struct TEdge {
        std::string From;
        std::string To;
        std::string Connection;
        size_t EdgeCount = 0;
    };

    std::vector<TGroup> groups;
    std::unordered_map<std::string, size_t> groupIndexes;
    std::unordered_map<const NQdb::NScheduler::TTaskNode*, std::string> nodeGroups;
    const auto& graphNodes = lowered.Graph->Nodes();
    nodeGroups.reserve(graphNodes.size());
    for (size_t i = 0; i < graphNodes.size(); ++i) {
        const auto& node = *graphNodes[i];
        const auto rawId = "n" + std::to_string(i);
        const auto kind = node.DebugKind.empty() ? "task" : node.DebugKind;
        const auto label = node.DebugLabel.empty() ? kind : node.DebugLabel;
        const auto groupKey = node.ExecStageId != NQdb::InvalidExecStageId
            ? "exec:" + std::to_string(node.ExecStageId)
            : (node.TaskGroupId != NQdb::NScheduler::InvalidTaskGroupId
                ? "task:" + std::to_string(node.TaskGroupId)
                : rawId);

        auto it = groupIndexes.find(groupKey);
        if (it == groupIndexes.end()) {
            const auto index = groups.size();
            it = groupIndexes.emplace(groupKey, index).first;
            groups.push_back(TGroup{
                .Id = "g" + std::to_string(index),
                .Kind = kind,
                .Label = label,
                .Group = groupKey,
                .ExecStageId = node.ExecStageId,
            });
        }

        auto& group = groups[it->second];
        ++group.TaskCount;
        group.InputCount += node.Inbound.size();
        group.OutputCount += node.Outbound.size();
        group.TaskIds.push_back(rawId);
        nodeGroups.emplace(graphNodes[i].get(), group.Id);
    }

    llvm::json::Array connections;
    std::unordered_map<const NQdb::NScheduler::IConnection*, std::string> connectionIds;
    auto addConnection = [&](const NQdb::NScheduler::IConnection* connection) {
        if (!connection || connectionIds.contains(connection)) {
            return;
        }
        auto id = "c" + std::to_string(connectionIds.size());
        connectionIds.emplace(connection, id);
        connections.push_back(llvm::json::Object{
            {"id", id},
            {"kind", ConnectionKindName(connection->Kind())},
            {"debugName", connection->DebugName()},
            {"srcCount", static_cast<int64_t>(connection->SrcCount())},
            {"dstCount", static_cast<int64_t>(connection->DstCount())},
        });
    };

    addConnection(lowered.FinalGather);
    for (const auto& edge : lowered.Graph->Edges()) {
        if (edge->Kind != NQdb::NScheduler::ETaskEdgeKind::Dependency) {
            addConnection(edge->Connection);
        }
    }

    llvm::json::Array edges;
    std::vector<TEdge> groupedEdges;
    std::unordered_map<std::string, size_t> edgeIndexes;
    for (const auto& edge : lowered.Graph->Edges()) {
        if (edge->Kind == NQdb::NScheduler::ETaskEdgeKind::Dependency) {
            continue;
        }
        const auto& from = nodeGroups.at(edge->Src);
        const auto& to = nodeGroups.at(edge->Dst);
        const auto& connection = connectionIds.at(edge->Connection);
        const auto edgeKey = from + "\n" + to + "\n" + connection;
        auto it = edgeIndexes.find(edgeKey);
        if (it == edgeIndexes.end()) {
            const auto index = groupedEdges.size();
            it = edgeIndexes.emplace(edgeKey, index).first;
            groupedEdges.push_back(TEdge{
                .From = from,
                .To = to,
                .Connection = connection,
            });
        }
        ++groupedEdges[it->second].EdgeCount;
    }

    llvm::json::Array nodes;
    for (const auto& group : groups) {
        llvm::json::Array taskIds;
        for (const auto& taskId : group.TaskIds) {
            taskIds.push_back(taskId);
        }
        llvm::json::Object artifactRefs;
        for (const auto& item : kernelArtifacts) {
            const bool belongs =
                group.ExecStageId != NQdb::InvalidExecStageId &&
                item.ExecStageId == group.ExecStageId;
            if (!belongs) {
                continue;
            }
            AddArrayArtifactRef(artifactRefs, "ast", item.Ast);
            AddArrayArtifactRef(artifactRefs, "ir", item.Ir);
            AddArrayArtifactRef(artifactRefs, "llvm", item.Llvm);
            AddArrayArtifactRef(artifactRefs, "wasm", item.Wasm);
        }

        llvm::json::Object node{
            {"id", group.Id},
            {"kind", group.Kind},
            {"label", group.TaskCount > 1
                ? group.Label + " x" + std::to_string(group.TaskCount)
                : group.Label},
            {"taskCount", static_cast<int64_t>(group.TaskCount)},
            {"taskGroup", group.Group},
            {"inputCount", static_cast<int64_t>(group.InputCount)},
            {"outputCount", static_cast<int64_t>(group.OutputCount)},
            {"details", llvm::json::Object{
                {"kind", group.Kind},
                {"label", group.Label},
                {"taskCount", static_cast<int64_t>(group.TaskCount)},
                {"taskIds", std::move(taskIds)},
            }},
        };
        if (group.ExecStageId != NQdb::InvalidExecStageId) {
            node["stageId"] = static_cast<int64_t>(group.ExecStageId);
        }
        if (!artifactRefs.empty()) {
            node["artifacts"] = std::move(artifactRefs);
        }
        nodes.push_back(std::move(node));
    }

    for (const auto& edge : groupedEdges) {
        edges.push_back(llvm::json::Object{
            {"from", edge.From},
            {"to", edge.To},
            {"connection", edge.Connection},
            {"edgeCount", static_cast<int64_t>(edge.EdgeCount)},
        });
    }

    return llvm::json::Object{
        {"nodes", std::move(nodes)},
        {"connections", std::move(connections)},
        {"edges", std::move(edges)},
        {"result", llvm::json::Object{
            {"connection", connectionIds.at(lowered.FinalGather)},
            {"lanes", static_cast<int64_t>(lowered.Lanes)},
        }},
    };
}

llvm::json::Object EmptyGraphJson(std::string reason = {}) {
    llvm::json::Object result{
        {"nodes", llvm::json::Array{}},
        {"connections", llvm::json::Array{}},
        {"edges", llvm::json::Array{}},
    };
    if (!reason.empty()) {
        result["error"] = std::move(reason);
    }
    return result;
}

std::string AddLogicalGraphNode(
    const NQdb::TOperatorPtr& op,
    llvm::json::Array& nodes,
    llvm::json::Array& edges,
    TArtifactStore& artifacts,
    size_t& nextNodeId,
    size_t& nextConnectionId,
    std::vector<std::string>* sectionNodes = nullptr)
{
    std::vector<std::string> childIds;
    for (const auto& child : op->Inputs()) {
        childIds.push_back(AddLogicalGraphNode(
            child,
            nodes,
            edges,
            artifacts,
            nextNodeId,
            nextConnectionId,
            sectionNodes));
    }

    const std::string nodeId = "ln" + std::to_string(nextNodeId++);
    const auto label = LogicalNodeKind(op);
    const auto tooltip = PlanLabel(op);
    llvm::json::Object artifactRefs;
    MaybeAddArtifactRef(
        artifactRefs,
        "ast",
        artifacts.Add("ast", LogicalNodeAstText(op), tooltip));
    nodes.push_back(llvm::json::Object{
        {"id", nodeId},
        {"kind", label},
        {"label", label},
        {"tooltip", tooltip},
        {"artifacts", std::move(artifactRefs)},
        {"details", LogicalNodeDetails(op)},
        {"inputCount", static_cast<int64_t>(childIds.size())},
        {"outputCount", 1},
    });
    if (sectionNodes) {
        sectionNodes->push_back(nodeId);
    }

    for (const auto& childId : childIds) {
        const auto connectionId = "lc" + std::to_string(nextConnectionId++);
        edges.push_back(llvm::json::Object{
            {"from", childId},
            {"to", nodeId},
            {"connection", connectionId},
            {"srcLane", 0},
            {"dstLane", 0},
        });
    }
    return nodeId;
}

void AddLogicalGraphSection(
    llvm::json::Array& sections,
    std::string id,
    std::string kind,
    std::string label,
    const std::vector<std::string>& nodeIds)
{
    if (nodeIds.empty()) {
        return;
    }
    llvm::json::Array nodes;
    for (const auto& nodeId : nodeIds) {
        nodes.push_back(nodeId);
    }
    sections.push_back(llvm::json::Object{
        {"id", std::move(id)},
        {"kind", std::move(kind)},
        {"label", std::move(label)},
        {"nodeIds", std::move(nodes)},
    });
}

llvm::json::Object LogicalGraphJson(
    const NQdb::TOperatorPtr& plan,
    TArtifactStore& artifacts)
{
    llvm::json::Array nodes;
    llvm::json::Array edges;
    llvm::json::Array sections;
    size_t nextNodeId = 0;
    size_t nextConnectionId = 0;
    for (const auto& def : CollectCteDefinitions(plan)) {
        std::vector<std::string> sectionNodes;
        AddLogicalGraphNode(
            def->Plan,
            nodes,
            edges,
            artifacts,
            nextNodeId,
            nextConnectionId,
            &sectionNodes);
        AddLogicalGraphSection(
            sections,
            "cte-" + std::to_string(def->Id),
            "cte",
            "cte #" + std::to_string(def->Id),
            sectionNodes);
    }
    for (const auto& mat : CollectMaterializations(plan)) {
        std::vector<std::string> sectionNodes;
        AddLogicalGraphNode(
            mat.Materialization->Plan,
            nodes,
            edges,
            artifacts,
            nextNodeId,
            nextConnectionId,
            &sectionNodes);
        AddLogicalGraphSection(
            sections,
            "cte-" + std::to_string(mat.Id),
            "materialized-cte",
            "cte #" + std::to_string(mat.Id) + " (materialized, x" +
                std::to_string(mat.Materialization->RefCount) + ")",
            sectionNodes);
    }
    std::vector<std::string> mainNodes;
    const auto rootId = AddLogicalGraphNode(
        plan,
        nodes,
        edges,
        artifacts,
        nextNodeId,
        nextConnectionId,
        &mainNodes);
    AddLogicalGraphSection(
        sections,
        "main",
        "main",
        "main",
        mainNodes);

    llvm::json::Array connections;
    for (size_t i = 0; i < nextConnectionId; ++i) {
        connections.push_back(llvm::json::Object{
            {"id", "lc" + std::to_string(i)},
            {"kind", "logical"},
            {"debugName", "logical-flow"},
            {"srcCount", 1},
            {"dstCount", 1},
        });
    }

    return llvm::json::Object{
        {"nodes", std::move(nodes)},
        {"sections", std::move(sections)},
        {"connections", std::move(connections)},
        {"edges", std::move(edges)},
        {"result", llvm::json::Object{
            {"node", rootId},
            {"lanes", 1},
        }},
    };
}

std::string AstText(const NQumir::NAst::TExprPtr& ast) {
    std::ostringstream out;
    try {
        NQumir::NAst::NCore::PrintAst(out, ast);
    } catch (const std::exception& e) {
        out << "<unprintable ast: " << e.what() << ">";
    }
    return out.str();
}

// Register every lowered kernel's AST as an artifact and (in embed mode)
// compile one fused query-level wasm module. Failures land in `diagnostics`;
// the bundle still forms, but exec will be marked unsupported for wasm stages.
TWasmFinalizeResult WasmFinalizeKernels(
    std::span<NQdb::TGeneratedKernel> kernels,
    TArtifactStore& artifacts,
    llvm::json::Array& diagnostics,
    bool embedWasm,
    const std::string& cacheDir,
    const std::shared_ptr<const NQdb::TExternalCatalogSnapshot>& externalCatalog,
    int wasmBits)
{
    TWasmFinalizeResult out;
    out.Kernels.reserve(kernels.size());
    for (size_t i = 0; i < kernels.size(); ++i) {
        const auto& kernel = kernels[i];
        TKernelArtifacts item{
            .ExecStageId = kernel.ExecStageId,
            .Name = kernel.Name,
        };
        if (!kernel.ExportArtifacts) {
            out.Kernels.push_back(std::move(item));
            continue;
        }
        const auto artifactStage = kernel.ExecStageId != NQdb::InvalidExecStageId
            ? "exec:" + std::to_string(kernel.ExecStageId)
            : std::string{};
        item.Ast = artifacts.Add(
            "ast", AstText(kernel.Ast), item.Name, artifactStage);
        out.Kernels.push_back(std::move(item));
    }

    if (!embedWasm || kernels.empty()) {
        return out;
    }

    std::vector<NQdb::TGeneratedKernel*> unique;
    std::vector<std::pair<size_t, size_t>> bindings;
    std::unordered_map<std::string, size_t> uniqueByKey;
    unique.reserve(kernels.size());
    bindings.reserve(kernels.size());
    for (size_t i = 0; i < kernels.size(); ++i) {
        if (!kernels[i].ExportArtifacts) {
            continue;
        }
        auto key = NQdb::MakeKernelDedupKey(kernels[i]);
        auto [it, inserted] = uniqueByKey.emplace(std::move(key), unique.size());
        if (inserted) {
            unique.push_back(&kernels[i]);
        }
        bindings.emplace_back(i, it->second);
    }
    if (unique.empty()) {
        return out;
    }

    auto fused = NQdb::BuildFusedProgram(unique);
    auto wasm = CompileKernelAstToWasm(
        fused.Program, WasmSlot0, fused.Entrypoints, cacheDir, externalCatalog, wasmBits);
    if (!wasm) {
        out.ErrorStage = "wasm-fusion";
        out.Error = wasm.error();
        diagnostics.push_back(llvm::json::Object{
            {"stage", out.ErrorStage},
            {"message", out.Error},
        });
        return out;
    }

    out.HeapBase = wasm->HeapBase;
    out.QueryWasm = artifacts.AddBinary(
        "wasm", std::move(wasm->Bytes), "query.fused", "query");

    for (const auto& [kernelIndex, uniqueIndex] : bindings) {
        const auto& originalEntrypoints = kernels[kernelIndex].Entrypoints;
        const auto& fusedEntrypoints = fused.UniqueEntrypoints[uniqueIndex];
        auto& exported = out.Kernels[kernelIndex].Entrypoints;
        for (size_t i = 0; i < originalEntrypoints.size(); ++i) {
            exported[originalEntrypoints[i]] = i < fusedEntrypoints.size()
                ? fusedEntrypoints[i]
                : originalEntrypoints[i];
        }
    }
    return out;
}


std::string BareColumnName(std::string_view qualified) {
    auto dot = qualified.rfind('.');
    return std::string(
        dot == std::string_view::npos ? qualified : qualified.substr(dot + 1));
}

llvm::json::Object CoreTypeJson(const NQumir::NAst::TTypePtr& type) {
    using namespace NQumir::NAst;
    const bool nullable = NQdb::IsNullableType(type);
    auto valueType = NQdb::UnwrapNullableType(type);
    if (auto decimal = NQdb::DecimalSpecOfValueType(valueType)) {
        return llvm::json::Object{
            {"type", "decimal"},
            {"storageType", "binint"},
            {"precision", decimal->Precision},
            {"scale", decimal->Scale},
            {"width", 16},
            {"nullable", nullable},
        };
    }
    if (NQdb::IsBinIntStorageType(valueType)) {
        return llvm::json::Object{
            {"type", "binint"},
            {"storageType", "binint"},
            {"width", 16},
            {"nullable", nullable},
        };
    }
    auto inner = UnwrapNamedType(valueType);
    std::string name;
    if (auto integer = TMaybeType<TIntegerType>(inner)) {
        name = integer.Cast()->ToString();
    } else if (TMaybeType<TFloatType>(inner)) {
        name = "f64";
    } else if (TMaybeType<TBoolType>(inner)) {
        name = "bool";
    } else if (TMaybeType<TStringType>(inner)) {
        name = "string";
    } else {
        name = inner ? inner->ToString() : "unknown";
    }
    return llvm::json::Object{
        {"type", std::move(name)},
        {"nullable", nullable},
    };
}

// Kernel ABI layout for wasm32 or wasm64. wasm64 offsets match the native
// layout (both use 8-byte pointers, so HashTableSize/PairBufferSize stay
// valid here). wasm32 offsets use real 4-byte-pointer values, checked
// against qumir's own TTypeTable::FieldOffset/SizeInBytes instead of hand
// math. For example TRowSet stays 48 bytes even with 4-byte pointers,
// because its i64 fields still need 8-byte alignment.
llvm::json::Object ExecLayoutJson(int64_t heapBase, int wasmBits) {
    if (heapBase <= 0) {
        heapBase = WasmSlot0;
    }
    const int64_t initialPages =
        (heapBase + WasmPageSize - 1) / WasmPageSize + WasmInitialSlackPages;
    llvm::json::Object layout;
    if (wasmBits == 32) {
        layout = llvm::json::Object{
            {"pointerSize", 4},
            {"column", llvm::json::Object{
                {"size", 24},
                {"data", 0},
                {"dataBitOffset", 4},
                {"mask", 8},
                {"maskBitOffset", 12},
                {"offsets", 16},
                {"offsetWidth", 20},
            }},
            {"rowset", llvm::json::Object{
                {"size", 48},
                {"columns", 0},
                {"columnCount", 8},
                {"rowCount", 16},
                {"selection", 24},
                {"hash", 28},
                {"private", 36},
            }},
            {"stringView", llvm::json::Object{
                {"size", 16},
                {"data", 0},
                {"length", 8},
            }},
            {"hashTable", llvm::json::Object{
                {"size", 80},
                {"sizeOffset", 48},
            }},
            {"pairBuffer", llvm::json::Object{
                {"size", 24},
                {"count", 0},
                {"capacity", 8},
                {"data", 16},
            }},
        };
    } else {
        layout = llvm::json::Object{
            {"pointerSize", 8},
            {"column", llvm::json::Object{
                {"size", 48},
                {"data", 0},
                {"dataBitOffset", 8},
                {"mask", 16},
                {"maskBitOffset", 24},
                {"offsets", 32},
                {"offsetWidth", 40},
            }},
            {"rowset", llvm::json::Object{
                {"size", 64},
                {"columns", 0},
                {"columnCount", 8},
                {"rowCount", 16},
                {"selection", 24},
                {"hash", 32},
                // Private holds the owners list for kernel-materialized rowsets
                // (owners[0] = count, then one qdb_alloc'd pointer per buffer);
                // the runtime walks it to free join output. See
                // destroyKernelOwnedRowSet.
                {"private", 48},
            }},
            {"stringView", llvm::json::Object{
                {"size", 16},
                {"data", 0},
                {"length", 8},
            }},
            // THashTable per modules/qumirdb.cpp; the runtime only reads Size.
            {"hashTable", llvm::json::Object{
                {"size", static_cast<int64_t>(NQdb::TKernelCompiler::HashTableSize)},
                {"sizeOffset", 72},
            }},
            {"pairBuffer", llvm::json::Object{
                {"size", static_cast<int64_t>(NQdb::TKernelCompiler::PairBufferSize)},
                {"count", 0},
                {"capacity", 8},
                {"data", 16},
            }},
        };
    }
    layout["sharedMemory"] = llvm::json::Object{
        {"heapBase", heapBase},
        {"initialPages", initialPages},
        {"globalBase", WasmSlot0},
        {"stackSize", WasmStackSize},
        {"kernelCount", 1},
    };
    return layout;
}

llvm::json::Array ProjectOutputJson(
    const NQdb::TProjectColumnPlan& plan)
{
    using namespace NQumir::NAst;
    auto* outStruct = static_cast<TStructType*>(plan.OutputType.get());
    llvm::json::Array out;
    for (size_t i = 0; i < plan.Columns.size(); ++i) {
        const auto& column = plan.Columns[i];
        const auto& [name, outType] = outStruct->Fields[i];
        auto entry = CoreTypeJson(outType);
        entry["name"] = name;
        if (!column.Computed) {
            entry["source"] = "passthrough";
            entry["inputIndex"] = column.Index;
        } else {
            const size_t computedIndex = static_cast<size_t>(column.Index);
            entry["source"] = "computed";
            entry["computedIndex"] = static_cast<int64_t>(computedIndex);
            entry["width"] = static_cast<int64_t>(
                plan.ComputedWidths[computedIndex]);
            entry["isString"] = static_cast<bool>(
                plan.ComputedIsString[computedIndex]);
        }
        out.push_back(std::move(entry));
    }
    return out;
}

llvm::json::Array SourceColumnsJson(const NQumir::NAst::TTypePtr& sourceType) {
    auto* st = static_cast<NQumir::NAst::TStructType*>(sourceType.get());
    llvm::json::Array out;
    for (const auto& [qname, type] : st->Fields) {
        auto entry = CoreTypeJson(type);
        entry["name"] = BareColumnName(qname);
        out.push_back(std::move(entry));
    }
    return out;
}

llvm::json::Array StructColumnsJson(const NQumir::NAst::TStructType& type) {
    llvm::json::Array out;
    for (const auto& [qname, fieldType] : type.Fields) {
        auto entry = CoreTypeJson(fieldType);
        entry["name"] = BareColumnName(qname);
        out.push_back(std::move(entry));
    }
    return out;
}

llvm::json::Object UnsupportedExec(
    std::string reason,
    std::string errorStage = {})
{
    llvm::json::Object result{
        {"supported", false},
        {"reason", reason},
    };
    if (!errorStage.empty()) {
        result["error"] = llvm::json::Object{
            {"stage", std::move(errorStage)},
            {"message", std::move(reason)},
        };
    }
    return result;
}

bool BrowserSupportsJoin(NQdb::EJoinType type, bool hasResidual) {
    using NQdb::EJoinType;
    if (type == EJoinType::Inner) {
        return true;
    }
    if ((type == EJoinType::Left || type == EJoinType::Right ||
         type == EJoinType::Full) && !hasResidual) {
        return true;
    }
    if (type == EJoinType::LeftSemi || type == EJoinType::LeftAnti) {
        return true;
    }
    return false;
}

// One lowered kernel joined with its exported artifacts, indexed by the stable
// exec stage assigned during scheduler lowering.
struct TKernelRef {
    const NQdb::TGeneratedKernel* Kernel = nullptr;
    const TKernelArtifacts* Artifacts = nullptr;
};
using TKernelIndex =
    std::unordered_map<NQdb::TExecStageId, std::vector<TKernelRef>>;

TKernelIndex BuildKernelIndex(
    const NQdb::TExecPlan& plan,
    std::span<const NQdb::TGeneratedKernel> kernels,
    std::span<const TKernelArtifacts> artifacts)
{
    if (kernels.size() != artifacts.size()) {
        throw std::runtime_error("exec codec: kernel/artifact count mismatch");
    }
    TKernelIndex index;
    for (const auto& node : plan.Nodes) {
        for (const auto kernelIndex : node.KernelIndexes) {
            if (kernelIndex >= kernels.size()) {
                throw std::runtime_error("exec codec: invalid bound kernel index");
            }
            index[node.StageId].push_back(
                {&kernels[kernelIndex], &artifacts[kernelIndex]});
        }
    }
    return index;
}

const TKernelRef* FindKernel(
    const TKernelIndex& index,
    NQdb::TExecStageId execStageId,
    std::string_view name)
{
    auto it = index.find(execStageId);
    if (it == index.end()) {
        return nullptr;
    }
    for (const auto& ref : it->second) {
        if (ref.Kernel->Name == name) {
            return &ref;
        }
    }
    return nullptr;
}

bool HasEntrypoint(const TKernelRef& ref, std::string_view name) {
    return ref.Artifacts &&
        ref.Artifacts->Entrypoints.contains(std::string(name));
}

bool HasEntrypointAt(const TKernelRef& ref, size_t index) {
    if (!ref.Kernel || !ref.Artifacts || index >= ref.Kernel->Entrypoints.size()) {
        return false;
    }
    return ref.Artifacts->Entrypoints.contains(ref.Kernel->Entrypoints[index]);
}

llvm::json::Object EntrypointsJson(
    const TKernelRef& ref,
    std::vector<std::pair<std::string, size_t>> aliases = {})
{
    llvm::json::Object out;
    if (!ref.Artifacts) {
        return out;
    }
    std::vector<std::string> names;
    names.reserve(ref.Artifacts->Entrypoints.size());
    for (const auto& [name, _] : ref.Artifacts->Entrypoints) {
        names.push_back(name);
    }
    std::ranges::sort(names);
    for (const auto& name : names) {
        out[name] = ref.Artifacts->Entrypoints.at(name);
    }
    for (const auto& [alias, index] : aliases) {
        if (!ref.Kernel || index >= ref.Kernel->Entrypoints.size()) {
            continue;
        }
        auto it = ref.Artifacts->Entrypoints.find(ref.Kernel->Entrypoints[index]);
        if (it != ref.Artifacts->Entrypoints.end()) {
            out[alias] = it->second;
        }
    }
    return out;
}

llvm::json::Array RegexesJson(const TKernelRef& ref) {
    llvm::json::Array out;
    if (!ref.Kernel || !ref.Kernel->RuntimeContext) {
        return out;
    }
    for (const auto& regex : ref.Kernel->RuntimeContext->Regexes.Specs()) {
        out.push_back(llvm::json::Object{
            {"pattern", regex.Pattern},
            {"replacement", regex.Replacement},
        });
    }
    return out;
}

// Builds a sort stage body from the operator's lowered wasm kernel. Browser
// sort/top-sort stages do not have a JS comparison fallback; an unavailable
// kernel is a plan/export error.
std::optional<llvm::json::Object> BuildSortStageJson(
    const std::vector<NQdb::TSortKey>& sortKeys,
    const NQumir::NAst::TStructType& inputStruct,
    const TKernelIndex& kernels,
    NQdb::TExecStageId execStageId,
    bool embedWasm,
    bool topSort,
    std::string& error)
{
    const char* stageName = topSort ? "top-sort" : "sort";
    if (!embedWasm) {
        error = std::string(stageName) + " stage requires embedded wasm kernel";
        return std::nullopt;
    }

    bool anyNullableKey = false;
    for (const auto& key : sortKeys) {
        int32_t index = -1;
        for (size_t f = 0; f < inputStruct.Fields.size(); ++f) {
            if (BareColumnName(inputStruct.Fields[f].first) == BareColumnName(key.Column)) {
                index = static_cast<int32_t>(f);
                break;
            }
        }
        if (index < 0) {
            error = std::string(stageName) +
                " key references an unknown column: " + key.Column;
            return std::nullopt;
        }
        anyNullableKey = anyNullableKey ||
            NQdb::IsNullableType(inputStruct.Fields[index].second);
    }

    const auto* wasmKernel = topSort
        ? FindKernel(kernels, execStageId, "top-sort.fused")
        : FindKernel(
            kernels, execStageId,
            anyNullableKey ? "sort.run.nullable.fused" : "sort.run.fused");
    if (!wasmKernel) {
        error = std::string(stageName) + " kernel was not generated";
        return std::nullopt;
    }
    if (!HasEntrypoint(*wasmKernel, topSort ? "qdb_top_sort_update" : "qdb_sort_run")) {
        error = std::string(stageName) + " kernel failed to compile to wasm";
        return std::nullopt;
    }

    llvm::json::Array keys;
    for (size_t i = 0; i < wasmKernel->Kernel->SortKeys.size(); ++i) {
        const auto& key = wasmKernel->Kernel->SortKeys[i];
        const auto& sortKey = sortKeys[i];
        const bool nullsFirst = sortKey.Nulls == NQdb::ESortNulls::First ||
            (sortKey.Nulls == NQdb::ESortNulls::Default &&
                sortKey.Direction == NQdb::ESortDirection::Desc);
        keys.push_back(llvm::json::Object{
            {"index", key.Index},
            {"width", key.WidthBytes},
            {"isString", key.IsString},
            {"desc", key.Desc},
            {"nullsFirst", nullsFirst},
        });
    }
    llvm::json::Object stage{
        {"entrypoints", EntrypointsJson(*wasmKernel)},
        {"sortKeys", SortKeysJson(sortKeys)},
        {"radixKeys", std::move(keys)},
        {"radixNullable", anyNullableKey},
    };
    return stage;
}

std::optional<llvm::json::Object> BuildWindowStageJson(
    NQdb::TWindowOperator& window,
    const NQumir::NAst::TStructType& inputStruct,
    const NQumir::NAst::TStructType& outputStruct,
    const TKernelIndex& kernels,
    NQdb::TExecStageId execStageId,
    bool embedWasm,
    std::string& error)
{
    if (!embedWasm) {
        error = "window stage requires embedded wasm kernel";
        return std::nullopt;
    }

    const auto* wasmKernel = FindKernel(
        kernels, execStageId, "window.run.fused");
    if (!wasmKernel) {
        error = "window kernel was not generated";
        return std::nullopt;
    }
    if (!HasEntrypoint(*wasmKernel, "qdb_window_run")) {
        error = "window kernel failed to compile to wasm";
        return std::nullopt;
    }

    std::vector<NQdb::TSortKey> sortKeys;
    sortKeys.reserve(window.PartitionKeys().size() + window.OrderKeys().size());
    for (const auto& partition : window.PartitionKeys()) {
        sortKeys.push_back(NQdb::TSortKey{.Column = partition});
    }
    for (const auto& order : window.OrderKeys()) {
        sortKeys.push_back(order);
    }
    if (sortKeys.size() != wasmKernel->Kernel->SortKeys.size()) {
        error = "window sort key metadata is unavailable";
        return std::nullopt;
    }

    llvm::json::Array keys;
    for (size_t i = 0; i < wasmKernel->Kernel->SortKeys.size(); ++i) {
        const auto& key = wasmKernel->Kernel->SortKeys[i];
        const auto& sortKey = sortKeys[i];
        const bool nullsFirst = sortKey.Nulls == NQdb::ESortNulls::First ||
            (sortKey.Nulls == NQdb::ESortNulls::Default &&
                sortKey.Direction == NQdb::ESortDirection::Desc);
        keys.push_back(llvm::json::Object{
            {"index", key.Index},
            {"width", key.WidthBytes},
            {"isString", key.IsString},
            {"desc", key.Desc},
            {"nullsFirst", nullsFirst},
        });
    }

    return llvm::json::Object{
        {"entrypoints", EntrypointsJson(*wasmKernel)},
        {"partitionKeys", StringArray(window.PartitionKeys())},
        {"sortKeys", SortKeysJson(sortKeys)},
        {"radixKeys", std::move(keys)},
        {"output", StructColumnsJson(outputStruct)},
        {"input", StructColumnsJson(inputStruct)},
    };
}

struct TExecPlanCodec {
    const NQdb::TExecPlan& Plan;
    const TKernelIndex& Kernels;
    bool EmbedWasm = false;
    bool OperatorInstrumentation = false;
    std::optional<llvm::json::Object> Unsupported;
    llvm::json::Array Nodes;
    llvm::json::Array Edges;

    const NQdb::TExecPlanNode& Input(
        const NQdb::TExecPlanNode& node,
        size_t input) const
    {
        if (input >= node.Inputs.size() ||
            node.Inputs[input] >= Plan.Nodes.size())
        {
            throw std::runtime_error("exec codec: invalid typed-plan input");
        }
        return Plan.Nodes[node.Inputs[input]];
    }

    void AddNode(
        const NQdb::TExecPlanNode& typed,
        llvm::json::Object encoded)
    {
        encoded["id"] = static_cast<int64_t>(typed.Id);
        if (typed.StageId != NQdb::InvalidExecStageId) {
            encoded["stageId"] = static_cast<int64_t>(typed.StageId);
        }
        if (OperatorInstrumentation) {
            llvm::json::Array widths;
            for (const auto width : NQdb::InstrumentationColumnWidths(typed.OutputType)) {
                widths.push_back(width);
            }
            encoded["instrumentationWidths"] = std::move(widths);
        }
        Nodes.push_back(std::move(encoded));
    }

    void EncodeNode(const NQdb::TExecPlanNode& node) {
        using namespace NQumir::NAst;

        if (node.Kind == NQdb::EExecPlanNodeKind::UnionAll) {
            auto* output = static_cast<TStructType*>(node.OutputType.get());
            AddNode(node, llvm::json::Object{
                {"kind", "union-all"},
                {"label", "union-all"},
                {"output", StructColumnsJson(*output)},
            });
            return;
        }
        if (!node.Operator) {
            Unsupported = UnsupportedExec("exec codec: stage has no operator metadata");
            return;
        }

        switch (node.Kind) {
            case NQdb::EExecPlanNodeKind::Source: {
                auto* source = static_cast<const NQdb::TSourceOperator*>(
                    node.Operator);
                AddNode(node, llvm::json::Object{
                    {"kind", "source"},
                    {"table", source->SourcePath()},
                    {"columns", SourceColumnsJson(node.OutputType)},
                });
                return;
            }
            case NQdb::EExecPlanNodeKind::Filter: {
                auto* filter = static_cast<const NQdb::TFilterOperator*>(
                    node.Operator);
                const auto* ref = FindKernel(Kernels, node.StageId, "filter");
                if (!ref) {
                    Unsupported = UnsupportedExec("filter kernel was not generated");
                    return;
                }
                if (EmbedWasm && !HasEntrypointAt(*ref, 0)) {
                    Unsupported = UnsupportedExec(
                        "filter kernel failed to compile to wasm");
                    return;
                }
                llvm::json::Object encoded{
                    {"kind", "filter"},
                    {"label", "filter " + SafeExprLine(filter->Predicate())},
                    {"predicate", SafeExprLine(filter->Predicate())},
                    {"entrypoints", EntrypointsJson(*ref, {{"filter", 0}})},
                };
                auto regexes = RegexesJson(*ref);
                if (!regexes.empty()) {
                    encoded["regexes"] = std::move(regexes);
                }
                if (node.KeptInputColumns) {
                    llvm::json::Array columns;
                    for (const auto index : *node.KeptInputColumns) {
                        columns.push_back(index);
                    }
                    encoded["keptColumns"] = std::move(columns);
                }
                AddNode(node, std::move(encoded));
                return;
            }
            case NQdb::EExecPlanNodeKind::RuntimeFilter: {
                Unsupported = UnsupportedExec(
                    "runtime-filter is only supported by the native runtime");
                return;
            }
            case NQdb::EExecPlanNodeKind::Project: {
                auto* project = const_cast<NQdb::TProjectOperator*>(
                    static_cast<const NQdb::TProjectOperator*>(node.Operator));
                auto* input = static_cast<TStructType*>(
                    Input(node, 0).OutputType.get());
                auto columnPlan = BuildProjectColumnPlan(*project, *input);
                llvm::json::Object entrypoints;
                if (!columnPlan.ComputedExprs.empty()) {
                    const auto* ref = FindKernel(
                        Kernels, node.StageId, "project");
                    if (!ref) {
                        Unsupported = UnsupportedExec(
                            "project kernel was not generated");
                        return;
                    }
                    if (EmbedWasm && !HasEntrypointAt(*ref, 0)) {
                        Unsupported = UnsupportedExec(
                            "project kernel failed to compile to wasm");
                        return;
                    }
                    entrypoints = EntrypointsJson(*ref, {{"project", 0}});
                }
                llvm::json::Object encoded{
                    {"kind", "project"},
                    {"entrypoints", std::move(entrypoints)},
                    {"output", ProjectOutputJson(columnPlan)},
                };
                if (const auto* ref = FindKernel(
                        Kernels, node.StageId, "project"))
                {
                    auto regexes = RegexesJson(*ref);
                    if (!regexes.empty()) {
                        encoded["regexes"] = std::move(regexes);
                    }
                }
                AddNode(node, std::move(encoded));
                return;
            }
            case NQdb::EExecPlanNodeKind::Aggregate: {
                auto* aggregate = static_cast<const NQdb::TAggregateOperator*>(
                    node.Operator);
                const auto* ref = FindKernel(
                    Kernels, node.StageId, "aggregate");
                if (!ref) {
                    Unsupported = UnsupportedExec(
                        "aggregate kernel was not generated");
                    return;
                }
                if (EmbedWasm &&
                    (!HasEntrypoint(*ref, "agg_dispatch") ||
                     !HasEntrypoint(*ref, "agg_finish_rowset")))
                {
                    Unsupported = UnsupportedExec(
                        "aggregate kernel failed to compile to wasm");
                    return;
                }
                auto* outputType = static_cast<TStructType*>(
                    node.OutputType.get());
                const size_t keyCount = ref->Kernel->AggKeys.size();
                llvm::json::Array output;
                for (size_t i = 0; i < outputType->Fields.size(); ++i) {
                    auto entry = CoreTypeJson(outputType->Fields[i].second);
                    entry["name"] = BareColumnName(outputType->Fields[i].first);
                    if (i < keyCount) {
                        entry["nullable"] =
                            ref->Kernel->AggKeys[i].IsNullable;
                    } else if (
                        i - keyCount < ref->Kernel->AggValues.size())
                    {
                        entry["nullable"] =
                            ref->Kernel->AggValues[i - keyCount].IsNullable;
                    }
                    output.push_back(std::move(entry));
                }
                llvm::json::Object encoded{
                    {"kind", "aggregate"},
                    {"label", AggregatePlanLabel(*aggregate)},
                    {"groupKeys", StringArray(aggregate->GroupKeys())},
                    // aht_init needs a power-of-two capacity of at least 8;
                    // grouping sets have no useful estimate, so start there.
                    {"initialCapacity",
                        aggregate->GroupingSets().empty()
                            ? EstimateAggregateInitialCapacity(*aggregate, 1)
                            : int64_t{8}},
                    {"entrypoints", EntrypointsJson(*ref)},
                    {"keyCount", static_cast<int64_t>(keyCount)},
                    {"output", std::move(output)},
                };
                if (OperatorInstrumentation) {
                    llvm::json::Array buffers;
                    for (const auto index : ref->Kernel->AggStringMetadataBuffers) {
                        buffers.push_back(index);
                    }
                    encoded["instrumentationStringBuffers"] = std::move(buffers);
                }
                if (!aggregate->GroupingSets().empty()) {
                    llvm::json::Array sets;
                    for (const auto& set : aggregate->GroupingSets()) {
                        llvm::json::Array item;
                        for (const auto index : set) {
                            item.push_back(static_cast<int64_t>(index));
                        }
                        sets.push_back(std::move(item));
                    }
                    encoded["groupingSets"] = std::move(sets);
                    encoded["groupKeyCount"] = static_cast<int64_t>(
                        aggregate->GroupKeys().size());
                }
                AddNode(node, std::move(encoded));
                return;
            }
            case NQdb::EExecPlanNodeKind::Join: {
                auto* join = static_cast<const NQdb::TJoinOperator*>(
                    node.Operator);
                const bool hasResidual = join->Filter() != nullptr;
                if (!BrowserSupportsJoin(join->JoinType(), hasResidual)) {
                    Unsupported = UnsupportedExec(
                        "browser join type is not supported yet: " +
                        std::string(JoinTypeName(join->JoinType())) +
                        (hasResidual ? " with residual" : ""));
                    return;
                }
                const auto* ref = FindKernel(Kernels, node.StageId, "join");
                if (!ref) {
                    Unsupported = UnsupportedExec("join kernel was not generated");
                    return;
                }
                if (EmbedWasm &&
                    (!HasEntrypoint(*ref, "jt_dispatch") ||
                     !HasEntrypoint(*ref, "jt_materialize")))
                {
                    Unsupported = UnsupportedExec(
                        "join kernel failed to compile to wasm");
                    return;
                }
                auto* left = static_cast<TStructType*>(
                    Input(node, 0).OutputType.get());
                auto* right = static_cast<TStructType*>(
                    Input(node, 1).OutputType.get());
                auto* output = static_cast<TStructType*>(
                    node.OutputType.get());
                std::vector<std::pair<std::string, std::string>> keyPairs;
                for (const auto& key : join->Keys()) {
                    keyPairs.emplace_back(key.Left, key.Right);
                }
                const auto keyDesc =
                    NKernel::BuildJoinKeyDescriptor(*left, *right, keyPairs);
                llvm::json::Array keys;
                for (size_t i = 0; i < join->Keys().size(); ++i) {
                    const auto& key = join->Keys()[i];
                    keys.push_back(llvm::json::Object{
                        {"left", BareColumnName(key.Left)},
                        {"right", BareColumnName(key.Right)},
                        {"leftIndex", keyDesc.Fields[i].LeftColumnIndex},
                        {"rightIndex", keyDesc.Fields[i].RightColumnIndex},
                    });
                }
                AddNode(node, llvm::json::Object{
                    {"kind", "join"},
                    {"label", JoinPlanLabel(*join)},
                    {"joinType", std::string(JoinTypeName(join->JoinType()))},
                    {"buildSide", std::string(JoinBuildSideName(
                        ChooseJoinBuildSide(*join)))},
                    {"entrypoints", EntrypointsJson(*ref)},
                    {"keySize", static_cast<int64_t>(keyDesc.Size)},
                    {"hasResidual", hasResidual},
                    {"keys", std::move(keys)},
                    {"leftColumns", StructColumnsJson(*left)},
                    {"rightColumns", StructColumnsJson(*right)},
                    {"output", StructColumnsJson(*output)},
                });
                return;
            }
            case NQdb::EExecPlanNodeKind::CrossJoin: {
                auto* join = static_cast<const NQdb::TJoinOperator*>(
                    node.Operator);
                if (join->JoinType() != NQdb::EJoinType::Inner) {
                    Unsupported = UnsupportedExec(
                        "browser keyless join type is not supported yet: " +
                        std::string(JoinTypeName(join->JoinType())));
                    return;
                }
                const auto* ref = FindKernel(
                    Kernels, node.StageId, "cross-join");
                if (!ref) {
                    Unsupported = UnsupportedExec(
                        "cross join kernel was not generated");
                    return;
                }
                if (EmbedWasm && !HasEntrypoint(*ref, "xj_dispatch")) {
                    Unsupported = UnsupportedExec(
                        "cross join kernel failed to compile to wasm");
                    return;
                }
                auto* left = static_cast<TStructType*>(
                    Input(node, 0).OutputType.get());
                auto* right = static_cast<TStructType*>(
                    Input(node, 1).OutputType.get());
                auto* output = static_cast<TStructType*>(
                    node.OutputType.get());
                AddNode(node, llvm::json::Object{
                    {"kind", "cross-join"},
                    {"label", JoinPlanLabel(*join)},
                    {"entrypoints", EntrypointsJson(*ref)},
                    {"leftColumns", StructColumnsJson(*left)},
                    {"rightColumns", StructColumnsJson(*right)},
                    {"output", StructColumnsJson(*output)},
                });
                return;
            }
            case NQdb::EExecPlanNodeKind::CrossResidualFilter: {
                auto* join = static_cast<const NQdb::TJoinOperator*>(
                    node.Operator);
                const auto* ref = FindKernel(Kernels, node.StageId, "filter");
                if (!ref) {
                    Unsupported = UnsupportedExec(
                        "cross join residual kernel was not generated");
                    return;
                }
                if (EmbedWasm && !HasEntrypointAt(*ref, 0)) {
                    Unsupported = UnsupportedExec(
                        "cross join residual kernel failed to compile to wasm");
                    return;
                }
                AddNode(node, llvm::json::Object{
                    {"kind", "filter"},
                    {"label", "filter " + SafeExprLine(join->Filter())},
                    {"predicate", SafeExprLine(join->Filter())},
                    {"entrypoints", EntrypointsJson(*ref, {{"filter", 0}})},
                });
                return;
            }
            case NQdb::EExecPlanNodeKind::CteProducer:
            case NQdb::EExecPlanNodeKind::CteConsumer: {
                auto* consumer = static_cast<const NQdb::TCteConsumer*>(
                    node.Operator);
                if (!node.MaterializationId) {
                    Unsupported = UnsupportedExec(
                        "exec codec: CTE materialization id is missing");
                    return;
                }
                const bool producer =
                    node.Kind == NQdb::EExecPlanNodeKind::CteProducer;
                const auto kind = producer ? "cte-producer" : "cte-consumer";
                const auto cteId =
                    static_cast<int64_t>(consumer->Def()->Id);
                auto* output = static_cast<TStructType*>(
                    node.OutputType.get());
                AddNode(node, llvm::json::Object{
                    {"kind", kind},
                    {"label", std::string(kind) + " " + std::to_string(cteId)},
                    {"materialization",
                        static_cast<int64_t>(*node.MaterializationId)},
                    {"cteId", cteId},
                    {"output", StructColumnsJson(*output)},
                });
                return;
            }
            case NQdb::EExecPlanNodeKind::Window: {
                auto* window = const_cast<NQdb::TWindowOperator*>(
                    static_cast<const NQdb::TWindowOperator*>(node.Operator));
                auto* input = static_cast<TStructType*>(
                    Input(node, 0).OutputType.get());
                auto* output = static_cast<TStructType*>(
                    node.OutputType.get());
                std::string error;
                auto encoded = BuildWindowStageJson(
                    *window, *input, *output, Kernels, node.StageId,
                    EmbedWasm, error);
                if (!encoded) {
                    Unsupported = UnsupportedExec(std::move(error));
                    return;
                }
                encoded->insert({"kind", "window"});
                encoded->insert({"label", "window"});
                AddNode(node, std::move(*encoded));
                return;
            }
            case NQdb::EExecPlanNodeKind::Sort:
            case NQdb::EExecPlanNodeKind::TopSort: {
                const bool top =
                    node.Kind == NQdb::EExecPlanNodeKind::TopSort;
                auto* input = static_cast<TStructType*>(
                    Input(node, 0).OutputType.get());
                std::string error;
                if (top) {
                    auto* sort = static_cast<const NQdb::TTopSortOperator*>(
                        node.Operator);
                    auto encoded = BuildSortStageJson(
                        sort->Keys(), *input, Kernels, node.StageId,
                        EmbedWasm, true, error);
                    if (!encoded) {
                        Unsupported = UnsupportedExec(std::move(error));
                        return;
                    }
                    encoded->insert({"kind", "top-sort"});
                    encoded->insert({"label", SortPlanLabel(
                        "top-sort", sort->Keys(), sort->Limit())});
                    encoded->insert({"limit", sort->Limit()});
                    AddNode(node, std::move(*encoded));
                    return;
                }
                auto* sort = static_cast<const NQdb::TSortOperator*>(
                    node.Operator);
                auto encoded = BuildSortStageJson(
                    sort->Keys(), *input, Kernels, node.StageId,
                    EmbedWasm, false, error);
                if (!encoded) {
                    Unsupported = UnsupportedExec(std::move(error));
                    return;
                }
                encoded->insert({"kind", "sort"});
                encoded->insert({"label", SortPlanLabel(
                    "sort", sort->Keys())});
                AddNode(node, std::move(*encoded));
                return;
            }
            case NQdb::EExecPlanNodeKind::Limit: {
                // A nested (non-root) limit is a streaming node; the root limit
                // is collapsed by the core builder into TExecPlan::RootLimit.
                auto* limit = static_cast<const NQdb::TLimitOperator*>(node.Operator);
                AddNode(node, llvm::json::Object{
                    {"kind", "limit"},
                    {"label", "limit " + std::to_string(limit->Limit())},
                    {"limit", limit->Limit()},
                    {"offset", limit->Offset()},
                });
                return;
            }
            case NQdb::EExecPlanNodeKind::LateMaterialize: {
                Unsupported = UnsupportedExec(
                    "late-materialize is only supported by the native runtime");
                return;
            }
            case NQdb::EExecPlanNodeKind::UnionAll:
                return;
        }
    }

    llvm::json::Object Encode(
        std::string queryWasm,
        int64_t heapBase,
        int wasmBits)
    {
        for (size_t i = 0; i < Plan.Nodes.size(); ++i) {
            const auto& node = Plan.Nodes[i];
            if (node.Id != i) {
                return UnsupportedExec(
                    "exec codec: typed plan node ids are not contiguous");
            }
            EncodeNode(node);
            if (Unsupported) {
                return std::move(*Unsupported);
            }
        }
        for (const auto& node : Plan.Nodes) {
            for (size_t input = 0; input < node.Inputs.size(); ++input) {
                Edges.push_back(llvm::json::Object{
                    {"from", static_cast<int64_t>(node.Inputs[input])},
                    {"to", static_cast<int64_t>(node.Id)},
                    {"input", static_cast<int64_t>(input)},
                    {"kind", "one-to-one"},
                });
            }
        }
        llvm::json::Object out{
            {"supported", true},
            {"embedWasm", EmbedWasm},
            {"layout", ExecLayoutJson(heapBase, wasmBits)},
            {"nodes", std::move(Nodes)},
            {"edges", std::move(Edges)},
            {"root", static_cast<int64_t>(Plan.Root)},
        };
        if (EmbedWasm && !queryWasm.empty()) {
            out["wasm"] = std::move(queryWasm);
        }
        if (Plan.RootLimit) {
            out["limit"] = llvm::json::Object{
                {"limit", Plan.RootLimit->Limit},
                {"offset", Plan.RootLimit->Offset},
            };
        }
        return out;
    }
};

llvm::json::Object EncodeExecPlan(
    const NQdb::TExecPlan* plan,
    std::span<const NQdb::TGeneratedKernel> kernels,
    std::span<const TKernelArtifacts> artifacts,
    bool embedWasm,
    std::string queryWasm,
    int64_t heapBase,
    int wasmBits)
{
    if (!plan) {
        return UnsupportedExec("scheduler lowering did not produce an exec plan");
    }
    try {
        auto kernelIndex = BuildKernelIndex(*plan, kernels, artifacts);
        const bool instrumentation = std::ranges::any_of(kernels, [](const auto& kernel) {
            return kernel.Name == "instrumentation";
        });
        return TExecPlanCodec{
            .Plan = *plan,
            .Kernels = kernelIndex,
            .EmbedWasm = embedWasm,
            .OperatorInstrumentation = instrumentation,
        }.Encode(std::move(queryWasm), heapBase, wasmBits);
    } catch (const NQumir::TError& e) {
        return UnsupportedExec(e.ToString());
    } catch (const std::exception& e) {
        return UnsupportedExec(e.what());
    }
}

llvm::json::Object BuildBundle(
    TExportRequest& request,
    const std::string& cacheDir)
{
    auto plan = ParseSql(request.Sql, request.Catalog);
    if (!plan) {
        return ErrorObject("parse", plan.error().ToString());
    }
    auto externalCatalog = std::move(plan->ExternalCatalog);
    auto logicalPlan = std::move(plan->Plan);

    NQdb::TPlanPassDiagnostics planDiagnostics;
    try {
        NQdb::ApplyPlanPasses(logicalPlan, {
            // The browser runtime has no data-source lookup task yet. Keep its
            // plan on the eager path until it can bind a lookup-capable source.
            .EnableRuntimeFilters = false,
            .LateMaterialization = {.Enabled = false},
            .Diagnostics = &planDiagnostics,
            .Annotation = {.ExternalCatalog = externalCatalog},
        });
    } catch (const NQumir::TError& e) {
        return ErrorObject("logical", e.ToString());
    } catch (const std::exception& e) {
        return ErrorObject("logical", e.what());
    }

    std::ostringstream runtimeText;
    try {
        NQdb::PrintRuntimePlan(runtimeText, logicalPlan);
    } catch (const std::exception& e) {
        runtimeText << "<unprintable runtime plan: " << e.what() << ">\n";
    }

    TArtifactStore artifacts;
    auto logicalGraph = LogicalGraphJson(logicalPlan, artifacts);
    llvm::json::Object legacyGraph = EmptyGraphJson();
    llvm::json::Object physicalGraph = EmptyGraphJson();
    llvm::json::Array diagnostics;
    std::ostringstream schedulerText;
    TWasmFinalizeResult wasmFinalized;
    std::vector<NQdb::TGeneratedKernel> loweredKernels;
    std::optional<NQdb::TExecPlan> typedExecPlan;
    std::string execPlanError;
    auto recordLowerError = [&](const std::string& message) {
        if (execPlanError.empty()) {
            execPlanError = message;
        }
        legacyGraph = EmptyGraphJson(message);
        physicalGraph = EmptyGraphJson(message);
        diagnostics.push_back(llvm::json::Object{
            {"stage", "lower"},
            {"message", message},
        });
    };
    try {
        std::ostringstream lowerDiagnostics;
        // One lowering; kernels arrive as ASTs on lowered.Kernels.
        auto lowered = NQdb::NScheduler::LowerPlanToGraph(
            logicalPlan,
            request.Scheduler,
            &lowerDiagnostics,
            externalCatalog);

        // Core owns graph contraction, ordered inputs, output propagation, and
        // stage/kernel binding. The exporter only encodes the returned plan.
        auto builtExecPlan = NQdb::BuildExecPlan(lowered);
        if (builtExecPlan) {
            typedExecPlan = std::move(*builtExecPlan);
        } else {
            execPlanError = builtExecPlan.error();
            diagnostics.push_back(llvm::json::Object{
                {"stage", "exec-plan"},
                {"message", execPlanError},
            });
        }
        if (request.Scheduler.Instrumentation.Operators && typedExecPlan) {
            NQdb::TKernelCompiler compiler({
                .Sink = &lowered.Kernels,
                .BindNow = false,
                .ExternalCatalog = externalCatalog,
            });
            compiler.CompileInstrumentation();
        }
        lowered.Graph->Build();

        // Wasm finalization: ast artifacts always, wasm in embed mode.
        wasmFinalized = WasmFinalizeKernels(
            lowered.Kernels,
            artifacts,
            diagnostics,
            request.EmbedWasm,
            cacheDir,
            externalCatalog,
            request.WasmBits);
        loweredKernels = std::move(lowered.Kernels);

        schedulerText << lowerDiagnostics.str();
        schedulerText << "\n========== SCHEDULER GRAPH ==========\n";
        schedulerText << "mode=" << static_cast<int>(request.Scheduler.Scheduler.Mode)
                      << " workers=" << request.Scheduler.Scheduler.WorkerCount
                      << " scan_tasks=";
        if (request.Scheduler.ScanSplit.MaxScanTasks == 0) {
            schedulerText << "auto";
        } else {
            schedulerText << request.Scheduler.ScanSplit.MaxScanTasks;
        }
        schedulerText << "\n";
        lowered.Graph->Print(schedulerText);
        schedulerText << "=====================================\n";
        legacyGraph = GraphJson(lowered, wasmFinalized.Kernels);
        physicalGraph = GraphJson(lowered, wasmFinalized.Kernels);
    } catch (const NQumir::TError& e) {
        recordLowerError(e.ToString());
    } catch (const std::exception& e) {
        recordLowerError(e.what());
    }

    // The codec reads the kernels already generated by the one native lowering.
    llvm::json::Object execPlan;
    if (!wasmFinalized.Error.empty()) {
        execPlan = UnsupportedExec(
            wasmFinalized.Error, wasmFinalized.ErrorStage);
    } else if (typedExecPlan) {
        execPlan = EncodeExecPlan(
            &*typedExecPlan,
            loweredKernels,
            wasmFinalized.Kernels,
            request.EmbedWasm,
            wasmFinalized.QueryWasm,
            wasmFinalized.HeapBase,
            request.WasmBits);
        for (size_t i = 0; i < loweredKernels.size(); ++i) {
            if (loweredKernels[i].Name == "instrumentation") {
                execPlan["operatorInstrumentation"] = EntrypointsJson({
                    &loweredKernels[i], &wasmFinalized.Kernels[i]});
            }
        }
    } else {
        execPlan = UnsupportedExec(execPlanError.empty()
            ? "scheduler lowering did not produce an exec plan"
            : execPlanError);
    }

    return llvm::json::Object{
            {"ok", true},
            {"format", "qdb.runtime.bundle"},
            {"version", 1},
            {"mode", SchedulerModeName(request.Scheduler.Scheduler.Mode)},
            {"dataset", llvm::json::Object{
                {"source", "browser"},
            }},
            {"planner", PlanPassDiagnosticsJson(planDiagnostics)},
            {"exec", std::move(execPlan)},
            {"plans", llvm::json::Object{
                {"logicalText", LogicalPlanTreeText(logicalPlan)},
                {"logicalAstText", LogicalPlanAstText(logicalPlan)},
                {"physicalText", runtimeText.str() + schedulerText.str()},
            }},
            {"graph", std::move(legacyGraph)},
            {"graphs", llvm::json::Object{
                {"logical", std::move(logicalGraph)},
                {"physical", std::move(physicalGraph)},
            }},
            {"kernels", llvm::json::Object{}},
            {"modules", llvm::json::Object{}},
            {"artifacts", artifacts.Take()},
            {"diagnostics", std::move(diagnostics)},
    };
}

void PrintHelp(const char* argv0) {
    std::cout << "Usage: " << argv0
              << " --plan-export --stdin-json --stdout-json [--cache dir]\n";
}

} // namespace

namespace NQdb {

int RunPlanExport(int argc, char** argv) {
    NQumir::NCodeGen::TLLVMInitializer llvmInit;

    bool stdinJson = false;
    bool stdoutJson = false;
    std::string cacheDir;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--stdin-json")) {
            stdinJson = true;
        } else if (!std::strcmp(argv[i], "--stdout-json")) {
            stdoutJson = true;
        } else if (!std::strcmp(argv[i], "--cache")) {
            if (i + 1 >= argc) {
                std::cerr << "qdb --plan-export: --cache requires a directory\n";
                return 2;
            }
            cacheDir = argv[++i];
        } else if (!std::strcmp(argv[i], "--help")) {
            PrintHelp(argv[0]);
            return 0;
        }
    }

    if (!stdinJson || !stdoutJson) {
        std::cerr << "qdb --plan-export: expected --stdin-json --stdout-json\n";
        return 2;
    }

    auto request = ParseRequest(ReadAllStdin());
    if (!request) {
        return PrintError("request", request.error());
    }

    llvm::json::Object bundle;
    {
        TOutputSilencer silence;
        bundle = BuildBundle(*request, cacheDir);
    }

    std::cout << ToJsonString(std::move(bundle)) << "\n";
    return 0;
}

} // namespace NQdb
