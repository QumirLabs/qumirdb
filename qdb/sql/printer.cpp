#include "printer.h"

#include <qumir/parser/core/printer.h>

#include <memory>
#include <sstream>

namespace NQdb {
namespace NSql {

using NQumir::NAst::TExprPtr;
namespace NCore = NQumir::NAst::NCore;

namespace {

std::string KindName(TSubqueryExpr::EKind kind) {
    switch (kind) {
        case TSubqueryExpr::EKind::Scalar: return "scalar";
        case TSubqueryExpr::EKind::Exists: return "exists";
        case TSubqueryExpr::EKind::In: return "in";
    }
    return "?";
}

std::string FrameTypeName(TSqlWindowFrame::EType type) {
    switch (type) {
        case TSqlWindowFrame::EType::Rows: return "rows";
        case TSqlWindowFrame::EType::Range: return "range";
    }
    return "?";
}

std::string FrameBoundName(TSqlFrameBound::EType type) {
    switch (type) {
        case TSqlFrameBound::EType::UnboundedPreceding: return "unbounded-preceding";
        case TSqlFrameBound::EType::Preceding: return "preceding";
        case TSqlFrameBound::EType::CurrentRow: return "current-row";
        case TSqlFrameBound::EType::Following: return "following";
        case TSqlFrameBound::EType::UnboundedFollowing: return "unbounded-following";
    }
    return "?";
}

std::string NullOrderName(TSqlOrderItem::ENullOrder nullOrder) {
    switch (nullOrder) {
        case TSqlOrderItem::ENullOrder::Default: return "";
        case TSqlOrderItem::ENullOrder::First: return " nulls-first";
        case TSqlOrderItem::ENullOrder::Last: return " nulls-last";
    }
    return "";
}

void PrintWindowFrameBound(
    const TSqlPtr<TSqlFrameBound>& bound,
    NCore::TPrinter& p,
    NCore::TPrintFrame frame)
{
    auto& out = p.GetOut();
    if (!bound) {
        out << "(null)";
        return;
    }
    out << '(' << FrameBoundName(bound->Type);
    if (bound->Expr) {
        p.Separator(frame.Level + 1);
        p.PrintExpr(bound->Expr, frame.AllowTypeWrap, frame.Level + 1);
    }
    out << ')';
}

void PrintWindowFrame(
    const TSqlPtr<TSqlWindowFrame>& windowFrame,
    NCore::TPrinter& p,
    NCore::TPrintFrame frame)
{
    auto& out = p.GetOut();
    if (!windowFrame) {
        return;
    }
    out << '(' << FrameTypeName(windowFrame->Type);
    p.Separator(frame.Level + 1);
    if (windowFrame->End) {
        out << "(between";
        p.Separator(frame.Level + 2);
        PrintWindowFrameBound(windowFrame->Start, p, {frame.AllowTypeWrap, frame.Level + 2});
        p.Separator(frame.Level + 2);
        PrintWindowFrameBound(windowFrame->End, p, {frame.AllowTypeWrap, frame.Level + 2});
        out << ')';
    } else {
        PrintWindowFrameBound(windowFrame->Start, p, {frame.AllowTypeWrap, frame.Level + 1});
    }
    out << ')';
}

void PrintWindowOrderItem(
    const TSqlPtr<TSqlOrderItem>& item,
    NCore::TPrinter& p,
    NCore::TPrintFrame frame)
{
    auto& out = p.GetOut();
    std::string head = item->Desc ? "(order desc" : "(order asc";
    head += NullOrderName(item->NullOrder);
    out << head;
    p.Separator(frame.Level + 1);
    p.PrintExpr(item->Expr, frame.AllowTypeWrap, frame.Level + 1);
    out << ')';
}

void PrintWindowSpec(
    const TSqlPtr<TSqlWindowSpec>& spec,
    NCore::TPrinter& p,
    NCore::TPrintFrame frame)
{
    auto& out = p.GetOut();
    out << "(over";
    if (spec) {
        if (spec->PartitionBy) {
            p.Separator(frame.Level + 1);
            out << "(partition-by";
            p.Separator(frame.Level + 2);
            p.PrintExpr(spec->PartitionBy, frame.AllowTypeWrap, frame.Level + 2);
            out << ')';
        }
        if (spec->OrderBy) {
            p.Separator(frame.Level + 1);
            out << "(order-by";
            for (const auto& item : spec->OrderBy->Items) {
                p.Separator(frame.Level + 2);
                PrintWindowOrderItem(item, p, {frame.AllowTypeWrap, frame.Level + 2});
            }
            out << ')';
        }
        if (spec->Frame) {
            p.Separator(frame.Level + 1);
            PrintWindowFrame(spec->Frame, p, {frame.AllowTypeWrap, frame.Level + 1});
        }
    }
    out << ')';
}

void PrintWindowExpr(
    TWindowExpr& window,
    NCore::TPrinter& p,
    NCore::TPrintFrame frame)
{
    auto& out = p.GetOut();
    out << "(window";
    p.Separator(frame.Level + 1);
    p.PrintExpr(window.Expr, frame.AllowTypeWrap, frame.Level + 1);
    p.Separator(frame.Level + 1);
    PrintWindowSpec(window.WindowSpec, p, {frame.AllowTypeWrap, frame.Level + 1});
    out << ')';
}

std::string JoinTypeName(ESqlJoinType type) {
    switch (type) {
        case ESqlJoinType::Inner: return "inner";
        case ESqlJoinType::Left: return "left";
        case ESqlJoinType::Right: return "right";
        case ESqlJoinType::LeftSemi: return "left-semi";
        case ESqlJoinType::RightSemi: return "right-semi";
        case ESqlJoinType::Full: return "full";
        case ESqlJoinType::Cross: return "cross";
    }
    return "?";
}

std::string SetOpName(TSqlSetOp::EOp op) {
    switch (op) {
        case TSqlSetOp::EOp::Union: return "union";
        case TSqlSetOp::EOp::Intersect: return "intersect";
        case TSqlSetOp::EOp::Except: return "except";
    }
    return "?";
}

std::string Join(const std::vector<std::string>& parts, char sep) {
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) {
            out += sep;
        }
        out += parts[i];
    }
    return out;
}

struct TSqlPrinter {
    std::ostream& Out;

