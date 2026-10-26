#include "estimate_stats.h"
#include "const_fold.h"
#include "flatten_conjuncts.h"
#include "flatten_disjuncts.h"
#include "unbound_vars.h"

#include <qdb/plan/ops/source.h>
#include <qdb/plan/ops/cte_ref.h>
#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/late_materialize.h>
#include <qdb/plan/ops/union.h>
#include <qdb/plan/types/nullable.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <span>
#include <unordered_set>

namespace NQdb {

using namespace NQumir::NAst;
using namespace NQumir::NAst::NLiterals;

double EstimateTypeWidth(const TTypePtr& type) {
    auto inner = UnwrapNamedType(UnwrapNullableType(type));
    if (auto integer = TMaybeType<TIntegerType>(inner)) {
        return std::max(1, integer.Cast()->BitWidth() / 8);
    }
    if (TMaybeType<TFloatType>(inner)) {
        return 8.0;
    }
    if (TMaybeType<TBoolType>(inner)) {
        return 1.0;
    }
    if (TMaybeType<TStringType>(inner)) {
        return 16.0;
    }
    if (auto structure = TMaybeType<TStructType>(inner)) {
        double total = 0.0;
        for (const auto& [_, fieldType] : structure.Cast()->Fields) {
            total += EstimateTypeWidth(fieldType);
        }
        return total;
    }
    return 8.0;
}

double EstimateJoinCost(
    double leftCost,
    double rightCost,
    double leftRows,
    double rightRows,
    double outputRows,
    double outputRowWidth,
    double keyWidth)
{
    constexpr double JoinOutputBatchRows = 1024.0;
    constexpr double JoinOutputBatchPenalty = 4096.0;

    const double safeLeftRows = std::max(leftRows, 0.0);
    const double safeRightRows = std::max(rightRows, 0.0);
    const double safeOutputRows = std::max(outputRows, 0.0);
    const double safeOutputWidth = std::max(outputRowWidth, 1.0);
    const double safeKeyWidth = std::max(keyWidth, 0.0);
    const double batches = std::ceil(safeOutputRows / JoinOutputBatchRows);

    return std::max(leftCost, 0.0)
        + std::max(rightCost, 0.0)
        + (safeLeftRows + safeRightRows) * safeKeyWidth
        + safeOutputRows * safeOutputWidth
        + batches * JoinOutputBatchPenalty;
}

/*

  Селективность одного конъюнкта (тут окупается гистограмма):
  - col = const -> 1/ndv; если const вне [min,max] -> 0. Точнее - по бакету гистограммы.
  - col < / <= / > / >= диапазон -> equi-depth гистограмма: доля = (кол-во границ по нужную сторону)/N с интерполяцией в "граничном" бакете. Нет гистограммы -> линейно по (const-min)/(max-min).
  - col BETWEEN a AND b (или два range-конъюнкта на одну колонку) -> доля гистограммы в [a,b].
  - col IS NULL -> null_count/RowCount; IS NOT NULL -> 1 - null_count/RowCount.
  - col IN (...) -> min(1, |list|/ndv).
  - col1 = col2 (residual) -> 1/max(ndv1, ndv2).
  min(ndv, RowCount).


  +-------------+-----------------------------------------------------+--------------------------------------------------------------------------------------------------------+
  |  Оператор   |                       RowCount                      |                                                колонки                                                 |
  +-------------+-----------------------------------------------------+--------------------------------------------------------------------------------------------------------+
  | Source      | из футера (TParquetSource::Stats())                 | min/max/null/ndv/hist                                                                                  |
  +-------------+-----------------------------------------------------+--------------------------------------------------------------------------------------------------------+
  | Project     | без изменений                                       | прямые ссылки - pass-through; производные (a*b, func) - stats теряются                                 |
  +-------------+-----------------------------------------------------+--------------------------------------------------------------------------------------------------------+
  | Join (equi) | |L|*|R| / max(ndv) в общем диапазоне ключей         | ключ: ndv=min; прочее pass-through, ndv<=RowCount; урезанный ndv - без гистограммы. Semi/anti - по |L| |
  +-------------+-----------------------------------------------------+--------------------------------------------------------------------------------------------------------+
  | Aggregate   | prod ndv(group-key), cap на вход; без group by -> 1 | group-key: ndv/min/max сохраняются; агрегаты - min/max неизвестны (кроме min()/max()/count)            |
  +-------------+-----------------------------------------------------+--------------------------------------------------------------------------------------------------------+
  | Sort        | pass-through                                        | pass-through                                                                                           |
  +-------------+-----------------------------------------------------+--------------------------------------------------------------------------------------------------------+
  | Limit       | min(RowCount, N)                                    | pass-through                                                                                           |
  +-------------+-----------------------------------------------------+--------------------------------------------------------------------------------------------------------+
*/

namespace {

TTypePtr ColumnTypeOf(const TOperatorPtr& op, const std::string& name) {
    if (!op) {
        return nullptr;
    }
    auto structure = TMaybeType<TStructType>(op->OutputColumns());
    if (!structure) {
        return nullptr;
    }
    for (const auto& [fieldName, fieldType] : structure.Cast()->Fields) {
        if (fieldName == name) {
            return fieldType;
        }
    }
    return nullptr;
}

} // namespace

double ColumnWidth(const TOperatorPtr& op, const std::string& name) {
    auto type = ColumnTypeOf(op, name);
    return type
        ? EstimateTypeWidth(type)
        : 8.0;
}

namespace {

double OutputRowWidth(const TOperatorPtr& op) {
    return op ? EstimateTypeWidth(op->OutputColumns()) : 1.0;
}

double JoinKeyWidth(const TOperatorPtr& left, const TOperatorPtr& right,
    const std::vector<TJoinKey>& keys)
{
    double total = 0.0;
    for (const auto& key : keys) {
        total += std::max(
            ColumnWidth(left, key.Left),
            ColumnWidth(right, key.Right));
    }
    return total;
}

constexpr double LikeSelectivity = 0.05;

// One end of a range on a column.
struct TRangeEnd {
    double Value = 0.0;
    bool Inclusive = false; // `<=` or `>=`
    double Below = 0.0; // fraction of rows below the end
};

struct TRangeBound {
    std::string Column;
    bool Upper = false; // `col < c` or `col <= c`
    TRangeEnd End;
};

// The tightest ends of a range on a column.
struct TColumnRange {
    std::optional<TRangeEnd> Lower;
    std::optional<TRangeEnd> Upper;

    bool Holds(double value) const {
        return (!Lower || value > Lower->Value || (value == Lower->Value && Lower->Inclusive))
            && (!Upper || value < Upper->Value || (value == Upper->Value && Upper->Inclusive));
    }
};

// The constants a predicate allows a column, each keeping 1/ndv of its
// non-NULL rows.
struct TValueSet {
    std::string Column;
    double Ndv = 1.0;
    // The fraction of the rows where the column is not NULL.
    double NonNull = 1.0;
    std::set<double> Numbers;
    std::set<std::string> Strings;
    // A NULL among the constants, which no value equals.
    bool HasNull = false;

    double Size() const {
        return static_cast<double>(Numbers.size() + Strings.size());
    }

    double Selectivity() const {
        return std::min(1.0, Size() / Ndv) * NonNull;
    }

    void Unite(TValueSet other) {
        Numbers.merge(other.Numbers);
        Strings.merge(other.Strings);
    }

    void Intersect(const TValueSet& other) {
        std::erase_if(Numbers, [&](double value) { return !other.Numbers.contains(value); });
        std::erase_if(Strings, [&](const std::string& value) { return !other.Strings.contains(value); });
    }

