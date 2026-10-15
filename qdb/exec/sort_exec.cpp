#include <qdb/exec/sort_exec.h>

#include <qdb/exec/kernel_rowset.h>
#include <qdb/exec/sort_key_ops.h>
#include <qdb/modules/qumirdb_runtime.h>
#include <qdb/plan/types/decimal.h>
#include <qdb/plan/types/nullable.h>

#include <qumir/parser/type.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <queue>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace NQdb {

using namespace NQumir::NAst;

struct TGatheredColumn {
    std::vector<char> Data;
    std::vector<int64_t> Offsets;
    std::vector<uint8_t> Mask;
    TColumn Column{};
};

struct TTopSortScratch {
    TRowSet State{};
    std::vector<TRowId> TempRowIds;
    std::vector<TRowId> Work;
    std::vector<uint32_t> Counts;
    std::vector<uint8_t> PickSrc;
    std::vector<uint32_t> PickIdx;
};

namespace {

constexpr size_t HeapTopKMaxRows = 1024;
constexpr size_t HeapTopKMinReduction = 4;

bool ShouldUseHeapTopK(size_t rows, size_t limit)
{
    // Keep heap repair at ten levels or less, and require the heap to discard
    // at least 75% of the batch. Radix remains the safer path for a large K.
    return limit > 0 && limit <= HeapTopKMaxRows &&
        limit <= rows / HeapTopKMinReduction;
}

struct TSortedRowSetData {
    std::vector<TGatheredColumn> Gathered;
    std::vector<TColumn> Columns;
};

struct TLimitRowSetData {
    TRowSet Input{};
    std::vector<uint8_t> Selection;
};

void DestroySortedRowSet(TRowSet* rowSet) {
    delete static_cast<TSortedRowSetData*>(rowSet->Private);
}

void DestroyLimitRowSet(TRowSet* rowSet) {
    auto* data = static_cast<TLimitRowSetData*>(rowSet->Private);
    Release(&data->Input);
    delete data;
}

void ReleaseKernelState(TRowSet& rowSet) {
    if (rowSet.RefCount > 0) {
        Release(&rowSet);
    }
    rowSet = {};
}

bool IsBitSet(const uint8_t* data, int64_t bit) {
    return ((data[bit / 8] >> (bit % 8)) & 1) != 0;
}

void ClearBit(std::vector<uint8_t>& mask, size_t i) {
    mask[i / 8] &= ~(uint8_t(1) << (i % 8));
}

bool SourceValid(const TColumn& col, int32_t row) {
    if (!col.Mask) {
        return true;
    }
    return IsBitSet(col.Mask, col.MaskBitOffset + row);
}

int64_t OffsetAt(const TColumn& col, int32_t i) {
    if (col.OffsetWidth == 8) {
        return static_cast<const int64_t*>(col.Offsets)[i];
    }
    return static_cast<const int32_t*>(col.Offsets)[i];
}

std::string_view StringAt(const TColumn& col, int32_t row) {
    const int64_t begin = OffsetAt(col, row);
    const int64_t end = OffsetAt(col, row + 1);
    return std::string_view(col.Data + begin, static_cast<size_t>(end - begin));
}

bool BoolAt(const TColumn& col, int32_t row) {
    return IsBitSet(reinterpret_cast<const uint8_t*>(col.Data), col.DataBitOffset + row);
}

template <class T>
int CompareScalar(T lhs, T rhs) {
    if (lhs < rhs) {
        return -1;
    }
    if (rhs < lhs) {
        return 1;
    }
    return 0;
}

template <class T>
T Load(const TColumn& col, int32_t row) {
    T value{};
    std::memcpy(&value, col.Data + static_cast<int64_t>(row) * sizeof(T), sizeof(T));
    return value;
}

int CompareValues(const TColumn& left, int32_t leftRow,
    const TColumn& right, int32_t rightRow,
    const TTypePtr& type)
{
    if (IsDecimalType(type) || IsBinIntStorageType(type)) {
        const auto lhs = Load<qdb_bin_int>(left, leftRow);
        const auto rhs = Load<qdb_bin_int>(right, rightRow);
        if (qdb_decimal_lt(lhs, rhs)) {
            return -1;
        }
        if (qdb_decimal_gt(lhs, rhs)) {
            return 1;
        }
        return 0;
    }

    auto valueType = UnwrapNamedType(UnwrapNullableType(type));
    if (auto integer = TMaybeType<TIntegerType>(valueType)) {
        switch (integer.Cast()->Kind) {
            case TIntegerType::I8:
                return CompareScalar(Load<int8_t>(left, leftRow), Load<int8_t>(right, rightRow));
            case TIntegerType::I16:
                return CompareScalar(Load<int16_t>(left, leftRow), Load<int16_t>(right, rightRow));
            case TIntegerType::I32:
                return CompareScalar(Load<int32_t>(left, leftRow), Load<int32_t>(right, rightRow));
            case TIntegerType::I64:
                return CompareScalar(Load<int64_t>(left, leftRow), Load<int64_t>(right, rightRow));
            case TIntegerType::U8:
                return CompareScalar(Load<uint8_t>(left, leftRow), Load<uint8_t>(right, rightRow));
            case TIntegerType::U16:
                return CompareScalar(Load<uint16_t>(left, leftRow), Load<uint16_t>(right, rightRow));
            case TIntegerType::U32:
                return CompareScalar(Load<uint32_t>(left, leftRow), Load<uint32_t>(right, rightRow));
            case TIntegerType::U64:
                return CompareScalar(Load<uint64_t>(left, leftRow), Load<uint64_t>(right, rightRow));
        }
    }
    if (TMaybeType<TFloatType>(valueType)) {
        return CompareScalar(Load<double>(left, leftRow), Load<double>(right, rightRow));
    }
    if (TMaybeType<TBoolType>(valueType)) {
        return CompareScalar(BoolAt(left, leftRow), BoolAt(right, rightRow));
    }
    if (TMaybeType<TStringType>(valueType)) {
        return StringAt(left, leftRow).compare(StringAt(right, rightRow));
    }
    throw std::runtime_error(
        "sort: unsupported key type " +
        (type ? type->ToString() : std::string("<null>")));
}

bool LessByKey(const TRowStore& store, TRowId leftId, TRowId rightId,
    const TSortKey& key, const TSortColumnRef& keyColumn)
{
    const auto& leftBatch = store.Batch(BatchIndex(leftId));
    const auto& rightBatch = store.Batch(BatchIndex(rightId));
    const auto& leftColumn = leftBatch.Columns[keyColumn.Index];
    const auto& rightColumn = rightBatch.Columns[keyColumn.Index];
    const int32_t leftRow = RowIndex(leftId);
    const int32_t rightRow = RowIndex(rightId);

    const bool leftValid = SourceValid(leftColumn, leftRow);
    const bool rightValid = SourceValid(rightColumn, rightRow);
    if (!leftValid || !rightValid) {
        if (leftValid == rightValid) {
            return false;
        }
        return EffectiveNulls(key) == ESortNulls::First ? !leftValid : leftValid;
    }

    int cmp = CompareValues(leftColumn, leftRow, rightColumn, rightRow, keyColumn.Type);
    if (key.Direction == ESortDirection::Desc) {
        cmp = -cmp;
    }
    return cmp < 0;
}

bool EqualByKey(const TRowStore& store, TRowId leftId, TRowId rightId,
    const TSortColumnRef& keyColumn)
{
    const auto& leftBatch = store.Batch(BatchIndex(leftId));
    const auto& rightBatch = store.Batch(BatchIndex(rightId));
    const auto& leftColumn = leftBatch.Columns[keyColumn.Index];
    const auto& rightColumn = rightBatch.Columns[keyColumn.Index];
    const int32_t leftRow = RowIndex(leftId);
    const int32_t rightRow = RowIndex(rightId);

    const bool leftValid = SourceValid(leftColumn, leftRow);
    const bool rightValid = SourceValid(rightColumn, rightRow);
    if (leftValid != rightValid) {
        return false;
    }
    if (!leftValid) {
        return true;
    }
    return CompareValues(leftColumn, leftRow, rightColumn, rightRow, keyColumn.Type) == 0;
}

bool SortRowsLess(const TRowStore& store, const std::vector<TSortKey>& keys,
    const std::vector<TSortColumnRef>& keyColumns, TRowId leftId, TRowId rightId)
{
    for (size_t i = 0; i < keys.size(); ++i) {
        if (LessByKey(store, leftId, rightId, keys[i], keyColumns[i])) {
            return true;
        }
        if (!EqualByKey(store, leftId, rightId, keyColumns[i])) {
            return false;
        }
    }
    return false;
}

size_t SortColumnFixedWidth(const TTypePtr& type) {
    if (IsDecimalType(type) || IsBinIntStorageType(type)) {
        return sizeof(qdb_bin_int);
    }

    auto valueType = UnwrapNamedType(UnwrapNullableType(type));
    if (auto integer = TMaybeType<TIntegerType>(valueType)) {
        return static_cast<size_t>(integer.Cast()->BitWidth() / 8);
    }
    if (TMaybeType<TFloatType>(valueType)) {
        return 8;
    }
    if (TMaybeType<TBoolType>(valueType)) {
        return 0;
    }
    if (TMaybeType<TStringType>(valueType)) {
        return 0;
    }
    throw std::runtime_error(
        "sort: unsupported output column type " +
        (type ? type->ToString() : std::string("<null>")));
}

void GatherColumn(const TRowStore& store, const std::vector<TRowId>& rowIds,
    int32_t srcColIdx, const TTypePtr& type, TGatheredColumn& out)
{
    const size_t n = rowIds.size();
    const auto valueType = UnwrapNamedType(UnwrapNullableType(type));
    const bool isBool = static_cast<bool>(TMaybeType<TBoolType>(valueType));
    const bool isString = static_cast<bool>(TMaybeType<TStringType>(valueType));
    const size_t width = SortColumnFixedWidth(type);

    out.Data.clear();
    out.Offsets.clear();
    out.Mask.assign((n + 7) / 8, 0xff);
    bool anyNull = false;

    auto markNull = [&](size_t j) {
        ClearBit(out.Mask, j);
        anyNull = true;
    };

    if (isString) {
        out.Offsets.resize(n + 1);
        out.Offsets[0] = 0;
        for (size_t j = 0; j < n; ++j) {
            const TRowId id = rowIds[j];
            const TColumn& col = store.Column(id, srcColIdx);
            const int32_t row = RowIndex(id);
            int64_t len = 0;
            if (!SourceValid(col, row)) {
                markNull(j);
            } else {
                len = OffsetAt(col, row + 1) - OffsetAt(col, row);
            }
            out.Offsets[j + 1] = out.Offsets[j] + len;
        }
        out.Data.resize(static_cast<size_t>(out.Offsets[n]));
        for (size_t j = 0; j < n; ++j) {
            const TRowId id = rowIds[j];
            const TColumn& col = store.Column(id, srcColIdx);
            const int32_t row = RowIndex(id);
            if (!SourceValid(col, row)) {
                continue;
            }
            const int64_t begin = OffsetAt(col, row);
            const int64_t len = OffsetAt(col, row + 1) - begin;
            if (len > 0) {
                std::memcpy(out.Data.data() + out.Offsets[j], col.Data + begin, len);
            }
        }
        out.Column = TColumn{
            .Data = out.Data.data(),
            .Mask = anyNull ? out.Mask.data() : nullptr,
            .Offsets = out.Offsets.data(),
            .OffsetWidth = 8,
        };
    } else if (isBool) {
        out.Data.assign((n + 7) / 8, 0);
        for (size_t j = 0; j < n; ++j) {
            const TRowId id = rowIds[j];
            const TColumn& col = store.Column(id, srcColIdx);
            const int32_t row = RowIndex(id);
            if (!SourceValid(col, row)) {
                markNull(j);
                continue;
            }
            if (BoolAt(col, row)) {
                out.Data[j / 8] |= char(uint8_t(1) << (j % 8));
            }
        }
        out.Column = TColumn{
            .Data = out.Data.data(),
            .DataBitOffset = 0,
            .Mask = anyNull ? out.Mask.data() : nullptr,
        };
    } else {
        out.Data.assign(n * width, 0);
        for (size_t j = 0; j < n; ++j) {
            const TRowId id = rowIds[j];
            const TColumn& col = store.Column(id, srcColIdx);
            const int32_t row = RowIndex(id);
            if (!SourceValid(col, row)) {
                markNull(j);
                continue;
            }
            std::memcpy(out.Data.data() + j * width, col.Data + static_cast<int64_t>(row) * width, width);
        }
        out.Column = TColumn{
            .Data = out.Data.data(),
            .Mask = anyNull ? out.Mask.data() : nullptr,
        };
    }

    if (!anyNull) {
        out.Mask.clear();
    }
}

} // namespace

