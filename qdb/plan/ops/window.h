#pragma once

#include <qdb/plan/ops/operator.h>
#include <qdb/plan/ops/sort.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace NQdb {

// One window-function application inside a TWindowOperator. Every function in a
// node shares the node's partition / order / frame spec (functions are grouped
// by identical spec at build time), so only the callable itself lives here.
struct TWindowFunc {
    std::string Name; // synthetic output column name
    std::string Func;  // "rank" | "sum" | "avg" | "max"
    NQumir::NAst::TExprPtr Arg; // parsed, unannotated; nullptr for rank()
};

enum class EWindowFrameMode {
    Rows,
    Range,
};

enum class EFrameBoundKind {
    UnboundedPreceding,
    Preceding,
    CurrentRow,
    Following,
    UnboundedFollowing,
};

struct TFrameBound {
    EFrameBoundKind Kind = EFrameBoundKind::CurrentRow;
    NQumir::NAst::TExprPtr Offset;  // set only for Preceding / Following
};

// An EXPLICIT frame clause, stored verbatim from SQL. When SQL writes a single
// bound (`ROWS/RANGE <start>`) it means `... BETWEEN <start> AND CURRENT ROW`, so
// the builder fills End with CURRENT ROW in that case. A window with NO frame
// clause is represented by an empty optional on the node (see Frame_ below), not
// by a defaulted TWindowFrame - its default-frame semantics are resolved later.
struct TWindowFrame {
    EWindowFrameMode Mode = EWindowFrameMode::Range;
    TFrameBound Start;
    TFrameBound End;
};

// A window operator applies one or more window functions that share a single
// spec (PARTITION BY + ORDER BY + frame). Unlike aggregate, it PRESERVES every
// input row and column and APPENDS one output column per function; queries with
// several distinct specs are lowered to a stack of these nodes.
class TWindowOperator : public IOperator {
public:
    static constexpr const char* OpId = "window";

    TWindowOperator(TOperatorPtr input,
        std::vector<std::string> partitionKeys,
        std::vector<TSortKey> orderKeys,
        std::optional<TWindowFrame> frame,
        std::vector<TWindowFunc> functions);

    std::string_view RelName() const override {
        return OpId;
    }

    std::unordered_set<std::string> ComputeReferencedColumns() const override;
    // Window forwards every input column and appends its own outputs, so (unlike
    // aggregate) it must not drop `needed`: the child has to produce every needed
    // column it actually owns, plus this node's own references. Mirrors join.
    std::unordered_set<std::string> RequiredColumnsForChild(
        size_t childIdx, const std::unordered_set<std::string>& needed) const override;
    std::vector<NQumir::NAst::TExprPtr> Children() const override {
        return {Input_};
    }

    const std::string ToString() const override;

    TOperatorPtr Input() const {
        return Input_;
    }
    TOperatorPtr& MutableInput() {
        return Input_;
    }

    const std::vector<std::string>& PartitionKeys() const {
        return PartitionKeys_;
    }

    std::vector<std::string>& MutablePartitionKeys() {
        return PartitionKeys_;
    }

    const std::vector<TSortKey>& OrderKeys() const {
        return OrderKeys_;
    }

    std::vector<TSortKey>& MutableOrderKeys() {
        return OrderKeys_;
    }

    const std::optional<TWindowFrame>& Frame() const {
        return Frame_;
    }

    std::optional<TWindowFrame>& MutableFrame() {
        return Frame_;
    }

    const std::vector<TWindowFunc>& Functions() const {
        return Functions_;
    }

    std::vector<TWindowFunc>& MutableFunctions() {
        return Functions_;
    }

    std::span<const TOperatorPtr> Inputs() const override {
        return std::span<const TOperatorPtr>(&Input_, 1);
    }

    std::span<TOperatorPtr> MutableInputs() override {
        return std::span<TOperatorPtr>(&Input_, 1);
    }

private:
    TOperatorPtr Input_;
    std::vector<std::string> PartitionKeys_;
    std::vector<TSortKey> OrderKeys_;
    std::optional<TWindowFrame> Frame_;
    std::vector<TWindowFunc> Functions_;
};

// Output schema = input fields ++ one field per window function (type derived
// from Func/Arg). Reused by the constructor and by AnnotateTypes after column
// pruning narrows the input schema.
NQumir::NAst::TTypePtr ComputeWindowOutputType(
    const NQumir::NAst::TTypePtr& inputSchema,
    const std::vector<TWindowFunc>& functions);

// Extra scale added to a decimal AVG result so the quotient keeps fractional
// precision (SQL avg widens scale) instead of truncating to the input scale.
inline constexpr int32_t WindowAvgExtraScale = 4;

// Result type of avg over `argType`: a decimal argument preserves integral digits
// and widens scale by WindowAvgExtraScale (capped at max precision), preserving
// nullability; any other numeric argument yields f64. Shared by the logical type
// (WindowResultType) and the exec lowering so the schema and kernel agree.
NQumir::NAst::TTypePtr ComputeWindowAvgResultType(
    const NQumir::NAst::TTypePtr& argType);

std::string_view WindowFrameModeName(EWindowFrameMode mode);
std::string_view FrameBoundKindName(EFrameBoundKind kind);

} // namespace NQdb
