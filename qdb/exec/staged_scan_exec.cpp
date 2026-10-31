#include <qdb/exec/staged_scan_exec.h>

#include <qdb/io/parquet/source.h>

#include <algorithm>
#include <utility>

namespace NQdb {

namespace {

// Owns the scanned batch and its selection while the batch is downstream.
struct TStagedBatchData {
    ~TStagedBatchData() {
        if (Input.Destroy) {
            Release(&Input);
        }
    }

    TRowSet Input = {};
    std::vector<TColumn> Columns;
    std::vector<uint8_t> Selection;
};

void DestroyStagedBatch(TRowSet* rowSet) {
    delete static_cast<TStagedBatchData*>(rowSet->Private);
}

int64_t CountSelected(const std::vector<uint8_t>& selection) {
    return std::count_if(selection.begin(), selection.end(), [](uint8_t selected) {
        return selected != 0;
    });
}

// Past half of the batch a sparse read saves little copying and still checks every row.
const uint8_t* ReadSelection(const std::vector<uint8_t>& selection, int64_t selected) {
    return selected * 2 > static_cast<int64_t>(selection.size())
        ? nullptr
        : selection.data();
}

} // namespace

TStagedScan::TStagedScan(
    std::unique_ptr<TParquetGroupedScan> scan,
    std::shared_ptr<const TStagedScanPlan> plan)
    : Scan_(std::move(scan))
    , Plan_(std::move(plan))
{}

TStagedScan::~TStagedScan() = default;

bool TStagedScan::Next(TRowSet& rowSet) {
    const auto& stages = Plan_->Stages;
    const size_t groups = stages.size() + (Plan_->Payload ? 1 : 0);
    // Reused while batches come out empty.
    auto data = std::make_unique<TStagedBatchData>();
    for (;;) {
        if (!Scan_->Next(data->Input)) {
            return false;
        }
        const auto rows = static_cast<size_t>(data->Input.RowCount);
        data->Selection.resize(rows);
        TRowSet view = data->Input;
        view.Selection = data->Selection.data();
        stages[0](view, nullptr);
        int64_t selected = CountSelected(data->Selection);
        size_t group = 1;
        for (; group < stages.size() && selected > 0; ++group) {
            Scan_->Read(group, ReadSelection(data->Selection, selected));
            Previous_.resize(rows);
            std::swap(data->Selection, Previous_);
            view.Selection = data->Selection.data();
            stages[group](view, Previous_.data());
            selected = CountSelected(data->Selection);
        }
        if (selected == 0) {
            for (; group < groups; ++group) {
                Scan_->Skip(group);
            }
            Release(&data->Input);
            data->Input = {};
            continue;
        }
        if (Plan_->Payload) {
            Scan_->Read(group, ReadSelection(data->Selection, selected));
        }

        TColumn* columns = data->Input.Columns;
        int64_t columnCount = data->Input.ColumnCount;
        if (Plan_->KeptColumns) {
            for (int32_t index : *Plan_->KeptColumns) {
                data->Columns.push_back(data->Input.Columns[index]);
            }
            columns = data->Columns.data();
            columnCount = static_cast<int64_t>(data->Columns.size());
        }
        auto* batch = data.release();
        rowSet = {
            .Columns = columns,
            .ColumnCount = columnCount,
            .RowCount = batch->Input.RowCount,
            .Selection = batch->Selection.data(),
            .Destroy = DestroyStagedBatch,
            .Private = batch,
            .RefCount = 1,
        };
        return true;
    }
}

} // namespace NQdb