    void Indent(int n) {
        for (int i = 0; i < n; ++i) {
            Out << ' ';
        }
    }

    void Line(int ind, std::string_view s) {
        Indent(ind);
        Out << s << '\n';
    }

    NCore::TPrintOptions ExprOptions() {
        NCore::TPrintOptions opts;
        opts.NodePrinters[TSubqueryExpr::NodeId] =
            [](NQumir::NAst::TExpr& node, NCore::TPrinter& p, NCore::TPrintFrame frame) {
                auto& sub = static_cast<TSubqueryExpr&>(node);
                auto& out = p.GetOut();
                out << "(subquery " << KindName(sub.Kind);
                if (sub.Operand) {
                    out << ' ';
                    p.PrintExpr(sub.Operand, false, frame.Level);
                }
                out << ' ' << PrintAst(std::static_pointer_cast<TSqlNode>(sub.Query));
                out << ')';
            };
        opts.NodePrinters[TWindowExpr::NodeId] =
            [](NQumir::NAst::TExpr& node, NCore::TPrinter& p, NCore::TPrintFrame frame) {
                PrintWindowExpr(static_cast<TWindowExpr&>(node), p, frame);
            };
        return opts;
    }

    // delegate expressions to the Qumir core printer, re-indenting its output
    void Expr(int ind, const TExprPtr& e) {
        if (!e) {
            Line(ind, "(null)");
            return;
        }
        std::istringstream lines(NCore::PrintAst(e, ExprOptions()));
        std::string line;
        while (std::getline(lines, line)) {
            Indent(ind);
            Out << line << '\n';
        }
    }

