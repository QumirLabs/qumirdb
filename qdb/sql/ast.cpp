#include "ast.h"

namespace NQdb {
namespace NSql {

void TSqlCte::Accept(ISqlVisitor& visitor) { visitor.Visit(*this); }
void TSqlWithClause::Accept(ISqlVisitor& visitor) { visitor.Visit(*this); }
void TSqlOrderItem::Accept(ISqlVisitor& visitor) { visitor.Visit(*this); }
void TSqlOrder::Accept(ISqlVisitor& visitor) { visitor.Visit(*this); }
void TSqlSelectItem::Accept(ISqlVisitor& visitor) { visitor.Visit(*this); }
void TSqlTableName::Accept(ISqlVisitor& visitor) { visitor.Visit(*this); }
void TSqlSubqueryTable::Accept(ISqlVisitor& visitor) { visitor.Visit(*this); }
void TIdentList::Accept(ISqlVisitor& visitor) { visitor.Visit(*this); }
void TJoinCondition::Accept(ISqlVisitor& visitor) { visitor.Visit(*this); }
void TSqlJoin::Accept(ISqlVisitor& visitor) { visitor.Visit(*this); }
void TSqlQuery::Accept(ISqlVisitor& visitor) { visitor.Visit(*this); }
void TSqlSelectList::Accept(ISqlVisitor& visitor) { visitor.Visit(*this); }
void TSqlFrameBound::Accept(ISqlVisitor& visitor) { visitor.Visit(*this); }
void TSqlWindowFrame::Accept(ISqlVisitor& visitor) { visitor.Visit(*this); }
void TSqlWindowSpec::Accept(ISqlVisitor& visitor) { visitor.Visit(*this); }
void TSqlRollUp::Accept(ISqlVisitor& visitor) { visitor.Visit(*this); }
void TSqlCube::Accept(ISqlVisitor& visitor) { visitor.Visit(*this); }
void TSqlGroupingExprOrList::Accept(ISqlVisitor& visitor) { visitor.Visit(*this); }
void TSqlGroupingSet::Accept(ISqlVisitor& visitor) { visitor.Visit(*this); }
void TSqlGroupBy::Accept(ISqlVisitor& visitor) { visitor.Visit(*this); }
void TSqlFrom::Accept(ISqlVisitor& visitor) { visitor.Visit(*this); }
void TSqlSelect::Accept(ISqlVisitor& visitor) { visitor.Visit(*this); }
void TSqlSetOp::Accept(ISqlVisitor& visitor) { visitor.Visit(*this); }
void TSqlExternalModule::Accept(ISqlVisitor& visitor) { visitor.Visit(*this); }
void TSqlExternalFunction::Accept(ISqlVisitor& visitor) { visitor.Visit(*this); }

namespace {

bool HasOffset(TSqlFrameBound::EType type) {
    return type == TSqlFrameBound::EType::Preceding || type == TSqlFrameBound::EType::Following;
}

} // namespace

TWindowExpr::TWindowExpr(NQumir::NAst::TExprPtr expr, const TSqlWindowSpec& spec)
    : NQumir::NAst::TExpr(expr->Location, NodeId)
{
    Operands.push_back(std::move(expr));
    if (spec.PartitionBy) {
        HasPartitionBy = true;
        Operands.push_back(spec.PartitionBy);
    }
    if (spec.OrderBy) {
        for (const auto& item : spec.OrderBy->Items) {
            Order.push_back({.Desc = item->Desc, .NullOrder = item->NullOrder});
            Operands.push_back(item->Expr);
        }
    }
    FrameType = spec.Frame->Type;
    FrameStart = spec.Frame->Start->Type;
    if (HasOffset(FrameStart)) {
        Operands.push_back(spec.Frame->Start->Expr);
    }
    if (spec.Frame->End) {
        FrameEnd = spec.Frame->End->Type;
        if (HasOffset(*FrameEnd)) {
            Operands.push_back(spec.Frame->End->Expr);
        }
    }
}

NQumir::NAst::TExprPtr TWindowExpr::FrameStartOffset() const {
    return HasOffset(FrameStart) ? Operands[1 + HasPartitionBy + Order.size()] : nullptr;
}

NQumir::NAst::TExprPtr TWindowExpr::FrameEndOffset() const {
    return FrameEnd && HasOffset(*FrameEnd) ? Operands.back() : nullptr;
}

} // namespace NSql
} // namespace NQdb
