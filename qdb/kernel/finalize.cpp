#include <qdb/kernel/finalize.h>

#include <qdb/catalog/external_module.h>
#include <qdb/kernel/compiler.h>
#include <qdb/kernel/finalize_fused.h>

#include <memory>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace NQdb {

namespace {

// Kept alive as long as any bound slot lives; owns the fused AST + payloads.
struct TFusedKernelStorage {
    NQumir::NAst::TExprPtr Program;
    std::vector<std::shared_ptr<void>> KernelStorage;
    // Native external runtimes must remain loaded while any generated entry
    // point can still be called.
    std::shared_ptr<const TExternalCatalogSnapshot> ExternalCatalog;
};

} // namespace

void JitFinalizeKernels(
    std::span<TGeneratedKernel> kernels,
    std::ostream* diagnostics,
    std::shared_ptr<const TExternalCatalogSnapshot> externalCatalog,
    const std::string& cacheDir)
{
    std::vector<TGeneratedKernel*> unique;
    std::vector<std::pair<TGeneratedKernel*, size_t>> bindings;
    std::unordered_map<std::string, size_t> uniqueByKey;
    for (auto& kernel : kernels) {
        if (!kernel.Slot) {
            throw std::logic_error(
                "JitFinalizeKernels: kernel '" + kernel.Name + "' has no slot");
        }
        if (kernel.Slot->Runner) {
            continue; // already bound
        }

        std::string key = MakeKernelDedupKey(kernel);
        auto [it, inserted] = uniqueByKey.emplace(std::move(key), unique.size());
        if (inserted) {
            unique.push_back(&kernel);
        }
        bindings.emplace_back(&kernel, it->second);
    }

    if (unique.empty()) {
        return;
    }

    auto fused = BuildFusedProgram(unique);

    if (diagnostics) {
        *diagnostics << "\n========== RUNTIME FUSED FINALIZE ==========\n"
                     << "[kernel-fusion] kernels: " << bindings.size()
                     << ", unique kernels: " << unique.size()
                     << ", type decls: " << fused.TypeDeclCount
                     << ", function decls: " << fused.FunctionDeclCount
                     << ", entrypoints: " << fused.Entrypoints.size()
                     << (cacheDir.empty() ? "" : ", cache: on")
                     << "\n";
    }

    auto options = KernelRunnerOptions();
    options.CoreInput = !(externalCatalog && externalCatalog->HasKumirModules());
    options.NativeCode = true;
    options.EnablePerfJitEventListener = true;
    options.PrintIr = diagnostics != nullptr;
    options.PrintLlvm = diagnostics != nullptr;
    auto runner = std::make_shared<NQumir::TLLVMRunner>(std::move(options));

    auto storage = std::make_shared<TFusedKernelStorage>();
    storage->ExternalCatalog = externalCatalog;
    storage->KernelStorage.reserve(bindings.size());
    for (const auto& [kernel, uniqueIndex] : bindings) {
        (void)uniqueIndex;
        if (kernel->Storage) {
            storage->KernelStorage.push_back(kernel->Storage);
        }
    }
    storage->Program = std::move(fused.Program);

    std::string error;
    std::unordered_map<std::string, void*> fns;
    std::shared_ptr<void> jitLifetime;
    std::optional<NQumir::NFrontend::TComposeResult> externalProgram;
    if (externalCatalog) {
        auto composed = externalCatalog->ComposeReferenced(storage->Program);
        if (!composed) {
            throw std::runtime_error(composed.error().ToString());
        }
        externalProgram = std::move(*composed);
    }
    if (externalProgram) {
        if (diagnostics && !cacheDir.empty()) {
            *diagnostics << "[external-modules] persistent JIT cache bypassed\n";
        }
        fns = CompileKernelAst(
            *runner, std::move(*externalProgram), fused.Entrypoints, &error);
    } else if (!cacheDir.empty()) {
        auto linked = CompileKernelAstCached(
            *runner, storage->Program, fused.Entrypoints, cacheDir, &error);
        fns = std::move(linked.Entries);
        jitLifetime = std::move(linked.Lifetime);
    } else {
        fns = CompileKernelAst(*runner, storage->Program, fused.Entrypoints, &error);
    }
    if (diagnostics) {
        *diagnostics << "========== END RUNTIME FUSED FINALIZE ==========\n";
    }

    std::vector<std::vector<void*>> uniqueFns(unique.size());
    for (size_t u = 0; u < unique.size(); ++u) {
        const auto& fusedEntrypoints = fused.UniqueEntrypoints[u];
        uniqueFns[u].resize(fusedEntrypoints.size(), nullptr);
        for (size_t i = 0; i < fusedEntrypoints.size(); ++i) {
            auto it = fns.find(fusedEntrypoints[i]);
            if (it == fns.end() || !it->second) {
                throw std::runtime_error(
                    "kernel '" + unique[u]->Name + "' entry '" +
                    unique[u]->Entrypoints[i] + "' compilation failed: " + error);
            }
            uniqueFns[u][i] = it->second;
        }
    }

    for (const auto& [kernel, uniqueIndex] : bindings) {
        kernel->Slot->Fns = uniqueFns[uniqueIndex];
        kernel->Slot->Runner = runner;
        kernel->Slot->JitLifetime = jitLifetime;
        kernel->Storage = storage;
    }
}

} // namespace NQdb
