#include <qdb/plan/passes/unbound_vars.h>

namespace NQdb {

namespace {

using namespace NQumir::NAst;

void Collect(
    const TExprPtr& expr,
    const std::unordered_set<std::string>& bound,
    std::unordered_set<std::string>& out)
{
    if (!expr) {
        return;
    }

    if (auto node = TMaybeNode<TIdentExpr>(expr)) {
        const auto& name = node.Cast()->Name;
        if (!bound.count(name)) {
            out.insert(name);
        }
        return;
    }

    // A call's callee is a function name, not a column reference.
    if (auto node = TMaybeNode<TCallExpr>(expr)) {
        for (const auto& arg : node.Cast()->Args) {
            Collect(arg, bound, out);
        }
        return;
    }

    if (auto node = TMaybeNode<TFunDecl>(expr)) {
        auto fun = node.Cast();
        auto innerBound = bound;
        for (const auto& p : fun->Params) {
            innerBound.insert(p->Name);
        }
        Collect(fun->Body, innerBound, out);
        return;
    }

    if (auto node = TMaybeNode<TForStmtExpr>(expr)) {
        auto loop = node.Cast();
        Collect(loop->From, bound, out);
        Collect(loop->To, bound, out);
        Collect(loop->Step, bound, out);
        auto innerBound = bound;
        innerBound.insert(loop->VarName);
        Collect(loop->Body, innerBound, out);
        return;
    }

    // TBlockExpr and TVarsBlockExpr: process statements sequentially so that
    // var declarations bind subsequent statements but not preceding ones.
    if (auto node = TMaybeNode<TBlockExpr>(expr)) {
        auto innerBound = bound;
        for (const auto& stmt : node.Cast()->Stmts) {
            if (auto var = TMaybeNode<TVarStmt>(stmt)) {
                // The initializer is evaluated before the name is in scope.
                if (var.Cast()->Init) Collect(var.Cast()->Init, innerBound, out);
                innerBound.insert(var.Cast()->Name);
            } else {
                Collect(stmt, innerBound, out);
            }
        }
        return;
    }

    if (auto node = TMaybeNode<TVarsBlockExpr>(expr)) {
        auto innerBound = bound;
        for (const auto& v : node.Cast()->Vars) {
            if (auto var = TMaybeNode<TVarStmt>(v)) {
                if (var.Cast()->Init) Collect(var.Cast()->Init, innerBound, out);
                innerBound.insert(var.Cast()->Name);
            } else {
                Collect(v, innerBound, out);
            }
        }
        return;
    }

    // TVarStmt itself doesn't introduce references - it's a declaration.
    if (TMaybeNode<TVarStmt>(expr)) {
        return;
    }

    for (const auto& child : expr->Children()) {
        Collect(child, bound, out);
    }
}

} // namespace

std::unordered_set<std::string> FindUnboundVars(const TExprPtr& expr) {
    std::unordered_set<std::string> result;
    Collect(expr, {}, result);
    return result;
}

} // namespace NQdb
