#include <qdb/kernel/vm_program.h>

#include <qdb/kernel/compiler.h>
#include <qdb/kernel/execution_context.h>
#include <qdb/kernel/vm_function.h>
#include <qdb/modules/qumirdb.h>
#include <qdb/utils/module_path.h>

#include <qumir/ir/eval.h>

#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <utility>

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

// Prepared bytecode is shared; registers, stack and interpreter module state
// are local to one worker. Kernels have no module constructor or destructor.
class TVmExecutionState {
public:
    ~TVmExecutionState() {
        for (auto& instance : Instances_) {
            (void)instance->Interpreter.Finalize();
        }
    }

    int64_t Invoke(
        const std::shared_ptr<TVmProgram>& program,
        NQumir::NIR::TFunction& function,
        std::vector<int64_t> args)
    {
        auto it = std::ranges::find_if(Instances_, [&](const auto& instance) {
            return instance->Program == program;
        });
        if (it == Instances_.end()) {
            auto instance = std::make_unique<TInstance>(program);
            if (auto initialized = instance->Interpreter.Initialize(); !initialized) {
                throw std::runtime_error(
                    "VM instance initialization failed: " + initialized.error().ToString());
            }
            it = Instances_.insert(Instances_.end(), std::move(instance));
        }
        auto result = (*it)->Interpreter.Invoke(function, std::move(args));
        if (!result) {
            throw std::runtime_error("VM kernel call failed: " + result.error().ToString());
        }
        return *result;
    }

private:
    struct TInstance {
        explicit TInstance(std::shared_ptr<TVmProgram> program)
            : Program(std::move(program))
            , Interpreter(Program->Module, Program->Compiler, Out, In)
        {}

        std::shared_ptr<TVmProgram> Program;
        std::ostringstream Out;
        std::istringstream In;
        NQumir::NIR::TInterpreter Interpreter;
    };

    std::vector<std::unique_ptr<TInstance>> Instances_;
};

int64_t TExecutionContext::Invoke(
    const std::shared_ptr<TVmProgram>& program,
    NQumir::NIR::TFunction& function,
    std::vector<int64_t> args)
{
    if (!Vm) {
        Vm = std::make_shared<TVmExecutionState>();
    }
    return Vm->Invoke(program, function, std::move(args));
}

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