    // Takes out the values `x <> c` or `x NOT IN (...)` rule out; with a NULL
    // among them the predicate is never TRUE.
    void Subtract(const TValueSet& other) {
        if (other.HasNull) {
            Numbers.clear();
            Strings.clear();
            return;
        }
        std::erase_if(Numbers, [&](double value) { return other.Numbers.contains(value); });
        std::erase_if(Strings, [&](const std::string& value) { return other.Strings.contains(value); });
    }
};

// Whether expr can be NULL, which makes a predicate over it UNKNOWN rather than
// FALSE. Only what is known to be NULL-free counts as such.
bool MayBeNull(const TExprPtr& expr, const TStructType& input) {
    if (TMaybeNode<TNumberExpr>(expr) || TMaybeNode<TStringLiteralExpr>(expr)) {
        return false;
    }
    if (auto ident = TMaybeNode<TIdentExpr>(expr)) {
        const auto& name = ident.Cast()->Name;
        for (const auto& [column, type] : input.Fields) {
            if (column == name) {
                return IsNullableType(type);
            }
        }
        return true;
    }
    std::vector<TExprPtr> operands;
    if (auto maybeCall = TMaybeNode<TCallExpr>(expr)) {
        auto call = maybeCall.Cast();
        auto callee = TMaybeNode<TIdentExpr>(call->Callee);
        if (!callee) {
            return true;
        }
        const auto& name = callee.Cast()->Name;
        if (name == "qdb_is_null") {
            return false;
        }
        // These return NULL only for a NULL argument.
        if (name != "qdb_in_list" && name != "qdb_string_view_sql_like") {
            return true;
        }
        operands = call->Args;
    } else if (TMaybeNode<TCastExpr>(expr) || TMaybeNode<TUnaryExpr>(expr) || TMaybeNode<TBinaryExpr>(expr)) {
        operands = expr->Children();
    } else {
        return true;
    }
    return std::ranges::any_of(operands, [&](const TExprPtr& operand) {
        return MayBeNull(operand, input);
    });
}

// One filter's statistics and schema, with the constant folder shared by the query.
class TSelectivityEstimator {
public:
    TSelectivityEstimator(
        const TStats& inputStats,
        const TStructType& operatorType,
        bool overSource,
        TConstFolder& folder)
        : InputStats_(inputStats)
        , OperatorType_(operatorType)
        , OverSource_(overSource)
        , Folder_(folder) {}

    double Estimate(const TExprPtr& atom) {
        if (auto maybeUnary = TMaybeNode<TUnaryExpr>(atom)) {
            auto unary = maybeUnary.Cast();
            if (unary->Operator == "!"_op) {
                return EstimateNot(*unary);
            }
        }
        if (auto maybeCall = TMaybeNode<TCallExpr>(atom)) {
            return EstimateCall(atom, *maybeCall.Cast());
        }
        if (auto maybeBinary = TMaybeNode<TBinaryExpr>(atom)) {
            return EstimateBinary(atom, *maybeBinary.Cast());
        }
        return 0.5;
    }

private:
    double EstimateNot(const TUnaryExpr& unary) {
        // NOT keeps the rows where the operand is FALSE. If it can also be
        // UNKNOWN, that is not the complement of TRUE, and the statistics
        // do not say how many rows are UNKNOWN.
        if (MayBeNull(unary.Operand, OperatorType_)) {
            return 0.5;
        }
        // NOT IN is the chain of `<>` it stands for: each value keeps
        // 1 - 1/ndv of the rows.
        if (auto values = ValueSet(unary.Operand)) {
            return std::pow(1.0 - 1.0 / values->Ndv, values->Size());
        }
        return 1.0 - Estimate(unary.Operand);
    }

    double EstimateCall(const TExprPtr& atom, const TCallExpr& call) {
        auto maybeCallee = TMaybeNode<TIdentExpr>(call.Callee);
        if (!maybeCallee) {
            return 0.5;
        }
        auto callee = maybeCallee.Cast();
        if (callee->Name == "qdb_string_view_sql_like") {
            return LikeSelectivity;
        }
        // A source's null count and row count describe the same rows.
        if (
            callee->Name == "qdb_is_null" && call.Args.size() == 1 &&
            OverSource_ && InputStats_.RowCount > 0)
        {
            if (auto column = TMaybeNode<TIdentExpr>(call.Args[0])) {
                auto colIt = InputStats_.ColumnStats.find(column.Cast()->Name);
                if (colIt != InputStats_.ColumnStats.end() && colIt->second->NullCount) {
                    return static_cast<double>(*colIt->second->NullCount)
                        / static_cast<double>(InputStats_.RowCount);
                }
            }
        }
        // A NULL is in no set.
        if (auto values = ValueSet(atom)) {
            return values->Selectivity();
        }
        return 0.5;
    }

    double EstimateBinary(const TExprPtr& atom, const TBinaryExpr& binary) {
        if (binary.Operator == "&&"_op) {
            return EstimateConjunction(atom);
        }
        if (binary.Operator == "||"_op) {
            return EstimateDisjunction(atom);
        }
        if (binary.Operator == "=="_op || binary.Operator == "!="_op) {
            return EstimateEquality(binary);
        }
        if (auto bound = RangeBound(atom)) {
            const double fraction = bound->Upper
                ? bound->End.Below
                : 1.0 - bound->End.Below;
            return fraction * NonNullFraction(bound->Column);
        }
        if (auto fraction = EstimateColumnComparison(binary)) {
            return *fraction;
        }
        return 0.5;
    }

    double EstimateConjunction(const TExprPtr& atom) {
        std::vector<TExprPtr> conjuncts;
        FlattenConjuncts(atom, conjuncts);
        // Bounds on one column form one range, and the sets of values allowed
        // one column intersect. Multiplying them as independent predicates
        // distorts a narrow range a lot, and counts a column constrained
        // twice twice.
        std::map<std::string, TColumnRange> ranges;
        std::map<std::string, TValueSet> sets;
        std::map<std::string, std::vector<std::pair<TExprPtr, TValueSet>>> exclusions;
        double selectivity = 1.0;
        for (const auto& conjunct : conjuncts) {
            if (auto bound = RangeBound(conjunct)) {
                auto& range = ranges[bound->Column];
                const auto& end = bound->End;
                // The tighter end wins; at the same constant the strict one.
                if (bound->Upper) {
                    if (
                        !range.Upper || end.Value < range.Upper->Value ||
                        (end.Value == range.Upper->Value && !end.Inclusive))
                    {
                        range.Upper = end;
                    }
                } else if (
                    !range.Lower || end.Value > range.Lower->Value ||
                    (end.Value == range.Lower->Value && !end.Inclusive))
                {
                    range.Lower = end;
                }
            } else if (auto values = ValueSet(conjunct)) {
                auto [it, inserted] = sets.try_emplace(values->Column, *values);
                if (!inserted) {
                    it->second.Intersect(*values);
                }
            } else if (auto excluded = ExcludedValues(conjunct)) {
                exclusions[excluded->Column].emplace_back(conjunct, std::move(*excluded));
            } else {
                selectivity *= Estimate(conjunct);
            }
        }
        // The values `<>` and `NOT IN` rule out leave a column's set; without
        // a set, each keeps its own share of the rows.
        for (const auto& [column, excluded] : exclusions) {
            auto it = sets.find(column);
            for (const auto& [conjunct, values] : excluded) {
                if (it != sets.end()) {
                    it->second.Subtract(values);
                } else {
                    selectivity *= Estimate(conjunct);
                }
            }
        }
        // A range on a column with a set of values keeps the values inside it.
        for (auto& [column, values] : sets) {
            if (auto it = ranges.find(column); it != ranges.end()) {
                std::erase_if(values.Numbers, [&](double value) { return !it->second.Holds(value); });
                ranges.erase(it);
            }
            selectivity *= values.Selectivity();
        }
        for (const auto& [column, range] : ranges) {
            const double upper = range.Upper
                ? range.Upper->Below
                : 1.0;
            const double lower = range.Lower
                ? range.Lower->Below
                : 0.0;
            double fraction = std::max(upper - lower, 0.0);
            // The histogram cannot resolve a range narrower than a bucket,
            // but a range that can hold a value of the column keeps one
            // distinct value. It never keeps more than either end alone.
            auto ndv = KnownNdv(column);
            if (
                range.Lower && range.Upper && ndv &&
                RangeCanHoldValue(column, *range.Lower, *range.Upper))
            {
                fraction = std::min({
                    std::max(fraction, 1.0 / *ndv),
                    upper,
                    1.0 - lower});
            }
            selectivity *= fraction * NonNullFraction(column);
        }
        return selectivity;
    }

    // OR keeps the rows that any disjunct keeps. The values allowed one
    // column are one set, and an OR on the column is their union; disjuncts
    // on different columns are taken as independent, so the rows that none of
    // them keeps are the product of their complements.
    double EstimateDisjunction(const TExprPtr& atom) {
        std::vector<TExprPtr> disjuncts;
        FlattenDisjuncts(atom, disjuncts);
        std::map<std::string, TValueSet> sets;
        double none = 1.0;
        for (const auto& disjunct : disjuncts) {
            if (auto values = ValueSet(disjunct)) {
                auto [it, inserted] = sets.try_emplace(values->Column, *values);
                if (!inserted) {
                    it->second.Unite(std::move(*values));
                }
            } else {
                none *= 1.0 - Estimate(disjunct);
            }
        }
        for (const auto& [column, values] : sets) {
            none *= 1.0 - values.Selectivity();
        }
        return 1.0 - none;
    }

