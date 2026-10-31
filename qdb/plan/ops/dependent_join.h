#pragma once

#include <qdb/kernel/annotate_type.h>
#include <qdb/plan/ops/operator.h>

#include <qumir/error.h>
#include <qumir/parser/core/printer.h>

#include <array>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace NQdb {

// The join variants of the canonical translation of SQL (Neumann, Leis and
// Kemper, "The Complete Story of Joins (in HyPer)", BTW 2017, sections 2-3).
enum class EDependentJoinKind {
    Inner,
    // Unmatched left rows are kept with NULL right columns.
    Left,
    Semi,
    Anti,
    // Every left row once, followed by a marker: TRUE when the predicate holds
    // for some right row, otherwise NULL when it is NULL for some right row,
    // otherwise FALSE. Without a predicate the marker is whether the right side
    // is empty. EXISTS and IN of any polarity become a mark join; NOT IN keeps
    // its three-valued result as the negated marker.
    Mark,
    // A left join that requires at most one right row per left row: a scalar
    // subquery. More than one row is a runtime error.
    Single,
};

std::string_view DependentJoinKindName(EDependentJoinKind kind);

// A join whose right side is evaluated for every left row and may read that
// row's columns through TOuterRefExpr (Neumann and Kemper, "Unnesting
// Arbitrary Queries", BTW 2015). The SQL builder of the new pipeline attaches
// every subquery and every correlated FROM item with it; unnesting is to
// replace it with ordinary joins. It never executes.
//
// `Scope` is the query block whose tree is the left input: an outer reference
// of that scope inside the right side reads the current left row.
class TDependentJoinOperator : public IOperator {
public:
    static constexpr const char* OpId = "dependent-join";

    // `predicate` may be null (TRUE). `marker` names the marker column of a
    // mark join and is empty otherwise.
    TDependentJoinOperator(
        TOperatorPtr left,
        TOperatorPtr right,
        EDependentJoinKind kind,
        uint32_t scope,
        NQumir::NAst::TExprPtr predicate,
        std::string marker,
        const NKernel::TAnnotationContext& annotation = {});

    std::string_view RelName() const override { return OpId; }
    std::vector<TColumnReference> ColumnReferences() const override;
    // The outer references of the right side are not tracked as column demand
    // yet, so each input keeps all of its columns.
    std::unordered_set<std::string> RequiredColumnsForChild(
        size_t childIdx, const std::unordered_set<std::string>& needed) const override;
    std::vector<NQumir::NAst::TExprPtr> Children() const override { return {Left(), Right()}; }
    const std::string ToString() const override;

    TOperatorPtr Left() const { return Inputs_[0]; }
    TOperatorPtr Right() const { return Inputs_[1]; }
    EDependentJoinKind Kind() const { return Kind_; }
    uint32_t Scope() const { return Scope_; }
    const NQumir::NAst::TExprPtr& Predicate() const { return Predicate_; }
    const std::string& Marker() const { return Marker_; }

    std::span<const TOperatorPtr> Inputs() const override {
        return std::span<const TOperatorPtr>(Inputs_);
    }

    std::span<TOperatorPtr> MutableInputs() override {
        return std::span<TOperatorPtr>(Inputs_);
    }

private:
    std::array<TOperatorPtr, 2> Inputs_{nullptr, nullptr};
    EDependentJoinKind Kind_;
    uint32_t Scope_;
    NQumir::NAst::TExprPtr Predicate_;
    std::string Marker_;
};

// Output schema by kind: Inner left ++ right; Left and Single left ++
// nullable(right); Semi and Anti left; Mark left ++ the marker, nullable when
// the predicate may be NULL. An output name on both sides is an error.
std::expected<NQumir::NAst::TTypePtr, NQumir::TError> ComputeDependentJoinOutputType(
    const NQumir::NAst::TTypePtr& left,
    const NQumir::NAst::TTypePtr& right,
    EDependentJoinKind kind,
    const NQumir::NAst::TExprPtr& predicate,
    const std::string& marker,
    const NKernel::TAnnotationContext& annotation = {});

// A column of the left input of an enclosing dependent join, read inside its
// right side: a free variable of the subtree. `Column` is a physical column of
// the tree of query block `Scope` where the dependent join attaches. The node
// carries the column's type, which expression typing takes for it.
struct TOuterRefExpr : NQumir::NAst::TExpr {
    static constexpr const char* NodeId = "OuterRef";

    uint32_t Scope = 0;
    std::string Column;

    TOuterRefExpr(NQumir::TLocation location, uint32_t scope, std::string column, NQumir::NAst::TTypePtr type)
        : NQumir::NAst::TExpr(std::move(location), std::move(type))
        , Scope(scope)
        , Column(std::move(column))
    { }

    const std::string_view NodeName() const override { return NodeId; }
    const std::string ToString() const override;
    void Accept(NQumir::NAst::IVisitor& visitor) override { visitor.VisitOtherwise(*this); }
};

// Prints an outer reference as `(outer <scope> <column>)`, for the node printers
// of plan explain output.
void PrintOuterRef(
    NQumir::NAst::TExpr& expr,
    NQumir::NAst::NCore::TPrinter& printer,
    NQumir::NAst::NCore::TPrintFrame frame);

// Fails at the first dependent join of the plan, CTE definitions included: the
// optimizer passes and execution need an unnested plan.
std::expected<void, NQumir::TError> RequireNoDependentJoins(const TOperatorPtr& plan);

} // namespace NQdb
