#include <qdb/catalog/external_module.h>

#include <qdb/plan/types/nullable.h>
#include <qdb/sql/ast.h>
#include <qdb/utils/rust_std_loader.h>
#include <qdb/utils/sha1.h>

#include <qumir/frontend/source_module_loader.h>
#include <qumir/modules/system/system.h>
#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>
#include <qumir/parser/lexer.h>
#include <qumir/parser/parser.h>
#include <qumir/semantics/kumir/pipeline.h>
#include <qumir/semantics/name_resolution/name_resolver.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace NQdb {

using namespace NQumir;
using namespace NQumir::NAst;

namespace {

std::string CatalogTypeKey(const TTypePtr& type) {
    if (!type) {
        return "?";
    }
    return NQumir::NAst::TypeKey(UnwrapNamedType(UnwrapNullableType(type)));
}

std::string FunctionKey(const TFunDecl& function) {
    std::string key = function.Name;
    key += '(';
    for (size_t i = 0; i < function.Params.size(); ++i) {
        if (i) {
            key += ',';
        }
        key += CatalogTypeKey(function.Params[i]->Type);
    }
    key += ')';
    return key;
}

std::string CallKey(const std::string& name, const std::vector<TTypePtr>& argTypes) {
    std::string key = name;
    key += '(';
    for (size_t i = 0; i < argTypes.size(); ++i) {
        if (i) {
            key += ',';
        }
        key += CatalogTypeKey(argTypes[i]);
    }
    key += ')';
    return key;
}

std::shared_ptr<TFunDecl> CloneFunction(const TFunDecl& source) {
    std::vector<TParam> params;
    params.reserve(source.Params.size());
    for (const auto& param : source.Params) {
        params.push_back(std::make_shared<TVarStmt>(
            param->Location, param->Name, param->Type));
    }
    auto result = std::make_shared<TFunDecl>(
        source.Location,
        source.Name,
        source.GenericParams,
        std::move(params),
        std::make_shared<TBlockExpr>(source.Location, std::vector<TExprPtr>{}),
        source.RetType);
    result->MangledName = source.MangledName;
    result->Type = source.Type;
    return result;
}

std::shared_ptr<TFunDecl> CloneDeclaration(const TFunDecl& source) {
    auto result = CloneFunction(source);
    if (result->MangledName.empty()) {
        result->MangledName = result->Name;
    }
    return result;
}

struct TParsedModule {
    TExprPtr Ast;
    std::vector<TPragma> Pragmas;
    std::map<std::string, std::shared_ptr<TFunDecl>> Functions;
};

std::expected<TParsedModule, TError> ParseSourceModule(
    const std::string& moduleName,
    const std::string& source,
    const std::string& language)
{
    TExprPtr ast;
    std::vector<TPragma> pragmas;
    std::istringstream input(source);
    if (language == "ozlang") {
        NCore::TTokenStream stream(input);
        NCore::TParser parser;
        auto parsed = parser.Parse(stream);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        ast = std::move(*parsed);
        pragmas = std::move(parser.Pragmas);
    } else {
        NSemantics::TNameResolver resolver;
        auto system = std::make_shared<NRegistry::SystemModule>();
        resolver.RegisterModule(system.get());
        (void)resolver.ImportModule("System");
        for (const auto& [alias, canonical] : NSemantics::NKumir::ModuleAliases()) {
            resolver.RegisterModuleAlias(alias, canonical);
        }
        TTokenStream stream(input);
        TParser parser;
        auto parsed = parser.parse(stream, &resolver);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        ast = std::move(*parsed);
        pragmas = stream.GetContext()->GetPragmas();
        std::erase_if(pragmas, [](const TPragma& pragma) {
            return pragma.Group == "language";
        });
        pragmas.push_back(TPragma{"language", {"overloads"}, {}});
    }

    auto block = TMaybeNode<TBlockExpr>(ast);
    if (!block) {
        return std::unexpected(TError(
            "external " + language + " module `" + moduleName + "' must be a block"));
    }

    // Reuse the source-loader validation contract: imported modules cannot
    // contain an entry point or arbitrary top-level statements.
    NFrontend::TSourceModuleLoader loader;
    auto loaded = loader.LoadAst(
        moduleName, ast, {}, pragmas, "<" + language + ":" + moduleName + ">");
    if (!loaded) {
        return std::unexpected(loaded.error());
    }

    std::map<std::string, std::shared_ptr<TFunDecl>> functions;
    for (const auto& stmt : block.Cast()->Stmts) {
        auto function = TMaybeNode<TFunDecl>(stmt);
        if (!function) {
            continue;
        }
        const auto key = FunctionKey(*function.Cast());
        if (!functions.emplace(key, function.Cast()).second) {
            return std::unexpected(TError(
                "external function `" + key + "' is declared more than once"));
        }
    }
    return TParsedModule{
        .Ast = std::move(ast),
        .Pragmas = std::move(pragmas),
        .Functions = std::move(functions),
    };
}

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot read " + path.string());
    }
    return std::string(
        std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::string RustCrateName(std::string_view moduleName) {
    std::string result;
    result.reserve(moduleName.size() + 1);
    for (char ch : moduleName) {
        const auto c = static_cast<unsigned char>(ch);
        result += std::isalnum(c) || ch == '_' ? ch : '_';
    }
    if (result.empty() || std::isdigit(static_cast<unsigned char>(result.front()))) {
        result.insert(result.begin(), '_');
    }
    return result;
}

std::string RustModuleCacheKey(
    const std::string& moduleName,
    const std::string& source,
    const std::string& target)
{
    std::string input;
    input.reserve(moduleName.size() + source.size() + target.size() + 32);
    auto add = [&](std::string_view value) {
        input.append(value);
        input.push_back('\0');
    };
    add("qdb-rust-module-v1");
    add(moduleName);
    add(source);
    add(target);
    return NUtils::Sha1Hex(input);
}

std::expected<std::monostate, TError> WriteFileAtomically(
    const std::filesystem::path& path,
    std::string_view contents,
    std::string_view description)
{
    std::error_code existsError;
    if (std::filesystem::exists(path, existsError)) {
        return std::monostate{};
    }
    if (existsError) {
        return std::unexpected(TError(
            "cannot inspect " + std::string(description) + ": " +
            existsError.message()));
    }

    std::string pattern = path.string() + ".XXXXXX";
    std::vector<char> temp(pattern.begin(), pattern.end());
    temp.push_back('\0');
    const int fd = ::mkstemp(temp.data());
    if (fd < 0) {
        return std::unexpected(TError(
            "cannot create " + std::string(description) + ": " +
            std::string(std::strerror(errno))));
    }

    size_t written = 0;
    while (written < contents.size()) {
        const ssize_t n = ::write(
            fd, contents.data() + written, contents.size() - written);
        if (n > 0) {
            written += static_cast<size_t>(n);
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            const int error = errno;
            ::close(fd);
            ::unlink(temp.data());
            return std::unexpected(TError(
                "cannot write " + std::string(description) + ": " +
                std::string(std::strerror(error))));
        }
    }
    if (::close(fd) != 0 || ::rename(temp.data(), path.c_str()) != 0) {
        const int error = errno;
        ::unlink(temp.data());
        return std::unexpected(TError(
            "cannot install " + std::string(description) + ": " +
            std::string(std::strerror(error))));
    }
    return std::monostate{};
}

std::expected<std::string, TError> CompileRust(
    const std::string& moduleName,
    const std::string& source,
    const std::string& target,
    const std::string& cacheDir)
{
    std::string pattern =
        (std::filesystem::temp_directory_path() / "qdb-rust-XXXXXX").string();
    std::vector<char> temp(pattern.begin(), pattern.end());
    temp.push_back('\0');
    char* created = ::mkdtemp(temp.data());
    if (!created) {
        return std::unexpected(TError(
            "cannot create rustc temporary directory: " +
            std::string(std::strerror(errno))));
    }

    const std::filesystem::path invocationDir(created);
    const auto stderrPath = invocationDir / "compiler.stderr";
    struct TCleanup {
        std::filesystem::path Path;
        ~TCleanup() {
            std::error_code ec;
            std::filesystem::remove_all(Path, ec);
        }
    } cleanup{invocationDir};

    const bool wasm64 = target == "wasm64-unknown-unknown";
    std::filesystem::path projectDir = invocationDir;
    std::filesystem::path targetDir = invocationDir / "target";
    if (wasm64 && !cacheDir.empty()) {
        const auto rustCache = std::filesystem::path(cacheDir) / "rust";
        projectDir = rustCache / "modules" /
            RustModuleCacheKey(moduleName, source, target);
        targetDir = rustCache / "target";
        std::error_code ec;
        std::filesystem::create_directories(projectDir, ec);
        if (ec) {
            return std::unexpected(TError(
                "cannot create Rust module cache `" + projectDir.string() +
                "': " + ec.message()));
        }
        std::filesystem::create_directories(targetDir, ec);
        if (ec) {
            return std::unexpected(TError(
                "cannot create Rust target cache `" + targetDir.string() +
                "': " + ec.message()));
        }
    }

    const auto sourcePath = projectDir / "module.rs";
    const auto outputPath = projectDir / "module.bc";
    if (wasm64 && !cacheDir.empty()) {
        auto written = WriteFileAtomically(sourcePath, source, "Rust module source");
        if (!written) {
            return std::unexpected(written.error());
        }
    } else {
        std::ofstream out(sourcePath, std::ios::binary);
        if (!out || !(out << source)) {
            return std::unexpected(TError("cannot write rustc input"));
        }
    }

    std::vector<std::string> args;
    if (wasm64) {
        // Rust does not ship wasm64 std artifacts yet. Cargo builds std from
        // rust-src, while rustc emits only the external crate as LLVM bitcode.
        const auto manifestPath = projectDir / "Cargo.toml";
        std::ostringstream manifest;
        manifest << R"([package]
name = ")" << RustCrateName(moduleName) << R"("
version = "0.0.0"
edition = "2021"

[lib]
path = "module.rs"
crate-type = ["rlib"]

[profile.release]
opt-level = 2
panic = "abort"
codegen-units = 1
)";
        if (!cacheDir.empty()) {
            auto written = WriteFileAtomically(
                manifestPath, manifest.str(), "Cargo manifest");
            if (!written) {
                return std::unexpected(written.error());
            }
        } else {
            std::ofstream out(manifestPath, std::ios::binary);
            if (!out || !(out << manifest.str())) {
                return std::unexpected(TError("cannot write Cargo manifest"));
            }
        }
        args = {
            "cargo", "rustc",
            "--manifest-path", manifestPath.string(),
            "--target-dir", targetDir.string(),
            "--target", target,
            "--release",
            "--lib",
            "-Z", "build-std=std,panic_abort",
            "--",
            "--emit=llvm-bc=" + outputPath.string(),
            "-C", "codegen-units=1",
        };
    } else {
        args = {
            "rustc",
            sourcePath.string(),
            "--crate-name", RustCrateName(moduleName),
            "--crate-type", "lib",
            "--edition", "2021",
            "--emit", "llvm-bc",
            "-o", outputPath.string(),
        };
        if (!target.empty()) {
            args.push_back("--target");
            args.push_back(target);
        }
    }
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& arg : args) {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0) {
        return std::unexpected(TError(
            "cannot start rustc: " + std::string(std::strerror(errno))));
    }
    if (pid == 0) {
        FILE* err = std::fopen(stderrPath.c_str(), "wb");
        if (err) {
            ::dup2(::fileno(err), STDERR_FILENO);
        }
        if (wasm64) {
            ::setenv("RUSTC_BOOTSTRAP", "1", 1);
            ::setenv("RUSTFLAGS", "-C panic=abort", 1);
        }
        ::execvp(argv[0], argv.data());
        std::fprintf(
            stderr, "cannot execute %s: %s\n", args[0].c_str(), std::strerror(errno));
        _exit(127);
    }

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return std::unexpected(TError(
                "cannot wait for rustc: " + std::string(std::strerror(errno))));
        }
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        std::string diagnostics;
        try {
            diagnostics = ReadFile(stderrPath);
        } catch (...) {
        }
        if (diagnostics.empty()) {
            diagnostics = args[0] + " failed";
        }
        return std::unexpected(TError(
            "external module `" + moduleName + "': " + diagnostics));
    }

    try {
        return ReadFile(outputPath);
    } catch (const std::exception& e) {
        return std::unexpected(TError(
            "external module `" + moduleName + "': " + e.what()));
    }
}

