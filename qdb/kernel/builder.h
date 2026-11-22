#pragma once

#include <qumir/parser/ast.h>
#include <qumir/parser/type.h>

#include <string>
#include <vector>

namespace NQdb {
namespace NKernel {
namespace NOz {

class TFunBuilder {
public:
    explicit TFunBuilder(std::string name);

    TFunBuilder& Param(std::string name, NQumir::NAst::TTypePtr type);
    TFunBuilder& Return(NQumir::NAst::TTypePtr type);
    TFunBuilder& Stmt(NQumir::NAst::TExprPtr stmt);

    TFunBuilder& Var(std::string name, NQumir::NAst::TTypePtr type);
    TFunBuilder& Assign(std::string name, NQumir::NAst::TExprPtr value);

    NQumir::NAst::TExprPtr Build() &&;

private:
    std::string Name_;
    std::vector<NQumir::NAst::TParam> Params_;
    NQumir::NAst::TTypePtr RetType_ = nullptr;
    std::vector<NQumir::NAst::TExprPtr> Stmts_;
};

NQumir::NAst::TExprPtr Ident(std::string name);
NQumir::NAst::TExprPtr Return(NQumir::NAst::TExprPtr value = nullptr);
NQumir::NAst::TExprPtr Block(std::vector<NQumir::NAst::TExprPtr> stmts);

NQumir::NAst::TExprPtr Var(std::string name, NQumir::NAst::TTypePtr type);
NQumir::NAst::TExprPtr Assign(std::string name, NQumir::NAst::TExprPtr value);

NQumir::NAst::TExprPtr Int(NQumir::NAst::TTypeFactory& types, int64_t v);
NQumir::NAst::TExprPtr TypedInt(NQumir::NAst::TTypeFactory& types, int64_t v, NQumir::NAst::TTypePtr type);
NQumir::NAst::TExprPtr Float(NQumir::NAst::TTypeFactory& types, double v);
NQumir::NAst::TExprPtr Bool(NQumir::NAst::TTypeFactory& types, bool v);
NQumir::NAst::TExprPtr String(NQumir::NAst::TTypeFactory& types, std::string v);

NQumir::NAst::TExprPtr Bin(NQumir::NAst::TOperator op, NQumir::NAst::TExprPtr l, NQumir::NAst::TExprPtr r);
NQumir::NAst::TExprPtr Add(NQumir::NAst::TExprPtr l, NQumir::NAst::TExprPtr r);
NQumir::NAst::TExprPtr Sub(NQumir::NAst::TExprPtr l, NQumir::NAst::TExprPtr r);
NQumir::NAst::TExprPtr Mul(NQumir::NAst::TExprPtr l, NQumir::NAst::TExprPtr r);
NQumir::NAst::TExprPtr Div(NQumir::NAst::TExprPtr l, NQumir::NAst::TExprPtr r);

NQumir::NAst::TExprPtr Call(std::string name, std::vector<NQumir::NAst::TExprPtr> args = {});
NQumir::NAst::TExprPtr Cast(NQumir::NAst::TExprPtr expr, NQumir::NAst::TTypePtr type);
NQumir::NAst::TExprPtr NullPtr(NQumir::NAst::TTypeFactory& types, NQumir::NAst::TTypePtr type);
NQumir::NAst::TExprPtr Unary(NQumir::NAst::TOperator op, NQumir::NAst::TExprPtr value);
NQumir::NAst::TExprPtr If(NQumir::NAst::TExprPtr cond, NQumir::NAst::TExprPtr thenBody, NQumir::NAst::TExprPtr elseBody = nullptr);

NQumir::NAst::TExprPtr While(NQumir::NAst::TExprPtr cond, NQumir::NAst::TExprPtr body);

NQumir::NAst::TExprPtr Index(NQumir::NAst::TExprPtr collection, NQumir::NAst::TExprPtr index);
NQumir::NAst::TExprPtr Index(std::string collection, NQumir::NAst::TExprPtr index);
NQumir::NAst::TExprPtr ArrayAssign(std::string collection, NQumir::NAst::TExprPtr index, NQumir::NAst::TExprPtr value);
NQumir::NAst::TExprPtr ArrayAssign(std::string collection, std::vector<NQumir::NAst::TExprPtr> indices, NQumir::NAst::TExprPtr value);

NQumir::NAst::TExprPtr Field(NQumir::NAst::TExprPtr object, std::string field);
NQumir::NAst::TExprPtr Field(std::string object, std::string field);
NQumir::NAst::TExprPtr FieldAssign(NQumir::NAst::TExprPtr object, std::string field, NQumir::NAst::TExprPtr value);

} // namespace NOz
} // namespace NKernel
} // namespace NQdb
