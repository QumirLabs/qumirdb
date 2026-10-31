#include <qdb/kernel/vm_program.h>

#include <qdb/kernel/compiler.h>
#include <qdb/kernel/vm_function.h>
#include <qdb/modules/qumirdb.h>
#include <qdb/utils/module_path.h>

#include <stdexcept>
#include <unordered_set>

namespace NQdb {

namespace {

// Entries are found by source name; a cacheable declaration would take its
// mangled symbol instead (as in TLLVMRunner).
void KeepEntryNames(const NQumir::NAst::TExprPtr& ast, const std::vector<std::string>& entryNames) {
    using namespace NQumir::NAst;
    auto block = TMaybeNode<TBlockExpr>(ast);
    if (!block) {
        return;
    }
    const std::unordered_set<std::string> entries(entryNames.begin(), entryNames.end());
    for (auto& stmt : block.Cast()->Stmts) {
        auto function = TMaybeNode<TFunDecl>(stmt);
        if (function && entries.contains(function.Cast()->Name)) {
            function.Cast()->Cacheable = false;
        }
    }
}

} // namespace

std::shared_ptr<TVmProgram> PrepareVmProgram(
    NQumir::NAst::TExprPtr ast,
    const std::vector<std::string>& entryNames)
{
    // The VM resolves externs with dlsym, so the runtime symbols must be linked.
    NQumir::NRegistry::EnsureQumirDbRuntimeSymbolsLinked();
    EnsureQumirDbUse(ast);
    KeepEntryNames(ast, entryNames);
    TVmFrontendContext frontend({NUtils::ModuleFile("qumirdb.oz")});
    auto program = std::make_shared<TVmProgram>();
    auto lowered = frontend.Lower(std::move(ast), program->Module, /*runDefiniteAssignment=*/false);
    if (!lowered) {
        throw std::runtime_error("VM kernel frontend failed: " + lowered.error());
    }
    std::vector<NQumir::NIR::TFunction*> roots;
    for (const auto& name : entryNames) {
        auto* function = program->Module.GetFunctionByName(name);
        if (!function) {
            throw std::runtime_error("VM kernel entry not found: " + name);
        }
        roots.push_back(function);
        program->Entries.emplace(name, function);
    }
    if (auto prepared = program->Compiler.Prepare(roots); !prepared) {
        throw std::runtime_error("VM kernel preparation failed: " + prepared.error().ToString());
    }
    return program;
}

} // namespace NQdb