TSortProcessor::TSortProcessor(
    TTypePtr outputType,
    std::vector<TSortKey> keys,
    std::vector<TSortColumnRef> keyColumns,
    TSortRadixKernel radixKernel,
    int64_t batchRows)
    : OutputType_(std::move(outputType))
    , Keys_(std::move(keys))
    , KeyColumns_(std::move(keyColumns))
    , RadixKernel_(std::move(radixKernel))
    , BatchRows_(batchRows)
{}

bool TSortProcessor::TryRadixSort()
{
    if (Rows_.empty()) {
        return true;
    }
    if (Rows_.size() > std::numeric_limits<uint32_t>::max()) {
        return false;
    }
    const bool useNullable = static_cast<bool>(RadixKernel_.NullableDispatch);
    auto dispatch = useNullable ? RadixKernel_.NullableDispatch : RadixKernel_.Dispatch;
    if (!RadixKernel_.Enabled || !dispatch) {
        return false;
    }
    if (KeyColumns_.size() != Keys_.size()) {
        return false;
    }

    std::vector<TRowId> work(Rows_.size() * RadixWorkStride(KeyColumns_));
    std::vector<uint32_t> counts(useNullable ? 257 : 256);
    auto descs = std::make_unique<bool[]>(Keys_.size());
    auto nullsFirsts = std::make_unique<bool[]>(Keys_.size());
    for (size_t k = 0; k < Keys_.size(); ++k) {
        descs[k] = Keys_[k].Direction == ESortDirection::Desc;
        nullsFirsts[k] = EffectiveNulls(Keys_[k]) == ESortNulls::First;
    }

    auto* store = const_cast<TRowSet*>(Store_.Data());
    TRowSet unused{};
    dispatch(store, Rows_.data(), work.data(), counts.data(),
        static_cast<int64_t>(Rows_.size()), descs.get(), nullsFirsts.get(),
        true, 0, 0, &unused);
    return true;
}

