#include <qdb/plan/build/expr.h>

#include <qdb/plan/ops/dependent_join.h>
#include <qdb/plan/sql_lowering.h>

#include <memory>
#include <utility>

namespace NQdb {
namespace NBuild {

namespace NAst = NQumir::NAst;
using NQumir::TError;
using namespace NAst::NLiterals;

namespace {

// Attaches a subquery to the block's tree (section 3.4, step 2 of the canonical
// translation) and returns what the expression reads instead: the marker of a
// mark join for EXISTS and IN, the value of a single join for a scalar subquery.
std::expected<NAst::TExprPtr, TError> AttachSubquery(TBlock& block, const NSql::TSubqueryExpr& subquery, TContext& context) {
    using EKind = NSql::TSubqueryExpr::EKind;
    // Named before the body is built, so markers are numbered in query order.
    const bool scalar = subquery.Kind == EKind::Scalar;
    auto column = context.Names.Next(scalar ? "__scalar" : "__mark");
    auto value = subquery.Kind == EKind::In
        ? context.Names.Next("__in")
        : column;
    TBlock inner{.Id = context.NextScope++, .Parent = &block};
    auto body = BuildQuery(*subquery.Query, inner, context);
    if (!body) {
        return std::unexpected(body.error());
    }
    auto kind = EDependentJoinKind::Mark;
    NAst::TExprPtr predicate;
    if (subquery.Kind != EKind::Exists) {
        if (ColumnNames(body->Plan).size() != 1) {
            return std::unexpected(TError(subquery.Location, "subquery must return exactly one column"));
        }
        auto renamed = RenameOutputs(std::move(body->Plan), {value}, context.Annotation);
        if (!renamed) {
            return std::unexpected(renamed.error());
        }
        body->Plan = std::move(*renamed);
    }
    if (scalar) {
        kind = EDependentJoinKind::Single;
    } else if (subquery.Kind == EKind::In) {
        predicate = std::make_shared<NAst::TBinaryExpr>(subquery.Location, "=="_op,
            subquery.Operand, Ident(subquery.Location, value));
    }
    auto attached = AttachDependent(block.Tree, block.Id, std::move(*body), kind, std::move(predicate),
        scalar ? "" : column, subquery.Location, context.Annotation);
    if (!attached) {
        return std::unexpected(attached.error());
    }
    return Ident(subquery.Location, std::move(column));
}

} // namespace

NAst::TExprPtr FindNode(const NAst::TExprPtr& expr, const std::function<bool(const NAst::TExprPtr&)>& match) {
    if (!expr) {
        return nullptr;
    }
    if (match(expr)) {
        return expr;
    }
    for (const auto& child : expr->Children()) {
        if (auto found = FindNode(child, match)) {
            return found;
        }
    }
    return nullptr;
}

bool IsSubquery(const NAst::TExprPtr& expr) {
    return NAst::TMaybeNode<NSql::TSubqueryExpr>(expr);
}

bool IsOuterRef(const NAst::TExprPtr& expr) {
    return NAst::TMaybeNode<TOuterRefExpr>(expr);
}

std::expected<void, TError> RejectSubqueries(const NAst::TExprPtr& expr, const std::string& clause) {
    if (auto subquery = FindNode(expr, IsSubquery)) {
        return std::unexpected(TError(subquery->Location, "subqueries in " + clause + " are not supported yet"));
    }
    return {};
}

TExprTranslator::TExprTranslator(TBlock& block, TContext& context, bool bind, bool attach)
    : Block_(block)
    , Context_(context)
    , Bind_(bind)
    , Attach_(attach)
{ }

std::expected<NAst::TExprPtr, TError> TExprTranslator::Translate(const NAst::TExprPtr& expr) {
    return Walk(expr, Attach_);
}

std::expected<NAst::TExprPtr, TError> TExprTranslator::Walk(const NAst::TExprPtr& node, bool attach) {
    if (!node) {
        return node;
    }
    if (auto done = Done_.find(node.get()); done != Done_.end()) {
        return done->second;
    }
    auto translated = Visit(node, attach);
    if (translated) {
        Done_.emplace(node.get(), *translated);
    }
    return translated;
}

std::expected<NAst::TExprPtr, TError> TExprTranslator::Visit(const NAst::TExprPtr& node, bool attach) {
    if (auto maybeIdent = NAst::TMaybeNode<NAst::TIdentExpr>(node)) {
        auto ident = maybeIdent.Cast();
        // count(*) takes a synthetic `*` argument.
        if (!Bind_ || ident->Name == "*") {
            return node;
        }
        return Resolve(Block_, ident, Aggregates_ > 0);
    }
    if (NAst::TMaybeNode<NSql::TWindowExpr>(node)) {
        return std::unexpected(TError(node->Location, "window functions are not supported by the new builder yet"));
    }
    if (auto maybeSubquery = NAst::TMaybeNode<NSql::TSubqueryExpr>(node)) {
        auto subquery = maybeSubquery.Cast();
        auto operand = Walk(subquery->Operand, attach);
        if (!operand) {
            return operand;
        }
        subquery->Operand = std::move(*operand);
        if (!attach) {
            return node;
        }
        return AttachSubquery(Block_, *subquery, Context_);
    }
    if (auto maybeCall = NAst::TMaybeNode<NAst::TCallExpr>(node)) {
        // A call's callee names a function, not a column.
        auto call = maybeCall.Cast();
        const bool aggregate = Bind_ && IsAggregateCall(node);
        Aggregates_ += aggregate;
        for (auto& argument : call->Args) {
            auto translated = Walk(argument, attach || aggregate);
            if (!translated) {
                return translated;
            }
            argument = std::move(*translated);
        }
        Aggregates_ -= aggregate;
        // SQL gives an aggregate over outer columns only to the enclosing query,
        // and aggregate slots are keyed by printed arguments.
        if (aggregate && FindNode(node, IsOuterRef)) {
            return std::unexpected(TError(node->Location,
                "an aggregate over columns of an enclosing query is not supported yet"));
        }
        return node;
    }
    for (auto* child : node->MutableChildren()) {
        auto translated = Walk(*child, attach);
        if (!translated) {
            return translated;
        }
        *child = std::move(*translated);
    }
    return node;
}

std::expected<NAst::TExprPtr, TError> BindOver(TBlock& block, TRelation relation, const NAst::TExprPtr& expr, TContext& context) {
    auto saved = std::exchange(block.Tree, std::move(relation));
    auto bound = TExprTranslator(block, context, true, false).Translate(expr);
    block.Tree = std::move(saved);
    return bound;
}

} // namespace NBuild
} // namespace NQdb