    void IdentList(int ind, std::string_view head, const TSqlPtr<TIdentList>& list) {
        std::string s(head);
        if (list) {
            s += ' ';
            s += Join(list->Items, ' ');
        }
        s += ')';
        Line(ind, s);
    }

    void Query(int ind, const TSqlPtr<TSqlQuery>& q) {
        if (!q) {
            Line(ind, "(query null)");
            return;
        }
        Line(ind, "(query");
        if (q->WithClause) {
            With(ind + 2, q->WithClause);
        }
        Body(ind + 2, q->Body);
        if (q->OrderBy) {
            Order(ind + 2, q->OrderBy);
        }
        if (q->Limit) {
            Line(ind + 2, "(limit");
            Expr(ind + 4, q->Limit);
            Line(ind + 2, ")");
        }
        if (q->Offset) {
            Line(ind + 2, "(offset");
            Expr(ind + 4, q->Offset);
            Line(ind + 2, ")");
        }
        Line(ind, ")");
    }

    void With(int ind, const TSqlPtr<TSqlWithClause>& w) {
        Line(ind, w->Recursive ? "(with recursive" : "(with");
        for (const auto& cte : w->Ctes) {
            Cte(ind + 2, cte);
        }
        Line(ind, ")");
    }

    void Cte(int ind, const TSqlPtr<TSqlCte>& c) {
        std::string head = "(cte " + c->Name;
        if (c->Columns) {
            head += " cols: " + Join(c->Columns->Items, ' ');
        }
        Line(ind, head);
        Query(ind + 2, c->Query);
        Line(ind, ")");
    }

    void Body(int ind, const TSqlNodePtr& body) {
        if (auto select = TMaybeNode<TSqlSelect>(body)) {
            Select(ind, select.Cast());
            return;
        }
        if (auto maybeSetOp = TMaybeNode<TSqlSetOp>(body)) {
            auto setOp = maybeSetOp.Cast();
            Line(ind, "(" + SetOpName(setOp->Op) +
                (setOp->Quantifier == ESetQuantifier::Distinct ? " distinct" : " all"));
            Body(ind + 2, setOp->Left);
            Body(ind + 2, setOp->Right);
            Line(ind, ")");
            return;
        }
        if (auto query = TMaybeNode<TSqlQuery>(body)) {
            Query(ind, query.Cast());
            return;
        }
        Line(ind, "(unknown-body)");
    }

    void Select(int ind, const TSqlPtr<TSqlSelect>& s) {
        Line(ind, s->Quantifier == ESetQuantifier::Distinct ? "(select distinct" : "(select");
        if (s->SelectList) {
            SelectList(ind + 2, s->SelectList);
        }
        if (s->From) {
            From(ind + 2, s->From);
        }
        if (s->Where) {
            Line(ind + 2, "(where");
            Expr(ind + 4, s->Where);
            Line(ind + 2, ")");
        }
        if (s->GroupBy) {
            GroupBy(ind + 2, s->GroupBy);
        }
        if (s->Having) {
            Line(ind + 2, "(having");
            Expr(ind + 4, s->Having);
            Line(ind + 2, ")");
        }
        Line(ind, ")");
    }

    void SelectList(int ind, const TSqlPtr<TSqlSelectList>& list) {
        Line(ind, "(select-list");
        for (const auto& item : list->Items) {
            if (item->Star) {
                std::string star = item->StarPrefix.empty()
                    ? "*"
                    : Join(item->StarPrefix, '.') + ".*";
                Line(ind + 2, "(star " + star + ")");
            } else {
                std::string head = "(item";
                if (item->Alias) {
                    head += " as " + *item->Alias;
                } else if (!item->ColumnAliases.empty()) {
                    head += " as (" + Join(item->ColumnAliases, ',') + ")";
                }
                Line(ind + 2, head);
                Expr(ind + 4, item->Expr);
                Line(ind + 2, ")");
            }
        }
        Line(ind, ")");
    }

    void From(int ind, const TSqlPtr<TSqlFrom>& from) {
        Line(ind, "(from");
        for (const auto& ref : from->Items) {
            TableRef(ind + 2, ref);
        }
        Line(ind, ")");
    }

