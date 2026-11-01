#include <qdb/kernel/vm_function.h>

#include <qdb/catalog/external_module.h>
#include <qdb/utils/module_path.h>

#include <qumir/frontend/compose.h>
#include <qumir/frontend/source_module_loader.h>
#include <qumir/ir/builder.h>
#include <qumir/ir/eval.h>
#include <qumir/ir/lowering/lower_ast.h>
#include <qumir/ir/vmcompiler.h>
#include <qumir/modules/builtins/builtins.h>
#include <qumir/modules/system/system.h>
#include <qumir/runtime/string.h>
#include <qumir/semantics/name_resolution/name_resolver.h>
#include <qumir/semantics/transform/transform.h>

#include <bit>
#include <exception>
#include <mutex>
#include <sstream>
#include <utility>

namespace NQdb {

using namespace NQumir::NAst;

struct TVmFrontendContext::TImpl {
    const std::vector<TPragma> Pragmas{
        TPragma{"language", {"overloads"}, {}},
    };
    std::shared_ptr<NQumir::NRegistry::SystemModule> System =
        std::make_shared<NQumir::NRegistry::SystemModule>();
    // qumirdb.oz kernels call builtin::memcmp/cttz.
    std::shared_ptr<NQumir::NRegistry::BuiltinsModule> Builtins =
        std::make_shared<NQumir::NRegistry::BuiltinsModule>();
    NQumir::NFrontend::TSourceModuleLoader Loader;
    std::optional<std::string> LoadError;
    uint64_t Counter = 0;
    std::mutex Mutex;
};

TVmFrontendContext::TVmFrontendContext(
    std::vector<std::string> moduleFiles)
    : Impl_(std::make_unique<TImpl>())
{
    for (const auto& path : moduleFiles) {
        if (auto error = RegisterSourceModule(path)) {
            Impl_->LoadError = std::move(*error);
            break;
        }
    }
}

TVmFrontendContext::~TVmFrontendContext() = default;

std::optional<std::string> TVmFrontendContext::RegisterSourceModule(
    const std::string& path)
{
    std::lock_guard lock(Impl_->Mutex);
    auto registered = Impl_->Loader.RegisterSourceModule(path);
    if (!registered) {
        return registered.error().ToString();
    }
    return std::nullopt;
}

std::optional<std::string> TVmFrontendContext::RegisterExternalDeclarations(
    const TExternalCatalogSnapshot& catalog)
{
    std::lock_guard lock(Impl_->Mutex);
    auto registered = catalog.RegisterDeclarations(Impl_->Loader);
    if (!registered) {
        return registered.error().ToString();
    }
    return std::nullopt;
}

std::string TVmFrontendContext::UniqueName(std::string_view prefix) {
    std::lock_guard lock(Impl_->Mutex);
    return std::string(prefix) + std::to_string(Impl_->Counter++) + "__";
}

std::expected<TExprPtr, std::string> TVmFrontendContext::Compose(
    TExprPtr module)
{
    std::lock_guard lock(Impl_->Mutex);
    if (Impl_->LoadError) {
        return std::unexpected(*Impl_->LoadError);
    }
    auto composed = NQumir::NFrontend::LoadAndCompose(
        Impl_->Loader, module, Impl_->Pragmas, /*cloneSourceModules*/ true);
    if (!composed) {
        return std::unexpected(composed.error().ToString());
    }
    return std::move(composed->Ast);
}

namespace {

std::expected<TExprPtr, std::string> TransformWithResolver(
    NQumir::NFrontend::TSourceModuleLoader& loader,
    const std::vector<TPragma>& pragmas,
    NQumir::NSemantics::TNameResolver& resolver,
    TExprPtr module,
    bool runDefiniteAssignment = true)
{
    auto composed = NQumir::NFrontend::LoadAndCompose(
        loader, module, pragmas, /*cloneSourceModules*/ true);
    if (!composed) {
        return std::unexpected(composed.error().ToString());
    }
    resolver.ApplyPragmas(composed->Pragmas);
    resolver.GetOrCreateRootScope()->RootLevel = false;
    if (auto error = resolver.Resolve(composed->Ast)) {
        return std::unexpected(error->ToString());
    }
    if (auto transformed = NQumir::NTransform::Pipeline(
            composed->Ast, resolver, {.RunDefiniteAssignment = runDefiniteAssignment});
        !transformed)
    {
        return std::unexpected(transformed.error().ToString());
    }
    return std::move(composed->Ast);
}

} // namespace

std::expected<TExprPtr, std::string> TVmFrontendContext::TransformLocked(
    TExprPtr module)
{
    if (Impl_->LoadError) {
        return std::unexpected(*Impl_->LoadError);
    }
    NQumir::NSemantics::TNameResolver resolver;
    resolver.ApplyPragmas(Impl_->Pragmas);
    resolver.RegisterModule(Impl_->System.get());
    resolver.RegisterModule(Impl_->Builtins.get());
    (void)resolver.ImportModule(Impl_->Builtins->Name());
    return TransformWithResolver(
        Impl_->Loader, Impl_->Pragmas, resolver, std::move(module));
}

std::expected<TExprPtr, std::string> TVmFrontendContext::Transform(
    TExprPtr module)
{
    std::lock_guard lock(Impl_->Mutex);
    return TransformLocked(std::move(module));
}

std::expected<void, std::string> TVmFrontendContext::Lower(
    TExprPtr module,
    NQumir::NIR::TModule& output,
    bool runDefiniteAssignment)
{
    std::lock_guard lock(Impl_->Mutex);
    if (Impl_->LoadError) {
        return std::unexpected(*Impl_->LoadError);
    }
    // Lowered modules are independent. A fresh resolver avoids retaining
    // transformed return-normalization state between compilations, while the
    // shared loader still prevents reparsing source modules.
    NQumir::NSemantics::TNameResolver resolver;
    resolver.ApplyPragmas(Impl_->Pragmas);
    resolver.RegisterModule(Impl_->System.get());
    resolver.RegisterModule(Impl_->Builtins.get());
    (void)resolver.ImportModule(Impl_->Builtins->Name());
    auto transformed = TransformWithResolver(
        Impl_->Loader, Impl_->Pragmas, resolver, std::move(module), runDefiniteAssignment);
    if (!transformed) {
        return std::unexpected(transformed.error());
    }
    NQumir::NIR::TBuilder builder(output);
    NQumir::NIR::TAstLowerer lowerer(output, builder, resolver);
    if (auto lowered = lowerer.LowerTop(*transformed); !lowered) {
        return std::unexpected(lowered.error().ToString());
    }
    return {};
}

TVmFrontendContext& QumirdbVmContext() {
    static TVmFrontendContext context({
        NUtils::ModuleFile("qumirdb.oz"),
    });
    return context;
}

struct TVmFunction::TImpl {
    NQumir::NIR::TModule Module;
    NQumir::NIR::TVMCompiler Compiler{Module};
    NQumir::NIR::TFunction* Entry = nullptr;
    std::ostringstream Out;
    std::istringstream In;
    std::unique_ptr<NQumir::NIR::TInterpreter> Interpreter;
};

TVmFunction::TVmFunction(std::unique_ptr<TImpl> impl)
    : Impl_(std::move(impl))
{}

TVmFunction::~TVmFunction() {
    if (Impl_ && Impl_->Interpreter) {
        (void)Impl_->Interpreter->Finalize();
    }
}

std::expected<std::unique_ptr<TVmFunction>, std::string> TVmFunction::Compile(
    TVmFrontendContext& context,
    TVmFunctionSpec spec)
{
    try {
        const std::string functionName =
            context.UniqueName(spec.NamePrefix);
        NQumir::TLocation loc{};
        auto fn = std::make_shared<TFunDecl>(
            loc,
            functionName,
            std::vector<TGenericParam>{},
            std::move(spec.Params),
            std::move(spec.Body),
            std::move(spec.ReturnType));
        std::vector<TExprPtr> declarations;
        declarations.reserve(spec.Imports.size() + 1);
        for (auto& name : spec.Imports) {
            declarations.push_back(
                std::make_shared<TUseExpr>(loc, std::move(name)));
        }
        declarations.push_back(std::move(fn));
        TExprPtr chunk = std::make_shared<TBlockExpr>(
            loc, std::move(declarations));

        auto impl = std::make_unique<TImpl>();
        if (auto lowered = context.Lower(std::move(chunk), impl->Module);
            !lowered)
        {
            return std::unexpected(lowered.error());
        }
        for (auto& function : impl->Module.Functions) {
            if (function.Name == functionName) {
                impl->Entry = &function;
                break;
            }
        }
        if (!impl->Entry) {
            return std::unexpected(
                "compiled module has no function " + functionName);
        }
        if (auto prepared = impl->Compiler.Prepare({impl->Entry}); !prepared) {
            return std::unexpected(prepared.error().ToString());
        }
        impl->Interpreter = std::make_unique<NQumir::NIR::TInterpreter>(
            impl->Module,
            impl->Compiler,
            impl->Out,
            impl->In);
        if (auto initialized = impl->Interpreter->Initialize(); !initialized) {
            return std::unexpected(initialized.error().ToString());
        }
        return std::unique_ptr<TVmFunction>(
            new TVmFunction(std::move(impl)));
    } catch (const std::exception& error) {
        return std::unexpected(error.what());
    } catch (...) {
        return std::unexpected("unknown VM compilation error");
    }
}

std::optional<std::string> TVmFunction::Eval(
    std::vector<int64_t> args) noexcept
{
    auto raw = EvalRaw(std::move(args));
    if (!raw) {
        return std::nullopt;
    }
    // Formats the result the way TInterpreter::Eval does.
    try {
        const auto& entry = *Impl_->Entry;
        std::ostringstream out;
        if (entry.ReturnTypeIsString) {
            auto* text = reinterpret_cast<char*>(*raw);
            out << text;
            NQumir::NRuntime::str_release(text);
        } else if (entry.ReturnTypeId >= 0) {
            Impl_->Module.Types.Format(out, std::bit_cast<uint64_t>(*raw), entry.ReturnTypeId);
        } else {
            out << *raw;
        }
        return out.str();
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<int64_t> TVmFunction::EvalRaw(
    std::vector<int64_t> args) noexcept
{
    if (!Impl_ || !Impl_->Entry || !Impl_->Interpreter) {
        return std::nullopt;
    }
    try {
        auto result = Impl_->Interpreter->Invoke(*Impl_->Entry, std::move(args));
        if (!result) {
            return std::nullopt;
        }
        return *result;
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace NQdb
