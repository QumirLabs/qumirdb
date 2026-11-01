#include <qdb/exec/staged_scan_exec.h>

#include <qdb/io/parquet/source.h>

#include <algorithm>
#include <utility>

namespace NQdb {

namespace {

// As many batches as the queue before a runtime filter task held while it waited.
constexpr size_t MaxBatchesAhead = 4;

// Owns the scanned batch, its selection and key hashes while the batch is downstream.
struct TStagedBatchData {
    ~TStagedBatchData() {
        if (Input.Destroy) {
            Release(&Input);
        }
    }

    TRowSet Input = {};
    std::vector<TColumn> Columns;
    std::vector<uint8_t> Selection;
    std::vector<uint64_t> Hashes;
};

void DestroyStagedBatch(TRowSet* rowSet) {
    delete static_cast<TStagedBatchData*>(rowSet->Private);
}

int64_t CountSelected(const std::vector<uint8_t>& selection) {
    return std::count_if(selection.begin(), selection.end(), [](uint8_t selected) {
        return selected != 0;
    });
}

} // namespace

// A batch on its way through the steps.
struct TStagedScan::TBatch {
    std::unique_ptr<TStagedBatchData> Data = std::make_unique<TStagedBatchData>();
    size_t Step = 0;
    // Column groups read so far after the first.
    size_t Group = 0;
    // Until a step selects, every row is selected.
    bool Selecting = false;
    int64_t Selected = 0;
    bool Hashed = false;
};

TStagedScan::TStagedScan(
    std::unique_ptr<TParquetGroupedScan> scan,
    std::shared_ptr<const TStagedScanPlan> plan)
    : Scan_(std::move(scan))
    , Plan_(std::move(plan))
    , ProbeStates_(Plan_->Steps.size())
{}

TStagedScan::~TStagedScan() = default;

EStagedScanResult TStagedScan::Next(TRowSet& rowSet) {
    for (;;) {
        if (!Batches_.empty()) {
            if (Advance(*Batches_.front(), true)) {
                auto batch = std::move(Batches_.front());
                Batches_.pop_front();
                if (Emit(*batch, rowSet)) {
                    return EStagedScanResult::Batch;
                }
                // Highly selective filters drop most batches: keep the buffers of one.
                Release(&batch->Data->Input);
                batch->Data->Input = {};
                Spare_ = std::move(batch);
                continue;
            }
            if (Batches_.size() >= MaxBatchesAhead || Exhausted_) {
                return EStagedScanResult::Pending;
            }
        } else if (Exhausted_) {
            return EStagedScanResult::Finished;
        }
        auto batch = Spare_
            ? std::move(Spare_)
            : std::make_unique<TBatch>();
        if (!Scan_->Next(batch->Data->Input)) {
            Exhausted_ = true;
            continue;
        }
        batch->Step = 0;
        batch->Group = 0;
        batch->Selecting = false;
        batch->Selected = batch->Data->Input.RowCount;
        batch->Hashed = false;
        if (!Batches_.empty()) {
            Advance(*batch, false);
        }
        Batches_.push_back(std::move(batch));
    }
}

// Runs the steps of `batch` until one waits: a runtime filter is unpublished, or, behind the
// first batch, a step reads a column group, as the groups are read in batch order.
bool TStagedScan::Advance(TBatch& batch, bool first) {
    auto& data = *batch.Data;
    const auto rows = static_cast<size_t>(data.Input.RowCount);
    for (; batch.Step < Plan_->Steps.size() && batch.Selected > 0; ++batch.Step) {
        const auto& step = Plan_->Steps[batch.Step];
        if ((step.ReadsGroup && !first) || (step.RuntimeFilter && !step.RuntimeFilter->Probe->Ready())) {
            return false;
        }
        if (step.ReadsGroup) {
            ReadGroup(batch, ++batch.Group);
        }
        TRowSet view = data.Input;
        view.Selection = batch.Selecting ? data.Selection.data() : nullptr;
        Next_.resize(rows);
        if (step.Filter) {
            TRowSet output = view;
            output.Selection = Next_.data();
            (*step.Filter)(output, view.Selection);
            batch.Selected = CountSelected(Next_);
        } else {
            const auto& filter = *step.RuntimeFilter;
            auto& state = ProbeStates_[batch.Step];
            if (!state) {
                state = filter.Probe->CreateProbeState();
            }
            const bool carries = Plan_->HashStep == batch.Step;
            const auto kept = ProbeRuntimeFilter(filter, *state, view, carries ? data.Hashes : Hashes_, Next_);
            // A paused or pass-all filter passes the batch.
            if (!kept) {
                continue;
            }
            batch.Hashed = batch.Hashed || carries;
            batch.Selected = static_cast<int64_t>(*kept);
        }
        std::swap(data.Selection, Next_);
        batch.Selecting = true;
    }
    return true;
}

// Reads the payload of a batch whose steps are done; false drops a batch no row passed.
bool TStagedScan::Emit(TBatch& batch, TRowSet& rowSet) {
    auto& data = *batch.Data;
    const size_t groups = Plan_->MaxSparseShares.size();
    if (batch.Selected == 0) {
        for (++batch.Group; batch.Group <= groups; ++batch.Group) {
            Scan_->Skip(data.Input, batch.Group);
        }
        return false;
    }
    for (++batch.Group; batch.Group <= groups; ++batch.Group) {
        ReadGroup(batch, batch.Group);
    }
    TColumn* columns = data.Input.Columns;
    int64_t columnCount = data.Input.ColumnCount;
    if (Plan_->KeptColumns) {
        for (int32_t index : *Plan_->KeptColumns) {
            data.Columns.push_back(data.Input.Columns[index]);
        }
        columns = data.Columns.data();
        columnCount = static_cast<int64_t>(data.Columns.size());
    }
    auto* owned = batch.Data.release();
    rowSet = {
        .Columns = columns,
        .ColumnCount = columnCount,
        .RowCount = owned->Input.RowCount,
        .Selection = batch.Selecting ? owned->Selection.data() : nullptr,
        .Hash = batch.Hashed ? owned->Hashes.data() : nullptr,
        .Destroy = DestroyStagedBatch,
        .Private = owned,
        .RefCount = 1,
    };
    return true;
}

void TStagedScan::ReadGroup(TBatch& batch, size_t group) {
    const auto rows = static_cast<double>(batch.Data->Input.RowCount);
    const bool sparse = batch.Selecting &&
        static_cast<double>(batch.Selected) <= Plan_->MaxSparseShares[group - 1] * rows;
    Scan_->Read(batch.Data->Input, group, sparse ? batch.Data->Selection.data() : nullptr);
}

} // namespace NQdb
