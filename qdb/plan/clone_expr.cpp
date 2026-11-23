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
        result = NQumir::MakeIntrusive<TIdentExpr>(
            expr->Location, ident.Cast()->Name);
    } else if (auto literal = TMaybeNode<TStringLiteralExpr>(expr)) {
        result = NQumir::MakeIntrusive<TStringLiteralExpr>(*literal.Cast());
    } else if (auto number = TMaybeNode<TNumberExpr>(expr)) {
        result = NQumir::MakeIntrusive<TNumberExpr>(*number.Cast());
    } else if (auto unary = TMaybeNode<TUnaryExpr>(expr)) {
        result = NQumir::MakeIntrusive<TUnaryExpr>(expr->Location,
            unary.Cast()->Operator, CloneExpr(unary.Cast()->Operand));
    } else if (auto binary = TMaybeNode<TBinaryExpr>(expr)) {
        result = NQumir::MakeIntrusive<TBinaryExpr>(expr->Location,
            binary.Cast()->Operator, CloneExpr(binary.Cast()->Left()),
            CloneExpr(binary.Cast()->Right()));
    } else if (auto call = TMaybeNode<TCallExpr>(expr)) {
        std::vector<TExprPtr> args;
        args.reserve(call.Cast()->Args().size());
        for (const auto& arg : call.Cast()->Args()) {
            args.push_back(CloneExpr(arg));
        }
        result = NQumir::MakeIntrusive<TCallExpr>(expr->Location,
            CloneExpr(call.Cast()->Callee()), std::move(args));
    } else if (auto cast = TMaybeNode<TCastExpr>(expr)) {
        result = NQumir::MakeIntrusive<TCastExpr>(expr->Location,
            CloneExpr(cast.Cast()->Operand), expr->Type);
    } else if (auto bitcast = TMaybeNode<TBitcastExpr>(expr)) {
        result = NQumir::MakeIntrusive<TBitcastExpr>(expr->Location,
            CloneExpr(bitcast.Cast()->Operand), expr->Type);
    } else if (auto ifExpr = TMaybeNode<TIfExpr>(expr)) {
        result = NQumir::MakeIntrusive<TIfExpr>(expr->Location,
            CloneExpr(ifExpr.Cast()->Cond()),
            CloneExpr(ifExpr.Cast()->Then()),
            ifExpr.Cast()->Else() ? CloneExpr(ifExpr.Cast()->Else()) : nullptr);
    } else if (auto block = TMaybeNode<TBlockExpr>(expr)) {
        std::vector<TExprPtr> stmts;
        stmts.reserve(block.Cast()->Stmts.size());
        for (const auto& s : block.Cast()->Stmts) stmts.push_back(CloneExpr(s));
        result = NQumir::MakeIntrusive<TBlockExpr>(expr->Location, std::move(stmts));
    } else if (auto var = TMaybeNode<TVarStmt>(expr)) {
        std::vector<std::pair<TExprPtr, TExprPtr>> bounds;
        bounds.reserve(var.Cast()->Bounds().size());
        for (const auto& [lo, hi] : var.Cast()->Bounds())
            bounds.emplace_back(CloneExpr(lo), CloneExpr(hi));
        auto v = NQumir::MakeIntrusive<TVarStmt>(expr->Location,
            var.Cast()->Name, expr->Type, std::move(bounds));
        if (var.Cast()->Init()) v->Init() = CloneExpr(var.Cast()->Init());
        result = std::move(v);
    } else if (auto assign = TMaybeNode<TAssignExpr>(expr)) {
        result = NQumir::MakeIntrusive<TAssignExpr>(expr->Location,
            assign.Cast()->Name, CloneExpr(assign.Cast()->Value));
    } else if (auto aarr = TMaybeNode<TArrayAssignExpr>(expr)) {
        std::vector<TExprPtr> idxs;
        idxs.reserve(aarr.Cast()->Indices().size());
        for (const auto& i : aarr.Cast()->Indices()) idxs.push_back(CloneExpr(i));
        result = NQumir::MakeIntrusive<TArrayAssignExpr>(expr->Location,
            aarr.Cast()->Name, std::move(idxs), CloneExpr(aarr.Cast()->Value()));
    } else if (auto ret = TMaybeNode<TReturnExpr>(expr)) {
        result = NQumir::MakeIntrusive<TReturnExpr>(expr->Location,
            ret.Cast()->Value ? CloneExpr(ret.Cast()->Value) : nullptr);
    } else if (TMaybeNode<TBreakStmt>(expr)) {
        result = NQumir::MakeIntrusive<TBreakStmt>(expr->Location);
    } else if (TMaybeNode<TContinueStmt>(expr)) {
        result = NQumir::MakeIntrusive<TContinueStmt>(expr->Location);
    } else if (auto idx = TMaybeNode<TIndexExpr>(expr)) {
        result = NQumir::MakeIntrusive<TIndexExpr>(expr->Location,
            CloneExpr(idx.Cast()->Collection()), CloneExpr(idx.Cast()->Index()));
    } else if (auto midx = TMaybeNode<TMultiIndexExpr>(expr)) {
        std::vector<TExprPtr> indices;
        indices.reserve(midx.Cast()->Indices().size());
        for (const auto& i : midx.Cast()->Indices()) indices.push_back(CloneExpr(i));
        result = NQumir::MakeIntrusive<TMultiIndexExpr>(expr->Location,
            CloneExpr(midx.Cast()->Collection()), std::move(indices));
    } else if (auto slice = TMaybeNode<TSliceExpr>(expr)) {
        result = NQumir::MakeIntrusive<TSliceExpr>(expr->Location,
            CloneExpr(slice.Cast()->Collection()),
            CloneExpr(slice.Cast()->Start()),
            CloneExpr(slice.Cast()->End()));
    } else if (auto fa = TMaybeNode<TFieldAccessExpr>(expr)) {
        result = NQumir::MakeIntrusive<TFieldAccessExpr>(expr->Location,
            CloneExpr(fa.Cast()->Object), fa.Cast()->FieldName);
    } else if (auto fassign = TMaybeNode<TFieldAssignExpr>(expr)) {
        result = NQumir::MakeIntrusive<TFieldAssignExpr>(expr->Location,
            CloneExpr(fassign.Cast()->Object()), fassign.Cast()->FieldName,
            CloneExpr(fassign.Cast()->Value()));
    } else if (auto sc = TMaybeNode<TStructConstructExpr>(expr)) {
        std::vector<TExprPtr> fields;
        fields.reserve(sc.Cast()->Fields.size());
        for (const auto& f : sc.Cast()->Fields) fields.push_back(CloneExpr(f));
        result = NQumir::MakeIntrusive<TStructConstructExpr>(expr->Location,
            expr->Type, std::move(fields));
    } else if (auto wh = TMaybeNode<TWhileStmtExpr>(expr)) {
        result = NQumir::MakeIntrusive<TWhileStmtExpr>(expr->Location,
            CloneExpr(wh.Cast()->Cond()), CloneExpr(wh.Cast()->Body()));
    } else if (auto rep = TMaybeNode<TRepeatStmtExpr>(expr)) {
        result = NQumir::MakeIntrusive<TRepeatStmtExpr>(expr->Location,
            CloneExpr(rep.Cast()->Body()), CloneExpr(rep.Cast()->Cond()));
    } else if (auto forst = TMaybeNode<TForStmtExpr>(expr)) {
        result = NQumir::MakeIntrusive<TForStmtExpr>(expr->Location,
            forst.Cast()->VarName,
            CloneExpr(forst.Cast()->From()), CloneExpr(forst.Cast()->To()),
            CloneExpr(forst.Cast()->Step()), CloneExpr(forst.Cast()->Body()));
    } else if (auto times = TMaybeNode<TTimesStmtExpr>(expr)) {
        result = NQumir::MakeIntrusive<TTimesStmtExpr>(expr->Location,
            CloneExpr(times.Cast()->Count()), CloneExpr(times.Cast()->Body()));
    } else if (auto await = TMaybeNode<TAwaitExpr>(expr)) {
        result = NQumir::MakeIntrusive<TAwaitExpr>(expr->Location,
            CloneExpr(await.Cast()->Operand));
    } else if (auto asrt = TMaybeNode<TAssertStmt>(expr)) {
        result = NQumir::MakeIntrusive<TAssertStmt>(expr->Location,
            CloneExpr(asrt.Cast()->Expr));
    } else if (TMaybeNode<TTypeDeclStmt>(expr)) {
        result = NQumir::MakeIntrusive<TTypeDeclStmt>(expr->Location, expr->Type);
    } else {
        throw NQumir::TError(
            "CloneExpr: unsupported node type " +
            std::string(expr->NodeName()));
    }
    result->Type = expr->Type;
    return result;
}

} // namespace NQdb