void TSortProcessor::Add(TRowSet& rowSet)
{
    if (Materialized_) {
        throw std::runtime_error("sort processor is already finished");
    }

    const int32_t batchIdx = Store_.PushBatch(rowSet);
    for (int32_t row = 0; row < rowSet.RowCount; ++row) {
        if (RowSelected(rowSet, row)) {
            Rows_.push_back(MakeRowId(batchIdx, row));
        }
    }
    rowSet = {};
}

void TSortProcessor::Finish()
{
    if (Materialized_) {
        return;
    }
    if (!TryRadixSort()) {
        throw std::runtime_error("sort kernel is unavailable");
    }

    Materialized_ = true;
}

bool TSortProcessor::Next(TRowSet& rowSet)
{
    Finish();
    if (Cursor_ >= Rows_.size()) {
        return false;
    }

    const size_t n = std::min<size_t>(
        static_cast<size_t>(BatchRows_), Rows_.size() - Cursor_);

    {
        auto dispatch = RadixKernel_.NullableDispatch
            ? RadixKernel_.NullableDispatch
            : RadixKernel_.Dispatch;
        if (RadixKernel_.Enabled && dispatch) {
            const int64_t out = dispatch(
                const_cast<TRowSet*>(Store_.Data()),
                Rows_.data(),
                nullptr,
                nullptr,
                static_cast<int64_t>(Rows_.size()),
                nullptr,
                nullptr,
                false,
                static_cast<int64_t>(Cursor_),
                static_cast<int64_t>(n),
                &rowSet);
            if (out <= 0) {
                return false;
            }
            rowSet.Destroy = DestroyKernelOwnedRowSet;
            Cursor_ += static_cast<size_t>(out);
            return true;
        }
    }

    throw std::runtime_error("sort materialize kernel is unavailable");
}

