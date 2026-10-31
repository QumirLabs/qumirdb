#include <qdb/exec/late_materialize_exec.h>

#include <memory>
#include <stdexcept>
#include <utility>

namespace NQdb {
namespace {

struct TLateMaterializedColumns {
    std::vector<TRowSet> Partitions;
    std::unique_ptr<TColumn[]> Columns;
};

void DestroyLateMaterializedColumns(TRowSet* rowSet) {
    auto* data = static_cast<TLateMaterializedColumns*>(rowSet->Private);
    for (auto& partition : data->Partitions) {
        Release(&partition);
    }
    delete data;
}

} // namespace

TLateMaterializeProcessor::TLateMaterializeProcessor(
    std::shared_ptr<const IPhysicalRowReader> reader,
    int32_t locatorColumn)
    : Reader_(std::move(reader))
    , LocatorColumn_(locatorColumn)
{
    if (!Reader_) {
        throw std::invalid_argument("late materialize: row reader is null");
    }
}

void TLateMaterializeProcessor::Add(TThreadExecutionContext&, TRowSet& rowSet) {
    if (LocatorColumn_ < 0 || LocatorColumn_ >= rowSet.ColumnCount) {
        throw std::runtime_error("late materialize: locator column is out of range");
    }
    const auto& column = rowSet.Columns[LocatorColumn_];
    const auto* ids = reinterpret_cast<const TPhysicalRowId*>(column.Data);
    for (int64_t row = 0; row < rowSet.RowCount; ++row) {
        if (!rowSet.Selection || rowSet.Selection[row] != 0) {
            RowIds_.push_back(ids[row]);
        }
    }
    Release(&rowSet);
    rowSet = {};
}

bool TLateMaterializeProcessor::Next(TThreadExecutionContext&, TRowSet& output) {
    if (Finished_) {
        return false;
    }
    Finished_ = true;
    if (RowIds_.empty()) {
        return false;
    }
    std::string error;
    if (!Reader_->ReadRows(RowIds_, output, &error)) {
        throw std::runtime_error("late materialize: " + error);
    }
    return true;
}

void MergeLateMaterializedColumns(
    std::vector<TRowSet>& partitions,
    TRowSet& output)
{
    if (partitions.empty()) {
        throw std::runtime_error(
            "late materialize: cannot merge an empty lookup result");
    }

    const int64_t rowCount = partitions.front().RowCount;
    int64_t columnCount = 0;
    for (const auto& partition : partitions) {
        if (partition.RowCount != rowCount) {
            throw std::runtime_error(
                "late materialize: lookup partitions have different row counts");
        }
        if (partition.Selection) {
            throw std::runtime_error(
                "late materialize: lookup partition has a selection vector");
        }
        columnCount += partition.ColumnCount;
    }

    auto data = std::make_unique<TLateMaterializedColumns>();
    data->Columns = std::make_unique<TColumn[]>(
        static_cast<size_t>(columnCount));
    int64_t outputColumn = 0;
    for (const auto& partition : partitions) {
        for (int64_t column = 0; column < partition.ColumnCount; ++column) {
            data->Columns[static_cast<size_t>(outputColumn++)] =
                partition.Columns[column];
        }
    }
    data->Partitions = std::move(partitions);

    output = TRowSet{
        .Columns = data->Columns.get(),
        .ColumnCount = columnCount,
        .RowCount = rowCount,
        .Selection = nullptr,
        .Hash = nullptr,
        .Destroy = DestroyLateMaterializedColumns,
        .Private = data.release(),
        .RefCount = 1,
    };
}

} // namespace NQdb
