#pragma once

#include <qdb/plan/ops/operator.h>

#include <qumir/error.h>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace NQdb {

enum class EJoinType {
    Inner,
    Left,       // LEFT OUTER
    Right,      // RIGHT OUTER
    Full,       // FULL OUTER
    LeftSemi,
    RightSemi,
    LeftAnti,
    RightAnti,
};

// Equi-join key: a pair of column names, one from each side
// (left.Left == right.Right is the equality predicate).
struct TJoinKey {
    std::string Left;
    std::string Right;
    // INTERSECT/EXCEPT compare NULLs as equal; ordinary SQL equality does not.
    bool NullsEqual = false;
};

// String <-> enum helpers (used by sexp and ToString).
std::string_view JoinTypeName(EJoinType type);
std::optional<EJoinType> ParseJoinType(std::string_view name);

// Runtime filters require a fixed build side; Auto is invalid.
enum class EJoinFilterSide { Left, Right };

std::string_view JoinFilterSideName(EJoinFilterSide side);
std::optional<EJoinFilterSide> ParseJoinFilterSide(std::string_view name);

// Execution builds the filter; the plan stores only its binding.
struct TRuntimeFilterSpec {
    uint32_t Id = 0;
    EJoinFilterSide BuildSide = EJoinFilterSide::Right;
};

// Non-trivial fields need explicit clone handling.
static_assert(std::is_trivially_copyable_v<TRuntimeFilterSpec>);

class TJoinOperator : public IOperator {
public:
    static constexpr const char* OpId = "join";

    // filter: optional residual predicate θ over the joined (left⊕right) row,
    // applied at the match point BEFORE a pair is emitted. nullptr if none.
    TJoinOperator(TOperatorPtr left, TOperatorPtr right,
        std::vector<TJoinKey> keys, EJoinType type,
        NQumir::NAst::TExprPtr filter);

    std::string_view RelName() const override { return OpId; }
    std::unordered_set<std::string> ComputeReferencedColumns() const override;
    // Per-side split: child i needs its own key columns ∪ (filter vars ∩ side)
    // ∪ (parent-required ∩ side). childIdx 0 = left, 1 = right.
    std::unordered_set<std::string> RequiredColumnsForChild(
        size_t childIdx, const std::unordered_set<std::string>& needed) const override;
    // First operator with TWO children.
    std::vector<NQumir::NAst::TExprPtr> Children() const override { return {Left(), Right()}; }
    const std::string ToString() const override;

    TOperatorPtr Left() const { return Inputs_[0]; }
    TOperatorPtr Right() const { return Inputs_[1]; }
    TOperatorPtr& MutableLeft() { return Inputs_[0]; }
    TOperatorPtr& MutableRight() { return Inputs_[1]; }
    const std::vector<TJoinKey>& Keys() const { return Keys_; }
    std::vector<TJoinKey>& MutableKeys() { return Keys_; }
    EJoinType JoinType() const { return Type_; }
    // Residual predicate, applied before emit; nullptr if absent.
    const NQumir::NAst::TExprPtr& Filter() const { return Filter_; }
    NQumir::NAst::TExprPtr& MutableFilter() { return Filter_; }
    const std::optional<TRuntimeFilterSpec>& RuntimeFilter() const {
        return RuntimeFilter_;
    }
    std::optional<TRuntimeFilterSpec>& MutableRuntimeFilter() {
        return RuntimeFilter_;
    }

    std::span<const TOperatorPtr> Inputs() const override {
        return std::span<const TOperatorPtr>(Inputs_);
    }

    std::span<TOperatorPtr> MutableInputs() override {
        return std::span<TOperatorPtr>(Inputs_);
    }

private:
    std::array<TOperatorPtr, 2> Inputs_{nullptr, nullptr};
    std::vector<TJoinKey> Keys_;
    EJoinType Type_;
    NQumir::NAst::TExprPtr Filter_; // parsed, unannotated; may be null
    std::optional<TRuntimeFilterSpec> RuntimeFilter_;
};

// Output schema by join type:
//   Inner: left ++ right
//   Left:  left ++ nullable(right)
//   Right: nullable(left) ++ right
//   Full:  nullable(left) ++ nullable(right)
//   Left{Semi,Anti}:  left columns only
//   Right{Semi,Anti}: right columns only
// Returns an error if both sides contribute columns and column names overlap
// (no automatic prefixing/aliasing yet).
std::expected<NQumir::NAst::TTypePtr, NQumir::TError> ComputeJoinOutputType(
    const NQumir::NAst::TTypePtr& left,
    const NQumir::NAst::TTypePtr& right,
    EJoinType type);

// keys: list of (leftCol, rightCol) pairs; must be non-empty.
// filter: residual predicate expression string; empty for no residual.
std::expected<TOperatorPtr, NQumir::TError>
MakeJoin(TOperatorPtr left, TOperatorPtr right,
    std::vector<std::pair<std::string, std::string>> keys,
    EJoinType type,
    const std::string& filter = "");

} // namespace NQdb
