#include <qdb/kernel/builder.h>

#include <utility>

namespace NQdb {
namespace NKernel {
namespace NOz {

using namespace NQumir::NAst;
using namespace NQumir::NAst::NLiterals;

namespace {

NQumir::TLocation Loc() {
    return {};
}

} // namespace

TFunBuilder::TFunBuilder(std::string name)
    : Name_(std::move(name))
{}

TFunBuilder& TFunBuilder::Param(std::string name, TTypePtr type) {
    Params_.push_back(NQumir::MakeIntrusive<TVarStmt>(
        Loc(), std::move(name), std::move(type)));
    return *this;
}

TFunBuilder& TFunBuilder::Return(TTypePtr type) {
    RetType_ = std::move(type);
    return *this;
}

TFunBuilder& TFunBuilder::Stmt(TExprPtr stmt) {
    Stmts_.push_back(std::move(stmt));
    return *this;
}

TFunBuilder& TFunBuilder::Var(std::string name, TTypePtr type) {
    return Stmt(NOz::Var(std::move(name), std::move(type)));
}

TFunBuilder& TFunBuilder::Assign(std::string name, TExprPtr value) {
    return Stmt(NOz::Assign(std::move(name), std::move(value)));
}

TExprPtr TFunBuilder::Build() && {
    auto body = NQumir::MakeIntrusive<TBlockExpr>(Loc(), std::move(Stmts_));
    return NQumir::MakeIntrusive<TFunDecl>(
        Loc(), std::move(Name_), std::vector<TGenericParam>{}, std::move(Params_), std::move(body),
        std::move(RetType_));
}

TExprPtr Ident(std::string name) {
    return NQumir::MakeIntrusive<TIdentExpr>(Loc(), std::move(name));
}

TExprPtr Return(TExprPtr value) {
    return NQumir::MakeIntrusive<TReturnExpr>(Loc(), std::move(value));
}

TExprPtr Block(std::vector<TExprPtr> stmts) {
    return NQumir::MakeIntrusive<TBlockExpr>(Loc(), std::move(stmts));
}

TExprPtr Var(std::string name, TTypePtr type) {
    return NQumir::MakeIntrusive<TVarStmt>(Loc(), std::move(name), std::move(type));
}

TExprPtr Assign(std::string name, TExprPtr value) {
    return NQumir::MakeIntrusive<TAssignExpr>(Loc(), std::move(name), std::move(value));
}

TExprPtr Int(NQumir::NAst::TTypeFactory& types, int64_t v) {
    return NQumir::MakeIntrusive<TNumberExpr>(Loc(), v, types);
}

TExprPtr TypedInt(NQumir::NAst::TTypeFactory& types, int64_t v, TTypePtr type) {
    auto e = NQumir::MakeIntrusive<TNumberExpr>(Loc(), v, types);
    e->Type = std::move(type);
    return e;
}

TExprPtr Float(NQumir::NAst::TTypeFactory& types, double v) {
    return NQumir::MakeIntrusive<TNumberExpr>(Loc(), v, types);
}

TExprPtr Bool(NQumir::NAst::TTypeFactory& types, bool v) {
    auto e = NQumir::MakeIntrusive<TNumberExpr>(Loc(), static_cast<int64_t>(v), types);
    e->Type = types.Make<TBoolType>();
    return e;
}

TExprPtr String(NQumir::NAst::TTypeFactory& types, std::string v) {
    return NQumir::MakeIntrusive<TStringLiteralExpr>(Loc(), std::move(v), types);
}

TExprPtr Bin(TOperator op, TExprPtr l, TExprPtr r) {
    return NQumir::MakeIntrusive<TBinaryExpr>(
        Loc(), std::move(op), std::move(l), std::move(r));
}

TExprPtr Add(TExprPtr l, TExprPtr r) { return Bin("+"_op, std::move(l), std::move(r)); }
TExprPtr Sub(TExprPtr l, TExprPtr r) { return Bin("-"_op, std::move(l), std::move(r)); }
TExprPtr Mul(TExprPtr l, TExprPtr r) { return Bin("*"_op, std::move(l), std::move(r)); }
TExprPtr Div(TExprPtr l, TExprPtr r) { return Bin("/"_op, std::move(l), std::move(r)); }

TExprPtr Call(std::string name, std::vector<TExprPtr> args) {
    return NQumir::MakeIntrusive<TCallExpr>(
        Loc(),
        Ident(std::move(name)),
        std::move(args));
}

TExprPtr Cast(TExprPtr expr, TTypePtr type) {
    return NQumir::MakeIntrusive<TCastExpr>(Loc(), std::move(expr), std::move(type));
}

TExprPtr NullPtr(NQumir::NAst::TTypeFactory& types, TTypePtr type) {
    return Cast(Int(types, 0), std::move(type));
}

TExprPtr Unary(TOperator op, TExprPtr value) {
    return NQumir::MakeIntrusive<TUnaryExpr>(
        Loc(), std::move(op), std::move(value));
}

TExprPtr If(TExprPtr cond, TExprPtr thenBody, TExprPtr elseBody) {
    return NQumir::MakeIntrusive<TIfExpr>(
        Loc(),
        std::move(cond),
        std::move(thenBody),
        std::move(elseBody));
}

TExprPtr While(TExprPtr cond, TExprPtr body) {
    auto loop = NQumir::MakeIntrusive<TWhileStmtExpr>(Loc(), std::move(cond), std::move(body));
    return loop;
}

TExprPtr Index(TExprPtr collection, TExprPtr index) {
    return NQumir::MakeIntrusive<TIndexExpr>(
        Loc(),
        std::move(collection),
        std::move(index));
}

TExprPtr Index(std::string collection, TExprPtr index) {
    return Index(Ident(std::move(collection)), std::move(index));
}

TExprPtr ArrayAssign(std::string collection, TExprPtr index, TExprPtr value) {
    std::vector<TExprPtr> indices;
    indices.push_back(std::move(index));
    return ArrayAssign(std::move(collection), std::move(indices), std::move(value));
}

TExprPtr ArrayAssign(std::string collection, std::vector<TExprPtr> indices, TExprPtr value) {
    return NQumir::MakeIntrusive<TArrayAssignExpr>(
        Loc(), std::move(collection), std::move(indices), std::move(value));
}

TExprPtr Field(TExprPtr object, std::string field) {
    return NQumir::MakeIntrusive<TFieldAccessExpr>(
        Loc(), std::move(object), std::move(field));
}

TExprPtr Field(std::string object, std::string field) {
    return Field(Ident(std::move(object)), std::move(field));
}

TExprPtr FieldAssign(TExprPtr object, std::string field, TExprPtr value) {
    return NQumir::MakeIntrusive<TFieldAssignExpr>(
        Loc(), std::move(object), std::move(field), std::move(value));
}

} // namespace NOz
} // namespace NKernel
} // namespace NQdb
