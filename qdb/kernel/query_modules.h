#pragma once

#include <qumir/error.h>
#include <qumir/frontend/source_module_loader.h>
#include <qumir/parser/type_factory.h>

#include <expected>
#include <map>
#include <string>
#include <variant>

namespace NQdb::NKernel {

// The source modules of one query, such as qumirdb.oz, each parsed in the query's factory
// once, when first loaded. Consumers compose clones of a module (cloneSourceModules), so its
// AST never changes. Cloning touches its non-atomic reference counts: one thread at a time.
class TQueryModules {
public:
    explicit TQueryModules(NQumir::NAst::TTypeFactory& types);

    NQumir::NAst::TTypeFactory& Types() const;

    // Loads the module of `file`, a NUtils::ModuleFile path, into `loader`.
    std::expected<std::monostate, NQumir::TError> Load(
        NQumir::NFrontend::TSourceModuleLoader& loader,
        const std::string& file);

private:
    NQumir::NAst::TTypeFactory& Types_;
    // One loader per file: a consumer composes only the modules it loads.
    std::map<std::string, NQumir::NFrontend::TSourceModuleLoader> Parsed_;
};

} // namespace NQdb::NKernel
