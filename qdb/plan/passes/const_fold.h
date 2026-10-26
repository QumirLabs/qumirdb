#pragma once

#include <qumir/parser/ast.h>

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace NQdb {

class TVmFrontendContext;

// Constant folding for the planning of one query. A closed expression is
// evaluated in the Qumir VM, as the kernels compute it. The folder owns the VM
// frontends it compiles in, each created on its first use, so the frontends
// and their compiler state are never shared between queries.
class TConstFolder {
public:
    TConstFolder();
    ~TConstFolder();

    TConstFolder(const TConstFolder&) = delete;
    TConstFolder& operator=(const TConstFolder&) = delete;

    // `expr` folded to a number literal, or `expr` itself when it is open or
    // cannot be typed or evaluated, so that folding never fails a query.
    NQumir::NAst::TExprPtr Fold(NQumir::NAst::TExprPtr expr);

    // The number a closed expression folds to, or null. The planner estimates
    // a plan several times, so the estimates of a query fold each constant
    // once, by printed expression.
    std::shared_ptr<NQumir::NAst::TNumberExpr> FoldNumber(
        const NQumir::NAst::TExprPtr& expr);

private:
    std::optional<std::string> Eval(
        const NQumir::NAst::TExprPtr& expr,
        const NQumir::NAst::TTypePtr& type);

    // Without source modules: an expression over Qumir's own types compiles
    // here in about 0.05 ms.
    std::unique_ptr<TVmFrontendContext> Builtin_;
    // With the qumirdb module, for an expression with a qdb type, function or
    // operator. Each compilation composes the whole module, about 13 ms. Once
    // the module is loaded, every compilation in a frontend composes it, so
    // the expressions that do not need it compile in the other one.
    std::unique_ptr<TVmFrontendContext> Qumirdb_;
    std::unordered_map<std::string, std::shared_ptr<NQumir::NAst::TNumberExpr>> Numbers_;
};

} // namespace NQdb
