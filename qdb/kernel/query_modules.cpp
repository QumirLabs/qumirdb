#include <qdb/kernel/query_modules.h>

#include <qdb/kernel/lib.h>
#include <qdb/modules/qumirdb.h>
#include <qdb/plan/types/date.h>
#include <qdb/plan/types/decimal.h>
#include <qdb/plan/types/nullable.h>
#include <qdb/utils/module_path.h>

#include <qumir/frontend/compose.h>
#include <qumir/modules/builtins/builtins.h>
#include <qumir/modules/system/system.h>
#include <qumir/parser/ast.h>
#include <qumir/semantics/name_resolution/name_resolver.h>

#include <filesystem>
#include <optional>

namespace NQdb::NKernel {

namespace {

// QumirDB's own named types, so that queries share the base's objects instead of making them.
void AddNamedTypes(NQumir::NAst::TTypeFactory& types) {
    using NQumir::NAst::TIntegerType;
    for (const auto& type : NQumir::NRegistry::QumirDbExternalTypes(types)) {
        NQumir::NRegistry::QumirDbNamedType(types, type.Name);
    }
    BinIntStorageType(types);
    DateCastTarget(types);
    MakeDateType(types, TIntegerType::I32);
    MakeDateType(types, TIntegerType::I64);
    MakeNullableType(types, types.Make<NQumir::NAst::TBoolType>());
    MakeNullableType(types, types.MakeInteger(TIntegerType::I64));
}

// Resolves the names of a clone of module `name` once in the base, as a query resolves its
// own clone. The generic instances the module mentions (Nullable[T], RgRange[i64], ...) are
// then complete base types, so a query never makes a copy of one.
std::optional<NQumir::TError> ResolveInBase(
    NQumir::NAst::TTypeFactory& types,
    NQumir::NFrontend::TSourceModuleLoader& loader,
    const std::string& name)
{
    using namespace NQumir::NAst;
    const std::vector<TPragma> pragmas{TPragma{"language", {"overloads"}, {}}};
    TExprPtr program = NQumir::MakeIntrusive<TBlockExpr>(
        NQumir::TLocation{}, std::vector<TExprPtr>{NQumir::MakeIntrusive<TUseExpr>(NQumir::TLocation{}, name)});
    auto composed = NQumir::NFrontend::LoadAndCompose(loader, program, pragmas, /*cloneSourceModules*/ true);
    if (!composed) {
        return composed.error();
    }
    NQumir::NRegistry::SystemModule system(types);
    NQumir::NRegistry::BuiltinsModule builtins(types);
    NQumir::NSemantics::TNameResolver resolver(types);
    resolver.ApplyPragmas(pragmas);
    resolver.RegisterModule(&system);
    resolver.RegisterModule(&builtins);
    (void)resolver.ImportModule(builtins.Name());
    resolver.ApplyPragmas(composed->Pragmas);
    resolver.GetOrCreateRootScope()->RootLevel = false;
    return resolver.Resolve(composed->Ast);
}

// The libraries the kernel generators compose, as (kind, name). The base parses all of them:
// once frozen, it cannot take the types of a library a query asks for later.
constexpr std::pair<const char*, const char*> KernelLibraryFiles[] = {
    {".", "instrumentation.oz"},
    {".", "row_table.oz"},
    {"aggregation", "aggregation_finalize_states.oz"},
    {"aggregation", "aggregation_hashtable_generic.oz"},
    {"aggregation", "aggregation_rows.oz"},
    {"aggregation", "aggregation_string_reducer.oz"},
    {"aggregation", "key_ops_i64.oz"},
    {"aggregation", "owned_arena_lifecycle.oz"},
    {"aggregation", "owned_blocks.oz"},
    {"aggregation", "robin_hood_dual_key.oz"},
    {"aggregation", "robin_hood_rehash_generic.oz"},
    {"aggregation", "string_ops.oz"},
    {"aggregation", "swiss_group.oz"},
    {"join", "join_cross.oz"},
    {"join", "join_mark.oz"},
    {"join", "join_residual_default.oz"},
    {"join", "join_rows.oz"},
    {"join", "join_semi_anti.oz"},
    {"join", "join_table.oz"},
    {"join", "join_update.oz"},
    {"join", "materialize.oz"},
    {"join", "runtime_filter.oz"},
    {"sort", "heap.oz"},
    {"sort", "materialize.oz"},
    {"sort", "radix.oz"},
    {"sort", "sort_rowids.oz"},
    {"sort", "window.oz"},
    {"sort", "window_range.oz"},
};

} // namespace

TModuleBase::TModuleBase() {
    AddNamedTypes(*Types_);
    for (const auto* fileName : {"qumirdb.oz", "row_group_interval.oz"}) {
        auto file = NUtils::ModuleFile(fileName);
        auto& loader = Loaders_.try_emplace(file, *Types_).first->second;
        if (auto registered = loader.RegisterSourceModule(file); !registered) {
            Modules_.emplace(file, std::unexpected(registered.error()));
            continue;
        }
        const auto name = std::filesystem::path(file).stem().string();
        auto module = loader.Load(name);
        if (module) {
            if (auto error = ResolveInBase(*Types_, loader, name)) {
                module = std::unexpected(*error);
            }
        }
        Modules_.emplace(file, std::move(module));
    }
    for (const auto& [kind, name] : KernelLibraryFiles) {
        KernelLibraries_.emplace(std::pair(kind, name), ParseFunctionLibrary(*Types_, ReadKernel(kind, name)));
    }
    Types_->Freeze();
}

std::shared_ptr<const NQumir::NAst::TTypeFactory> TModuleBase::Types() const {
    return Types_;
}

std::expected<const NQumir::NFrontend::TSourceModule*, NQumir::TError> TModuleBase::Find(
    const std::string& file) const
{
    auto it = Modules_.find(file);
    if (it == Modules_.end()) {
        return std::unexpected(NQumir::TError(file + " is not a module of the base"));
    }
    return it->second;
}

std::expected<const std::vector<NQumir::NAst::TExprPtr>*, NQumir::TError> TModuleBase::KernelLibrary(
    const std::string& kind,
    const std::string& name) const
{
    auto it = KernelLibraries_.find(std::pair(kind, name));
    if (it == KernelLibraries_.end()) {
        return std::unexpected(NQumir::TError(kind + "/" + name + " is not a kernel library of the base"));
    }
    if (!it->second) {
        return std::unexpected(it->second.error());
    }
    return &*it->second;
}

std::shared_ptr<NQumir::NAst::TTypeFactory> MakeQueryTypes(const TModuleBase& base) {
    return std::make_shared<NQumir::NAst::TTypeFactory>(base.Types());
}

TQueryModules::TQueryModules(NQumir::NAst::TTypeFactory& types, std::shared_ptr<const TModuleBase> base)
    : Types_(types)
    , Base_(std::move(base))
{
}

NQumir::NAst::TTypeFactory& TQueryModules::Types() const {
    return Types_;
}

std::expected<std::monostate, NQumir::TError> TQueryModules::Load(
    NQumir::NFrontend::TSourceModuleLoader& loader,
    const std::string& file)
{
    auto module = Base_->Find(file);
    if (!module) {
        return std::unexpected(module.error());
    }
    const auto& parsed = **module;
    auto loaded = loader.LoadAst(parsed.Name, parsed.Ast, parsed.LlvmBitcode, parsed.Pragmas, parsed.Path);
    if (!loaded) {
        return std::unexpected(loaded.error());
    }
    return std::monostate{};
}

std::expected<std::vector<NQumir::NAst::TExprPtr>, NQumir::TError> TQueryModules::KernelLibrary(
    const std::string& kind,
    const std::string& name,
    const std::unordered_set<std::string>& exclude) const
{
    using namespace NQumir::NAst;
    auto parsed = Base_->KernelLibrary(kind, name);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    std::vector<TExprPtr> library;
    for (const auto& stmt : **parsed) {
        auto fun = TMaybeNode<TFunDecl>(stmt);
        if (!fun || !exclude.contains(fun.Cast()->Name)) {
            // Library code, like a module: the kernel frontends skip what the program does not reach.
            // The kernel fusion keys library functions by this origin, so it names the file uniquely.
            library.push_back(DeepCloneExpr(stmt));
            library.back()->Origin = kind + "/" + name;
        }
    }
    return library;
}

} // namespace NQdb::NKernel
