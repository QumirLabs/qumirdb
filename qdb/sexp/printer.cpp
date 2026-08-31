#include <qdb/sexp/printer.h>

#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/late_materialize.h>
#include <qdb/plan/ops/limit.h>
#include <qdb/plan/ops/operator.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/sort.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/ops/union.h>
#include <qdb/plan/ops/window.h>
#include <qdb/plan/ops/cte_ref.h>
#include <qdb/plan/ops/cte_consumer.h>

#include <stdexcept>

namespace NQdb {
namespace NSexp {

using namespace NQumir::NAst::NCore;

namespace {

void PrintRel(NQumir::NAst::TExpr& expr, TPrinter& printer, TPrintFrame frame) {
    auto& op = static_cast<IOperator&>(expr);
    auto& out = printer.GetOut();
    const auto rel = op.RelName();

    if (rel == TSourceOperator::OpId) {
        auto& src = static_cast<TSourceOperator&>(op);
        out << "(rel source";
        if (!src.SourcePath().empty()) {
            out << ' ';
            printer.PrintString(src.SourcePath(), '"');
        }
        if (!src.GetAlias().empty()) {
            out << ' ';
            printer.PrintString(src.GetAlias(), '"');
        }
        if (src.RowGroupPredicate()) {
            printer.Separator(frame.Level + 1);
            printer.PrintExpr(
                src.RowGroupPredicate(), frame.AllowTypeWrap, frame.Level + 1);
        }
        out << ')';
        return;
    }

    if (rel == TCteRef::OpId) {
        auto& ref = static_cast<TCteRef&>(op);
        out << "(rel cte-ref " << ref.Def()->Id << ')';
        return;
    }

    if (rel == TCteConsumer::OpId) {
        auto& consumer = static_cast<TCteConsumer&>(op);
        out << "(rel cte-consumer " << consumer.Def()->Id
            << " x" << consumer.Materialization()->RefCount << ')';
        return;
    }

    if (rel == TFilterOperator::OpId) {
        auto& filt = static_cast<TFilterOperator&>(op);
        out << "(rel filter";
        printer.Separator(frame.Level + 1);
        printer.PrintExpr(filt.Input(), frame.AllowTypeWrap, frame.Level + 1);
        printer.Separator(frame.Level + 1);
        printer.PrintExpr(filt.Predicate(), frame.AllowTypeWrap, frame.Level + 1);
        out << ')';
        return;
    }

    if (rel == TProjectOperator::OpId) {
        auto& proj = static_cast<TProjectOperator&>(op);
        out << "(rel project";
        printer.Separator(frame.Level + 1);
        printer.PrintExpr(proj.Input(), frame.AllowTypeWrap, frame.Level + 1);
        for (const auto& spec : proj.Projections()) {
            printer.Separator(frame.Level + 1);
            out << '(';
            printer.PrintIdentifier(spec.Name);
            printer.Space();
            printer.PrintExpr(spec.Expression, frame.AllowTypeWrap, frame.Level + 2);
            out << ')';
        }
        out << ')';
        return;
    }

    if (rel == TSortOperator::OpId) {
        auto& sort = static_cast<TSortOperator&>(op);
        out << "(rel sort";
        printer.Separator(frame.Level + 1);
        printer.PrintExpr(sort.Input(), frame.AllowTypeWrap, frame.Level + 1);
        for (const auto& key : sort.Keys()) {
            printer.Separator(frame.Level + 1);
            out << '(';
            printer.PrintIdentifier(key.Column);
            printer.Space();
            out << SortDirectionName(key.Direction);
            printer.Space();
            out << SortNullsName(key.Nulls);
            out << ')';
        }
        out << ')';
        return;
    }

    if (rel == TTopSortOperator::OpId) {
        auto& sort = static_cast<TTopSortOperator&>(op);
        out << "(rel top-sort";
        printer.Separator(frame.Level + 1);
        printer.PrintExpr(sort.Input(), frame.AllowTypeWrap, frame.Level + 1);
        for (const auto& key : sort.Keys()) {
            printer.Separator(frame.Level + 1);
            out << '(';
            printer.PrintIdentifier(key.Column);
            printer.Space();
            out << SortDirectionName(key.Direction);
            printer.Space();
            out << SortNullsName(key.Nulls);
            out << ')';
        }
        printer.Separator(frame.Level + 1);
        out << "(limit " << sort.Limit() << ')';
        out << ')';
        return;
    }

    if (rel == TLimitOperator::OpId) {
        auto& limit = static_cast<TLimitOperator&>(op);
        out << "(rel limit";
        printer.Separator(frame.Level + 1);
        printer.PrintExpr(limit.Input(), frame.AllowTypeWrap, frame.Level + 1);
        printer.Separator(frame.Level + 1);
        out << "(limit " << limit.Limit() << ')';
        if (limit.Offset() != 0) {
            printer.Separator(frame.Level + 1);
            out << "(offset " << limit.Offset() << ')';
        }
        out << ')';
        return;
    }

    if (rel == TLateMaterializeOperator::OpId) {
        auto& late = static_cast<TLateMaterializeOperator&>(op);
        out << "(rel late-materialize";
        printer.Separator(frame.Level + 1);
        printer.PrintExpr(late.Input(), frame.AllowTypeWrap, frame.Level + 1);
        printer.Separator(frame.Level + 1);
        out << "(locator ";
        printer.PrintIdentifier(late.LocatorColumn());
        out << ')';
        for (const auto& column : late.Columns()) {
            printer.Separator(frame.Level + 1);
            out << "(column ";
            printer.PrintIdentifier(column.OutputName);
            printer.Space();
            printer.PrintIdentifier(column.PhysicalName);
            printer.Space();
            printer.PrintType(column.Type, frame.Level + 2);
            out << ')';
        }
        out << ')';
        return;
    }

    if (rel == TAggregateOperator::OpId) {
        auto& agg = static_cast<TAggregateOperator&>(op);
        out << "(rel aggregate";
        printer.Separator(frame.Level + 1);
        printer.PrintExpr(agg.Input(), frame.AllowTypeWrap, frame.Level + 1);
        printer.Separator(frame.Level + 1);
        out << "(keys";
        for (const auto& key : agg.GroupKeys()) {
            printer.Space();
            printer.PrintIdentifier(key);
        }
        out << ')';
        for (const auto& set : agg.GroupingSets()) {
            printer.Separator(frame.Level + 1);
            out << "(set";
            for (size_t idx : set) {
                printer.Space();
                printer.PrintIdentifier(agg.GroupKeys()[idx]);
            }
            out << ')';
        }
        for (const auto& spec : agg.Aggs()) {
            printer.Separator(frame.Level + 1);
            out << "(agg ";
            printer.PrintIdentifier(spec.Name);
            printer.Space();
            printer.PrintIdentifier(spec.Func);
            if (spec.Arg) {
                printer.Space();
                printer.PrintExpr(spec.Arg, frame.AllowTypeWrap, frame.Level + 2);
            }
            out << ')';
        }
        out << ')';
        return;
    }

    if (rel == TJoinOperator::OpId) {
        auto& join = static_cast<TJoinOperator&>(op);
        out << "(rel join";
        printer.Separator(frame.Level + 1);
        printer.PrintExpr(join.Left(), frame.AllowTypeWrap, frame.Level + 1);
        printer.Separator(frame.Level + 1);
        printer.PrintExpr(join.Right(), frame.AllowTypeWrap, frame.Level + 1);
        // Key list: ((lk rk) (lk rk) ...)
        printer.Separator(frame.Level + 1);
        out << '(';
        bool firstKey = true;
        for (const auto& key : join.Keys()) {
            if (!firstKey) {
                printer.Space();
            }
            firstKey = false;
            out << '(';
            printer.PrintIdentifier(key.Left);
            printer.Space();
            printer.PrintIdentifier(key.Right);
            if (key.NullsEqual) {
                out << " nulls_equal";
            }
            out << ')';
        }
        out << ')';
        // Join type as a bare keyword: (inner)
        printer.Separator(frame.Level + 1);
        out << '(' << JoinTypeName(join.JoinType()) << ')';
        if (join.Filter()) {
            printer.Separator(frame.Level + 1);
            out << "(residual";
            printer.Space();
            printer.PrintExpr(join.Filter(), frame.AllowTypeWrap, frame.Level + 1);
            out << ')';
        }
        if (const auto& runtimeFilter = join.RuntimeFilter()) {
            printer.Separator(frame.Level + 1);
            out << "(emit-filter " << runtimeFilter->Id << ' '
                << JoinFilterSideName(runtimeFilter->BuildSide) << ')';
        }
        out << ')';
        return;
    }

    if (rel == TUnionAllOperator::OpId) {
        auto& un = static_cast<TUnionAllOperator&>(op);
        out << "(rel union-all";
        for (const auto& input : un.Inputs()) {
            printer.Separator(frame.Level + 1);
            printer.PrintExpr(input, frame.AllowTypeWrap, frame.Level + 1);
        }
        out << ')';
        return;
    }

    if (rel == TWindowOperator::OpId) {
        auto& window = static_cast<TWindowOperator&>(op);
        out << "(rel window";
        printer.Separator(frame.Level + 1);
        printer.PrintExpr(window.Input(), frame.AllowTypeWrap, frame.Level + 1);
        if (!window.PartitionKeys().empty()) {
            printer.Separator(frame.Level + 1);
            out << "(partition";
            for (const auto& key : window.PartitionKeys()) {
                printer.Space();
                printer.PrintIdentifier(key);
            }
            out << ')';
        }
        if (!window.OrderKeys().empty()) {
            printer.Separator(frame.Level + 1);
            out << "(order";
            for (const auto& key : window.OrderKeys()) {
                printer.Space();
                out << '(';
                printer.PrintIdentifier(key.Column);
                printer.Space();
                out << SortDirectionName(key.Direction);
                printer.Space();
                out << SortNullsName(key.Nulls);
                out << ')';
            }
            out << ')';
        }
        if (const auto& windowFrame = window.Frame()) {
            printer.Separator(frame.Level + 1);
            out << "(frame " << WindowFrameModeName(windowFrame->Mode);
            auto printBound = [&](const char* tag, const TFrameBound& bound) {
                printer.Space();
                out << '(' << tag << ' ' << FrameBoundKindName(bound.Kind);
                if (bound.Offset) {
                    printer.Space();
                    printer.PrintExpr(bound.Offset, frame.AllowTypeWrap, frame.Level + 2);
                }
                out << ')';
            };
            printBound("start", windowFrame->Start);
            printBound("end", windowFrame->End);
            out << ')';
        }
        for (const auto& func : window.Functions()) {
            printer.Separator(frame.Level + 1);
            out << "(fn ";
            printer.PrintIdentifier(func.Name);
            printer.Space();
            printer.PrintIdentifier(func.Func);
            if (func.Arg) {
                printer.Space();
                printer.PrintExpr(func.Arg, frame.AllowTypeWrap, frame.Level + 2);
            }
            out << ')';
        }
        out << ')';
        return;
    }

    throw std::runtime_error("PrintRel: unknown rel operator: " + std::string(rel));
}

} // namespace

TPrintExprFactory MakeRelPrinters() {
    return {{IOperator::NodeId, PrintRel}};
}

void PrintRelPlan(std::ostream& out, const TOperatorPtr& plan) {
    if (!CollectMaterializations(plan).empty()) {
        throw std::runtime_error(
            "PrintRelPlan: materialized CTE plans are not serializable yet");
    }

    auto defs = CollectCteDefinitions(plan);
    NQumir::NAst::NCore::TPrintOptions options{.NodePrinters = MakeRelPrinters()};
    if (defs.empty()) {
        NQumir::NAst::NCore::PrintAst(out, plan, options);
        return;
    }
    // (query (cte <id> <def-plan>) ... (main <main-plan>)): definitions are
    // dependency-first so a nested (rel cte-ref N) is always defined earlier.
    TPrinter printer(out, std::move(options));
    out << "(query";
    for (const auto& def : defs) {
        printer.Separator(1);
        out << "(cte " << def->Id;
        printer.Separator(2);
        printer.PrintExpr(def->Plan, true, 2);
        out << ')';
    }
    printer.Separator(1);
    out << "(main";
    printer.Separator(2);
    printer.PrintExpr(plan, true, 2);
    out << "))";
}

} // namespace NSexp
} // namespace NQdb
