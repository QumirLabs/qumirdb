#include <qdb/kernel/query_modules.h>

#include <qdb/modules/qumirdb.h>
#include <qdb/plan/types/date.h>
#include <qdb/plan/types/decimal.h>
#include <qdb/plan/types/nullable.h>
#include <qdb/utils/module_path.h>

#include <filesystem>

namespace NQdb::NKernel {

namespace {

// QumirDB's own named types, so that queries share the base's objects. They come before
// the modules, so the modules' Nullable[bool] and Nullable[i64] are complete base types.
void AddNamedTypes(NQumir::NAst::TTypeFactory& types) {
    using NQumir::NAst::TIntegerType;
    for (const auto& type : NQumir::NRegistry::QumirDbExternalTypes(types)) {
        NQumir::NRegistry::QumirDbNamedType(types, type.Name);
    }
    BinIntStorageType(types);
    DateCastTarget(types);
    MakeDateType(types, TIntegerType::I32);
    MakeDateType(types, TIntegerType::I64);
    MakeNullableType(types, types.Make<NQumir::NAst::TBoolType>());
    MakeNullableType(types, types.MakeInteger(TIntegerType::I64));
}

} // namespace

TModuleBase::TModuleBase() {
    AddNamedTypes(*Types_);
    for (const auto* name : {"qumirdb.oz", "row_group_interval.oz"}) {
        auto file = NUtils::ModuleFile(name);
        auto& loader = Loaders_.try_emplace(file, *Types_).first->second;
        if (auto registered = loader.RegisterSourceModule(file); !registered) {
            Modules_.emplace(file, std::unexpected(registered.error()));
            continue;
        }
        Modules_.emplace(file, loader.Load(std::filesystem::path(file).stem().string()));
    }
    Types_->Freeze();
}

std::shared_ptr<const NQumir::NAst::TTypeFactory> TModuleBase::Types() const {
    return Types_;
}

std::expected<const NQumir::NFrontend::TSourceModule*, NQumir::TError> TModuleBase::Find(
    const std::string& file) const
{
    return Modules_.at(file);
}

std::shared_ptr<NQumir::NAst::TTypeFactory> MakeQueryTypes(const TModuleBase& base) {
    return std::make_shared<NQumir::NAst::TTypeFactory>(base.Types());
}

TQueryModules::TQueryModules(NQumir::NAst::TTypeFactory& types, std::shared_ptr<const TModuleBase> base)
    : Types_(types)
    , Base_(std::move(base))
{
}

NQumir::NAst::TTypeFactory& TQueryModules::Types() const {
    return Types_;
}

std::expected<std::monostate, NQumir::TError> TQueryModules::Load(
    NQumir::NFrontend::TSourceModuleLoader& loader,
    const std::string& file)
{
    auto module = Base_->Find(file);
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
