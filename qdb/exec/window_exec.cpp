#include <qdb/exec/window_exec.h>

#include <qdb/exec/kernel_rowset.h>
#include <qdb/exec/sort_key_ops.h>
#include <qdb/plan/types/nullable.h>

#include <qumir/parser/type.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <stdexcept>

namespace NQdb {

using namespace NQumir::NAst;

TWindowProcessor::TWindowProcessor(
    TTypePtr outputType,
    std::vector<TSortKey> keys,
    std::vector<TSortColumnRef> keyColumns,
    TSortRadixKernel kernel)
    : OutputType_(std::move(outputType))
    , Keys_(std::move(keys))
    , KeyColumns_(std::move(keyColumns))
    , Kernel_(std::move(kernel))
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
        [](const TSortColumnRef& keyColumn) { return IsNullableType(keyColumn.Type); });
    std::vector<TRowId> work(Rows_.size() * RadixWorkStride(KeyColumns_));
    std::vector<uint32_t> counts(nullable ? 257 : 256);
    auto descs = std::make_unique<bool[]>(Keys_.size());
    auto nullsFirsts = std::make_unique<bool[]>(Keys_.size());
    for (size_t k = 0; k < Keys_.size(); ++k) {
        descs[k] = Keys_[k].Direction == ESortDirection::Desc;
        nullsFirsts[k] = EffectiveSortNulls(Keys_[k].Direction, Keys_[k].Nulls) == ESortNulls::First;
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
    rowSet.Destroy = DestroyKernelOwnedRowSet;
    return true;
}

} // namespace NQdb