    // A NULL compares as UNKNOWN, so `=` and `!=` keep only non-NULL rows:
    // their share of the column's values, of its non-NULL rows.
    double EstimateEquality(const TBinaryExpr& binary) {
        const bool negated = binary.Operator == "!="_op;
        auto leftIdent = TMaybeNode<TIdentExpr>(binary.Left);
        auto rightIdent = TMaybeNode<TIdentExpr>(binary.Right);
        if (leftIdent && rightIdent) {
            return EstimateColumnEquality(
                leftIdent.Cast()->Name,
                rightIdent.Cast()->Name,
                negated);
        }
        const std::string* column = nullptr;
        const TExprPtr* value = nullptr;
        if (leftIdent && IsConstExpr(binary.Right)) {
            column = &leftIdent.Cast()->Name;
            value = &binary.Right;
        } else if (rightIdent && IsConstExpr(binary.Left)) {
            column = &rightIdent.Cast()->Name;
            value = &binary.Left;
        }
        if (!column) {
            return 0.5;
        }
        const double selectivity = EstimateConstantEquality(*column, *value);
        return (negated ? 1.0 - selectivity : selectivity) * NonNullFraction(*column);
    }

    double EstimateColumnEquality(
        const std::string& leftName,
        const std::string& rightName,
        bool negated) const
    {
        // NaN != NaN, so a float column keeps the general estimate below.
        auto type = ColumnType(leftName);
        const bool mayBeNaN = type
            && TMaybeType<TFloatType>(UnwrapNamedType(UnwrapNullableType(type)));
        if (leftName == rightName && !mayBeNaN) {
            // A column equals itself on every non-NULL row, whatever its
            // NDV; `x != x` holds on no row.
            return negated
                ? 0.0
                : NonNullFraction(leftName);
        }
        double selectivity = 0.5;
        auto leftNdv = KnownNdv(leftName);
        auto rightNdv = KnownNdv(rightName);
        if (leftNdv && rightNdv) {
            selectivity = 1.0 / std::max(*leftNdv, *rightNdv);
        }
        return (negated ? 1.0 - selectivity : selectivity)
            * NonNullFraction(leftName) * NonNullFraction(rightName);
    }

    double EstimateConstantEquality(const std::string& colName, const TExprPtr& value) {
        auto it = InputStats_.ColumnStats.find(colName);
        if (it == InputStats_.ColumnStats.end()) {
            return 0.5;
        }
        auto inRangeResult = InRange(value, colName);
        if (inRangeResult.has_value() && !*inRangeResult) {
            return 0.0;
        }
        auto ndv = KnownNdv(colName);
        return ndv
            ? 1.0 / *ndv
            : 0.5;
    }

    static bool IsConstExpr(const TExprPtr& expr) {
        return FindUnboundVars(expr).empty();
    }

    // The type of a column's values; a nullable column's NULLs have no value.
    TTypePtr ColumnType(const std::string& colName) const {
        for (const auto& [col, type] : OperatorType_.Fields) {
            if (col == colName) {
                return UnwrapNullableType(type);
            }
        }
        return nullptr;
    }

    // The NDV a column's non-NULL rows are divided among, when the statistics
    // have one. An NDV of 0 divides nothing and counts as unknown: an all-NULL
    // column keeps no rows through its null count instead.
    std::optional<double> KnownNdv(const std::string& colName) const {
        auto it = InputStats_.ColumnStats.find(colName);
        if (it == InputStats_.ColumnStats.end() || !it->second->Ndv || *it->second->Ndv == 0) {
            return std::nullopt;
        }
        return static_cast<double>(*it->second->Ndv);
    }

    // Fraction of the rows where a column is not NULL. A source's null count
    // describes its rows; above other operators the NULLs left are not known,
    // and they are not counted.
    double NonNullFraction(const std::string& colName) const {
        auto it = InputStats_.ColumnStats.find(colName);
        if (
            !OverSource_ || InputStats_.RowCount == 0 ||
            it == InputStats_.ColumnStats.end() || !it->second->NullCount)
        {
            return 1.0;
        }
        return 1.0 - std::min(1.0,
            static_cast<double>(*it->second->NullCount) / static_cast<double>(InputStats_.RowCount));
    }

    std::shared_ptr<TNumberExpr> ConstNumber(const TExprPtr& expr) {
        if (auto number = TMaybeNode<TNumberExpr>(expr)) {
            return number.Cast();
        }
        if (!IsConstExpr(expr)) {
            return nullptr;
        }
        return Folder_.FoldNumber(expr);
    }

    std::optional<double> FractionBelow(
        const std::string& colName,
        const TExprPtr& bound,
        bool strict)
    {
        auto colIt = InputStats_.ColumnStats.find(colName);
        auto type = ColumnType(colName);
        // Check the statistics first: evaluating the bound is the costly step.
        if (
            colIt == InputStats_.ColumnStats.end() || colIt->second->Histogram.empty() ||
            !(TMaybeType<TFloatType>(type) || TMaybeType<TIntegerType>(type)))
        {
            return std::nullopt;
        }
        auto num = ConstNumber(bound);
        if (!num) {
            return std::nullopt;
        }
        return num->IsFloat()
            ? FractionBelowValue(*colIt->second, type, num->FloatValue, strict)
            : FractionBelowValue(*colIt->second, type, num->IntValue, strict);
    }

    // Fraction of a numeric column's non-NULL rows below an integer or a
    // double value.
    template <class TValue>
    static std::optional<double> FractionBelowValue(
        const TStats::TColumnStats& stats,
        const TTypePtr& type,
        TValue value,
        bool strict)
    {
        if (TMaybeType<TFloatType>(type)) {
            return stats.FractionBelow(static_cast<double>(value), strict);
        }
        if (!TMaybeType<TIntegerType>(type)) {
            return std::nullopt;
        }
        if constexpr (std::is_floating_point_v<TValue>) {
            if (std::isnan(value)) {
                return std::nullopt;
            }
            // Outside the int64 range the bound is above or below every value.
            if (value >= 0x1p63) {
                return 1.0;
            }
            if (value < -0x1p63) {
                return 0.0;
            }
            // An integer is below 24.5 exactly when it is <= 24.
            const double whole = std::floor(value);
            return stats.FractionBelow(static_cast<int64_t>(whole), strict && whole == value);
        } else {
            return stats.FractionBelow(value, strict);
        }
    }

    // `x < y` for two columns taken as independent: the mean, over the rows
    // of y, of the fraction of x below them. Each bucket of y's equi-depth
    // histogram holds an equal share of its rows, spread evenly between its
    // boundaries, so it contributes the mean fraction at its two ends. A NULL
    // on either side compares as UNKNOWN.
    std::optional<double> EstimateColumnComparison(const TBinaryExpr& binary) const {
        const bool less = binary.Operator == "<"_op || binary.Operator == "<="_op;
        const bool greater = binary.Operator == ">"_op || binary.Operator == ">="_op;
        auto maybeLeft = TMaybeNode<TIdentExpr>(binary.Left);
        auto maybeRight = TMaybeNode<TIdentExpr>(binary.Right);
        if ((!less && !greater) || !maybeLeft || !maybeRight) {
            return std::nullopt;
        }
        // `x > y` is `y < x`, and `<` counts the rows of x strictly below y.
        const bool strict = binary.Operator == "<"_op || binary.Operator == ">"_op;
        const auto& lowerName = (less ? maybeLeft : maybeRight).Cast()->Name;
        const auto& upperName = (less ? maybeRight : maybeLeft).Cast()->Name;
        auto lowerIt = InputStats_.ColumnStats.find(lowerName);
        auto upperIt = InputStats_.ColumnStats.find(upperName);
        if (lowerIt == InputStats_.ColumnStats.end() || upperIt == InputStats_.ColumnStats.end()) {
            return std::nullopt;
        }
        const auto& lower = *lowerIt->second;
        const auto& upper = *upperIt->second;
        const auto lowerType = ColumnType(lowerName);
        const auto upperType = ColumnType(upperName);
        const bool upperFloat = static_cast<bool>(TMaybeType<TFloatType>(upperType));
        if (upper.Histogram.size() < 2 || !(upperFloat || TMaybeType<TIntegerType>(upperType))) {
            return std::nullopt;
        }
        auto below = [&](size_t boundary) {
            return upperFloat
                ? FractionBelowValue(lower, lowerType, *upper.GetHistogramValue<double>(boundary), strict)
                : FractionBelowValue(lower, lowerType, *upper.GetHistogramValue<int64_t>(boundary), strict);
        };
        const size_t buckets = upper.Histogram.size() - 1;
        double sum = 0.0;
        for (size_t bucket = 0; bucket < buckets; ++bucket) {
            auto start = below(bucket);
            auto end = below(bucket + 1);
            if (!start || !end) {
                return std::nullopt;
            }
            sum += (*start + *end) / 2.0;
        }
        return sum / static_cast<double>(buckets)
            * NonNullFraction(lowerName) * NonNullFraction(upperName);
    }

