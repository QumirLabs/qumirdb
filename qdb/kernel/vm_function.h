#pragma once

#include <qumir/parser/ast.h>

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace NQumir::NFrontend {
class TSourceModuleLoader;
}

namespace NQumir::NIR {
struct TModule;
}

namespace NQdb {

class TExternalCatalogSnapshot;

struct TVmFunctionSpec {
    std::string NamePrefix;
    std::vector<NQumir::NAst::TParam> Params;
    std::shared_ptr<NQumir::NAst::TBlockExpr> Body;
    NQumir::NAst::TTypePtr ReturnType;
    std::vector<std::string> Imports;
};

// Owns the expensive, reusable frontend state. Source modules are registered
// and parsed once; each independent transform/lowering gets a fresh resolver so
// semantic mutations cannot leak between compilations. Loader access is
// serialized because its parsed-module cache is stateful.
class TVmFrontendContext {
public:
    explicit TVmFrontendContext(std::vector<std::string> moduleFiles = {});
    ~TVmFrontendContext();

    TVmFrontendContext(const TVmFrontendContext&) = delete;
    TVmFrontendContext& operator=(const TVmFrontendContext&) = delete;

    std::optional<std::string> RegisterSourceModule(const std::string& path);
    std::optional<std::string> RegisterExternalDeclarations(
        const TExternalCatalogSnapshot& catalog);

    std::string UniqueName(std::string_view prefix);
    std::expected<NQumir::NAst::TExprPtr, std::string> Compose(
        NQumir::NAst::TExprPtr module);
    std::expected<NQumir::NAst::TExprPtr, std::string> Transform(
        NQumir::NAst::TExprPtr module);
    // Kernels skip definite-assignment analysis, as KernelRunnerOptions does.
    std::expected<void, std::string> Lower(
        NQumir::NAst::TExprPtr module,
        NQumir::NIR::TModule& output,
        bool runDefiniteAssignment = true);

private:
    struct TImpl;
    std::expected<NQumir::NAst::TExprPtr, std::string> TransformLocked(
        NQumir::NAst::TExprPtr module);
    std::unique_ptr<TImpl> Impl_;
};

// Shared default context used by expression annotation.
TVmFrontendContext& QumirdbVmContext();

// Reusable AST -> compose -> resolve -> IR -> bytecode pipeline for small
// functions. The bytecode is prepared at compilation and only read after it;
// each caller evaluates it in a TVmFunctionState of its own, so the states of
// one function may run on separate threads.
class TVmFunction {
public:
    static std::expected<std::unique_ptr<TVmFunction>, std::string> Compile(
        TVmFrontendContext& context,
        TVmFunctionSpec spec);

    ~TVmFunction();

    TVmFunction(const TVmFunction&) = delete;
    TVmFunction& operator=(const TVmFunction&) = delete;
    TVmFunction(TVmFunction&&) = delete;
    TVmFunction& operator=(TVmFunction&&) = delete;

    // One call in a fresh state, for planning-time evaluation.
    std::optional<std::string> Eval(std::vector<int64_t> args) const noexcept;
    std::optional<int64_t> EvalRaw(std::vector<int64_t> args) const noexcept;

private:
    friend class TVmFunctionState;
    struct TImpl;
    explicit TVmFunction(std::unique_ptr<TImpl> impl);

    std::unique_ptr<TImpl> Impl_;
};

// The interpreter of one caller of a TVmFunction: registers, stack and module
// globals. The function must outlive it, and one thread at a time uses it.
class TVmFunctionState {
public:
    explicit TVmFunctionState(const TVmFunction& function);
    ~TVmFunctionState();

    TVmFunctionState(const TVmFunctionState&) = delete;
    TVmFunctionState& operator=(const TVmFunctionState&) = delete;
    TVmFunctionState(TVmFunctionState&&) = delete;
    TVmFunctionState& operator=(TVmFunctionState&&) = delete;

    std::optional<int64_t> EvalRaw(std::vector<int64_t> args) noexcept;

private:
    struct TImpl;
    std::unique_ptr<TImpl> Impl_;
};

} // namespace NQdb
