#pragma once

#include <qumir/modules/module.h>
#include <qumir/parser/type_factory.h>

namespace NQumir {
namespace NRegistry {

std::vector<TExternalType> QumirDbExternalTypes(NAst::TTypeFactory& types);

// The named type `name` of qumirdb.oz with its struct. A query compiles in one factory,
// so QumirDB builds these complete: kernel resolution then finds them as they are.
NAst::TTypePtr QumirDbNamedType(NAst::TTypeFactory& types, std::string_view name);
void EnsureQumirDbRuntimeSymbolsLinked();

} // namespace NRegistry
} // namespace NQumir
