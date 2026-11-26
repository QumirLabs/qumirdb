#include <qdb/exec/window_exec.h>

#include <qdb/exec/kernel_rowset.h>
#include <qdb/exec/sort_key_ops.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace NQdb {

TSortRadixKernel MakeWindowKernel(TKernelCompiler::TSortRadixCompositeDispatch dispatch)
{
    return {.Enabled = true, .Dispatch = std::move(dispatch)};
}

namespace {

struct TCarryRowSetData {
    int64_t Value = 0;
    TColumn Column{};
};

void DestroyCarryRowSet(TRowSet* rowSet) {
    delete static_cast<TCarryRowSetData*>(rowSet->Private);
}

uint64_t SelectedRows(const TRowSet& rowSet) {
    if (!rowSet.Selection) {
        return static_cast<uint64_t>(rowSet.RowCount);
    }
    return static_cast<uint64_t>(std::count_if(
        rowSet.Selection, rowSet.Selection + rowSet.RowCount,
        [](uint8_t selected) { return selected != 0; }));
}

} // namespace

TWindowProcessor::TWindowProcessor(
    std::vector<TSortKey> keys,
    std::vector<TSortColumnRef> keyColumns,
    TSortRadixKernel kernel,
    TKernelCompiler::TWindowRangeDispatch rangeKernel)
    : Keys_(std::move(keys))
    , KeyColumns_(std::move(keyColumns))
    , Kernel_(std::move(kernel))
    , RangeKernel_(std::move(rangeKernel))
{}

void TWindowProcessor::Add(TRowSet& rowSet)
{
    if (Emitted_) {
        throw std::runtime_error("window processor is already finished");
    }
    const int32_t batchIdx = Store_.PushBatch(rowSet);
    for (int32_t row = 0; row < rowSet.RowCount; ++row) {
        if (RowSelected(rowSet, row)) {
            Rows_.push_back(MakeRowId(batchIdx, row));
        }
    }
    rowSet = {};
}

void TWindowProcessor::AddCarry(TRowSet& carry)
{
    Carries_.PushBatch(carry);
    carry = {};
}

void TWindowProcessor::Finish() {}

bool TWindowProcessor::Next(TRowSet& rowSet)
{
    if (Emitted_) {
        return false;
    }
    Emitted_ = true;
    if (Rows_.empty()) {
        return false;
    }
    if (!Kernel_.Enabled || !Kernel_.Dispatch) {
        throw std::runtime_error("window kernel is unavailable");
    }
    if (KeyColumns_.size() != Keys_.size()) {
        throw std::runtime_error("window: key column mismatch");
    }

    // The nullable radix (used when any key is nullable) needs a 257th bucket
    // for the segregated NULL rows.
    const bool nullable = std::any_of(
        KeyColumns_.begin(), KeyColumns_.end(),
        [](const TSortColumnRef& keyColumn) { return keyColumn.Value.Nullable; });
    std::vector<TRowId> work(Rows_.size() * RadixWorkStride(KeyColumns_));
    std::vector<uint32_t> counts(nullable ? 257 : 256);
    auto descs = std::make_unique<bool[]>(Keys_.size());
    auto nullsFirsts = std::make_unique<bool[]>(Keys_.size());
    for (size_t k = 0; k < Keys_.size(); ++k) {
        descs[k] = Keys_[k].Direction == ESortDirection::Desc;
        nullsFirsts[k] = EffectiveSortNulls(Keys_[k].Direction, Keys_[k].Nulls) == ESortNulls::First;
    }

    if (RangeKernel_) {
        const int64_t out = RangeKernel_.RunCarried(
            const_cast<TRowSet*>(Store_.Data()),
            Rows_.data(),
            work.data(),
            counts.data(),
            static_cast<int64_t>(Rows_.size()),
            descs.get(),
            nullsFirsts.get(),
            const_cast<TRowSet*>(Carries_.Data()),
            Carries_.BatchCount(),
            &rowSet);
        if (out <= 0) {
            return false;
        }
        AdoptKernelOwnedRowSet(rowSet);
        return true;
    }

    // One call: sort the row-ids in place, then materialize the whole partition
    // (input columns + window columns) into a kernel-owned rowset.
    const int64_t out = Kernel_.Dispatch(
        const_cast<TRowSet*>(Store_.Data()),
        Rows_.data(),
        work.data(),
        counts.data(),
        static_cast<int64_t>(Rows_.size()),
        descs.get(),
        nullsFirsts.get(),
        true,
        0,
        static_cast<int64_t>(Rows_.size()),
        &rowSet);
    if (out <= 0) {
        return false;
    }
    AdoptKernelOwnedRowSet(rowSet);
    return true;
}

TWindowSampleProcessor::TWindowSampleProcessor(
    TKernelCompiler::TWindowRangeDispatch kernel, uint64_t sampleRows)
    : Kernel_(std::move(kernel))
    , SampleRows_(std::max<uint64_t>(sampleRows, 1))
{}

TWindowSampleProcessor::~TWindowSampleProcessor()
{
    for (size_t i = Cursor_; i < Batches_.size(); ++i) {
        Release(&Batches_[i]);
    }
}

void TWindowSampleProcessor::Add(TRowSet& rowSet)
{
    SelectedRows_ += SelectedRows(rowSet);
    Batches_.push_back(rowSet);
    rowSet = {};
}

