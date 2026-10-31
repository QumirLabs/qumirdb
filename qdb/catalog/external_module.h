#pragma once

#include <qumir/error.h>
#include <qumir/frontend/compose.h>
#include <qumir/parser/ast.h>

#include <expected>
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

    explicit TExternalCatalogSnapshot(std::shared_ptr<const TState> state);
    void RememberResolvedReturnType(
        const std::string& name,
        const std::vector<NQumir::NAst::TTypePtr>& argTypes,
        NQumir::NAst::TTypePtr returnType) const;

    std::shared_ptr<const TState> State_;
    std::shared_ptr<TResolvedTypes> ResolvedTypes_;

    friend class TExternalModuleCatalog;
    friend class NKernel::TExternalTypingResolver;
};

// In-memory, instance-owned catalog. It intentionally has no persistence.
class TExternalModuleCatalog {
public:
    TExternalModuleCatalog();

    std::expected<std::monostate, NQumir::TError> Apply(
        const NSql::TSqlExternalModule& module);
    std::expected<std::monostate, NQumir::TError> Apply(
        const NSql::TSqlExternalFunction& function);

    std::shared_ptr<const TExternalCatalogSnapshot> Snapshot() const;

private:
    mutable std::mutex Mutex_;
    std::shared_ptr<TExternalCatalogSnapshot::TState> State_;
};

} // namespace NQdb
