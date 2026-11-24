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
struct TSourceModule;
}

namespace NQumir::NIR {
struct TModule;
}

namespace NQdb {

class TExternalCatalogSnapshot;

namespace NKernel {
class TQueryModules;
}

struct TVmFunctionSpec {
    std::string NamePrefix;
    std::vector<NQumir::NAst::TParam> Params;
    NQumir::TIntrusivePtr<NQumir::NAst::TBlockExpr> Body;
    NQumir::NAst::TTypePtr ReturnType = nullptr;
    std::vector<std::string> Imports;
};

// A frontend in the query's type factory, which must outlive the context. Its source
// modules come parsed from the query's modules, and every composition clones them. Each
// independent transform/lowering gets a fresh resolver so semantic mutations cannot leak
// between compilations. Used only from the compiling thread.
class TVmFrontendContext {
public:
    // Without source modules.
    explicit TVmFrontendContext(NQumir::NAst::TTypeFactory& types);
    // With the modules of `moduleFiles` (NUtils::ModuleFile paths).
    TVmFrontendContext(
        NKernel::TQueryModules& modules,
        const std::vector<std::string>& moduleFiles);
    ~TVmFrontendContext();

    TVmFrontendContext(const TVmFrontendContext&) = delete;
    TVmFrontendContext& operator=(const TVmFrontendContext&) = delete;

    NQumir::NAst::TTypeFactory& Types();

    std::optional<std::string> RegisterExternalDeclarations(
        const TExternalCatalogSnapshot& catalog);

    std::string UniqueName(std::string_view prefix);
    // The loaded modules as parsed, to read only: compositions clone them.
    std::vector<const NQumir::NFrontend::TSourceModule*> SourceModules();
    std::expected<NQumir::NAst::TExprPtr, std::string> Transform(
        NQumir::NAst::TExprPtr module);
    // Lowers engine-generated code, kernels and planning-time functions, without
    // definite-assignment analysis, as KernelRunnerOptions does: on the composed
    // qumirdb module the analysis takes most of a compilation.
    std::expected<void, std::string> Lower(
        NQumir::NAst::TExprPtr module,
        NQumir::NIR::TModule& output,
        // Roots of the reachability cut: the kernel entry names, possibly library functions.
        // nullptr means no cut: every program function is lowered.
        const std::vector<std::string>* entries);

private:
    struct TImpl;
    std::unique_ptr<TImpl> Impl_;
};

// Reusable AST -> compose -> resolve -> IR -> interpreter pipeline for small
// planning-time functions. The compiled module and interpreter stay alive so
// the same function can be evaluated repeatedly with different arguments.
// The bytecode is prepared at compilation and calls touch no global state, so
// separate functions may run on separate threads.
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

    std::optional<std::string> Eval(std::vector<int64_t> args) noexcept;
    std::optional<int64_t> EvalRaw(std::vector<int64_t> args) noexcept;

private:
    struct TImpl;
    explicit TVmFunction(std::unique_ptr<TImpl> impl);

    std::unique_ptr<TImpl> Impl_;
};

} // namespace NQdb
