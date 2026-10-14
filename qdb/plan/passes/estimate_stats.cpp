#include "estimate_stats.h"
#include "const_fold.h"
#include "flatten_conjuncts.h"
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

#include <qumir/parser/core/printer.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

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
  | Join (equi) | |L|*|R| / max(ndv_L.key, ndv_R.key)                 | ключ: ndv=min, min/max = пересечение; остальное pass-through, ndv<=RowCount. Semi/anti - зажать по |L| |
  +-------------+-----------------------------------------------------+--------------------------------------------------------------------------------------------------------+
  | Aggregate   | prod ndv(group-key), cap на вход; без group by -> 1 | group-key: ndv/min/max сохраняются; агрегаты - min/max неизвестны (кроме min()/max()/count)            |
  +-------------+-----------------------------------------------------+--------------------------------------------------------------------------------------------------------+
  | Sort        | pass-through                                        | pass-through                                                                                           |
  +-------------+-----------------------------------------------------+--------------------------------------------------------------------------------------------------------+
  | Limit       | min(RowCount, N)                                    | pass-through                                                                                           |
  +-------------+-----------------------------------------------------+--------------------------------------------------------------------------------------------------------+
*/

double ColumnWidth(const TOperatorPtr& op, const std::string& name) {
    if (!op) {
        return 8.0;
    }
    auto structure = TMaybeType<TStructType>(op->OutputColumns());
    if (!structure) {
        return 8.0;
    }
    for (const auto& [fieldName, fieldType] : structure.Cast()->Fields) {
        if (fieldName == name) {
            return EstimateTypeWidth(fieldType);
        }
    }
    return 8.0;
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

// One side of a range on a column.
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

// overSource: the input is a table scan, so its null counts and its row count
// describe the same rows.
double EstimateSelectivity(const TExprPtr& atom, TStatsPtr inputStats, std::shared_ptr<TStructType> operatorType,
    bool overSource, TFoldedBounds& foldedBounds)
{
    if (!operatorType) {
        return 0.5;
    }

    auto isConstExpr = [&](const TExprPtr& expr) {
        return FindUnboundVars(expr).empty();
    };

    auto columnType = [&](const std::string& colName) -> TTypePtr {
        for (auto& [col, type] : operatorType->Fields) {
            if (col == colName) {
                return type;
            }
        }
        return nullptr;
    };

    // A closed expression (typed literal, cast, date + interval) as a number.
    auto constNumber = [&](const TExprPtr& expr) -> std::shared_ptr<TNumberExpr> {
        if (auto number = TMaybeNode<TNumberExpr>(expr)) {
            return number.Cast();
        }
        if (!isConstExpr(expr)) {
            return nullptr;
        }
        auto [it, inserted] = foldedBounds.try_emplace(NCore::PrintAst(expr));
        if (inserted) {
            if (auto folded = TMaybeNode<TNumberExpr>(ConstFold(expr))) {
                it->second = folded.Cast();
            }
        }
        return it->second;
    };

    // Fraction of rows with the column <= bound, or < bound when strict.
    auto fractionBelow = [&](
        const std::string& colName,
        const TExprPtr& bound,
        bool strict) -> std::optional<double>
    {
        auto colIt = inputStats->ColumnStats.find(colName);
        auto type = columnType(colName);
        // Check the statistics first: evaluating the bound is the costly step.
        if (colIt == inputStats->ColumnStats.end() || colIt->second->Histogram.empty()
            || !(TMaybeType<TFloatType>(type) || TMaybeType<TIntegerType>(type)))
        {
            return std::nullopt;
        }
        auto num = constNumber(bound);
        if (!num) {
            return std::nullopt;
        }
        if (TMaybeType<TFloatType>(type)) {
            double c = num->IsFloat()
                ? num->FloatValue
                : static_cast<double>(num->IntValue);
            return colIt->second->FractionBelow(c, strict);
        }
        if (TMaybeType<TIntegerType>(type)) {
            if (num->IsFloat()) {
                const double value = num->FloatValue;
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
                return colIt->second->FractionBelow(
                    static_cast<int64_t>(whole), strict && whole == value);
            }
            return colIt->second->FractionBelow(num->IntValue, strict);
        }
        return std::nullopt;
    };

    // `col OP const` or `const OP col` over a column with a histogram.
    auto rangeBound = [&](const TExprPtr& expr) -> std::optional<TRangeBound> {
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
            auto number = constNumber(bound);
            return number->IsFloat() ? number->FloatValue : static_cast<double>(number->IntValue);
        };
        // `col OP const`
        if (auto leftIdent = TMaybeNode<TIdentExpr>(binary->Left)) {
            const auto& name = leftIdent.Cast()->Name;
            if (auto f = fractionBelow(name, binary->Right, less != inclusive)) {
                return TRangeBound{name, less, {value(binary->Right), inclusive, *f}};
            }
        }
        // `const OP col`  ==  `col (flip OP) const`
        if (auto rightIdent = TMaybeNode<TIdentExpr>(binary->Right)) {
            const auto& name = rightIdent.Cast()->Name;
            if (auto f = fractionBelow(name, binary->Left, greater != inclusive)) {
                return TRangeBound{name, greater, {value(binary->Left), inclusive, *f}};
            }
        }
        return std::nullopt;
    };

    // Whether a value of the column can lie between the two ends: the range is
    // not empty and meets the column's min and max.
    auto rangeCanHoldValue = [&](const std::string& colName, TRangeEnd lower, TRangeEnd upper) {
        const auto& colStats = inputStats->ColumnStats.at(colName);
        if (!colStats->MinValue || !colStats->MaxValue) {
            return false; // the statistics cannot show it
        }
        double min = 0.0;
        double max = 0.0;
        if (auto integer = TMaybeType<TIntegerType>(columnType(colName))) {
            // The smallest and the largest integer between the ends.
            lower.Value = lower.Inclusive ? std::ceil(lower.Value) : std::floor(lower.Value) + 1.0;
            upper.Value = upper.Inclusive ? std::floor(upper.Value) : std::ceil(upper.Value) - 1.0;
            lower.Inclusive = upper.Inclusive = true;
            const bool isSigned = integer.Cast()->IsSigned();
            min = isSigned ? static_cast<double>(*colStats->GetMinValue<int64_t>())
                : static_cast<double>(*colStats->GetMinValue<uint64_t>());
            max = isSigned ? static_cast<double>(*colStats->GetMaxValue<int64_t>())
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
    };

    auto inRange = [&](TExprPtr expr, const std::string& colName) -> std::optional<bool> {
        if (inputStats->ColumnStats.count(colName) == 0) {
            return std::nullopt;
        }
        auto colStats = inputStats->ColumnStats[colName];
        if (!colStats->MinValue || !colStats->MaxValue) {
            return std::nullopt;
        }
        bool isIntegerType = false;
        bool isSigned = false;
        bool isFloatType = false;
        for (auto& [col, type] : operatorType->Fields) {
            if (col == colName) {
                if (auto integerType = TMaybeType<TIntegerType>(type)) {
                    isIntegerType = true;
                    isSigned = integerType.Cast()->IsSigned();
                } else if (TMaybeType<TFloatType>(type)) {
                    isFloatType = true;
                } else {
                    return std::nullopt;
                }
                break;
            }
        }
        auto number = constNumber(expr);
        if (!number) {
            return std::nullopt;
        }
        if (isIntegerType) {
            if (number->IsFloat()) {
                if (isSigned) {
                    return *colStats->GetMinValue<int64_t>() <= number->FloatValue
                        && number->FloatValue <= colStats->GetMaxValue<int64_t>();
                } else {
                    return *colStats->GetMinValue<uint64_t>() <= number->FloatValue
                        && number->FloatValue <= colStats->GetMaxValue<uint64_t>();
                }
            } else {
                if (isSigned) {
                    return *colStats->GetMinValue<int64_t>() <= number->IntValue
                        && number->IntValue <= colStats->GetMaxValue<int64_t>();
                } else {
                    return *colStats->GetMinValue<uint64_t>() <= number->IntValue
                        && number->IntValue <= colStats->GetMaxValue<uint64_t>();
                }
            }
        } else {
            // float
            // TODO: only double supported
            if (number->IsFloat()) {
                return *colStats->GetMinValue<double>() <= number->FloatValue
                    && number->FloatValue <= colStats->GetMaxValue<double>();
            } else {
                return *colStats->GetMinValue<double>() <= number->IntValue
                    && number->IntValue <= colStats->GetMaxValue<double>();
            }
        }
    };

    // `col IN (c1, ..., cn)`: the number of distinct constants that can equal a
    // value of col, and the distinct count of col. NULL equals no value, and a
    // constant outside the column's min and max neither. Empty when an item
    // cannot be resolved or the column has no distinct count.
    auto inListValues = [&](const TExprPtr& expr) -> std::optional<std::pair<double, double>> {
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
        const auto& name = column.Cast()->Name;
        auto colIt = inputStats->ColumnStats.find(name);
        if (colIt == inputStats->ColumnStats.end() || !colIt->second->Ndv || *colIt->second->Ndv == 0) {
            return std::nullopt;
        }
        std::set<double> numbers;
        std::set<std::string> strings;
        for (size_t i = 1; i < call->Args.size(); ++i) {
            const auto& item = call->Args[i];
            if (auto maybeNull = TMaybeNode<TCallExpr>(item)) {
                auto nullCallee = TMaybeNode<TIdentExpr>(maybeNull.Cast()->Callee);
                if (nullCallee && nullCallee.Cast()->Name == "qdb_sql_null") {
                    continue;
                }
            }
            if (auto maybeCast = TMaybeNode<TCastExpr>(item)) {
                if (auto literal = TMaybeNode<TStringLiteralExpr>(maybeCast.Cast()->Operand)) {
                    strings.insert(literal.Cast()->Value);
                    continue;
                }
            }
            auto number = constNumber(item);
            if (!number) {
                return std::nullopt;
            }
            const double value = number->IsFloat()
                ? number->FloatValue
                : static_cast<double>(number->IntValue);
            if (std::isnan(value) || inRange(item, name) == false) {
                continue;
            }
            numbers.insert(value);
        }
        return std::pair{static_cast<double>(numbers.size() + strings.size()),
            static_cast<double>(*colIt->second->Ndv)};
    };

    if (auto maybeUnary = TMaybeNode<TUnaryExpr>(atom)) {
        auto unary = maybeUnary.Cast();
        if (unary->Operator == "!"_op) {
            // NOT keeps the rows where the operand is FALSE. If it can also be
            // UNKNOWN, that is not the complement of TRUE, and the statistics
            // do not say how many rows are UNKNOWN.
            if (MayBeNull(unary->Operand, *operatorType)) {
                return 0.5;
            }
            // NOT IN is the chain of `<>` it stands for: each value keeps
            // 1 - 1/ndv of the rows.
            if (auto values = inListValues(unary->Operand)) {
                return std::pow(1.0 - 1.0 / values->second, values->first);
            }
            return 1.0 - EstimateSelectivity(
                unary->Operand, inputStats, operatorType, overSource, foldedBounds);
        }
    }

    // col LIKE pattern -> qdb_string_view_sql_like; assume high selectivity.
    if (auto maybeCall = TMaybeNode<TCallExpr>(atom)) {
        auto call = maybeCall.Cast();
        if (auto maybeCallee = TMaybeNode<TIdentExpr>(call->Callee)) {
            auto callee = maybeCallee.Cast();
            if (callee->Name == "qdb_string_view_sql_like") {
                return LikeSelectivity;
            }
            // col IS NULL -> qdb_is_null(col): the share of nulls in the table.
            if (callee->Name == "qdb_is_null" && call->Args.size() == 1
                && overSource && inputStats->RowCount > 0)
            {
                if (auto column = TMaybeNode<TIdentExpr>(call->Args[0])) {
                    auto colIt = inputStats->ColumnStats.find(column.Cast()->Name);
                    if (colIt != inputStats->ColumnStats.end() && colIt->second->NullCount) {
                        return static_cast<double>(*colIt->second->NullCount)
                            / static_cast<double>(inputStats->RowCount);
                    }
                }
            }
            // col IN (c1, ..., cn) -> qdb_in_list(col, c1, ..., cn)
            if (auto values = inListValues(atom)) {
                return std::min(1.0, values->first / values->second);
            }
        }
    }

    if (auto maybeBinary = TMaybeNode<TBinaryExpr>(atom)) {
        auto binary = maybeBinary.Cast();
        if (binary->Operator == "&&"_op) {
            std::vector<TExprPtr> conjuncts;
            FlattenConjuncts(atom, conjuncts);
            // Bounds on one column form one range. Multiplying them as
            // independent predicates distorts a narrow range a lot.
            std::map<std::string, TColumnRange> ranges;
            double selectivity = 1.0;
            for (const auto& conjunct : conjuncts) {
                if (auto bound = rangeBound(conjunct)) {
                    auto& range = ranges[bound->Column];
                    const auto& end = bound->End;
                    // The tighter end wins; at the same constant the strict one.
                    if (bound->Upper) {
                        if (!range.Upper || end.Value < range.Upper->Value
                            || (end.Value == range.Upper->Value && !end.Inclusive))
                        {
                            range.Upper = end;
                        }
                    } else if (!range.Lower || end.Value > range.Lower->Value
                        || (end.Value == range.Lower->Value && !end.Inclusive))
                    {
                        range.Lower = end;
                    }
                } else {
                    selectivity *= EstimateSelectivity(
                        conjunct, inputStats, operatorType, overSource, foldedBounds);
                }
            }
            for (const auto& [column, range] : ranges) {
                const double upper = range.Upper ? range.Upper->Below : 1.0;
                const double lower = range.Lower ? range.Lower->Below : 0.0;
                double fraction = std::max(upper - lower, 0.0);
                // The histogram cannot resolve a range narrower than a bucket,
                // but a range that can hold a value of the column keeps one
                // distinct value. It never keeps more than either end alone.
                auto ndv = inputStats->ColumnStats[column]->Ndv;
                if (range.Lower && range.Upper && ndv && *ndv > 0
                    && rangeCanHoldValue(column, *range.Lower, *range.Upper))
                {
                    fraction = std::min({std::max(fraction, 1.0 / static_cast<double>(*ndv)),
                        upper, 1.0 - lower});
                }
                selectivity *= fraction;
            }
            return selectivity;
        }

        if (binary->Operator == "==" || binary->Operator == "!=") {
            double selectivity = 0.5;
            auto leftIdent = TMaybeNode<TIdentExpr>(binary->Left);
            auto rightIdent = TMaybeNode<TIdentExpr>(binary->Right);
            if (leftIdent && rightIdent) {
                const auto& leftName = leftIdent.Cast()->Name;
                const auto& rightName = rightIdent.Cast()->Name;
                // NaN != NaN, so a float column keeps the general estimate below.
                auto type = columnType(leftName);
                const bool mayBeNaN = type
                    && TMaybeType<TFloatType>(UnwrapNamedType(UnwrapNullableType(type)));
                if (leftName == rightName && !mayBeNaN) {
                    // A column equals itself on every non-NULL row, whatever its
                    // NDV; `x != x` holds on no row.
                    auto it = inputStats->ColumnStats.find(leftName);
                    const double nulls = it != inputStats->ColumnStats.end()
                        ? static_cast<double>(it->second->NullCount.value_or(0))
                        : 0.0;
                    const double rows = std::max(
                        1.0, static_cast<double>(inputStats->RowCount));
                    return binary->Operator == "!="_op
                        ? 0.0
                        : 1.0 - std::min(1.0, nulls / rows);
                }
                if (inputStats->ColumnStats.count(leftName) > 0
                    && inputStats->ColumnStats.count(rightName) > 0)
                {
                    auto leftNdv = inputStats->ColumnStats[leftName]->Ndv;
                    auto rightNdv = inputStats->ColumnStats[rightName]->Ndv;
                    if (leftNdv && rightNdv) {
                        selectivity = 1.0 / std::max(*leftNdv, *rightNdv);
                    }
                }
            } else if (leftIdent && isConstExpr(binary->Right)) {
                const auto& leftName = leftIdent.Cast()->Name;
                if (inputStats->ColumnStats.count(leftName) > 0) {
                    auto inRangeResult = inRange(binary->Right, leftName);
                    if (inRangeResult.has_value() && !*inRangeResult) {
                        selectivity = 0.0;
                    } else {
                        auto leftNdv = inputStats->ColumnStats[leftName]->Ndv;
                        if (leftNdv) {
                            selectivity = 1.0 / *leftNdv;
                        }
                    }
                }
            } else if (rightIdent && isConstExpr(binary->Left)) {
                const auto& rightName = rightIdent.Cast()->Name;
                if (inputStats->ColumnStats.count(rightName) > 0) {
                    auto inRangeResult = inRange(binary->Left, rightName);
                    if (inRangeResult.has_value() && !*inRangeResult) {
                        selectivity = 0.0;
                    } else {
                        auto rightNdv = inputStats->ColumnStats[rightName]->Ndv;
                        if (rightNdv) {
                            selectivity = 1.0 / *rightNdv;
                        }
                    }
                }
            }
            if (binary->Operator == "!=") {
                selectivity = 1.0 - selectivity;
            }
            return selectivity;
        } // ==, !=

        if (auto bound = rangeBound(atom)) {
            return bound->Upper ? bound->End.Below : 1.0 - bound->End.Below;
        }
    }

    return 0.5; // fallback selectivity for unknown atoms
}

TStatsPtr ComputeFilterStats(const std::shared_ptr<TFilterOperator>& filter, TFoldedBounds& foldedBounds) {
    auto inputStats = filter->Input()->Stats_;
    if (!inputStats) {
        return nullptr;
    }
    double selectivity = EstimateSelectivity(filter->Predicate(), inputStats,
        TMaybeType<TStructType>(filter->Input()->OutputColumns()).Cast(),
        TMaybeOp<TSourceOperator>(filter->Input()), foldedBounds);
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

TStatsPtr ComputeJoinStats(const std::shared_ptr<TJoinOperator>& join) {
    auto leftStats = join->Left()->Stats_;
    auto rightStats = join->Right()->Stats_;
    if (!leftStats || !rightStats) {
        return nullptr;
    }
    std::vector<std::pair<double, double>> keyNdvs;
    for (const auto& key : join->Keys()) {
        double leftNdv = UnknownNdv;
        double rightNdv = UnknownNdv;
        auto leftColStatsIt = leftStats->ColumnStats.find(key.Left);
        auto rightColStatsIt = rightStats->ColumnStats.find(key.Right);
        if (leftColStatsIt != leftStats->ColumnStats.end() && leftColStatsIt->second->Ndv) {
            leftNdv = *leftColStatsIt->second->Ndv;
        }
        if (rightColStatsIt != rightStats->ColumnStats.end() && rightColStatsIt->second->Ndv) {
            rightNdv = *rightColStatsIt->second->Ndv;
        }
        keyNdvs.emplace_back(leftNdv, rightNdv);
    }
    auto card = EstimateEquiJoin(leftStats->RowCount, rightStats->RowCount, keyNdvs);
    double ndvL = card.NdvL;
    double ndvR = card.NdvR;
    auto outputStats = std::make_shared<TStats>();

    auto jt = join->JoinType();
    if (jt == EJoinType::Inner || jt == EJoinType::Left || jt == EJoinType::Full || jt == EJoinType::LeftSemi || jt == EJoinType::LeftAnti) {
        for (const auto& [colName, colStats] : leftStats->ColumnStats) {
            // assume qualification
            outputStats->ColumnStats[colName] = colStats;
        }
    }
    if (jt == EJoinType::Inner || jt == EJoinType::Right || jt == EJoinType::Full || jt == EJoinType::RightSemi || jt == EJoinType::RightAnti) {
        for (const auto& [colName, colStats] : rightStats->ColumnStats) {
            // assume qualification
            outputStats->ColumnStats[colName] = colStats;
        }
    }
    uint64_t rowCount = std::max<uint64_t>(1, (uint64_t)card.Rows);
    switch (join->JoinType()) {
        case EJoinType::LeftSemi:
        case EJoinType::LeftAnti: {
            double semiFrac = std::min(1.0, ndvR / std::max(ndvL, 1.0));
            if (join->JoinType() == EJoinType::LeftAnti) {
                semiFrac = 1.0 - semiFrac;
            }
            rowCount = std::max<uint64_t>(1, (uint64_t)(semiFrac * (double)leftStats->RowCount));
            break;
        }
        case EJoinType::RightSemi:
        case EJoinType::RightAnti: {
            double semiFrac = std::min(1.0, ndvL / std::max(ndvR, 1.0));
            if (join->JoinType() == EJoinType::RightAnti) {
                semiFrac = 1.0 - semiFrac;
            }
            rowCount = std::max<uint64_t>(1, (uint64_t)(semiFrac * (double)rightStats->RowCount));
            break;
        }
        case EJoinType::Left:
            rowCount = std::max(rowCount, leftStats->RowCount);
            break;
        case EJoinType::Right:
            rowCount = std::max(rowCount, rightStats->RowCount);
            break;
        case EJoinType::Full:
            rowCount = std::max(rowCount, leftStats->RowCount + rightStats->RowCount);
            break;
        default:
            break;
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

TStatsPtr ComputeStatsFor(TOperatorPtr op, TFoldedBounds& foldedBounds) {
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
        return ComputeFilterStats(maybeFilter.Cast(), foldedBounds);
    }
    if (auto maybeProject = TMaybeOp<TProjectOperator>(op)) {
        return ComputeProjectStats(maybeProject.Cast());
    }
    if (auto maybeJoin = TMaybeOp<TJoinOperator>(op)) {
        return ComputeJoinStats(maybeJoin.Cast());
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

TJoinCardinality EstimateEquiJoin(
    double leftRows,
    double rightRows,
    const std::vector<std::pair<double, double>>& keyNdvs)
{
    double ndvL = 1.0;
    double ndvR = 1.0;
    for (const auto& [leftNdv, rightNdv] : keyNdvs) {
        ndvL *= leftNdv;
        ndvR *= rightNdv;
    }
    // The inner-join denominator uses the key DOMAIN (uncapped ndv). A filter
    // reduces a side's row count but not its key domain (ColumnStats ndv is not
    // rescaled), so capping ndv to the filtered row count would collapse the
    // domain and make a PK-FK join look non-reducing - e.g. a filtered dimension
    // whose surviving rows exceed the fact-side FK ndv (TPC-DS q72). For an
    // unfiltered base table ndv <= rows already, so this leaves it unchanged.
    double denom = std::max({ndvL, ndvR, 1.0});
    double rows = leftRows * rightRows / denom;
    if (keyNdvs.size() > 1) {
        bool sameCompositeDomain = true;
        for (const auto& [leftNdv, rightNdv] : keyNdvs) {
            const double lo = std::max(1.0, std::min(leftNdv, rightNdv));
            const double hi = std::max(leftNdv, rightNdv);
            if (hi / lo > 4.0) {
                sameCompositeDomain = false;
                break;
            }
        }
        if (sameCompositeDomain) {
            rows = std::max(rows, std::max(leftRows, rightRows));
        }
    }
    // Returned distinct counts are clamped to input rows (distinct-in-input) for
    // semi/anti fractions and output column stats.
    ndvL = std::min(ndvL, leftRows);
    ndvR = std::min(ndvR, rightRows);
    return {rows, ndvL, ndvR};
}

TStatsPtr EstimateStats(TOperatorPtr op, TFoldedBounds& foldedBounds) {
    for (const auto& input : op->Inputs()) {
        input->Stats_ = EstimateStats(input, foldedBounds);
    }

    return op->Stats_ = ComputeStatsFor(op, foldedBounds);
}

TStatsPtr EstimateStats(TOperatorPtr op) {
    TFoldedBounds foldedBounds;
    return EstimateStats(op, foldedBounds);
}

} // namespace NQdb
