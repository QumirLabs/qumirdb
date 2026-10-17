#pragma once

#include <qdb/plan/build.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/project.h>

#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace NQdb {
namespace NBuild {

// Builds one dependent FROM item over a distinct outer-parameter domain.
class TDependentTableBuilder {
public:
    explicit TDependentTableBuilder(const TTableSourceFactory& sources);

    std::expected<std::optional<TOperatorPtr>, NQumir::TError> Build(
        const NSql::TSqlPtr<NSql::TSqlTableRef>& ref,
        TOperatorPtr left,
        EJoinType type,
        NQumir::NAst::TExprPtr condition = nullptr);

private:
    static void DetachTypes(const TOperatorPtr& op);
    static TOperatorPtr ClonePlan(const TOperatorPtr& op);
    static std::optional<std::string> Resolve(
        const NQumir::NAst::TConcreteTypePtr<NQumir::NAst::TStructType>& schema,
        const std::string& name);
    std::unordered_set<std::string> CollectDeclaredColumns(
        const NSql::TSqlPtr<NSql::TSqlTableRef>& table) const;
    static bool IsDeterministic(const NQumir::NAst::TExprPtr& expr);
    static bool IsDeterministicPlan(const TOperatorPtr& op);
    static bool ContainsAggregate(const NQumir::NAst::TExprPtr& expr);
    void GuardAggregateArguments(NQumir::NAst::TExprPtr& node);
    NQumir::NAst::TExprPtr RewriteExpression(const NQumir::NAst::TExprPtr& expr);
    TOperatorPtr BuildInput() const;

    const TTableSourceFactory& Sources_;
    TOperatorPtr Base_;
    TOperatorPtr Domain_;
    NQumir::NAst::TConcreteTypePtr<NQumir::NAst::TStructType> LocalSchema_;
    std::string Prefix_;
    std::string Marker_;
    std::unordered_map<std::string, std::string> Rename_;
    std::vector<std::string> ParameterColumns_;
    std::vector<TJoinKey> InputKeys_;
    NQumir::NAst::TExprPtr Predicate_;
    bool GlobalAggregate_ = false;
    bool ConstantKey_ = false;
    std::vector<TProjectionSpec> GuardedArguments_;
    std::unordered_map<std::string, std::string> ArgumentNames_;
};

} // namespace NBuild
} // namespace NQdb