    void TableRef(int ind, const TSqlPtr<TSqlTableRef>& ref) {
        if (auto table = TMaybeNode<TSqlTableName>(ref)) {
            auto name = table.Cast();
            std::string head = "(table " + Join(name->Name, '.');
            if (name->Alias) {
                head += " as " + *name->Alias;
            }
            head += ")";
            Line(ind, head);
            return;
        }
        if (auto maybeSub = TMaybeNode<TSqlSubqueryTable>(ref)) {
            auto sub = maybeSub.Cast();
            std::string head = "(subquery-table";
            if (sub->Alias) {
                head += " as " + *sub->Alias;
            }
            if (sub->ColumnAliases) {
                head += " cols: " + Join(sub->ColumnAliases->Items, ' ');
            }
            Line(ind, head);
            Query(ind + 2, sub->Query);
            Line(ind, ")");
            return;
        }
        if (auto join = TMaybeNode<TSqlJoin>(ref)) {
            JoinNode(ind, join.Cast());
            return;
        }
        Line(ind, "(unknown-table)");
    }

    void JoinNode(int ind, const TSqlPtr<TSqlJoin>& j) {
        Line(ind, std::string("(join ") + (j->Natural ? "natural " : "") + JoinTypeName(j->Type));
        TableRef(ind + 2, j->Left);
        TableRef(ind + 2, j->Right);
        if (j->Condition) {
            if (j->Condition->On) {
                Line(ind + 2, "(on");
                Expr(ind + 4, j->Condition->On);
                Line(ind + 2, ")");
            } else if (j->Condition->UsingColumns) {
                IdentList(ind + 2, "(using", j->Condition->UsingColumns);
            }
        }
        Line(ind, ")");
    }

    void GroupingElement(int ind, const TSqlNodePtr& item) {
        if (auto e = TMaybeNode<TSqlGroupingExprOrList>(item)) {
            Expr(ind, e.Cast()->Exprs);
        } else if (auto r = TMaybeNode<TSqlRollUp>(item)) {
            Line(ind, "(rollup");
            Expr(ind + 2, r.Cast()->Exprs);
            Line(ind, ")");
        } else if (auto c = TMaybeNode<TSqlCube>(item)) {
            Line(ind, "(cube");
            Expr(ind + 2, c.Cast()->Exprs);
            Line(ind, ")");
        } else if (auto gs = TMaybeNode<TSqlGroupingSet>(item)) {
            Line(ind, "(grouping-sets");
            for (const auto& set : gs.Cast()->Items) {
                GroupingElement(ind + 2, set);
            }
            Line(ind, ")");
        }
    }

    void GroupBy(int ind, const TSqlPtr<TSqlGroupBy>& gb) {
        Line(ind, "(group-by");
        for (const auto& item : gb->Items) {
            GroupingElement(ind + 2, item);
        }
        Line(ind, ")");
    }

    void Order(int ind, const TSqlPtr<TSqlOrder>& order) {
        Line(ind, "(order-by");
        for (const auto& item : order->Items) {
            std::string head = item->Desc ? "(order desc" : "(order asc";
            if (item->NullOrder == TSqlOrderItem::ENullOrder::First) {
                head += " nulls-first";
            } else if (item->NullOrder == TSqlOrderItem::ENullOrder::Last) {
                head += " nulls-last";
            }
            Line(ind + 2, head);
            Expr(ind + 4, item->Expr);
            Line(ind + 2, ")");
        }
        Line(ind, ")");
    }
};

} // namespace

std::string PrintAst(const TSqlNodePtr& node) {
    std::ostringstream out;
    TSqlPrinter printer{out};

    if (auto query = TMaybeNode<TSqlQuery>(node)) {
        printer.Query(0, query.Cast());
    } else if (node) {
        printer.Body(0, node);
    } else {
        out << "(null)\n";
    }

    return out.str();
}

} // namespace NSql
} // namespace NQdb
