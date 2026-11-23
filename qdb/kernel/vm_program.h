#pragma once

#include <qumir/ir/builder.h>
#include <qumir/ir/vmcompiler.h>
#include <qumir/parser/ast.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace NQdb {

namespace NKernel {
class TQueryModules;
} // namespace NKernel

// The bytecode of one query's kernels. Prepared once; after that every
// thread's interpreter only reads it.
struct TVmProgram {
    NQumir::NIR::TModule Module;
    NQumir::NIR::TVMCompiler Compiler{Module};
    std::unordered_map<std::string, NQumir::NIR::TFunction*> Entries;
};

// Lowers a kernel AST, built in the factory of the query's `modules`, with its qumirdb.oz and
// the frontend settings of KernelRunnerOptions, and prepares the bytecode of the entries
// and all they call. Throws on any frontend or preparation error.
std::shared_ptr<TVmProgram> PrepareVmProgram(
    NKernel::TQueryModules& modules,
    NQumir::NAst::TExprPtr ast,
    const std::vector<std::string>& entryNames);

} // namespace NQdb