void CollectCallNames(const TExprPtr& expr, std::unordered_set<std::string>& names) {
    if (!expr) {
        return;
    }
    if (auto call = TMaybeNode<TCallExpr>(expr)) {
        if (auto ident = TMaybeNode<TIdentExpr>(call.Cast()->Callee)) {
            names.insert(ident.Cast()->Name);
        }
    }
    for (const auto& child : expr->Children()) {
        CollectCallNames(child, names);
    }
}

} // namespace

struct TExternalCatalogSnapshot::TState {
    struct TRustRuntime {
        std::mutex Mutex;
        std::shared_ptr<NUtils::TRustStdLoader> Loader;
    };

    struct TBitcodeCache {
        std::mutex Mutex;
        std::unordered_map<std::string, std::string> ByTarget;
    };

    struct TModule {
        std::string Name;
        std::string Language;
        std::string Source;
        std::map<std::string, std::shared_ptr<TFunDecl>> Functions;
        std::shared_ptr<TBitcodeCache> Bitcode = std::make_shared<TBitcodeCache>();
    };

    std::map<std::string, std::shared_ptr<const TModule>> Modules;
    std::shared_ptr<TRustRuntime> RustRuntime = std::make_shared<TRustRuntime>();
};

struct TExternalCatalogSnapshot::TResolvedTypes {
    std::mutex Mutex;
    std::unordered_map<std::string, TTypePtr> ReturnTypes;
};

