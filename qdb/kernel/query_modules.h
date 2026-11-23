#pragma once

#include <qumir/error.h>
#include <qumir/frontend/source_module_loader.h>
#include <qumir/parser/type_factory.h>

#include <expected>
#include <map>
#include <memory>
#include <string>
#include <variant>

namespace NQdb::NKernel {

// The source modules every query may use, qumirdb.oz and row_group_interval.oz, parsed once
// into a frozen base factory with QumirDB's named types. A process builds one base and passes
// it to its queries. The base and its ASTs are used only from the compiling thread: TExpr
// reference counts are not atomic.
class TModuleBase {
public:
    // Nothing in the base changes after it is built.
    TModuleBase();

    std::shared_ptr<const NQumir::NAst::TTypeFactory> Types() const;

    // The parsed module of `file`, a NUtils::ModuleFile path of one of the two modules.
    std::expected<const NQumir::NFrontend::TSourceModule*, NQumir::TError> Find(const std::string& file) const;

private:
    std::shared_ptr<NQumir::NAst::TTypeFactory> Types_ = std::make_shared<NQumir::NAst::TTypeFactory>();
    std::map<std::string, NQumir::NFrontend::TSourceModuleLoader> Loaders_;
    std::map<std::string, std::expected<const NQumir::NFrontend::TSourceModule*, NQumir::TError>> Modules_;
};

// A query's type factory: a child of `base`, so the base's types are its types.
std::shared_ptr<NQumir::NAst::TTypeFactory> MakeQueryTypes(const TModuleBase& base);

// The source modules of one query. Consumers compose clones of the base's parsed modules
// (cloneSourceModules), so a module's AST never changes. `types` is the query's factory,
// a child of `base`.
class TQueryModules {
public:
    TQueryModules(NQumir::NAst::TTypeFactory& types, std::shared_ptr<const TModuleBase> base);

    NQumir::NAst::TTypeFactory& Types() const;

    // Loads the module of `file`, a NUtils::ModuleFile path of a base module, into `loader`.
    std::expected<std::monostate, NQumir::TError> Load(
        NQumir::NFrontend::TSourceModuleLoader& loader,
        const std::string& file);

private:
    NQumir::NAst::TTypeFactory& Types_;
    std::shared_ptr<const TModuleBase> Base_;
};

} // namespace NQdb::NKernel
