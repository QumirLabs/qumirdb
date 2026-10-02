#include "estimate_stats.h"
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

#include <algorithm>
#include <cmath>
#include <limits>

namespace NQdb {

using namespace NQumir::NAst;

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
  - col = const → 1/ndv; если const ∉ [min,max] → 0. Точнее — по бакету гистограммы.
  - col < / <= / > / >= диапазон → equi-depth гистограмма: доля = (кол-во границ по нужную сторону)/N с интерполяцией в «граничном» бакете. Нет гистограммы → линейно по (const-min)/(max-min).
  - col BETWEEN a AND b (или два range-конъюнкта на одну колонку) → доля гистограммы в [a,b].
  - col IS NULL → null_count/RowCount; IS NOT NULL → 1 - ….
  - col IN (…) → min(1, |list|/ndv).
  - col1 = col2 (residual) → 1/max(ndv1, ndv2).
  min(ndv, RowCount).


  ┌─────────────┬─────────────────────────────────────────────────┬───────────────────────────────────────────────────────────────────────────────────────────────────────┐
  │  Оператор   │                    RowCount                     │                                                колонки                                                │
  ├─────────────┼─────────────────────────────────────────────────┼───────────────────────────────────────────────────────────────────────────────────────────────────────┤
  │ Source      │ из футера (TParquetSource::Stats())             │ min/max/null/ndv/hist                                                                                 │
  ├─────────────┼─────────────────────────────────────────────────┼───────────────────────────────────────────────────────────────────────────────────────────────────────┤
  │ Project     │ без изменений                                   │ прямые ссылки — pass-through; производные (a*b, func) — stats теряются                                │
  ├─────────────┼─────────────────────────────────────────────────┼───────────────────────────────────────────────────────────────────────────────────────────────────────┤
  │ Join (equi) │ |L|·|R| / max(ndv_L.key, ndv_R.key)             │ ключ: ndv=min, min/max = пересечение; остальное pass-through, ndv≤RowCount. Semi/anti — зажать по |L| │
  ├─────────────┼─────────────────────────────────────────────────┼───────────────────────────────────────────────────────────────────────────────────────────────────────┤
  │ Aggregate   │ Π ndv(group-key), cap на вход; без group by → 1 │ group-key: ndv/min/max сохраняются; агрегаты — min/max неизвестны (кроме min()/max()/count)           │
  ├─────────────┼─────────────────────────────────────────────────┼───────────────────────────────────────────────────────────────────────────────────────────────────────┤
  │ Sort        │ pass-through                                    │ pass-through                                                                                          │
  ├─────────────┼─────────────────────────────────────────────────┼───────────────────────────────────────────────────────────────────────────────────────────────────────┤
  │ Limit       │ min(RowCount, N)                                │ pass-through                                                                                          │
  └─────────────┴─────────────────────────────────────────────────┴───────────────────────────────────────────────────────────────────────────────────────────────────────┘
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

double EstimateSelectivity(const TExprPtr& atom, TStatsPtr inputStats, std::shared_ptr<TStructType> operatorType) {
    // TODO: eval consts

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

    auto fractionBelow = [&](
        const std::string& colName,
        const std::shared_ptr<TNumberExpr>& num) -> std::optional<double>
    {
        auto colIt = inputStats->ColumnStats.find(colName);
        if (colIt == inputStats->ColumnStats.end()) {
            return std::nullopt;
        }
        auto type = columnType(colName);
        if (TMaybeType<TFloatType>(type)) {
            double c = num->IsFloat()
                ? num->FloatValue
                : static_cast<double>(num->IntValue);
            return colIt->second->FractionBelow(c);
        }
        if (TMaybeType<TIntegerType>(type)) {
            int64_t c = num->IsFloat()
                ? static_cast<int64_t>(num->FloatValue)
                : num->IntValue;
            return colIt->second->FractionBelow(c);
        }
        return std::nullopt;
    };

    auto inRange = [&](TExprPtr expr, const std::string& colName) -> std::optional<bool> {
        if (inputStats->ColumnStats.count(colName) == 0) {
            return std::nullopt;
        }
        auto colStats = inputStats->ColumnStats[colName];
        if (!colStats->MinValue || !colStats->MaxValue) {
            return std::nullopt;
        }
        auto maybeNumber = TMaybeNode<TNumberExpr>(expr);
        if (!maybeNumber) {
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
        auto number = maybeNumber.Cast();
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

    // col LIKE pattern -> qdb_string_view_sql_like; assume high selectivity.
    if (auto maybeCall = TMaybeNode<TCallExpr>(atom)) {
        if (auto callee = TMaybeNode<TIdentExpr>(maybeCall.Cast()->Callee)) {
            if (callee.Cast()->Name == "qdb_string_view_sql_like") {
                return LikeSelectivity;
            }
        }
    }

    if (auto maybeBinary = TMaybeNode<TBinaryExpr>(atom)) {
        auto binary = maybeBinary.Cast();
        if (binary->Operator == "==" || binary->Operator == "!=") {
            double selectivity = 0.5;
            auto leftIdent = TMaybeNode<TIdentExpr>(binary->Left);
            auto rightIdent = TMaybeNode<TIdentExpr>(binary->Right);
            if (leftIdent && rightIdent) {
                auto leftName = std::string(leftIdent.Cast()->Name);
                auto rightName = std::string(rightIdent.Cast()->Name);
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
                auto leftName = std::string(leftIdent.Cast()->Name);
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
                auto rightName = std::string(rightIdent.Cast()->Name);
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

        if (binary->Operator == "<" || binary->Operator == "<="
            || binary->Operator == ">" || binary->Operator == ">=") {
            const bool greater = binary->Operator == ">" || binary->Operator == ">=";
            auto leftIdent = TMaybeNode<TIdentExpr>(binary->Left);
            auto rightIdent = TMaybeNode<TIdentExpr>(binary->Right);
            auto leftNumber = TMaybeNode<TNumberExpr>(binary->Left);
            auto rightNumber = TMaybeNode<TNumberExpr>(binary->Right);
            // `col OP const`
            if (leftIdent && rightNumber) {
                if (auto f = fractionBelow(std::string(leftIdent.Cast()->Name), rightNumber.Cast())) {
                    return greater ? 1.0 - *f : *f;
                }
            }
            // `const OP col`  ==  `col (flip OP) const`
            if (rightIdent && leftNumber) {
                if (auto f = fractionBelow(std::string(rightIdent.Cast()->Name), leftNumber.Cast())) {
                    return greater ? *f : 1.0 - *f;
                }
            }
        }
    }

    return 0.5; // fallback selectivity for unknown atoms
}

TStatsPtr ComputeFilterStats(const std::shared_ptr<TFilterOperator>& filter) {
    auto inputStats = filter->Input()->Stats_;
    if (!inputStats) {
        return nullptr;
    }
    std::vector<TExprPtr> conjuncts;
    FlattenConjuncts(filter->Predicate(), conjuncts);
    double selectivity = 1.0;
    for (const auto& conjunct : conjuncts) {
        selectivity *= EstimateSelectivity(conjunct, inputStats, TMaybeType<TStructType>(filter->Input()->OutputColumns()).Cast());
    }
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
    // (and ultimately the source) stats, so copy-on-write before touching Ndv —
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

TStatsPtr ComputeStatsFor(TOperatorPtr op) {
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
        return ComputeFilterStats(maybeFilter.Cast());
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
    // domain and make a PK–FK join look non-reducing — e.g. a filtered dimension
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

TStatsPtr EstimateStats(TOperatorPtr op) {
    for (const auto& input : op->Inputs()) {
        input->Stats_ = EstimateStats(input);
    }

    return op->Stats_ = ComputeStatsFor(op);
}

} // namespace NQdb