TMergeProcessor::TMergeProcessor(
    TTypePtr outputType,
    std::vector<TSortKey> keys,
    std::vector<TSortColumnRef> keyColumns,
    size_t runCount,
    int64_t batchRows)
    : OutputType_(std::move(outputType))
    , Keys_(std::move(keys))
    , KeyColumns_(std::move(keyColumns))
    , BatchRows_(batchRows)
    , Runs_(runCount)
{}

void TMergeProcessor::Add(TRowSet& rowSet, size_t run)
{
    if (Materialized_) {
        throw std::runtime_error("merge processor is already finished");
    }
    const int32_t batchIdx = Store_.PushBatch(rowSet);
    const TRowSet& batch = Store_.Batch(batchIdx);
    for (int32_t row = 0; row < batch.RowCount; ++row) {
        if (RowSelected(batch, row)) {
            Runs_[run].push_back(MakeRowId(batchIdx, row));
        }
    }
    rowSet = {};
}

void TMergeProcessor::Finish()
{
    if (Materialized_) {
        return;
    }

    // TODO(browser-path): this k-way merge runs the sort comparator
    // (SortRowsLess) and the output gather (GatherColumn) in C++. Like the window
    // operator, it should move to a generated oz kernel - any C++ compute on the
    // data path complicates the JIT/browser (WASM) execution path, which runs the
    // oz kernels. Remove this C++ fallback once a merge kernel exists.
    size_t total = 0;
    for (const auto& run : Runs_) {
        total += run.size();
    }
    Merged_.reserve(total);

    // Min-heap over the current head of each run. std::priority_queue is a
    // max-heap, so the comparator returns true when `a` should sit below `b`,
    // i.e. when a's key is greater than b's (b < a).
    using THead = std::pair<TRowId, size_t>;
    auto worse = [&](const THead& a, const THead& b) {
        return SortRowsLess(Store_, Keys_, KeyColumns_, b.first, a.first);
    };
    std::priority_queue<THead, std::vector<THead>, decltype(worse)> heap(worse);

    std::vector<size_t> pos(Runs_.size(), 0);
    for (size_t i = 0; i < Runs_.size(); ++i) {
        if (!Runs_[i].empty()) {
            heap.push({Runs_[i][0], i});
            pos[i] = 1;
        }
    }
    while (!heap.empty()) {
        const THead top = heap.top();
        heap.pop();
        Merged_.push_back(top.first);
        const size_t run = top.second;
        if (pos[run] < Runs_[run].size()) {
            heap.push({Runs_[run][pos[run]], run});
            ++pos[run];
        }
    }

    Materialized_ = true;
}