    std::optional<TRangeBound> RangeBound(const TExprPtr& expr) {
        auto maybeBinary = TMaybeNode<TBinaryExpr>(expr);
        if (!maybeBinary) {
            return std::nullopt;
        }
        auto binary = maybeBinary.Cast();
        const bool less = binary->Operator == "<"_op || binary->Operator == "<="_op;
        const bool greater = binary->Operator == ">"_op || binary->Operator == ">="_op;
        if (!less && !greater) {
            return std::nullopt;
        }
        // An upper bound keeps the rows below it and a lower bound drops
        // them, so `<` and `>=` count the rows strictly below the constant.
        const bool inclusive = binary->Operator == "<="_op || binary->Operator == ">="_op;
        // The bound folded already when the fraction was found.
        auto value = [&](const TExprPtr& bound) {
            auto number = ConstNumber(bound);
            return number->IsFloat()
                ? number->FloatValue
                : static_cast<double>(number->IntValue);
        };
        // `col OP const`
        if (auto leftIdent = TMaybeNode<TIdentExpr>(binary->Left)) {
            const auto& name = leftIdent.Cast()->Name;
            if (auto f = FractionBelow(name, binary->Right, less != inclusive)) {
                return TRangeBound{name, less, {value(binary->Right), inclusive, *f}};
            }
        }
        // `const OP col`  ==  `col (flip OP) const`
        if (auto rightIdent = TMaybeNode<TIdentExpr>(binary->Right)) {
            const auto& name = rightIdent.Cast()->Name;
            if (auto f = FractionBelow(name, binary->Left, greater != inclusive)) {
                return TRangeBound{name, greater, {value(binary->Left), inclusive, *f}};
            }
        }
        return std::nullopt;
    }

    bool RangeCanHoldValue(const std::string& colName, TRangeEnd lower, TRangeEnd upper) const {
        const auto& colStats = InputStats_.ColumnStats.at(colName);
        if (!colStats->MinValue || !colStats->MaxValue) {
            return false; // the statistics cannot show it
        }
        double min = 0.0;
        double max = 0.0;
        if (auto integer = TMaybeType<TIntegerType>(ColumnType(colName))) {
            // The smallest and the largest integer between the ends.
            lower.Value = lower.Inclusive
                ? std::ceil(lower.Value)
                : std::floor(lower.Value) + 1.0;
            upper.Value = upper.Inclusive
                ? std::floor(upper.Value)
                : std::ceil(upper.Value) - 1.0;
            lower.Inclusive = upper.Inclusive = true;
            const bool isSigned = integer.Cast()->IsSigned();
            min = isSigned
                ? static_cast<double>(*colStats->GetMinValue<int64_t>())
                : static_cast<double>(*colStats->GetMinValue<uint64_t>());
            max = isSigned
                ? static_cast<double>(*colStats->GetMaxValue<int64_t>())
                : static_cast<double>(*colStats->GetMaxValue<uint64_t>());
        } else {
            min = *colStats->GetMinValue<double>();
            max = *colStats->GetMaxValue<double>();
        }
        auto below = [](double a, bool aInclusive, double b, bool bInclusive) {
            return a < b || (a == b && !(aInclusive && bInclusive));
        };
        return !below(upper.Value, upper.Inclusive, lower.Value, lower.Inclusive)
            && !below(upper.Value, upper.Inclusive, min, true)
            && !below(max, true, lower.Value, lower.Inclusive);
    }

    std::optional<bool> InRange(const TExprPtr& expr, const std::string& colName) {
        if (InputStats_.ColumnStats.count(colName) == 0) {
            return std::nullopt;
        }
        auto colStats = InputStats_.ColumnStats.at(colName);
        if (!colStats->MinValue || !colStats->MaxValue) {
            return std::nullopt;
        }
        const auto type = ColumnType(colName);
        auto integerType = TMaybeType<TIntegerType>(type);
        if (type && !integerType && !TMaybeType<TFloatType>(type)) {
            return std::nullopt;
        }
        auto number = ConstNumber(expr);
        if (!number) {
            return std::nullopt;
        }
        if (integerType) {
            return integerType.Cast()->IsSigned()
                ? NumberInRange<int64_t>(*colStats, *number)
                : NumberInRange<uint64_t>(*colStats, *number);
        }
        // TODO: only double supported
        return NumberInRange<double>(*colStats, *number);
    }

    template<class TValue>
    static bool NumberInRange(const TStats::TColumnStats& stats, const TNumberExpr& number) {
        if (number.IsFloat()) {
            return *stats.GetMinValue<TValue>() <= number.FloatValue
                && number.FloatValue <= *stats.GetMaxValue<TValue>();
        }
        return *stats.GetMinValue<TValue>() <= number.IntValue
            && number.IntValue <= *stats.GetMaxValue<TValue>();
    }

    // The values `expr` allows a column, when it allows constants: `col = c`,
    // `col IN (...)` and an OR of these on one column. A NULL, a NaN and a
    // number outside the column's min/max match no row and are left out. Only
    // a column with an NDV has a set.
    std::optional<TValueSet> ValueSet(const TExprPtr& expr) {
        if (auto maybeBinary = TMaybeNode<TBinaryExpr>(expr)) {
            auto binary = maybeBinary.Cast();
            if (binary->Operator == "||"_op) {
                std::vector<TExprPtr> disjuncts;
                FlattenDisjuncts(expr, disjuncts);
                std::optional<TValueSet> values;
                for (const auto& disjunct : disjuncts) {
                    auto part = ValueSet(disjunct);
                    if (!part || (values && part->Column != values->Column)) {
                        return std::nullopt;
                    }
                    if (values) {
                        values->Unite(std::move(*part));
                    } else {
                        values = std::move(part);
                    }
                }
                return values;
            }
            return binary->Operator == "=="_op
                ? ComparedValue(*binary)
                : std::nullopt;
        }
        auto maybeCall = TMaybeNode<TCallExpr>(expr);
        if (!maybeCall) {
            return std::nullopt;
        }
        auto call = maybeCall.Cast();
        auto callee = TMaybeNode<TIdentExpr>(call->Callee);
        if (!callee || callee.Cast()->Name != "qdb_in_list" || call->Args.size() < 2) {
            return std::nullopt;
        }
        auto column = TMaybeNode<TIdentExpr>(call->Args[0]);
        if (!column) {
            return std::nullopt;
        }
        return ColumnValues(column.Cast()->Name, std::span(call->Args).subspan(1));
    }

    // The values a conjunct rules out of a column: `col <> c`, `NOT IN` and
    // the NOT of an OR of equalities on one column.
    std::optional<TValueSet> ExcludedValues(const TExprPtr& expr) {
        if (auto maybeUnary = TMaybeNode<TUnaryExpr>(expr)) {
            auto unary = maybeUnary.Cast();
            return unary->Operator == "!"_op
                ? ValueSet(unary->Operand)
                : std::nullopt;
        }
        auto maybeBinary = TMaybeNode<TBinaryExpr>(expr);
        if (!maybeBinary) {
            return std::nullopt;
        }
        auto binary = maybeBinary.Cast();
        return binary->Operator == "!="_op
            ? ComparedValue(*binary)
            : std::nullopt;
    }

    // The constant a column is compared with, as a set of one value.
    std::optional<TValueSet> ComparedValue(const TBinaryExpr& binary) {
        auto maybeLeft = TMaybeNode<TIdentExpr>(binary.Left);
        auto maybeRight = TMaybeNode<TIdentExpr>(binary.Right);
        if (maybeLeft && !maybeRight && IsConstExpr(binary.Right)) {
            auto left = maybeLeft.Cast();
            return ColumnValues(left->Name, std::span(&binary.Right, 1));
        }
        if (maybeRight && !maybeLeft && IsConstExpr(binary.Left)) {
            auto right = maybeRight.Cast();
            return ColumnValues(right->Name, std::span(&binary.Left, 1));
        }
        return std::nullopt;
    }