namespace {

std::expected<std::monostate, TError> EnsureRustRuntime(
    const TExternalCatalogSnapshot::TState& state)
{
    std::lock_guard lock(state.RustRuntime->Mutex);
    if (state.RustRuntime->Loader) {
        return std::monostate{};
    }
    try {
        state.RustRuntime->Loader = std::make_shared<NUtils::TRustStdLoader>();
        return std::monostate{};
    } catch (const std::exception& e) {
        return std::unexpected(TError(
            "cannot load the native Rust std runtime: " + std::string(e.what())));
    }
}

std::shared_ptr<TExternalCatalogSnapshot::TState::TModule> CloneModule(
    const std::shared_ptr<const TExternalCatalogSnapshot::TState::TModule>& source,
    bool keepBitcode)
{
    auto result = std::make_shared<TExternalCatalogSnapshot::TState::TModule>();
    result->Name = source->Name;
    result->Language = source->Language;
    result->Source = source->Source;
    result->Functions = source->Functions;
    if (keepBitcode) {
        result->Bitcode = source->Bitcode;
    }
    return result;
}

std::expected<std::string, TError> ModuleBitcode(
    const TExternalCatalogSnapshot::TState::TModule& module,
    const std::string& target,
    const std::string& cacheDir)
{
    {
        std::lock_guard lock(module.Bitcode->Mutex);
        if (auto it = module.Bitcode->ByTarget.find(target);
            it != module.Bitcode->ByTarget.end())
        {
            return it->second;
        }
    }
    auto compiled = CompileRust(module.Name, module.Source, target, cacheDir);
    if (!compiled) {
        return std::unexpected(compiled.error());
    }
    std::lock_guard lock(module.Bitcode->Mutex);
    auto [it, inserted] = module.Bitcode->ByTarget.emplace(target, std::move(*compiled));
    return it->second;
}

} // namespace