bool TMergeProcessor::Next(TRowSet& rowSet)
{
    Finish();
    if (Cursor_ >= Merged_.size()) {
        return false;
    }

    const size_t n = std::min<size_t>(
        static_cast<size_t>(BatchRows_), Merged_.size() - Cursor_);
    const std::vector<TRowId> slice(
        Merged_.begin() + Cursor_, Merged_.begin() + Cursor_ + n);

    auto* outputType = static_cast<TStructType*>(OutputType_.get());
    if (!outputType) {
        throw std::runtime_error("merge output must have TStructType");
    }
    auto* data = new TSortedRowSetData;
    data->Gathered.resize(outputType->Fields.size());
    data->Columns.resize(outputType->Fields.size());
    for (size_t c = 0; c < outputType->Fields.size(); ++c) {
        GatherColumn(Store_, slice, static_cast<int32_t>(c),
            outputType->Fields[c].second, data->Gathered[c]);
        data->Columns[c] = data->Gathered[c].Column;
    }

    rowSet = TRowSet{
        .Columns = data->Columns.data(),
        .ColumnCount = static_cast<int64_t>(data->Columns.size()),
        .RowCount = static_cast<int64_t>(n),
        .Selection = nullptr,
        .Destroy = DestroySortedRowSet,
        .Private = data,
        .RefCount = 1,
    };
    Cursor_ += n;
    return true;
}

