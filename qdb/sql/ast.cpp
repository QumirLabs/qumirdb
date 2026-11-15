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

} // namespace NSql
} // namespace NQdb