TExternalCatalogSnapshot::TExternalCatalogSnapshot(std::shared_ptr<const TState> state)
    : State_(std::move(state))
    , ResolvedTypes_(std::make_shared<TResolvedTypes>())
{
}

std::expected<std::optional<TTypePtr>, TError>
TExternalCatalogSnapshot::ResolveReturnType(
    const std::string& name,
    const std::vector<TTypePtr>& argTypes) const
{
    bool knownName = false;
    const std::string key = CallKey(name, argTypes);
    {
        std::lock_guard lock(ResolvedTypes_->Mutex);
        if (auto it = ResolvedTypes_->ReturnTypes.find(key);
            it != ResolvedTypes_->ReturnTypes.end())
        {
            return std::optional<TTypePtr>{it->second};
        }
    }
    for (const auto& [_, module] : State_->Modules) {
        for (const auto& [functionKey, function] : module->Functions) {
            knownName = knownName || function->Name == name;
            if (functionKey == key) {
                return std::optional<TTypePtr>{function->RetType};
            }
        }
    }
    if (knownName) {
        return std::unexpected(TError(
            "no external function overload matches `" + key + "'"));
    }
    return std::optional<TTypePtr>{};
}

void TExternalCatalogSnapshot::RememberResolvedReturnType(
    const std::string& name,
    const std::vector<TTypePtr>& argTypes,
    TTypePtr returnType) const
{
    std::lock_guard lock(ResolvedTypes_->Mutex);
    ResolvedTypes_->ReturnTypes[CallKey(name, argTypes)] = std::move(returnType);
}

bool TExternalCatalogSnapshot::HasFunction(const std::string& name) const {
    for (const auto& [_, module] : State_->Modules) {
        if (std::ranges::any_of(
                module->Functions,
                [&](const auto& item) { return item.second->Name == name; }))
        {
            return true;
        }
    }
    return false;
}