TTopSortProcessor::TTopSortProcessor(
    TTypePtr outputType,
    std::vector<TSortKey> keys,
    std::vector<TSortColumnRef> keyColumns,
    TSortRadixKernel radixKernel,
    int64_t limit,
    int64_t batchRows)
    : OutputType_(std::move(outputType))
    , Keys_(std::move(keys))
    , KeyColumns_(std::move(keyColumns))
    , RadixKernel_(std::move(radixKernel))
    , Limit_(limit)
    , BatchRows_(batchRows)
    , Scratch_(std::make_unique<TTopSortScratch>())
{}

TTopSortProcessor::~TTopSortProcessor()
{
    if (Scratch_) {
        ReleaseKernelState(Scratch_->State);
    }
}

void TTopSortProcessor::SetDiagnostics(TOperatorDiagnostics* diagnostics) {
    Diagnostics_ = diagnostics;
}

void TTopSortProcessor::Add(TRowSet& batch)
{
    if (Materialized_) {
        throw std::runtime_error("top-sort processor is already finished");
    }
    if (Limit_ <= 0) {
        Release(&batch);
        batch = {};
        return;
    }
    if (!RadixKernel_.Enabled || !RadixKernel_.TopSortDispatch) {
        Release(&batch);
        batch = {};
        throw std::runtime_error("top-sort kernel is unavailable");
    }
    if (!static_cast<TStructType*>(OutputType_.get())) {
        throw std::runtime_error("top-sort output must have TStructType");
    }

    auto& scratch = *Scratch_;
    scratch.TempRowIds.clear();
    scratch.TempRowIds.reserve(static_cast<size_t>(batch.RowCount));
    for (int32_t row = 0; row < batch.RowCount; ++row) {
        if (RowSelected(batch, row)) {
            scratch.TempRowIds.push_back(MakeRowId(0, row));
        }
    }
    size_t selectedRows = scratch.TempRowIds.size();
    if (selectedRows == 0) {
        Release(&batch);
        batch = {};
        return;
    }

    bool sortInput = true;
    if (RadixKernel_.HeapTopKDispatch &&
        ShouldUseHeapTopK(selectedRows, static_cast<size_t>(Limit_))) {
        const int64_t kept = RadixKernel_.HeapTopKDispatch(
            &batch,
            scratch.TempRowIds.data(),
            static_cast<int64_t>(selectedRows),
            Limit_);
        if (kept <= 0 || static_cast<size_t>(kept) > selectedRows) {
            Release(&batch);
            batch = {};
            throw std::runtime_error("heap top-K kernel failed");
        }
        selectedRows = static_cast<size_t>(kept);
        sortInput = false;
    }

    const bool useNullable = static_cast<bool>(RadixKernel_.NullableDispatch);
    const size_t stateRows = static_cast<size_t>(scratch.State.RowCount);
    const size_t limit = static_cast<size_t>(Limit_);
    const size_t pickCapacity = std::min(limit, stateRows + selectedRows);
    scratch.TempRowIds.resize(std::max(selectedRows, pickCapacity));
    if (sortInput) {
        scratch.Work.resize(selectedRows * RadixWorkStride(KeyColumns_));
        scratch.Counts.assign(useNullable ? 257 : 256, 0);
    } else {
        scratch.Work.clear();
        scratch.Counts.clear();
    }
    scratch.PickSrc.resize(pickCapacity);
    scratch.PickIdx.resize(pickCapacity);

    TRowSet next{};
    const int64_t produced = RadixKernel_.TopSortDispatch(
        &scratch.State,
        &batch,
        scratch.TempRowIds.data(),
        scratch.Work.data(),
        scratch.Counts.data(),
        static_cast<int64_t>(selectedRows),
        sortInput,
        scratch.PickSrc.data(),
        scratch.PickIdx.data(),
        Limit_,
        &next);
    if (produced < 0) {
        Release(&batch);
        batch = {};
        throw std::runtime_error("top-sort kernel update failed");
    }
    if (produced > 0) {
        next.Destroy = DestroyKernelOwnedRowSet;
        ReleaseKernelState(scratch.State);
        scratch.State = next;
        if (Diagnostics_) {
            Diagnostics_->RetainInput(scratch.State, 0, false);
        }
    }
    Release(&batch);
    batch = {};
}

