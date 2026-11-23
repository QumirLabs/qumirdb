#pragma once

#include <qumir/error.h>
#include <qumir/frontend/compose.h>
#include <qumir/parser/ast.h>
#include <qumir/parser/type_factory.h>

#include <expected>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace NQdb::NSql {
struct TSqlExternalFunction;
struct TSqlExternalModule;
} // namespace NQdb::NSql

namespace NQdb::NKernel {
class TExternalTypingResolver;
} // namespace NQdb::NKernel

namespace NQdb {

// The catalog as one query sees it: the declarations of its modules, parsed in the
// query's type factory.
class TExternalCatalogSnapshot {
public:
    struct TState;

    // nullopt means that the name does not belong to the external catalog.
    // A known name without a matching overload is returned as an error. Signatures
    // already resolved by Qumir annotation take precedence over the exact catalog
    // declaration lookup (e.g. when overload resolution applies a coercion).
    std::expected<std::optional<NQumir::NAst::TTypePtr>, NQumir::TError>
    ResolveReturnType(
        const std::string& name,
        const std::vector<NQumir::NAst::TTypePtr>& argTypes) const;

    bool HasFunction(const std::string& name) const;
    bool HasKumirModules() const;
    std::vector<std::string> ModuleNames() const;
    std::expected<std::monostate, NQumir::TError> RegisterDeclarations(
        NQumir::NFrontend::TSourceModuleLoader& loader) const;

    // Whether `ast` calls a function of an external module; compiles nothing.
    bool ReferencesModules(const NQumir::NAst::TExprPtr& ast) const;

    // Finds external calls in `ast`, lazily compiles only their modules and
    // composes their declarations + LLVM bitcode. nullopt means no external
    // module was referenced. An empty target selects rustc's native target;
    // cacheDir enables the persistent Rust/Cargo cache for cross-compilation.
    std::expected<std::optional<NQumir::NFrontend::TComposeResult>, NQumir::TError>
    ComposeReferenced(
        const NQumir::NAst::TExprPtr& ast,
        const std::string& target = {},
        const std::string& cacheDir = {}) const;

private:
    struct TResolvedTypes;
    using TFunctions = std::map<std::string, NQumir::TIntrusivePtr<NQumir::NAst::TFunDecl>>;
    // A parsed Kumir or oz-lang module; compositions clone it.
    struct TParsedSource {
        NQumir::NAst::TExprPtr Ast;
        std::vector<NQumir::NAst::TPragma> Pragmas;
    };

    TExternalCatalogSnapshot(
        std::shared_ptr<const TState> state,
        std::shared_ptr<NQumir::NAst::TTypeFactory> types);
    void RememberResolvedReturnType(
        const std::string& name,
        const std::vector<NQumir::NAst::TTypePtr>& argTypes,
        NQumir::NAst::TTypePtr returnType) const;

    std::shared_ptr<const TState> State_;
    // The query's factory, declared first: the declarations below are its types.
    std::shared_ptr<NQumir::NAst::TTypeFactory> Types_;
    // By module name, then by function key.
    std::map<std::string, TFunctions> Functions_;
    // By module name; Functions_ points into them.
    std::map<std::string, TParsedSource> Sources_;
    std::shared_ptr<TResolvedTypes> ResolvedTypes_;

    friend class TExternalModuleCatalog;
    friend class NKernel::TExternalTypingResolver;
};

// In-memory, instance-owned catalog. It intentionally has no persistence. It
// outlives queries, so it keeps module sources and declarations as text and each
// query parses them into its own type factory.
class TExternalModuleCatalog {
public:
    TExternalModuleCatalog();

    // `types` is the factory of the query that runs the statement.
    std::expected<std::monostate, NQumir::TError> Apply(
        NQumir::NAst::TTypeFactory& types,
        const NSql::TSqlExternalModule& module);
    std::expected<std::monostate, NQumir::TError> Apply(
        const NSql::TSqlExternalFunction& function);

    // Throws NQumir::TError when a module no longer parses.
    std::shared_ptr<const TExternalCatalogSnapshot> Snapshot(
        std::shared_ptr<NQumir::NAst::TTypeFactory> types) const;

private:
    mutable std::mutex Mutex_;
    std::shared_ptr<TExternalCatalogSnapshot::TState> State_;
};

} // namespace NQdb