bool TExternalCatalogSnapshot::HasKumirModules() const {
    return std::ranges::any_of(State_->Modules, [](const auto& item) {
        return item.second->Language == "kumir";
    });
}

std::vector<std::string> TExternalCatalogSnapshot::ModuleNames() const {
    std::vector<std::string> result;
    result.reserve(State_->Modules.size());
    for (const auto& [name, _] : State_->Modules) {
        result.push_back(name);
    }
    return result;
}

std::expected<std::monostate, TError>
TExternalCatalogSnapshot::RegisterDeclarations(
    NQumir::NFrontend::TSourceModuleLoader& loader) const
{
    for (const auto& [_, module] : State_->Modules) {
        std::vector<TExprPtr> declarations;
        declarations.reserve(module->Functions.size());
        for (const auto& [__, function] : module->Functions) {
            declarations.push_back(CloneDeclaration(*function));
        }
        auto loaded = loader.LoadAst(
            module->Name,
            std::make_shared<TBlockExpr>(TLocation{}, std::move(declarations)),
            {},
            {TPragma{"language", {"overloads"}, {}}});
        if (!loaded) {
            return std::unexpected(loaded.error());
        }
    }
    return std::monostate{};
}

namespace {

// The modules that define a function `ast` calls.
std::vector<std::shared_ptr<const TExternalCatalogSnapshot::TState::TModule>> ReferencedModules(
    const TExternalCatalogSnapshot::TState& state, const TExprPtr& ast)
{
    std::unordered_set<std::string> callNames;
    CollectCallNames(ast, callNames);

    std::vector<std::shared_ptr<const TExternalCatalogSnapshot::TState::TModule>> modules;
    for (const auto& [_, module] : state.Modules) {
        bool referenced = std::ranges::any_of(
            module->Functions,
            [&](const auto& item) { return callNames.contains(item.second->Name); });
        if (referenced) {
            modules.push_back(module);
        }
    }
    return modules;
}

} // namespace

bool TExternalCatalogSnapshot::ReferencesModules(const TExprPtr& ast) const {
    return !ReferencedModules(*State_, ast).empty();
}

std::expected<std::optional<NQumir::NFrontend::TComposeResult>, TError>
TExternalCatalogSnapshot::ComposeReferenced(
    const TExprPtr& ast,
    const std::string& target,
    const std::string& cacheDir) const
{
    auto modules = ReferencedModules(*State_, ast);
    if (modules.empty()) {
        return std::optional<NQumir::NFrontend::TComposeResult>{};
    }

    // Cross-compiled modules bring their own target runtime. Native Rust
    // bitcode instead resolves alloc/panic/etc. through the process symbols.
    const bool usesNativeRust = target.empty() && std::ranges::any_of(
        modules, [](const auto& module) { return module->Language == "rust"; });
    if (usesNativeRust) {
        auto loaded = EnsureRustRuntime(*State_);
        if (!loaded) {
            return std::unexpected(loaded.error());
        }
    }

    auto inputBlock = TMaybeNode<TBlockExpr>(ast);
    if (!inputBlock) {
        return std::unexpected(TError("external modules require a block program"));
    }
    auto program = std::make_shared<TBlockExpr>(
        ast->Location, inputBlock.Cast()->Stmts);
    program->Type = ast->Type;
    program->Origin = ast->Origin;

    NQumir::NFrontend::TSourceModuleLoader loader;
    for (const auto& module : modules) {
        std::expected<const NFrontend::TSourceModule*, TError> loaded =
            std::unexpected(TError("unsupported external module language"));
        if (module->Language == "rust") {
            std::vector<TExprPtr> declarations;
            declarations.reserve(module->Functions.size());
            for (const auto& [_, function] : module->Functions) {
                declarations.push_back(CloneDeclaration(*function));
            }
            auto bitcode = ModuleBitcode(*module, target, cacheDir);
            if (!bitcode) {
                return std::unexpected(bitcode.error());
            }
            loaded = loader.LoadAst(
                module->Name,
                std::make_shared<TBlockExpr>(TLocation{}, std::move(declarations)),
                {std::move(*bitcode)},
                {TPragma{"language", {"overloads"}, {}}});
        } else if (module->Language == "kumir" || module->Language == "ozlang") {
            auto parsed = ParseSourceModule(
                module->Name, module->Source, module->Language);
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            loaded = loader.LoadAst(
                module->Name,
                std::move(parsed->Ast),
                {},
                std::move(parsed->Pragmas));
        }
        if (!loaded) {
            return std::unexpected(loaded.error());
        }
        program->Stmts.insert(
            program->Stmts.begin(),
            std::make_shared<TUseExpr>(TLocation{}, module->Name));
    }

    auto composed = NQumir::NFrontend::LoadAndCompose(
        loader, program, {TPragma{"language", {"overloads"}, {}}});
    if (!composed) {
        return std::unexpected(composed.error());
    }
    return std::optional<NQumir::NFrontend::TComposeResult>{std::move(*composed)};
}

