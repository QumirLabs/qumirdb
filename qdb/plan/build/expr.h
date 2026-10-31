#pragma once

#include <qdb/plan/build/binder.h>
#include <qdb/plan/build/context.h>
#include <qdb/plan/build/relation.h>

#include <qumir/error.h>
#include <qumir/parser/ast.h>

#include <expected>
#include <functional>
#include <string>
#include <unordered_map>

namespace NQdb {
namespace NBuild {

// The first node of `expr` that `match` accepts; the bodies of subqueries are
// other blocks and are not searched.
NQumir::NAst::TExprPtr FindNode(
    const NQumir::NAst::TExprPtr& expr,
    const std::function<bool(const NQumir::NAst::TExprPtr&)>& match);

bool IsSubquery(const NQumir::NAst::TExprPtr& expr);

bool IsOuterRef(const NQumir::NAst::TExprPtr& expr);

std::expected<void, NQumir::TError> RejectSubqueries(const NQumir::NAst::TExprPtr& expr, const std::string& clause);

// Translates the expressions of a clause in place: binds their names and
// attaches their subqueries to the block's tree. A subtree the parser shares,
// such as the operand of BETWEEN, is translated once.
class TExprTranslator {
public:
    // `bind` resolves names. `attach` attaches the subqueries outside aggregate
    // calls; those in an aggregate's arguments are attached while binding, since
    // the arguments are evaluated per input row, below the aggregate.
    TExprTranslator(TBlock& block, TContext& context, bool bind, bool attach);

    std::expected<NQumir::NAst::TExprPtr, NQumir::TError> Translate(const NQumir::NAst::TExprPtr& expr);

private:
    std::expected<NQumir::NAst::TExprPtr, NQumir::TError> Walk(const NQumir::NAst::TExprPtr& node, bool attach);
    std::expected<NQumir::NAst::TExprPtr, NQumir::TError> Visit(const NQumir::NAst::TExprPtr& node, bool attach);

    TBlock& Block_;
    TContext& Context_;
    bool Bind_;
    bool Attach_;
    // The aggregate calls the walk is in.
    int Aggregates_ = 0;
    std::unordered_map<const NQumir::NAst::TExpr*, NQumir::NAst::TExprPtr> Done_;
};

// Binds an expression that reads one relation, such as a JOIN condition.
std::expected<NQumir::NAst::TExprPtr, NQumir::TError> BindOver(
    TBlock& block,
    TRelation relation,
    const NQumir::NAst::TExprPtr& expr,
    TContext& context);

} // namespace NBuild
} // namespace NQdb