    std::optional<TValueSet> ColumnValues(const std::string& name, std::span<const TExprPtr> items) {
        auto ndv = KnownNdv(name);
        if (!ndv) {
            return std::nullopt;
        }
        TValueSet values{
            .Column = name,
            .Ndv = *ndv,
            .NonNull = NonNullFraction(name),
        };
        for (const auto& item : items) {
            if (auto maybeNull = TMaybeNode<TCallExpr>(item)) {
                auto nullCallee = TMaybeNode<TIdentExpr>(maybeNull.Cast()->Callee);
                if (nullCallee && nullCallee.Cast()->Name == "qdb_sql_null") {
                    values.HasNull = true;
                    continue;
                }
            }
            if (auto maybeCast = TMaybeNode<TCastExpr>(item)) {
                if (auto literal = TMaybeNode<TStringLiteralExpr>(maybeCast.Cast()->Operand)) {
                    values.Strings.insert(literal.Cast()->Value);
                    continue;
                }
            }
            auto number = ConstNumber(item);
            if (!number) {
                return std::nullopt;
            }
            const double value = number->IsFloat()
                ? number->FloatValue
                : static_cast<double>(number->IntValue);
            if (std::isnan(value) || InRange(item, name) == false) {
                continue;
            }
            values.Numbers.insert(value);
        }
        return values;
    }

