#include <qdb/kernel/query_modules.h>

#include <filesystem>

namespace NQdb::NKernel {

TQueryModules::TQueryModules(NQumir::NAst::TTypeFactory& types)
    : Types_(types)
{
}

NQumir::NAst::TTypeFactory& TQueryModules::Types() const {
    return Types_;
}

std::expected<std::monostate, NQumir::TError> TQueryModules::Load(
    NQumir::NFrontend::TSourceModuleLoader& loader,
    const std::string& file)
{
    auto [it, inserted] = Parsed_.try_emplace(file, Types_);
    if (inserted) {
        if (auto registered = it->second.RegisterSourceModule(file); !registered) {
            Parsed_.erase(it);
            return std::unexpected(registered.error());
        }
    }
    auto module = it->second.Load(std::filesystem::path(file).stem().string());
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

} // namespace NQdb::NKernel
