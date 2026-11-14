#include <qdb/plan/clone_expr.h>

#include <qumir/error.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace NQdb {

using namespace NQumir::NAst;

TExprPtr CloneExpr(const TExprPtr& expr) {
    TExprPtr result;
    if (auto ident = TMaybeNode<TIdentExpr>(expr)) {
        result = std::make_shared<TIdentExpr>(
            expr->Location, ident.Cast()->Name);
    } else if (auto literal = TMaybeNode<TStringLiteralExpr>(expr)) {
        result = std::make_shared<TStringLiteralExpr>(
            expr->Location, literal.Cast()->Value);
    } else if (auto number = TMaybeNode<TNumberExpr>(expr)) {
        result = number.Cast()->IsFloat()
            ? std::static_pointer_cast<TExpr>(std::make_shared<TNumberExpr>(
                expr->Location, number.Cast()->FloatValue))
            : std::static_pointer_cast<TExpr>(std::make_shared<TNumberExpr>(
                expr->Location, number.Cast()->IntValue));
    } else if (auto unary = TMaybeNode<TUnaryExpr>(expr)) {
        result = std::make_shared<TUnaryExpr>(expr->Location,
            unary.Cast()->Operator, CloneExpr(unary.Cast()->Operand));
    } else if (auto binary = TMaybeNode<TBinaryExpr>(expr)) {
        result = std::make_shared<TBinaryExpr>(expr->Location,
            binary.Cast()->Operator, CloneExpr(binary.Cast()->Left),
            CloneExpr(binary.Cast()->Right));
    } else if (auto call = TMaybeNode<TCallExpr>(expr)) {
        std::vector<TExprPtr> args;
        args.reserve(call.Cast()->Args.size());
        for (const auto& arg : call.Cast()->Args) {
            args.push_back(CloneExpr(arg));
        }
        result = std::make_shared<TCallExpr>(expr->Location,
            CloneExpr(call.Cast()->Callee), std::move(args));
    } else if (auto cast = TMaybeNode<TCastExpr>(expr)) {
        result = std::make_shared<TCastExpr>(expr->Location,
            CloneExpr(cast.Cast()->Operand), expr->Type);
    } else if (auto bitcast = TMaybeNode<TBitcastExpr>(expr)) {
        result = std::make_shared<TBitcastExpr>(expr->Location,
            CloneExpr(bitcast.Cast()->Operand), expr->Type);
    } else if (auto ifExpr = TMaybeNode<TIfExpr>(expr)) {
        result = std::make_shared<TIfExpr>(expr->Location,
            CloneExpr(ifExpr.Cast()->Cond),
            CloneExpr(ifExpr.Cast()->Then),
            ifExpr.Cast()->Else ? CloneExpr(ifExpr.Cast()->Else) : nullptr);
    } else if (auto block = TMaybeNode<TBlockExpr>(expr)) {
        std::vector<TExprPtr> stmts;
        stmts.reserve(block.Cast()->Stmts.size());
        for (const auto& s : block.Cast()->Stmts) stmts.push_back(CloneExpr(s));
        result = std::make_shared<TBlockExpr>(expr->Location, std::move(stmts));
    } else if (auto var = TMaybeNode<TVarStmt>(expr)) {
        std::vector<std::pair<TExprPtr, TExprPtr>> bounds;
        bounds.reserve(var.Cast()->Bounds.size());
        for (const auto& [lo, hi] : var.Cast()->Bounds)
            bounds.emplace_back(CloneExpr(lo), CloneExpr(hi));
        auto v = std::make_shared<TVarStmt>(expr->Location,
            var.Cast()->Name, expr->Type, std::move(bounds));
        if (var.Cast()->Init) v->Init = CloneExpr(var.Cast()->Init);
        result = std::move(v);
    } else if (auto assign = TMaybeNode<TAssignExpr>(expr)) {
        result = std::make_shared<TAssignExpr>(expr->Location,
            assign.Cast()->Name, CloneExpr(assign.Cast()->Value));
    } else if (auto aarr = TMaybeNode<TArrayAssignExpr>(expr)) {
        std::vector<TExprPtr> idxs;
        idxs.reserve(aarr.Cast()->Indices.size());
        for (const auto& i : aarr.Cast()->Indices) idxs.push_back(CloneExpr(i));
        result = std::make_shared<TArrayAssignExpr>(expr->Location,
            aarr.Cast()->Name, std::move(idxs), CloneExpr(aarr.Cast()->Value));
    } else if (auto ret = TMaybeNode<TReturnExpr>(expr)) {
        result = std::make_shared<TReturnExpr>(expr->Location,
            ret.Cast()->Value ? CloneExpr(ret.Cast()->Value) : nullptr);
    } else if (TMaybeNode<TBreakStmt>(expr)) {
        result = std::make_shared<TBreakStmt>(expr->Location);
    } else if (TMaybeNode<TContinueStmt>(expr)) {
        result = std::make_shared<TContinueStmt>(expr->Location);
    } else if (auto idx = TMaybeNode<TIndexExpr>(expr)) {
        result = std::make_shared<TIndexExpr>(expr->Location,
            CloneExpr(idx.Cast()->Collection), CloneExpr(idx.Cast()->Index));
    } else if (auto midx = TMaybeNode<TMultiIndexExpr>(expr)) {
        std::vector<TExprPtr> indices;
        indices.reserve(midx.Cast()->Indices.size());
        for (const auto& i : midx.Cast()->Indices) indices.push_back(CloneExpr(i));
        result = std::make_shared<TMultiIndexExpr>(expr->Location,
            CloneExpr(midx.Cast()->Collection), std::move(indices));
    } else if (auto slice = TMaybeNode<TSliceExpr>(expr)) {
        result = std::make_shared<TSliceExpr>(expr->Location,
            CloneExpr(slice.Cast()->Collection),
            CloneExpr(slice.Cast()->Start),
            CloneExpr(slice.Cast()->End));
    } else if (auto fa = TMaybeNode<TFieldAccessExpr>(expr)) {
        result = std::make_shared<TFieldAccessExpr>(expr->Location,
            CloneExpr(fa.Cast()->Object), fa.Cast()->FieldName);
    } else if (auto fassign = TMaybeNode<TFieldAssignExpr>(expr)) {
        result = std::make_shared<TFieldAssignExpr>(expr->Location,
            CloneExpr(fassign.Cast()->Object), fassign.Cast()->FieldName,
            CloneExpr(fassign.Cast()->Value));
    } else if (auto sc = TMaybeNode<TStructConstructExpr>(expr)) {
        std::vector<TExprPtr> fields;
        fields.reserve(sc.Cast()->Fields.size());
        for (const auto& f : sc.Cast()->Fields) fields.push_back(CloneExpr(f));
        result = std::make_shared<TStructConstructExpr>(expr->Location,
            expr->Type, std::move(fields));
    } else if (auto wh = TMaybeNode<TWhileStmtExpr>(expr)) {
        result = std::make_shared<TWhileStmtExpr>(expr->Location,
            CloneExpr(wh.Cast()->Cond), CloneExpr(wh.Cast()->Body));
    } else if (auto rep = TMaybeNode<TRepeatStmtExpr>(expr)) {
        result = std::make_shared<TRepeatStmtExpr>(expr->Location,
            CloneExpr(rep.Cast()->Body), CloneExpr(rep.Cast()->Cond));
    } else if (auto forst = TMaybeNode<TForStmtExpr>(expr)) {
        result = std::make_shared<TForStmtExpr>(expr->Location,
            forst.Cast()->VarName,
            CloneExpr(forst.Cast()->From), CloneExpr(forst.Cast()->To),
            CloneExpr(forst.Cast()->Step), CloneExpr(forst.Cast()->Body));
    } else if (auto times = TMaybeNode<TTimesStmtExpr>(expr)) {
        result = std::make_shared<TTimesStmtExpr>(expr->Location,
            CloneExpr(times.Cast()->Count), CloneExpr(times.Cast()->Body));
    } else if (auto await = TMaybeNode<TAwaitExpr>(expr)) {
        result = std::make_shared<TAwaitExpr>(expr->Location,
            CloneExpr(await.Cast()->Operand));
    } else if (auto asrt = TMaybeNode<TAssertStmt>(expr)) {
        result = std::make_shared<TAssertStmt>(expr->Location,
            CloneExpr(asrt.Cast()->Expr));
    } else if (TMaybeNode<TTypeDeclStmt>(expr)) {
        result = std::make_shared<TTypeDeclStmt>(expr->Location, expr->Type);
    } else {
        throw NQumir::TError(
            "CloneExpr: unsupported node type " +
            std::string(expr->NodeName()));
    }
    result->Type = expr->Type;
    return result;
}

} // namespace NQdb
