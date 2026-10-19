#include <qdb/plan/dependent_table_builder.h>

#include <qdb/plan/build_internal.h>
#include <qdb/plan/clone_expr.h>
#include <qdb/plan/clone_operator.h>
#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/cte_ref.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/window.h>
#include <qdb/plan/passes/flatten_conjuncts.h>
#include <qdb/plan/passes/qualify_columns.h>
#include <qdb/plan/passes/unbound_vars.h>

#include <qumir/parser/core/printer.h>

#include <algorithm>
#include <map>
#include <set>
#include <utility>

namespace NQdb {
namespace NBuild {

namespace NAst = NQumir::NAst;
using NQumir::TError;

TDependentTableBuilder::TDependentTableBuilder(const TTableSourceFactory& sources)
    : Sources_(sources)
{}

void TDependentTableBuilder::DetachTypes(const TOperatorPtr& op) {
    op->Type = std::make_shared<NAst::TFunctionType>(
        *static_cast<NAst::TFunctionType*>(op->Type.get()));
    for (const auto& child : op->Inputs()) {
        DetachTypes(child);
    }
}

TOperatorPtr TDependentTableBuilder::ClonePlan(const TOperatorPtr& op) {
    // Scope binding mutates cached schemas, so each private copy owns its types.
    auto copy = CloneOperator(op);
    DetachTypes(copy);
    return copy;
}

std::optional<std::string> TDependentTableBuilder::Resolve(
    const NAst::TConcreteTypePtr<NAst::TStructType>& schema,
    const std::string& name)
{
    for (const auto& [field, _] : schema->Fields) {
        if (field == name) {
            return field;
        }
    }
    if (name.find('.') != std::string::npos) {
        return std::nullopt;
    }
    std::optional<std::string> found;
    for (const auto& [field, _] : schema->Fields) {
        auto dot = field.rfind('.');
        if (field.substr(dot == std::string::npos ? 0 : dot + 1) == name) {
            if (found) {
                return std::nullopt;
            }
            found = field;
        }
    }
    return found;
}

std::unordered_set<std::string> TDependentTableBuilder::CollectDeclaredColumns(
    const NSql::TSqlPtr<NSql::TSqlTableRef>& table) const
{
    // Discover local names without building and rewriting the SQL AST twice.
    std::unordered_set<std::string> names;
    if (auto subTable = NSql::TMaybeNode<NSql::TSqlSubqueryTable>(table)) {
        auto node = subTable.Cast();
        std::set<std::string> outputs;
        if (node->ColumnAliases) {
            outputs.insert(node->ColumnAliases->Items.begin(), node->ColumnAliases->Items.end());
        } else if (auto set = NSql::TMaybeNode<NSql::TSqlSetOp>(node->Query->Body)) {
            auto first = std::make_shared<NSql::TSqlSubqueryTable>(*node);
            first->Query = std::make_shared<NSql::TSqlQuery>(*node->Query);
            first->Query->Body = set.Cast()->Left;
            return CollectDeclaredColumns(first);
        } else if (auto select = NSql::TMaybeNode<NSql::TSqlSelect>(node->Query->Body)) {
            for (size_t i = 0; i < select.Cast()->SelectList->Items.size(); ++i) {
                const auto& item = select.Cast()->SelectList->Items[i];
                if (!item->ColumnAliases.empty()) {
                    outputs.insert(item->ColumnAliases.begin(), item->ColumnAliases.end());
                } else if (!item->Star) {
                    outputs.insert(ItemName(*item, i));
                } else if (select.Cast()->From) {
                    std::string starPrefix;
                    for (const auto& part : item->StarPrefix) {
                        starPrefix += part + ".";
                    }
                    for (const auto& input : select.Cast()->From->Items) {
                        for (const auto& name : CollectDeclaredColumns(input)) {
                            if (starPrefix.empty() || name.starts_with(starPrefix)) {
                                const auto dot = name.rfind('.');
                                outputs.insert(name.substr(dot == std::string::npos ? 0 : dot + 1));
                            }
                        }
                    }
                }
            }
        }
        for (const auto& name : outputs) {
            names.insert(name);
            if (node->Alias) {
                names.insert(*node->Alias + "." + name);
            }
        }
    } else if (auto join = NSql::TMaybeNode<NSql::TSqlJoin>(table)) {
        names = CollectDeclaredColumns(join.Cast()->Left);
        auto right = CollectDeclaredColumns(join.Cast()->Right);
        names.insert(right.begin(), right.end());
    } else {
        CollectLocalColumns(table, Sources_, names);
    }
    return names;
}

std::expected<void, TError> TDependentTableBuilder::ValidateJoinConditions(
    const NSql::TSqlPtr<NSql::TSqlTableRef>& table,
    const NAst::TConcreteTypePtr<NAst::TStructType>& outerSchema) const
{
    auto maybeJoin = NSql::TMaybeNode<NSql::TSqlJoin>(table);
    if (!maybeJoin) {
        return {};
    }
    const auto join = maybeJoin.Cast();
    for (const auto& input : {join->Left, join->Right}) {
        if (auto valid = ValidateJoinConditions(input, outerSchema); !valid) {
            return valid;
        }
    }
    if (join->Condition && join->Condition->On) {
        // ON sees this join's operands, not sibling FROM items. Moving a
        // correlated ON to the domain join would change null extension.
        const auto local = CollectDeclaredColumns(table);
        for (const auto& name : FindUnboundVars(join->Condition->On)) {
            if (!local.contains(name) && Resolve(outerSchema, name)) {
                return std::unexpected(TError(
                    "outer references in JOIN ON inside dependent FROM are not supported: " + name));
            }
        }
    }
    return {};
}

bool TDependentTableBuilder::IsDeterministic(const NAst::TExprPtr& expr) {
    // The catalog has no volatility contract; domain copies require known calls.
    static const std::set<std::string> DeterministicCalls = {
        "distinct", "coalesce", "qdb_is_null", "qdb_is_true", "qdb_sql_null",
        "qdb_in_list", "qdb_substring", "qdb_date_year", "qdb_string_view_sql_like",
        "qdb_sqrt"};
    if (!expr) {
        return true;
    }
    if (auto call = NAst::TMaybeNode<NAst::TCallExpr>(expr)) {
        auto callee = NAst::TMaybeNode<NAst::TIdentExpr>(call.Cast()->Callee);
        if (!callee || (!IsAggFunc(ToLower(callee.Cast()->Name))
            && !DeterministicCalls.contains(ToLower(callee.Cast()->Name))))
        {
            return false;
        }
        return std::ranges::all_of(call.Cast()->Args, IsDeterministic);
    }
    return std::ranges::all_of(expr->Children(), IsDeterministic);
}

bool TDependentTableBuilder::IsDeterministicPlan(const TOperatorPtr& op) {
    if (auto project = TMaybeOp<TProjectOperator>(op)) {
        for (const auto& spec : project.Cast()->Projections()) {
            if (!IsDeterministic(spec.Expression)) {
                return false;
            }
        }
    } else if (auto filter = TMaybeOp<TFilterOperator>(op)) {
        if (!IsDeterministic(filter.Cast()->Predicate())) {
            return false;
        }
    } else if (auto aggregate = TMaybeOp<TAggregateOperator>(op)) {
        for (const auto& spec : aggregate.Cast()->Aggs()) {
            if (!IsDeterministic(spec.Arg)) {
                return false;
            }
        }
    } else if (auto join = TMaybeOp<TJoinOperator>(op)) {
        if (!IsDeterministic(join.Cast()->Filter())) {
            return false;
        }
    } else if (auto cte = TMaybeOp<TCteRef>(op)) {
        return IsDeterministicPlan(cte.Cast()->Def()->Plan);
    } else if (TMaybeOp<TWindowOperator>(op)) {
        return false;
    }
    return std::ranges::all_of(op->Inputs(), IsDeterministicPlan);
}

bool TDependentTableBuilder::ContainsAggregate(const NAst::TExprPtr& expr) {
    if (!expr) {
        return false;
    }
    if (AsAggCall(expr)) {
        return true;
    }
    return std::ranges::any_of(expr->Children(), ContainsAggregate);
}

void TDependentTableBuilder::GuardAggregateArguments(NAst::TExprPtr& node) {
    if (auto agg = AsAggCall(node)) {
        auto call = NAst::TMaybeNode<NAst::TCallExpr>(node).Cast();
        if (agg->Star) {
            call->Args = {Ident(node->Location, Marker_)};
        } else if (agg->Arg) {
            auto argument = std::make_shared<NAst::TIfExpr>(node->Location,
                Call(node->Location, "qdb_is_null", {Ident(node->Location, Marker_)}),
                Call(node->Location, "qdb_sql_null", {}), agg->Arg);
            auto [it, inserted] = ArgumentNames_.try_emplace(NAst::NCore::PrintAst(argument));
            if (inserted) {
                it->second = Prefix_ + "argument_" + std::to_string(GuardedArguments_.size());
                GuardedArguments_.push_back({.Name = it->second, .Expression = argument});
            }
            call->Args = {agg->Distinct
                ? Call(node->Location, "distinct", {Ident(node->Location, it->second)})
                : Ident(node->Location, it->second)};
        }
        return;
    }
    for (auto* child : node->MutableChildren()) {
        GuardAggregateArguments(*child);
    }
}

NAst::TExprPtr TDependentTableBuilder::RewriteExpression(const NAst::TExprPtr& expr) {
    if (!expr) {
        return nullptr;
    }
    auto copy = CloneExpr(expr);
    RenameIdents(copy, Rename_);
    if (GlobalAggregate_) {
        GuardAggregateArguments(copy);
    }
    return copy;
}

TOperatorPtr TDependentTableBuilder::BuildInput() const {
    auto right = ClonePlan(Base_);
    if (GlobalAggregate_ || ConstantKey_) {
        std::vector<TProjectionSpec> marked;
        for (const auto& [name, _] : LocalSchema_->Fields) {
            marked.push_back({.Name = name, .Expression = Ident({}, name)});
        }
        marked.push_back({.Name = Marker_, .Expression = I64Literal({}, 1)});
        if (ConstantKey_) {
            marked.push_back({.Name = Prefix_ + "right_key", .Expression = I64Literal({}, 0)});
        }
        right = std::make_shared<TProjectOperator>(right, std::move(marked));
    }
    auto parameterInput = ClonePlan(Domain_);
    if (ConstantKey_) {
        std::vector<TProjectionSpec> columns;
        for (const auto& name : ParameterColumns_) {
            columns.push_back({.Name = name, .Expression = Ident({}, name)});
        }
        columns.push_back({.Name = Prefix_ + "left_key", .Expression = I64Literal({}, 0)});
        parameterInput = std::make_shared<TProjectOperator>(parameterInput, std::move(columns));
    }
    TOperatorPtr joined = std::make_shared<TJoinOperator>(parameterInput, right,
        InputKeys_, EJoinType::Inner,
        Predicate_ ? CloneExpr(Predicate_) : nullptr);
    if (GlobalAggregate_) {
        // Residual evaluation belongs to the ordinary INNER join. Restore
        // empty parameter groups with a separate key-only LEFT join; this
        // also works when the correlated predicate is not an equality.
        std::vector<TProjectionSpec> matches;
        std::vector<TJoinKey> identity;
        for (size_t i = 0; i < ParameterColumns_.size(); ++i) {
            const auto& name = ParameterColumns_[i];
            const auto matched = Prefix_ + "matched_parameter_" + std::to_string(i);
            matches.push_back({.Name = matched, .Expression = Ident({}, name)});
            identity.push_back({.Left = name, .Right = matched, .NullsEqual = true});
        }
        for (const auto& [name, _] : LocalSchema_->Fields) {
            matches.push_back({.Name = name, .Expression = Ident({}, name)});
        }
        matches.push_back({.Name = Marker_, .Expression = Ident({}, Marker_)});
        joined = std::make_shared<TJoinOperator>(ClonePlan(Domain_),
            std::make_shared<TProjectOperator>(joined, std::move(matches)),
            std::move(identity), EJoinType::Left, nullptr);
    }
    if (!GuardedArguments_.empty()) {
        std::vector<TProjectionSpec> arguments;
        for (const auto& [name, _] : joined->OutputColumns()->Fields) {
            arguments.push_back({.Name = name, .Expression = Ident({}, name)});
        }
        for (const auto& argument : GuardedArguments_) {
            arguments.push_back({.Name = argument.Name, .Expression = CloneExpr(argument.Expression)});
        }
        joined = std::make_shared<TProjectOperator>(joined, std::move(arguments));
    }
    return joined;
}

// A dependent FROM item is evaluated once per distinct outer parameter tuple.
// Joining that result back to the original input restores its bag multiplicity.
std::expected<std::optional<TOperatorPtr>, TError> TDependentTableBuilder::Build(
    const NSql::TSqlPtr<NSql::TSqlTableRef>& ref,
    TOperatorPtr left,
    EJoinType type,
    NAst::TExprPtr condition)
{
    GuardedArguments_.clear();
    ArgumentNames_.clear();
    auto maybeSub = NSql::TMaybeNode<NSql::TSqlSubqueryTable>(ref);
    if (!maybeSub) {
        return std::optional<TOperatorPtr>{};
    }
    auto sub = maybeSub.Cast();
    if (sub->Query->WithClause) {
        if (sub->Lateral) {
            return std::unexpected(TError("LATERAL with a local WITH clause is not supported"));
        }
        return std::optional<TOperatorPtr>{};
    }
    auto maybeSelect = NSql::TMaybeNode<NSql::TSqlSelect>(sub->Query->Body);
    if (!maybeSelect || !maybeSelect.Cast()->From) {
        if (sub->Lateral) {
            return std::unexpected(TError("LATERAL requires a SELECT with FROM"));
        }
        return std::optional<TOperatorPtr>{};
    }
    auto original = maybeSelect.Cast();
    left = ClonePlan(left);
    // Bind each relation within its own scope before introducing the domain.
    // Otherwise a bare local column could become ambiguous with an outer column.
    AssignSourceAliases(left);
    QualifyColumns(left);
    auto outerSchema = left->OutputColumns();
    if (!outerSchema) {
        return std::unexpected(TError("dependent FROM requires resolved input schemas"));
    }
    for (const auto& table : original->From->Items) {
        if (auto valid = ValidateJoinConditions(table, outerSchema); !valid) {
            return std::unexpected(valid.error());
        }
    }
    std::vector<NAst::TExprPtr> expressions{original->Where, original->Having};
    for (const auto& item : original->SelectList->Items) {
        if (!item->Star) {
            expressions.push_back(item->Expr);
        }
    }
    if (original->GroupBy) {
        for (const auto& item : original->GroupBy->Items) {
            if (auto group = NSql::TMaybeNode<NSql::TSqlGroupingExprOrList>(item)) {
                expressions.push_back(group.Cast()->Exprs);
            }
        }
    }
    if (sub->Query->OrderBy) {
        for (const auto& item : sub->Query->OrderBy->Items) {
            expressions.push_back(item->Expr);
        }
    }
    std::unordered_set<std::string> localDeclarations;
    for (const auto& table : original->From->Items) {
        auto names = CollectDeclaredColumns(table);
        localDeclarations.insert(names.begin(), names.end());
    }
    bool possiblyDependent = false;
    for (const auto& expr : expressions) {
        for (const auto& name : FindUnboundVars(expr)) {
            possiblyDependent |= !localDeclarations.contains(name) && Resolve(outerSchema, name).has_value();
        }
    }
    if (!possiblyDependent) {
        return std::optional<TOperatorPtr>{};
    }
    auto base = BuildFrom(*original->From, Sources_);
    if (!base) {
        return std::unexpected(base.error());
    }
    AssignSourceAliases(*base);
    QualifyColumns(*base);
    Base_ = *base;
    LocalSchema_ = Base_->OutputColumns();
    if (!LocalSchema_) {
        return std::unexpected(TError("dependent FROM requires resolved input schemas"));
    }
    std::map<std::string, std::string> outerRefs;
    std::unordered_map<std::string, std::string> localRefs;
    std::set<std::string> referenced;
    for (size_t i = 0; i < original->SelectList->Items.size(); ++i) {
        referenced.insert(ItemName(*original->SelectList->Items[i], i));
    }
    if (sub->ColumnAliases) {
        referenced.insert(sub->ColumnAliases->Items.begin(), sub->ColumnAliases->Items.end());
    }
    for (const auto& expr : expressions) {
        for (const auto& name : FindUnboundVars(expr)) {
            referenced.insert(name);
            if (auto local = Resolve(LocalSchema_, name)) {
                localRefs[name] = *local;
            } else if (name.find('.') == std::string::npos
                && std::ranges::any_of(LocalSchema_->Fields, [&](const auto& field) {
                    const auto dot = field.first.rfind('.');
                    return field.first.substr(dot == std::string::npos ? 0 : dot + 1) == name;
                }))
            {
                return std::unexpected(TError("ambiguous local column in dependent FROM: " + name));
            } else if (auto outer = Resolve(outerSchema, name)) {
                outerRefs[name] = *outer;
            }
        }
    }
    if (outerRefs.empty()) {
        return std::optional<TOperatorPtr>{};
    }
    if (!std::ranges::all_of(expressions, IsDeterministic)
        || !IsDeterministicPlan(left) || !IsDeterministicPlan(*base))
    {
        return std::unexpected(TError(
            "dependent FROM requires deterministic inputs and known deterministic SQL functions"));
    }
    if (type != EJoinType::Inner && type != EJoinType::Left) {
        return std::unexpected(TError("dependent FROM supports INNER, CROSS and LEFT joins"));
    }
    if (sub->Query->WithClause || sub->Query->OrderBy || sub->Query->Limit || sub->Query->Offset
        || HasWindowExpr(*sub->Query))
    {
        return std::unexpected(TError(
            "dependent FROM with WITH, ORDER BY, LIMIT, OFFSET or windows is not supported"));
    }
    for (const auto& expr : expressions) {
        if (HasSubquery(expr)) {
            return std::unexpected(TError("nested subqueries inside dependent FROM are not supported"));
        }
    }
    if (original->GroupBy) {
        for (const auto& item : original->GroupBy->Items) {
            if (!NSql::TMaybeNode<NSql::TSqlGroupingExprOrList>(item)) {
                return std::unexpected(TError("grouping sets inside dependent FROM are not supported"));
            }
        }
    }
    Prefix_ = "__lateral_";
    auto used = [&](const std::string& name) {
        return referenced.contains(name) || Resolve(outerSchema, name) || Resolve(LocalSchema_, name);
    };
    while (std::ranges::any_of(referenced, [&](const auto& name) { return name.starts_with(Prefix_); })) {
        Prefix_ += "_";
    }
    std::vector<TProjectionSpec> parameters;
    std::vector<TJoinKey> keys;
    Rename_ = localRefs;
    std::map<std::string, std::string> parameterNames;
    for (const auto& [refName, outerName] : outerRefs) {
        auto [it, inserted] = parameterNames.try_emplace(outerName);
        if (inserted) {
            it->second = Prefix_ + "parameter_" + std::to_string(parameters.size());
            while (used(it->second)) {
                it->second += "_";
            }
            parameters.push_back({.Name = it->second, .Expression = Ident({}, outerName)});
            keys.push_back({.Left = outerName, .Right = it->second, .NullsEqual = true});
        }
        Rename_[refName] = it->second;
    }
    ParameterColumns_.clear();
    for (const auto& parameter : parameters) {
        ParameterColumns_.push_back(parameter.Name);
    }
    Domain_ = ApplyDistinct(std::make_shared<TProjectOperator>(
        ClonePlan(left), std::move(parameters)), ParameterColumns_);
    const bool aggregated = original->GroupBy || std::ranges::any_of(expressions, ContainsAggregate);
    GlobalAggregate_ = aggregated && !original->GroupBy;
    Marker_ = Prefix_ + "matched";
    while (used(Marker_)) {
        Marker_ += "_";
    }
    // A global aggregate must see one placeholder for an empty parameter group.
    // Guard every aggregate argument so this placeholder is never a SQL input row,
    // including COUNT(*) and aggregates over constants or outer-only expressions.
    NSql::TSqlSelect select = *original;
    select.Where = nullptr;
    select.Having = RewriteExpression(original->Having);
    select.SelectList = std::make_shared<NSql::TSqlSelectList>();
    for (size_t i = 0; i < original->SelectList->Items.size(); ++i) {
        const auto& item = original->SelectList->Items[i];
        if (item->Star) {
            std::string starPrefix;
            for (const auto& part : item->StarPrefix) {
                starPrefix += part + ".";
            }
            for (const auto& [name, _] : LocalSchema_->Fields) {
                if (starPrefix.empty() || name.starts_with(starPrefix)) {
                    auto expanded = std::make_shared<NSql::TSqlSelectItem>();
                    expanded->Expr = Ident({}, name);
                    auto dot = name.rfind('.');
                    expanded->Alias = name.substr(dot == std::string::npos ? 0 : dot + 1);
                    select.SelectList->Items.push_back(expanded);
                }
            }
        } else {
            auto copy = std::make_shared<NSql::TSqlSelectItem>(*item);
            copy->Expr = RewriteExpression(item->Expr);
            select.SelectList->Items.push_back(copy);
        }
    }
    for (const auto& name : ParameterColumns_) {
        auto item = std::make_shared<NSql::TSqlSelectItem>();
        item->Expr = Ident({}, name);
        item->Alias = name;
        select.SelectList->Items.push_back(item);
    }
    if (aggregated) {
        select.GroupBy = std::make_shared<NSql::TSqlGroupBy>();
        for (const auto& name : ParameterColumns_) {
            select.GroupBy->Items.push_back(std::make_shared<NSql::TSqlGroupingExprOrList>(Ident({}, name)));
        }
        if (original->GroupBy) {
            for (const auto& item : original->GroupBy->Items) {
                select.GroupBy->Items.push_back(std::make_shared<NSql::TSqlGroupingExprOrList>(
                    RewriteExpression(NSql::TMaybeNode<NSql::TSqlGroupingExprOrList>(item).Cast()->Exprs)));
            }
        }
    }
    Predicate_ = original->Where ? CloneExpr(original->Where) : nullptr;
    RenameIdents(Predicate_, Rename_);
    InputKeys_.clear();
    std::vector<NAst::TExprPtr> conjuncts;
    FlattenConjuncts(Predicate_, conjuncts);
    for (const auto& conjunct : conjuncts) {
        auto eq = NAst::TMaybeNode<NAst::TBinaryExpr>(conjunct);
        if (!eq || eq.Cast()->Operator != "==") {
            continue;
        }
        auto a = NAst::TMaybeNode<NAst::TIdentExpr>(eq.Cast()->Left);
        auto b = NAst::TMaybeNode<NAst::TIdentExpr>(eq.Cast()->Right);
        if (!a || !b) {
            continue;
        }
        auto isParameter = [&](const auto& name) {
            return std::ranges::find(ParameterColumns_, name) != ParameterColumns_.end();
        };
        if (isParameter(a.Cast()->Name) && Resolve(LocalSchema_, b.Cast()->Name)) {
            InputKeys_.push_back({a.Cast()->Name, b.Cast()->Name});
        } else if (isParameter(b.Cast()->Name) && Resolve(LocalSchema_, a.Cast()->Name)) {
            InputKeys_.push_back({b.Cast()->Name, a.Cast()->Name});
        }
    }
    ConstantKey_ = InputKeys_.empty();
    if (ConstantKey_) {
        // The common pipeline needs a key for a non-scalar parameterized join.
        // A constant key preserves arbitrary correlated residuals.
        InputKeys_.push_back({Prefix_ + "left_key", Prefix_ + "right_key"});
    }
    auto built = BuildSelect(select, Sources_, BuildInput());
    if (!built) {
        return std::unexpected(built.error());
    }
    auto names = OutputColumnNames(*built, "dependent FROM");
    if (!names) {
        return std::unexpected(names.error());
    }
    const size_t arity = names->size() - ParameterColumns_.size();
    if (sub->ColumnAliases && sub->ColumnAliases->Items.size() != arity) {
        return std::unexpected(TError("column alias count does not match output columns"));
    }
    std::vector<TProjectionSpec> rightColumns;
    for (const auto& name : ParameterColumns_) {
        rightColumns.push_back({.Name = name, .Expression = Ident({}, name)});
    }
    std::vector<TProjectionSpec> output;
    for (const auto& [name, _] : outerSchema->Fields) {
        output.push_back({.Name = name, .Expression = Ident({}, name)});
    }
    for (size_t i = 0; i < arity; ++i) {
        auto originalName = (*names)[i];
        std::string name = sub->ColumnAliases ? sub->ColumnAliases->Items[i] : originalName;
        if (auto valid = ValidateOutputColumnName(name); !valid) {
            return std::unexpected(valid.error());
        }
        if (sub->Alias) {
            auto dot = name.rfind('.');
            name = *sub->Alias + "." + name.substr(dot == std::string::npos ? 0 : dot + 1);
        }
        rightColumns.push_back({.Name = name, .Expression = Ident({}, originalName)});
        output.push_back({.Name = name, .Expression = Ident({}, name)});
    }
    auto right = std::make_shared<TProjectOperator>(*built, std::move(rightColumns));
    if (type == EJoinType::Left && condition) {
        std::vector<NAst::TExprPtr> on;
        FlattenConjuncts(condition, on);
        for (const auto& conjunct : on) {
            const auto refs = FindUnboundVars(conjunct);
            const bool rightLocal = !refs.empty() && std::ranges::all_of(refs, [&](const auto& name) {
                return Resolve(right->OutputColumns(), name).has_value()
                    && !Resolve(outerSchema, name);
            });
            auto equality = NAst::TMaybeNode<NAst::TBinaryExpr>(conjunct);
            bool equiKey = false;
            if (equality && equality.Cast()->Operator == "==") {
                auto a = NAst::TMaybeNode<NAst::TIdentExpr>(equality.Cast()->Left);
                auto b = NAst::TMaybeNode<NAst::TIdentExpr>(equality.Cast()->Right);
                equiKey = a && b && (
                    (Resolve(outerSchema, a.Cast()->Name) && Resolve(right->OutputColumns(), b.Cast()->Name))
                    || (Resolve(outerSchema, b.Cast()->Name) && Resolve(right->OutputColumns(), a.Cast()->Name)));
            }
            if ((!rightLocal && !equiKey) || !IsDeterministic(conjunct)) {
                return std::unexpected(TError(
                    "LEFT dependent FROM ON requires equi-keys or deterministic right-local predicates"));
            }
        }
    }
    auto joined = std::make_shared<TJoinOperator>(left, right, std::move(keys), type,
        condition ? CloneExpr(condition) : nullptr);
    return std::optional<TOperatorPtr>{std::make_shared<TProjectOperator>(joined, std::move(output))};
}

} // namespace NBuild
} // namespace NQdb
