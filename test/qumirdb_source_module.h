#pragma once

#include <qdb/utils/module_path.h>

#include <qumir/parser/ast.h>
#include <qumir/runner/runner_llvm.h>

#include <string>

namespace NQdb::NTest {

inline void ConfigureQumirDbSourceModule(NQumir::TLLVMRunnerOptions& options) {
    options.ModuleFiles = {NUtils::ModuleFile("qumirdb.oz")};
}

inline void AddQumirDbUse(const NQumir::NAst::TExprPtr& ast) {
    using namespace NQumir::NAst;
    auto block = TMaybeNode<TBlockExpr>(ast);
    if (!block) {
        return;
    }
    for (const auto& stmt : block.Cast()->Stmts) {
        auto use = TMaybeNode<TUseExpr>(stmt);
        if (use && use.Cast()->ModuleName == "qumirdb") {
            return;
        }
    }
    block.Cast()->Stmts.insert(
        block.Cast()->Stmts.begin(),
        NQumir::MakeIntrusive<TUseExpr>(NQumir::TLocation{}, "qumirdb"));
}

} // namespace NQdb::NTest