bool TWindowSampleProcessor::Sample(TRowSet& sample)
{
    if (SelectedRows_ == 0) {
        return false;
    }
    // Every step-th selected row, from the middle of each step.
    const uint64_t step = (SelectedRows_ + SampleRows_ - 1) / SampleRows_;
    std::vector<int64_t> rows;
    rows.reserve(SelectedRows_ / step + 1);
    uint64_t selected = 0;
    for (size_t batch = 0; batch < Batches_.size(); ++batch) {
        const auto& rowSet = Batches_[batch];
        for (int32_t row = 0; row < rowSet.RowCount; ++row) {
            if (!RowSelected(rowSet, row)) {
                continue;
            }
            if (selected++ % step == step / 2) {
                rows.push_back(MakeRowId(static_cast<int32_t>(batch), row));
            }
        }
    }
    if (rows.empty()) {
        return false;
    }
    Kernel_.Gather(Batches_.data(), rows.data(), 0, static_cast<int64_t>(rows.size()), &sample);
    AdoptKernelOwnedRowSet(sample);
    return true;
}

bool TWindowSampleProcessor::Next(TRowSet& rowSet)
{
    if (Cursor_ == Batches_.size()) {
        return false;
    }
    rowSet = Batches_[Cursor_++];
    return true;
}

TWindowRouteProcessor::TWindowRouteProcessor(
    TKernelCompiler::TWindowRangeDispatch kernel, size_t ranges)
    : Kernel_(std::move(kernel))
    , Ranges_(std::max<size_t>(ranges, 1))
{}

TWindowRouteProcessor::~TWindowRouteProcessor()
{
    for (size_t i = 0; i < SampleBatches_; ++i) {
        Release(&Store_[i]);
    }
}

void TWindowRouteProcessor::AddSample(TRowSet& rowSet)
{
    if (Picked_) {
        throw std::runtime_error("window route: sample after the first routed batch");
    }
    Store_.push_back(rowSet);
    ++SampleBatches_;
    rowSet = {};
}

// Splitter i (of ranges - 1) is sample row floor(i * S / ranges), so every range
// expects about the same share of the sample. Equal splitters leave a range empty.
void TWindowRouteProcessor::PickSplitters()
{
    Picked_ = true;
    uint64_t sampleRows = 0;
    for (size_t batch = 0; batch < SampleBatches_; ++batch) {
        sampleRows += static_cast<uint64_t>(Store_[batch].RowCount);
    }
    if (sampleRows > 0) {
        size_t batch = 0;
        uint64_t batchStart = 0;
        for (size_t i = 1; i < Ranges_; ++i) {
            const uint64_t position = i * sampleRows / Ranges_;
            while (position >= batchStart + static_cast<uint64_t>(Store_[batch].RowCount)) {
                batchStart += static_cast<uint64_t>(Store_[batch].RowCount);
                ++batch;
            }
            Splitters_.push_back(MakeRowId(
                static_cast<int32_t>(batch), static_cast<int32_t>(position - batchStart)));
        }
    }
    Tails_.assign(Splitters_.size(), 0);
    Store_.push_back({});
}

void TWindowRouteProcessor::Route(TRowSet& rowSet, std::vector<std::pair<size_t, TRowSet>>& out)
{
    if (!Picked_) {
        PickSplitters();
    }
    out.clear();
    const auto rows = static_cast<size_t>(std::max<int64_t>(rowSet.RowCount, 0));
    const auto splitters = static_cast<int64_t>(Splitters_.size());
    std::vector<int64_t> ranges(rows);
    std::vector<int64_t> offsets(Splitters_.size() + 2);
    std::vector<int64_t> rowIds(rows);
    Store_.back() = rowSet;
    const int64_t routed = Kernel_.Route(
        Store_.data(), Splitters_.data(), splitters, static_cast<int64_t>(SampleBatches_),
        ranges.data(), offsets.data(), rowIds.data(), Tails_.data());
    for (size_t range = 0; range <= Splitters_.size() && routed > 0; ++range) {
        const int64_t start = offsets[range];
        const int64_t count = offsets[range + 1] - start;
        if (count == 0) {
            continue;
        }
        if (count == rowSet.RowCount) {
            // The whole batch is one range: hand it on without a copy.
            out.emplace_back(range, rowSet);
            rowSet = {};
            break;
        }
        TRowSet piece{};
        Kernel_.Gather(Store_.data(), rowIds.data(), start, count, &piece);
        AdoptKernelOwnedRowSet(piece);
        out.emplace_back(range, piece);
    }
    Store_.back() = {};
    if (rowSet.RefCount) {
        Release(&rowSet);
    }
    rowSet = {};
}

bool TWindowRouteProcessor::Carry(size_t range, TRowSet& carry)
{
    if (!Picked_) {
        PickSplitters();
    }
    if (Carries_.empty()) {
        Carries_.assign(Splitters_.size() + 1, 0);
        Kernel_.Carries(
            Store_.data(), Splitters_.data(), static_cast<int64_t>(Splitters_.size()),
            Tails_.data(), Carries_.data());
    }
    if (range >= Carries_.size() || Carries_[range] == 0) {
        return false;
    }
    auto* data = new TCarryRowSetData{.Value = Carries_[range]};
    data->Column = TColumn{.Data = reinterpret_cast<char*>(&data->Value)};
    carry = TRowSet{
        .Columns = &data->Column,
        .ColumnCount = 1,
        .RowCount = 1,
        .Destroy = DestroyCarryRowSet,
        .Private = data,
        .RefCount = 1,
    };
    return true;
}

} // namespace NQdb