TExternalModuleCatalog::TExternalModuleCatalog()
    : State_(std::make_shared<TExternalCatalogSnapshot::TState>())
{
}

std::expected<std::monostate, TError> TExternalModuleCatalog::Apply(
    const NSql::TSqlExternalModule& definition)
{
    if (definition.Language != "rust" && definition.Language != "kumir"
        && definition.Language != "ozlang") {
        return std::unexpected(TError(
            "unsupported external module language `" + definition.Language + "'"));
    }

    std::optional<TParsedModule> parsedSource;
    if (definition.Language != "rust") {
        auto parsed = ParseSourceModule(
            definition.Name, definition.Code, definition.Language);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        parsedSource = std::move(*parsed);
    }

    std::lock_guard lock(Mutex_);
    auto existing = State_->Modules.find(definition.Name);
    if (existing != State_->Modules.end() && !definition.Replace) {
        return std::unexpected(TError(
            "external module `" + definition.Name + "' already exists"));
    }

    auto next = std::make_shared<TExternalCatalogSnapshot::TState>(*State_);
    auto module = std::make_shared<TExternalCatalogSnapshot::TState::TModule>();
    module->Name = definition.Name;
    module->Language = definition.Language;
    module->Source = definition.Code;
    if (parsedSource) {
        for (const auto& [key, _] : parsedSource->Functions) {
            for (const auto& [otherName, other] : State_->Modules) {
                if (otherName != definition.Name && other->Functions.contains(key)) {
                    return std::unexpected(TError(
                        "external function `" + key + "' already exists"));
                }
            }
        }
        module->Functions = std::move(parsedSource->Functions);
    } else if (existing != State_->Modules.end()
        && existing->second->Language == "rust") {
        module->Functions = existing->second->Functions;
    }
    next->Modules[definition.Name] = std::move(module);
    State_ = std::move(next);
    return std::monostate{};
}

std::expected<std::monostate, TError> TExternalModuleCatalog::Apply(
    const NSql::TSqlExternalFunction& definition)
{
    std::lock_guard lock(Mutex_);
    if (!definition.Func) {
        return std::unexpected(TError("external function declaration is empty"));
    }
    auto targetModule = State_->Modules.find(definition.ModuleName);
    if (targetModule == State_->Modules.end()) {
        return std::unexpected(TError(
            "external module `" + definition.ModuleName + "' does not exist"));
    }
    if (targetModule->second->Language != "rust") {
        return std::unexpected(TError(
            "CREATE FUNCTION is not required for `" +
            targetModule->second->Language + "' modules"));
    }
    const std::string key = FunctionKey(*definition.Func);

    std::optional<std::string> oldModule;
    for (const auto& [moduleName, module] : State_->Modules) {
        if (module->Functions.contains(key)) {
            oldModule = moduleName;
            break;
        }
    }
    if (oldModule && !definition.Replace) {
        return std::unexpected(TError(
            "external function `" + key + "' already exists"));
    }

    auto next = std::make_shared<TExternalCatalogSnapshot::TState>(*State_);
    if (oldModule && *oldModule != definition.ModuleName) {
        auto oldCopy = CloneModule(next->Modules.at(*oldModule), true);
        oldCopy->Functions.erase(key);
        next->Modules[*oldModule] = std::move(oldCopy);
    }
    auto targetCopy = CloneModule(next->Modules.at(definition.ModuleName), true);
    targetCopy->Functions[key] = CloneFunction(*definition.Func);
    next->Modules[definition.ModuleName] = std::move(targetCopy);
    State_ = std::move(next);
    return std::monostate{};
}

std::shared_ptr<const TExternalCatalogSnapshot> TExternalModuleCatalog::Snapshot() const {
    std::lock_guard lock(Mutex_);
    return std::shared_ptr<const TExternalCatalogSnapshot>(
        new TExternalCatalogSnapshot(State_));
}

} // namespace NQdb