void TTopSortProcessor::Finish()
{
    Materialized_ = true;
}

bool TTopSortProcessor::Next(TRowSet& rowSet)
{
    Finish();
    if (!Scratch_ || Cursor_ >= static_cast<size_t>(Scratch_->State.RowCount)) {
        return false;
    }

    const size_t n = std::min<size_t>(
        static_cast<size_t>(BatchRows_),
        static_cast<size_t>(Scratch_->State.RowCount) - Cursor_);
    Scratch_->TempRowIds.resize(n);
    for (size_t i = 0; i < n; ++i) {
        Scratch_->TempRowIds[i] = MakeRowId(0, static_cast<int32_t>(Cursor_ + i));
    }

    auto dispatch = RadixKernel_.NullableDispatch
        ? RadixKernel_.NullableDispatch
        : RadixKernel_.Dispatch;
    if (!RadixKernel_.Enabled || !dispatch) {
        throw std::runtime_error("top-sort materialize kernel is unavailable");
    }
    const int64_t produced = dispatch(
        &Scratch_->State,
        Scratch_->TempRowIds.data(),
        nullptr,
        nullptr,
        static_cast<int64_t>(n),
        nullptr,
        nullptr,
        false,
        0,
        static_cast<int64_t>(n),
        &rowSet);
    if (produced <= 0) {
        return false;
    }
    rowSet.Destroy = DestroyKernelOwnedRowSet;
    Cursor_ += static_cast<size_t>(produced);
    return true;
}

TLimitProcessor::TLimitProcessor(int64_t limit, int64_t offset)
    : Limit_(limit)
    , Offset_(offset)
{}

bool TLimitProcessor::Finished() const
{
    if (Limit_ <= 0 || Emitted_ >= Limit_) {
        return true;
    }
    return false;
}

bool TLimitProcessor::Process(TRowSet& input, TRowSet& rowSet)
{
    if (Finished()) {
        Release(&input);
        return false;
    }

    auto* data = new TLimitRowSetData;
    data->Input = input;
    data->Selection.assign(static_cast<size_t>(input.RowCount), uint8_t{0});

    bool any = false;
    for (int32_t row = 0; row < input.RowCount && Emitted_ < Limit_; ++row) {
        if (!RowSelected(input, row)) {
            continue;
        }
        if (Skipped_ < Offset_) {
            ++Skipped_;
            continue;
        }
        data->Selection[row] = 1;
        ++Emitted_;
        any = true;
    }

    if (!any) {
        Release(&data->Input);
        delete data;
        return false;
    }

    rowSet = TRowSet{
        .Columns = input.Columns,
        .ColumnCount = input.ColumnCount,
        .RowCount = input.RowCount,
        .Selection = data->Selection.data(),
        .Destroy = DestroyLimitRowSet,
        .Private = data,
        .RefCount = 1,
    };
    return true;
}

} // namespace NQdb
