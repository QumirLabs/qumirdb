#include <qdb/kernel/execution_context.h>

#include <qdb/kernel/vm_program.h>

#include <qumir/ir/eval.h>

#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace NQdb {

struct TExecutionContext::TInstance {
    explicit TInstance(std::shared_ptr<TVmProgram> program)
        : Program(std::move(program))
        , Interpreter(Program->Module, Program->Compiler, Out, In)
    {}

    std::shared_ptr<TVmProgram> Program;
    // Kernels do no console I/O; the interpreter only needs the streams.
    std::ostringstream Out;
    std::istringstream In;
    NQumir::NIR::TInterpreter Interpreter;
};

TExecutionContext::TExecutionContext() = default;

TExecutionContext::~TExecutionContext() {
    for (auto& instance : Instances_) {
        // Kernel programs have no module destructor, so there is nothing to report.
        (void)instance->Interpreter.Finalize();
    }
}

int64_t TExecutionContext::Invoke(
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

} // namespace NQdb