    const TStats& InputStats_;
    const TStructType& OperatorType_;
    const bool OverSource_;
    TConstFolder& Folder_;
};

// Selectivity of a join's residual over the joined rows: the columns of both
// inputs side by side, the cross product of their rows. Only the residual's
// own columns are read.
double EstimateResidual(
    const TOperatorPtr& left,
    const TOperatorPtr& right,
    const TExprPtr& residual,
    TConstFolder& folder)
{
    if (!left->Stats_ || !right->Stats_) {
        return 0.5;
    }
    TStats stats;
    std::vector<std::pair<std::string, TTypePtr>> fields;
    for (const auto& column : FindUnboundVars(residual)) {
        for (const auto& input : {left, right}) {
            if (auto type = FieldType(input->OutputColumns().get(), column)) {
                fields.emplace_back(column, std::move(type));
                if (auto it = input->Stats_->ColumnStats.find(column); it != input->Stats_->ColumnStats.end()) {
                    stats.ColumnStats.insert(*it);
                }
                break;
            }
        }
    }
    const double rows = static_cast<double>(left->Stats_->RowCount)
        * static_cast<double>(right->Stats_->RowCount);
    stats.RowCount = static_cast<uint64_t>(std::min(rows, 0x1p63));
    const TStructType type(std::move(fields));
    const double selectivity = TSelectivityEstimator(stats, type, false, folder).Estimate(residual);
    return std::clamp(selectivity, 0.0, 1.0);
}

// A source's null counts and row count describe the same rows.
double EstimateSelectivity(
    const TExprPtr& atom,
    const TStatsPtr& inputStats,
    const std::shared_ptr<TStructType>& operatorType,
    bool overSource,
    TConstFolder& folder)
{
    if (!operatorType) {
        return 0.5;
    }
    return TSelectivityEstimator(*inputStats, *operatorType, overSource, folder).Estimate(atom);
}

TStatsPtr ComputeFilterStats(const std::shared_ptr<TFilterOperator>& filter, TConstFolder& folder) {
    auto inputStats = filter->Input()->Stats_;
    if (!inputStats) {
        return nullptr;
    }
    double selectivity = EstimateSelectivity(filter->Predicate(), inputStats,
        TMaybeType<TStructType>(filter->Input()->OutputColumns()).Cast(),
        TMaybeOp<TSourceOperator>(filter->Input()), folder);
    selectivity = std::clamp(selectivity, 0.0, 1.0);
    auto outputStats = std::make_shared<TStats>();
    outputStats->ColumnStats = inputStats->ColumnStats;
    outputStats->RowCount = std::max<uint64_t>(1, inputStats->RowCount * selectivity);
    outputStats->Cost = inputStats->Cost
        + static_cast<double>(inputStats->RowCount)
            * std::max(OutputRowWidth(filter->Input()), 1.0);
    return outputStats;
}

TStatsPtr ComputeProjectStats(const std::shared_ptr<TProjectOperator>& project) {
    auto inputStats = project->Input()->Stats_;
    if (!inputStats) {
        return nullptr;
    }
    auto outputStats = std::make_shared<TStats>();
    outputStats->RowCount = inputStats->RowCount;
    outputStats->Cost = inputStats->Cost
        + static_cast<double>(outputStats->RowCount)
            * std::max(OutputRowWidth(project), 1.0);
    for (const auto& proj : project->Projections()) {
        if (auto ident = TMaybeNode<TIdentExpr>(proj.Expression)) {
            auto colName = ident.Cast()->Name;
            auto it = inputStats->ColumnStats.find(colName);
            if (it != inputStats->ColumnStats.end()) {
                outputStats->ColumnStats[proj.Name] = it->second;
            }
        }
    }
    return outputStats;
}

// Fallback NDV for a join key column that has no statistics.
constexpr double UnknownNdv = 100.0;

struct TJoinCardinality {
    double Rows = 0.0;
    double NdvL = 1.0; // \prod ndv over left key columns, clamped to leftRows
    double NdvR = 1.0;
};

// One side of an equi-join key pair.
struct TJoinKeySide {
    double Ndv = UnknownNdv;
    // Fraction of the side's rows whose key lies in the range of values both
    // sides' keys span; 1 when a range is unknown.
    double SharedFraction = 1.0;
};

struct TJoinKeyStats {
    TJoinKeySide Left;
    TJoinKeySide Right;
};

TStats::TColumnStatsPtr ColumnStatsOf(const TOperatorPtr& op, const std::string& name) {
    if (!op || !op->Stats_) {
        return nullptr;
    }
    auto it = op->Stats_->ColumnStats.find(name);
    return it != op->Stats_->ColumnStats.end()
        ? it->second
        : nullptr;
}

// The range of a numeric column's values, as doubles.
struct TValueRange {
    double Min = 0.0;
    double Max = 0.0;
    bool Integer = false;
    bool Signed = false;
};

std::optional<TValueRange> ValueRange(const TOperatorPtr& op, const std::string& name) {
    auto stats = ColumnStatsOf(op, name);
    if (!stats || !stats->MinValue || !stats->MaxValue) {
        return std::nullopt;
    }
    // A named type such as DECIMAL is not compared by its raw value.
    auto type = UnwrapNullableType(ColumnTypeOf(op, name));
    TValueRange range;
    if (auto integer = TMaybeType<TIntegerType>(type)) {
        range.Integer = true;
        range.Signed = integer.Cast()->IsSigned();
        range.Min = range.Signed
            ? static_cast<double>(*stats->GetMinValue<int64_t>())
            : static_cast<double>(*stats->GetMinValue<uint64_t>());
        range.Max = range.Signed
            ? static_cast<double>(*stats->GetMaxValue<int64_t>())
            : static_cast<double>(*stats->GetMaxValue<uint64_t>());
        // A double holds every integer exactly only up to 2^53.
        if (std::abs(range.Min) > 0x1p53 || std::abs(range.Max) > 0x1p53) {
            return std::nullopt;
        }
    } else if (TMaybeType<TFloatType>(type)) {
        range.Min = *stats->GetMinValue<double>();
        range.Max = *stats->GetMaxValue<double>();
        if (!std::isfinite(range.Min) || !std::isfinite(range.Max)) {
            return std::nullopt;
        }
    } else {
        return std::nullopt;
    }
    if (range.Min > range.Max) {
        return std::nullopt;
    }
    return range;
}

// Fraction of a column's rows with a value in [lo, hi], a part of its range.
// Only a histogram tells how the values spread over the range; without one,
// the range only bounds them.
double FractionWithin(const TStats::TColumnStats& stats, const TValueRange& range, double lo, double hi) {
    if (range.Integer) {
        lo = std::ceil(lo);
        hi = std::floor(hi);
    }
    if (lo > hi) {
        return 0.0;
    }
    if (stats.Histogram.empty()) {
        return 1.0;
    }
    auto within = [&]<class TValue>(TValue lower, TValue upper) {
        return *stats.FractionBelow(upper) - *stats.FractionBelow(lower, true);
    };
    if (!range.Integer) {
        return within(lo, hi);
    }
    return range.Signed
        ? within(static_cast<int64_t>(lo), static_cast<int64_t>(hi))
        : within(static_cast<uint64_t>(lo), static_cast<uint64_t>(hi));
}

// Statistics of the key pair `leftColumn = rightColumn` of two inputs whose
// stats are estimated.
TJoinKeyStats EstimateJoinKey(
    const TOperatorPtr& left,
    const std::string& leftColumn,
    const TOperatorPtr& right,
    const std::string& rightColumn)
{
    TJoinKeyStats key;
    auto leftStats = ColumnStatsOf(left, leftColumn);
    auto rightStats = ColumnStatsOf(right, rightColumn);
    if (leftStats && leftStats->Ndv) {
        key.Left.Ndv = static_cast<double>(*leftStats->Ndv);
    }
    if (rightStats && rightStats->Ndv) {
        key.Right.Ndv = static_cast<double>(*rightStats->Ndv);
    }
    auto leftRange = ValueRange(left, leftColumn);
    auto rightRange = ValueRange(right, rightColumn);
    if (leftRange && rightRange) {
        const double lo = std::max(leftRange->Min, rightRange->Min);
        const double hi = std::min(leftRange->Max, rightRange->Max);
        key.Left.SharedFraction = FractionWithin(*leftStats, *leftRange, lo, hi);
        key.Right.SharedFraction = FractionWithin(*rightStats, *rightRange, lo, hi);
    }
    return key;
}

// Inner equi-join cardinality over the range both sides' keys span: rows =
// sharedL*sharedR / max(domainL, domainR), where a side's domain is its key ndv
// inside that range and its shared rows are the rows there. `twoRelations`
// tells that every key pair joins the same two relations. See "Join
// cardinality" in docs/arch/optimizations.md.
TJoinCardinality EstimateEquiJoin(
    double leftRows,
    double rightRows,
    const std::vector<TJoinKeyStats>& keys,
    bool twoRelations)
{
    double ndvL = 1.0;
    double ndvR = 1.0;
    double sharedL = 1.0;
    double sharedR = 1.0;
    for (const auto& key : keys) {
        ndvL *= key.Left.Ndv;
        ndvR *= key.Right.Ndv;
        sharedL *= key.Left.SharedFraction;
        sharedR *= key.Right.SharedFraction;
    }
    // The inner-join denominator uses the key DOMAIN (uncapped ndv). A filter
    // reduces a side's row count but not its key domain (ColumnStats ndv is not
    // rescaled), so capping ndv to the filtered row count would collapse the
    // domain and make a PK-FK join look non-reducing - e.g. a filtered dimension
    // whose surviving rows exceed the fact-side FK ndv (TPC-DS q72). For an
    // unfiltered base table ndv <= rows already, so this leaves it unchanged.
    //
    // Only keys inside the range both sides span can match, so each side's
    // domain is restricted to that range: a fact references a few years of a
    // date dimension that spans two centuries.
    const double domainL = ndvL * sharedL;
    const double domainR = ndvR * sharedR;
    // An input's distinct keys are placed inside the shared range as far as
    // it holds them, and its rows follow its keys. Unfiltered, an input has
    // its whole NDV, and the range holds the fraction measured above. A
    // filtered input keeps fewer keys, which are taken to fit: a query filters
    // a dimension for the rows it joins, so a filter on the date dimension is
    // judged against the dates the fact references, not against all of them.
    auto sharedRows = [](double rows, double ndv, double domain) {
        const double distinct = std::min(ndv, rows);
        return distinct > domain
            ? rows * domain / distinct
            : rows;
    };
    const double sharedRowsL = sharedRows(leftRows, ndvL, domainL);
    const double sharedRowsR = sharedRows(rightRows, ndvR, domainR);
    double rows = sharedRowsL * sharedRowsR / std::max({domainL, domainR, 1.0});
    // Several pairs between two relations whose NDVs match can be a composite
    // key, whose columns are not independent. Pairs from different relations
    // on one side are separate keys, and their independence stands.
    if (keys.size() > 1 && twoRelations) {
        bool sameCompositeDomain = true;
        for (const auto& key : keys) {
            const double lo = std::max(1.0, std::min(key.Left.Ndv, key.Right.Ndv));
            const double hi = std::max(key.Left.Ndv, key.Right.Ndv);
            if (hi / lo > 4.0) {
                sameCompositeDomain = false;
                break;
            }
        }
        if (sameCompositeDomain) {
            rows = std::max(rows, std::max(sharedRowsL, sharedRowsR));
        }
    }
    // Returned distinct counts are clamped to input rows (distinct-in-input) for
    // semi/anti fractions and output column stats.
    ndvL = std::min(ndvL, leftRows);
    ndvR = std::min(ndvR, rightRows);
    return {rows, ndvL, ndvR};
}

// The relation that produces `column` in the rows of `op`: the operator that
// computes it, reached through the joins, filters and renaming projections
// that pass it on.
const IOperator* ColumnRelation(const TOperatorPtr& op, const std::string& column) {
    if (auto maybeJoin = TMaybeOp<TJoinOperator>(op)) {
        auto join = maybeJoin.Cast();
        return ColumnRelation(
            FieldType(join->Left()->OutputColumns().get(), column) ? join->Left() : join->Right(),
            column);
    }
    if (auto maybeFilter = TMaybeOp<TFilterOperator>(op)) {
        return ColumnRelation(maybeFilter.Cast()->Input(), column);
    }
    if (auto maybeProject = TMaybeOp<TProjectOperator>(op)) {
        auto project = maybeProject.Cast();
        for (const auto& projection : project->Projections()) {
            if (projection.Name != column) {
                continue;
            }
            if (auto maybeIdent = TMaybeNode<TIdentExpr>(projection.Expression)) {
                return ColumnRelation(project->Input(), maybeIdent.Cast()->Name);
            }
            break;
        }
    }
    return op.get();
}

// The key equalities that hold in the rows of `op`: the keys of the inner
// joins below it, through the filters that pass their rows on.
void CollectKeyEqualities(const TOperatorPtr& op, std::vector<const TJoinKey*>& out) {
    if (auto maybeFilter = TMaybeOp<TFilterOperator>(op)) {
        CollectKeyEqualities(maybeFilter.Cast()->Input(), out);
        return;
    }
    auto maybeJoin = TMaybeOp<TJoinOperator>(op);
    if (!maybeJoin) {
        return;
    }
    auto join = maybeJoin.Cast();
    if (join->JoinType() != EJoinType::Inner) {
        return;
    }
    for (const auto& key : join->Keys()) {
        out.push_back(&key);
    }
    CollectKeyEqualities(join->Left(), out);
    CollectKeyEqualities(join->Right(), out);
}

// The relations of the columns that `column` equals in the rows of `op`.
std::vector<const IOperator*> EquatedRelations(
    const TOperatorPtr& op,
    const std::string& column,
    const std::vector<const TJoinKey*>& equalities)
{
    std::vector<std::string_view> members{column};
    for (size_t i = 0; i < members.size(); ++i) {
        for (const auto* key : equalities) {
            const auto other = key->Left == members[i] ? std::string_view(key->Right)
                : key->Right == members[i] ? std::string_view(key->Left)
                : std::string_view();
            if (!other.empty() && std::ranges::find(members, other) == members.end()) {
                members.push_back(other);
            }
        }
    }
    std::vector<const IOperator*> relations;
    for (const auto member : members) {
        const auto* relation = ColumnRelation(op, std::string(member));
        if (std::ranges::find(relations, relation) == relations.end()) {
            relations.push_back(relation);
        }
    }
    return relations;
}

// Whether every key pair joins the same two relations: whether a relation on
// each side holds a column of every pair's equivalence class. ExtractEquiJoins
// keys a join with one column of a class on each side, and the joins below
// can equate it with columns of other relations, so which relations a key
// joins is a property of its class, not of the column chosen.
bool KeysJoinTwoRelations(
    const TOperatorPtr& left,
    const TOperatorPtr& right,
    const std::vector<TJoinKey>& keys)
{
    auto oneRelation = [&](const TOperatorPtr& input, std::string TJoinKey::* side) {
        std::vector<const TJoinKey*> equalities;
        CollectKeyEqualities(input, equalities);
        std::vector<const IOperator*> shared;
        for (size_t i = 0; i < keys.size(); ++i) {
            auto relations = EquatedRelations(input, keys[i].*side, equalities);
            if (i == 0) {
                shared = std::move(relations);
            } else {
                std::erase_if(shared, [&](const IOperator* relation) {
                    return std::ranges::find(relations, relation) == relations.end();
                });
            }
            if (shared.empty()) {
                return false;
            }
        }
        return true;
    };
    return oneRelation(left, &TJoinKey::Left) && oneRelation(right, &TJoinKey::Right);
}

// Fraction of the rows with key matches that keep one the residual accepts.
// A row with m matches, each accepted with probability s, loses them all
// with probability (1 - s)^m.
double MatchAccepted(double selectivity, double matches, double matchedRows) {
    if (selectivity >= 1.0) {
        return 1.0;
    }
    const double perRow = std::max(1.0, matches / std::max(matchedRows, 1.0));
    return 1.0 - std::pow(1.0 - selectivity, perRow);
}

// Selectivity of a join's residual over its inputs. The inputs are already
// filtered by the side filters derived from it, which it implies: over them
// it keeps P(residual) / P(side filters) of the key matches, rather than its
// P(residual) once more.
double ResidualSelectivity(
    const TOperatorPtr& left,
    const TOperatorPtr& right,
    const TExprPtr& residual,
    const std::vector<TExprPtr>& derivedSideFilters,
    TConstFolder& folder)
{
    if (!residual) {
        return 1.0;
    }
    double selectivity = EstimateResidual(left, right, residual, folder);
    for (const auto& filter : derivedSideFilters) {
        const double implied = EstimateResidual(left, right, filter, folder);
        if (implied > 0.0) {
            selectivity /= implied;
        }
    }
    return std::min(selectivity, 1.0);
}

// Rows of a join of two inputs whose stats are estimated.
uint64_t JoinRows(
    const TOperatorPtr& left,
    const TOperatorPtr& right,
    const std::vector<TJoinKey>& keys,
    EJoinType type,
    const TExprPtr& residualPredicate,
    const std::vector<TExprPtr>& derivedSideFilters,
    TConstFolder& folder)
{
    const auto& leftStats = *left->Stats_;
    const auto& rightStats = *right->Stats_;
    std::vector<TJoinKeyStats> keyStats;
    keyStats.reserve(keys.size());
    for (const auto& key : keys) {
        keyStats.push_back(EstimateJoinKey(left, key.Left, right, key.Right));
    }
    auto card = EstimateEquiJoin(
        leftStats.RowCount,
        rightStats.RowCount,
        keyStats,
        keys.size() > 1 && KeysJoinTwoRelations(left, right, keys));
    // The residual accepts a share of the key matches, taken as independent
    // of the keys.
    const double residual = ResidualSelectivity(
        left,
        right,
        residualPredicate,
        derivedSideFilters,
        folder);
    const double ndvL = card.NdvL;
    const double ndvR = card.NdvR;
    uint64_t rowCount = std::max<uint64_t>(1, (uint64_t)(card.Rows * residual));
    switch (type) {
        case EJoinType::LeftSemi:
        case EJoinType::LeftAnti: {
            double semiFrac = std::min(1.0, ndvR / std::max(ndvL, 1.0));
            semiFrac *= MatchAccepted(residual, card.Rows, semiFrac * (double)leftStats.RowCount);
            if (type == EJoinType::LeftAnti) {
                semiFrac = 1.0 - semiFrac;
            }
            rowCount = std::max<uint64_t>(1, (uint64_t)(semiFrac * (double)leftStats.RowCount));
            break;
        }
        case EJoinType::RightSemi:
        case EJoinType::RightAnti: {
            double semiFrac = std::min(1.0, ndvL / std::max(ndvR, 1.0));
            semiFrac *= MatchAccepted(residual, card.Rows, semiFrac * (double)rightStats.RowCount);
            if (type == EJoinType::RightAnti) {
                semiFrac = 1.0 - semiFrac;
            }
            rowCount = std::max<uint64_t>(1, (uint64_t)(semiFrac * (double)rightStats.RowCount));
            break;
        }
        case EJoinType::Left:
            rowCount = std::max(rowCount, leftStats.RowCount);
            break;
        case EJoinType::Right:
            rowCount = std::max(rowCount, rightStats.RowCount);
            break;
        case EJoinType::Full:
            rowCount = std::max(rowCount, leftStats.RowCount + rightStats.RowCount);
            break;
        case EJoinType::LeftMark:
            // Every left row, exactly once.
            rowCount = std::max<uint64_t>(1, leftStats.RowCount);
            break;
        default:
            break;
    }
    return rowCount;
}

TStatsPtr ComputeJoinStats(
    const std::shared_ptr<TJoinOperator>& join,
    TConstFolder& folder,
    const std::unordered_set<std::string>* columns = nullptr)
{
    auto leftStats = join->Left()->Stats_;
    auto rightStats = join->Right()->Stats_;
    if (!leftStats || !rightStats) {
        return nullptr;
    }
    const uint64_t rowCount = JoinRows(
        join->Left(),
        join->Right(),
        join->Keys(),
        join->JoinType(),
        join->Filter(),
        join->DerivedSideFilters(),
        folder);
    auto outputStats = std::make_shared<TStats>();

    auto keep = [&](const std::string& column) {
        return !columns || columns->contains(column);
    };
    auto jt = join->JoinType();
    if (jt == EJoinType::Inner || jt == EJoinType::Left || jt == EJoinType::Full || jt == EJoinType::LeftSemi || jt == EJoinType::LeftAnti || jt == EJoinType::LeftMark) {
        for (const auto& [colName, colStats] : leftStats->ColumnStats) {
            // assume qualification
            if (keep(colName)) {
                outputStats->ColumnStats[colName] = colStats;
            }
        }
    }
    if (jt == EJoinType::Inner || jt == EJoinType::Right || jt == EJoinType::Full || jt == EJoinType::RightSemi || jt == EJoinType::RightAnti) {
        for (const auto& [colName, colStats] : rightStats->ColumnStats) {
            // assume qualification
            if (keep(colName)) {
                outputStats->ColumnStats[colName] = colStats;
            }
        }
    }
    outputStats->RowCount = rowCount;
    outputStats->Cost = EstimateJoinCost(
        leftStats->Cost,
        rightStats->Cost,
        static_cast<double>(leftStats->RowCount),
        static_cast<double>(rightStats->RowCount),
        static_cast<double>(rowCount),
        OutputRowWidth(join),
        JoinKeyWidth(join->Left(), join->Right(), join->Keys()));

    // Post-join ndv fixup. ColumnStats are shared_ptr aliased with the child
    // (and ultimately the source) stats, so copy-on-write before touching Ndv -
    // never mutate the shared objects in place.
    auto capNdv = [](TStats::TColumnStatsPtr& slot, uint64_t cap) {
        if (!slot || !slot->Ndv || *slot->Ndv <= cap) {
            return; // only shrink a known, too-large ndv
        }
        auto copy = std::make_shared<TStats::TColumnStats>(*slot);
        copy->Ndv = cap;
        // The values left are a selection of those the histogram describes,
        // and the statistics do not say where in the range they lie. Min/max
        // still bound them.
        copy->Histogram.clear();
        slot = std::move(copy);
    };
    // A column can't have more distinct values than the surviving rows.
    for (auto& [name, slot] : outputStats->ColumnStats) {
        capNdv(slot, rowCount);
    }
    // Equi-join keys collapse to their matched distinct count ~ min(ndvL, ndvR).
    auto ndvOf = [](const TStatsPtr& s, const std::string& col) {
        auto it = s->ColumnStats.find(col);
        return (it != s->ColumnStats.end() && it->second->Ndv)
            ? *it->second->Ndv : std::numeric_limits<uint64_t>::max();
    };
    for (const auto& key : join->Keys()) {
        uint64_t keyNdv = std::min(ndvOf(leftStats, key.Left), ndvOf(rightStats, key.Right));
        if (auto it = outputStats->ColumnStats.find(key.Left); it != outputStats->ColumnStats.end()) {
            capNdv(it->second, keyNdv);
        }
        if (auto it = outputStats->ColumnStats.find(key.Right); it != outputStats->ColumnStats.end()) {
            capNdv(it->second, keyNdv);
        }
    }
    return outputStats;
}

TStatsPtr ComputeSourceStats(const std::shared_ptr<TSourceOperator>& source) {
    auto raw = source->GetSource().Stats();
    if (!raw) {
        return nullptr;
    }
    const std::string& alias = source->GetAlias();
    auto out = std::make_shared<TStats>();
    out->RowCount = raw->RowCount;
    out->Cost = static_cast<double>(out->RowCount)
        * std::max(OutputRowWidth(source), 1.0);
    for (const auto& col : source->GetSource().Schema().Columns) {
        auto it = raw->ColumnStats.find(std::string(col.Name));
        if (it != raw->ColumnStats.end()) {
            out->ColumnStats[alias + "." + std::string(col.Name)] = it->second;
        }
    }
    return out;
}

TStatsPtr ComputeUnionStats(const std::shared_ptr<TUnionAllOperator>& un) {
    auto&& outStruct = un->OutputColumns();
    if (!outStruct) {
        return nullptr;
    }
    const size_t numCols = outStruct->Fields.size();

    uint64_t rows = 0;
    double cost = 0.0;
    // Per output column: summed ndv / null count, dropped to unknown as soon as any
    // branch lacks it. min/max are left unknown (raw-bit ranges don't merge safely).
    std::vector<uint64_t> ndv(numCols, 0);
    std::vector<uint64_t> nulls(numCols, 0);
    std::vector<bool> ndvLive(numCols, true);
    std::vector<bool> nullLive(numCols, true);

    for (const auto& branch : un->Inputs()) {
        auto bs = branch->Stats_;
        auto&& bStruct = branch->OutputColumns();
        if (!bs || !bStruct || bStruct->Fields.size() != numCols) {
            return nullptr;
        }
        rows += bs->RowCount;
        cost += bs->Cost;
        for (size_t j = 0; j < numCols; ++j) {
            auto it = bs->ColumnStats.find(bStruct->Fields[j].first);
            auto src = it != bs->ColumnStats.end() ? it->second : nullptr;
            if (src && src->Ndv) {
                ndv[j] += *src->Ndv;
            } else {
                ndvLive[j] = false;
            }
            if (src && src->NullCount) {
                nulls[j] += *src->NullCount;
            } else {
                nullLive[j] = false;
            }
        }
    }

    auto out = std::make_shared<TStats>();
    out->RowCount = std::max<uint64_t>(1, rows);
    out->Cost = cost;
    for (size_t j = 0; j < numCols; ++j) {
        auto col = std::make_shared<TStats::TColumnStats>();
        if (ndvLive[j]) {
            col->Ndv = std::min(ndv[j], out->RowCount); // upper bound: branches may overlap
        }
        if (nullLive[j]) {
            col->NullCount = nulls[j];
        }
        out->ColumnStats[outStruct->Fields[j].first] = std::move(col);
    }
    return out;
}

TStatsPtr ComputeAggregateStats(const std::shared_ptr<TAggregateOperator>& aggregate)
{
    auto inputStats = aggregate->Input()->Stats_;
    if (!inputStats) {
        return nullptr;
    }

    const auto& keys = aggregate->GroupKeys();
    const double inputRows = std::max<double>(1.0, static_cast<double>(inputStats->RowCount));

    auto ndvOf = [&](const std::string& column) {
        auto it = inputStats->ColumnStats.find(column);
        if (it != inputStats->ColumnStats.end() && it->second->Ndv) {
            return static_cast<double>(*it->second->Ndv);
        }
        return UnknownNdv;
    };
    auto groupsFor = [&](const std::vector<std::string>& columns) {
        double groups = 1.0;
        for (const auto& column : columns) {
            groups *= std::max(1.0, std::min(ndvOf(column), inputRows));
            if (groups >= inputRows) {
                return inputRows;
            }
        }
        return std::min(groups, inputRows);
    };

    const bool global = !keys.empty() &&
        std::ranges::all_of(keys, [](const std::string& key) {
            return key == "__group__";
        });

    double rows;
    if (keys.empty() || global) {
        rows = 1.0;
    } else if (!aggregate->GroupingSets().empty()) {
        rows = 0.0;
        for (const auto& set : aggregate->GroupingSets()) {
            std::vector<std::string> columns;
            columns.reserve(set.size());
            for (size_t index : set) {
                if (index < keys.size()) {
                    columns.push_back(keys[index]);
                }
            }
            rows += columns.empty() ? 1.0 : groupsFor(columns);
        }
    } else {
        rows = groupsFor(keys);
    }

    auto outputStats = std::make_shared<TStats>();
    outputStats->RowCount = std::max<uint64_t>(1, static_cast<uint64_t>(rows));
    const double inputPasses = aggregate->GroupingSets().empty()
        ? 1.0
        : static_cast<double>(aggregate->GroupingSets().size());
    outputStats->Cost = inputStats->Cost
        + inputRows * inputPasses * std::max(OutputRowWidth(aggregate), 1.0);

    for (const auto& key : keys) {
        auto it = inputStats->ColumnStats.find(key);
        if (it == inputStats->ColumnStats.end()) {
            continue;
        }
        auto column = std::make_shared<TStats::TColumnStats>(*it->second);
        if (column->Ndv) {
            column->Ndv = std::min<uint64_t>(*column->Ndv, outputStats->RowCount);
        }
        column->NullCount = std::nullopt;
        column->Histogram.clear();
        outputStats->ColumnStats[key] = std::move(column);
    }
    return outputStats;
}

// The statistics of a relation that yields at most one row, such as a scalar
// subquery: however many rows its operators would produce, each column holds
// at most one value.
TStatsPtr BoundToOneRow(const TStats& stats) {
    auto bounded = std::make_shared<TStats>(stats);
    bounded->RowCount = std::min<uint64_t>(bounded->RowCount, 1);
    for (auto& [name, column] : bounded->ColumnStats) {
        if (column && column->Ndv && *column->Ndv > 1) {
            auto copy = std::make_shared<TStats::TColumnStats>(*column);
            copy->Ndv = 1;
            copy->Histogram.clear();
            column = std::move(copy);
        }
    }
    return bounded;
}

TStatsPtr ComputeStatsFor(TOperatorPtr op, TConstFolder& folder) {
    if (auto source = TMaybeOp<TSourceOperator>(op)) {
        return ComputeSourceStats(source.Cast());
    }
    if (auto ref = TMaybeOp<TCteRef>(op)) {
        return ref.Cast()->Def()->Plan->Stats_;
    }
    if (auto maybeUnion = TMaybeOp<TUnionAllOperator>(op)) {
        return ComputeUnionStats(maybeUnion.Cast());
    }

    if (auto maybeFilter = TMaybeOp<TFilterOperator>(op)) {
        return ComputeFilterStats(maybeFilter.Cast(), folder);
    }
    if (auto maybeProject = TMaybeOp<TProjectOperator>(op)) {
        return ComputeProjectStats(maybeProject.Cast());
    }
    if (auto maybeJoin = TMaybeOp<TJoinOperator>(op)) {
        return ComputeJoinStats(maybeJoin.Cast(), folder);
    }
    if (auto maybeAggregate = TMaybeOp<TAggregateOperator>(op)) {
        return ComputeAggregateStats(maybeAggregate.Cast());
    }
    if (auto maybeLate = TMaybeOp<TLateMaterializeOperator>(op)) {
        auto result = std::make_shared<TStats>();
        if (maybeLate.Cast()->Input()->Stats_) {
            result->RowCount = maybeLate.Cast()->Input()->Stats_->RowCount;
            result->Cost = maybeLate.Cast()->Input()->Stats_->Cost;
        }
        return result;
    }

    return nullptr; // TODO: sort / limit
}

} // namespace

TStatsPtr EstimateJoinStats(
    const std::shared_ptr<TJoinOperator>& join,
    TConstFolder& folder,
    const std::unordered_set<std::string>* columns)
{
    return ComputeJoinStats(join, folder, columns);
}

uint64_t EstimateInnerJoinRows(
    const TOperatorPtr& left,
    const TOperatorPtr& right,
    const std::vector<TJoinKey>& keys,
    const TExprPtr& residual,
    TConstFolder& folder)
{
    return JoinRows(left, right, keys, EJoinType::Inner, residual, {}, folder);
}

TStatsPtr EstimateStats(TOperatorPtr op, TConstFolder& folder) {
    for (const auto& input : op->Inputs()) {
        input->Stats_ = EstimateStats(input, folder);
    }
    auto stats = ComputeStatsFor(op, folder);
    if (stats && op->AtMostOneRow) {
        stats = BoundToOneRow(*stats);
    }
    return op->Stats_ = std::move(stats);
}

TStatsPtr EstimateStats(TOperatorPtr op) {
    TConstFolder folder;
    return EstimateStats(op, folder);
}

} // namespace NQdb
