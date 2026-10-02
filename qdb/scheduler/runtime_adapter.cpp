#include <qdb/scheduler/runtime_adapter.h>

#include <qdb/scheduler/graph.h>
#include <qdb/plan/types/decimal.h>
#include <qdb/plan/types/nullable.h>

#include <qumir/error.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace NQdb {
namespace NScheduler {
namespace {

using namespace NQumir::NAst;

// Flush a blocking task's buffered output rowset. Returns the result to return
// immediately (OK once pushed, BLOCKED_OUTPUT if the queue is full), or nullopt
// when nothing is pending so the caller proceeds to produce the next output.
std::optional<ETaskResult> FlushPendingOutput(
    bool& hasOutput,
    TOutputPort& output,
    TRowSet& current)
{
    if (!hasOutput) {
        return std::nullopt;
    }
    if (!output.CanPush()) {
        return ETaskResult::BLOCKED_OUTPUT;
    }
    if (!output.Push(std::move(current))) {
        return ETaskResult::BLOCKED_OUTPUT;
    }
    current = {};
    hasOutput = false;
    return ETaskResult::OK;
}

struct THashShuffleRowSetData {
    std::shared_ptr<TRowSet> Input;
    std::vector<uint8_t> Selection;
    std::shared_ptr<std::vector<uint64_t>> Hashes; // nullptr if not computed for this batch
};

struct TShuffleColumnData {
    std::vector<char> Data;
    std::vector<int64_t> Offsets;
    std::vector<uint8_t> Mask;
    TColumn Column{};
};

struct TMaterializedShuffleRowSetData {
    std::vector<TShuffleColumnData> Gathered;
    std::vector<TColumn> Columns;
};

void DestroyHashShuffleRowSet(TRowSet* rowSet) {
    auto* data = static_cast<THashShuffleRowSetData*>(rowSet->Private);
    delete data;
}

void DestroyMaterializedShuffleRowSet(TRowSet* rowSet) {
    delete static_cast<TMaterializedShuffleRowSetData*>(rowSet->Private);
}

void DestroySharedRowSet(TRowSet* rowSet) {
    Release(rowSet);
    delete rowSet;
}

bool RowSelected(const TRowSet& rowSet, int64_t row) {
    return !rowSet.Selection || rowSet.Selection[row] != 0;
}

bool SourceValid(const TColumn& col, int32_t row) {
    if (!col.Mask) {
        return true;
    }
    const int32_t bit = col.MaskBitOffset + row;
    return ((col.Mask[bit / 8] >> (bit % 8)) & 1) != 0;
}

int64_t OffsetAt(const TColumn& col, int32_t i) {
    if (col.OffsetWidth == 8) {
        return static_cast<const int64_t*>(col.Offsets)[i];
    }
    return static_cast<const int32_t*>(col.Offsets)[i];
}

bool BoolAt(const TColumn& col, int32_t row) {
    const auto* bits = reinterpret_cast<const uint8_t*>(col.Data);
    const int32_t bit = col.DataBitOffset + row;
    return ((bits[bit / 8] >> (bit % 8)) & 1) != 0;
}

void ClearBit(std::vector<uint8_t>& mask, size_t i) {
    mask[i / 8] &= ~(uint8_t(1) << (i % 8));
}

TTypePtr ShuffleValueType(const TTypePtr& type) {
    return UnwrapNamedType(UnwrapNullableType(type));
}

size_t ShuffleColumnFixedWidth(const TTypePtr& type) {
    if (IsDecimalType(type)) {
        return 16;
    }
    auto valueType = ShuffleValueType(type);
    if (auto integer = TMaybeType<TIntegerType>(valueType)) {
        return static_cast<size_t>(integer.Cast()->BitWidth() / 8);
    }
    if (TMaybeType<TFloatType>(valueType)) {
        return 8;
    }
    if (TMaybeType<TBoolType>(valueType) || TMaybeType<TStringType>(valueType)) {
        return 0;
    }
    throw NQumir::TError(
        "hash shuffle cannot materialize column of type " +
        (type ? type->ToString() : std::string("<null>")));
}

size_t EstimateShuffleRowBytes(const TStructType& inputType) {
    size_t total = 0;
    for (const auto& field : inputType.Fields) {
        const auto valueType = ShuffleValueType(field.second);
        if (TMaybeType<TStringType>(valueType)) {
            total += sizeof(int64_t) + 32;
        } else if (TMaybeType<TBoolType>(valueType)) {
            total += 1;
        } else {
            total += ShuffleColumnFixedWidth(field.second);
        }
    }
    return std::max<size_t>(total, 1);
}

const TStructType& ShuffleInputStruct(const TTypePtr& type) {
    auto structure = TMaybeType<TStructType>(UnwrapNamedType(type));
    if (!structure) {
        throw std::runtime_error("hash shuffle input must have TStructType");
    }
    return *structure.Cast();
}

} // namespace

EFetchResult TInputPort::Fetch(TRowSet& rowSet) const {
    return Connection
        ? Connection->Fetch(Lane, rowSet)
        : EFetchResult::FINISHED;
}

bool TOutputPort::CanPush() const {
    return !Connection || Connection->CanPush(Lane);
}

bool TOutputPort::Push(TRowSet&& rowSet) const {
    return Connection
        ? Connection->Push(Lane, std::move(rowSet))
        : true;
}

void TOutputPort::Finish() const {
    if (Connection) {
        Connection->Finish(Lane);
    }
}

TSourceCode::TSourceCode(TNext next)
    : Next(std::move(next))
{}

TUnaryCode::TUnaryCode(TProcess process)
    : Process(std::move(process))
{}

TSinkCode::TSinkCode(TWrite write)
    : Write(std::move(write))
{}

TBlockingCode::TBlockingCode(TProcess process)
    : Process(std::move(process))
{}

TBinaryBlockingCode::TBinaryBlockingCode(TProcess process)
    : Process(std::move(process))
{}

TMergeCode::TMergeCode(TProcess process)
    : Process(std::move(process))
{}

THashShuffleCode::THashShuffleCode(THash hash)
    : Hash(std::move(hash))
{}

THashShuffleCode::THashShuffleCode(THash hash,
    NQumir::NAst::TTypePtr inputType,
    size_t targetOutputBatchRows,
    size_t maxOutputBatchRows,
    size_t targetOutputBatchBytes)
    : Hash(std::move(hash))
    , InputType(std::move(inputType))
    , TargetOutputBatchRows(targetOutputBatchRows)
    , MaxOutputBatchRows(maxOutputBatchRows)
    , TargetOutputBatchBytes(targetOutputBatchBytes)
{}

TSourceTask::TSourceTask(
    std::shared_ptr<const TSourceCode> code,
    std::shared_ptr<void> state,
    TOutputPort output)
    : Code_(std::move(code))
    , State_(std::move(state))
    , Output_(output)
{}

ETaskResult TSourceTask::Execute() {
    if (Finished_) {
        return ETaskResult::FINISHED;
    }
    if (!Output_.CanPush()) {
        return ETaskResult::BLOCKED_OUTPUT;
    }

    TRowSet rowSet{};
    if (!Code_->Next(State_.get(), rowSet)) {
        Finished_ = true;
        Output_.Finish();
        return ETaskResult::FINISHED;
    }

    if (!Output_.Push(std::move(rowSet))) {
        Release(&rowSet);
        return ETaskResult::BLOCKED_OUTPUT;
    }
    return ETaskResult::OK;
}

TUnaryTask::TUnaryTask(
    std::shared_ptr<const TUnaryCode> code,
    std::shared_ptr<void> state,
    TInputPort input,
    TOutputPort output)
    : Code_(std::move(code))
    , State_(std::move(state))
    , Input_(input)
    , Output_(output)
{}

TUnaryTask::~TUnaryTask() {
    if (HasInput_) {
        Release(&CurrentInput_);
    }
}

ETaskResult TUnaryTask::Execute() {
    if (OutputFinished_) {
        return ETaskResult::FINISHED;
    }

    if (!HasInput_ && !InputFinished_) {
        auto fetch = Input_.Fetch(CurrentInput_);
        if (fetch == EFetchResult::OK) {
            HasInput_ = true;
        } else if (fetch == EFetchResult::FINISHED) {
            InputFinished_ = true;
        } else {
            return ETaskResult::NEED_DATA;
        }
    }

    if (!HasInput_) {
        OutputFinished_ = true;
        Output_.Finish();
        return ETaskResult::FINISHED;
    }

    if (!Output_.CanPush()) {
        return ETaskResult::BLOCKED_OUTPUT;
    }

    Code_->Process(State_.get(), CurrentInput_);
    if (!Output_.Push(std::move(CurrentInput_))) {
        return ETaskResult::BLOCKED_OUTPUT;
    }
    CurrentInput_ = {};
    HasInput_ = false;
    return ETaskResult::OK;
}

const std::shared_ptr<const TUnaryCode>& TUnaryTask::Code() const {
    return Code_;
}

const std::shared_ptr<void>& TUnaryTask::State() const {
    return State_;
}

TSinkTask::TSinkTask(
    std::shared_ptr<const TSinkCode> code,
    std::shared_ptr<void> state,
    TInputPort input)
    : Code_(std::move(code))
    , State_(std::move(state))
    , Input_(input)
{}

ETaskResult TSinkTask::Execute() {
    if (Finished_) {
        return ETaskResult::FINISHED;
    }

    TRowSet rowSet{};
    auto fetch = Input_.Fetch(rowSet);
    if (fetch == EFetchResult::NO_DATA) {
        return ETaskResult::NEED_DATA;
    }
    if (fetch == EFetchResult::FINISHED) {
        Finished_ = true;
        return ETaskResult::FINISHED;
    }

    Code_->Write(State_.get(), rowSet);
    Release(&rowSet);
    return ETaskResult::OK;
}

TBlockingTask::TBlockingTask(
    std::shared_ptr<const TBlockingCode> code,
    std::shared_ptr<void> state,
    TInputPort input,
    TOutputPort output)
    : Code_(std::move(code))
    , State_(std::move(state))
    , Input_(input)
    , Output_(output)
{}

TBlockingTask::~TBlockingTask() {
    if (HasOutput_) {
        Release(&CurrentOutput_);
    }
}

ETaskResult TBlockingTask::Execute() {
    if (OutputFinished_) {
        return ETaskResult::FINISHED;
    }

    if (auto result = FlushPendingOutput(HasOutput_, Output_, CurrentOutput_)) {
        return *result;
    }

    auto result = Code_->Process(State_.get(), Input_, CurrentOutput_);
    if (result == ETaskResult::OK) {
        HasOutput_ = true;
        return ETaskResult::OK;
    }
    if (result == ETaskResult::FINISHED) {
        OutputFinished_ = true;
        Output_.Finish();
    }
    return result;
}

TBinaryBlockingTask::TBinaryBlockingTask(
    std::shared_ptr<const TBinaryBlockingCode> code,
    std::shared_ptr<void> state,
    TInputPort left,
    TInputPort right,
    TOutputPort output)
    : Code_(std::move(code))
    , State_(std::move(state))
    , Left_(left)
    , Right_(right)
    , Output_(output)
{}

TBinaryBlockingTask::~TBinaryBlockingTask() {
    if (HasOutput_) {
        Release(&CurrentOutput_);
    }
}

ETaskResult TBinaryBlockingTask::Execute() {
    if (OutputFinished_) {
        return ETaskResult::FINISHED;
    }

    if (auto result = FlushPendingOutput(HasOutput_, Output_, CurrentOutput_)) {
        return *result;
    }

    auto result = Code_->Process(State_.get(), Left_, Right_, CurrentOutput_);
    if (result == ETaskResult::OK) {
        HasOutput_ = true;
        return ETaskResult::OK;
    }
    if (result == ETaskResult::FINISHED) {
        OutputFinished_ = true;
        Output_.Finish();
    }
    return result;
}

bool TBinaryBlockingTask::WantsInput(const TTaskEdge& edge) const {
    if (!Code_->RequiredSide) {
        return true;
    }
    const auto side = Code_->RequiredSide(State_.get());
    if (side == ERequiredSide::Both) {
        return true;
    }
    const TInputPort& wanted = side == ERequiredSide::Left ? Left_ : Right_;
    return edge.Connection == wanted.Connection && edge.DstLane == wanted.Lane;
}

TMergeTask::TMergeTask(
    std::shared_ptr<const TMergeCode> code,
    std::shared_ptr<void> state,
    std::vector<TInputPort> inputs,
    TOutputPort output)
    : Code_(std::move(code))
    , State_(std::move(state))
    , Inputs_(std::move(inputs))
    , Output_(output)
{}

TMergeTask::~TMergeTask() {
    if (HasOutput_) {
        Release(&CurrentOutput_);
    }
}

ETaskResult TMergeTask::Execute() {
    if (OutputFinished_) {
        return ETaskResult::FINISHED;
    }

    if (auto result = FlushPendingOutput(HasOutput_, Output_, CurrentOutput_)) {
        return *result;
    }

    auto result = Code_->Process(State_.get(), Inputs_, CurrentOutput_);
    if (result == ETaskResult::OK) {
        HasOutput_ = true;
        return ETaskResult::OK;
    }
    if (result == ETaskResult::FINISHED) {
        OutputFinished_ = true;
        Output_.Finish();
    }
    return result;
}

struct THashShuffleTask::TPendingOutput {
    size_t DstLane = 0;
    TRowSet RowSet = {};
};

struct THashShuffleTask::TBufferedBatch {
    std::shared_ptr<TRowSet> Input;
    std::vector<int32_t> Rows;
    std::shared_ptr<std::vector<uint64_t>> Hashes; // nullptr if not computed for this batch
};

struct THashShuffleTask::TDestinationBuffer {
    std::vector<TBufferedBatch> Batches;
    size_t RowCount = 0;
    size_t ByteCount = 0;
};

THashShuffleTask::THashShuffleTask(
    std::shared_ptr<const THashShuffleCode> code,
    std::shared_ptr<void> state,
    TInputPort input,
    THashShuffleConnection& output,
    size_t sourceLane)
    : Code_(std::move(code))
    , State_(std::move(state))
    , Input_(input)
    , Output_(&output)
    , SourceLane_(sourceLane)
{}

THashShuffleTask::~THashShuffleTask() {
    ClearPending();
}

ETaskResult THashShuffleTask::Execute() {
    if (OutputFinished_) {
        return ETaskResult::FINISHED;
    }

    if (PendingIndex_ < Pending_.size()) {
        return PushPending();
    }

    TRowSet rowSet{};
    auto fetch = Input_.Fetch(rowSet);
    if (fetch == EFetchResult::NO_DATA) {
        return ETaskResult::NEED_DATA;
    }
    if (fetch == EFetchResult::FINISHED) {
        FlushAllBuffers();
        auto result = PushPending();
        if (result != ETaskResult::OK) {
            return result;
        }
        if (Code_->ProducedFilter) {
            // This lane is done contributing; the last one to say so publishes.
            auto builder = FilterBuilder_
                ? std::move(*FilterBuilder_)
                : Code_->ProducedFilter->MakeBuilder();
            Code_->ProducedFilter->FinishProducer(
                std::move(builder).TakePartial());
            FilterBuilder_.reset();
        }
        OutputFinished_ = true;
        Output_->Finish(SourceLane_);
        return ETaskResult::FINISHED;
    }

    Scatter(rowSet);
    if (rowSet.RefCount) {
        Release(&rowSet);
    }
    return PushPending();
}

bool THashShuffleTask::BatchingEnabled() const {
    return Code_->InputType && Code_->TargetOutputBatchRows > 0;
}

// Routing needs hashes only when there are several partitions, but a filter
// needs them whatever the fan-out, so the single-partition short cut cannot be
// taken while one is attached.
bool THashShuffleTask::NeedsHashes(size_t partitions) const {
    return partitions > 1 || Code_->ProducedFilter || Code_->AppliedFilter;
}

void THashShuffleTask::AccumulateFilter(const TRowSet& rowSet) {
    if (!Code_->ProducedFilter) {
        return;
    }
    if (!FilterBuilder_) {
        FilterBuilder_.emplace(Code_->ProducedFilter->MakeBuilder());
    }
    for (int64_t row = 0; row < rowSet.RowCount; ++row) {
        if (RowSelected(rowSet, row)) {
            FilterBuilder_->Add(Hashes_[static_cast<size_t>(row)]);
        }
    }
}

bool THashShuffleTask::FilterKeeps(size_t row) const {
    return !Code_->AppliedFilter || Code_->AppliedFilter->MayContain(Hashes_[row]);
}

void THashShuffleTask::EnsureBuffers(size_t partitions) {
    if (Buffers_.size() == partitions) {
        return;
    }
    Buffers_.clear();
    Buffers_.reserve(partitions);
    for (size_t i = 0; i < partitions; ++i) {
        Buffers_.push_back(std::make_unique<TDestinationBuffer>());
    }
}

void THashShuffleTask::Scatter(TRowSet& rowSet) {
    if (BatchingEnabled()) {
        ScatterBuffered(rowSet);
    } else {
        ScatterViews(rowSet);
    }
}

void THashShuffleTask::ScatterBuffered(TRowSet& rowSet) {
    const size_t partitions = Output_->DstCount();
    if (partitions == 0 || rowSet.RowCount <= 0) {
        return;
    }
    EnsureBuffers(partitions);

    const auto& inputType = ShuffleInputStruct(Code_->InputType);
    if (rowSet.ColumnCount < static_cast<int64_t>(inputType.Fields.size())) {
        throw std::runtime_error("hash shuffle rowset has fewer columns than schema");
    }

    auto input = std::shared_ptr<TRowSet>(new TRowSet(rowSet), DestroySharedRowSet);
    rowSet = {};

    std::vector<std::vector<int32_t>> rows(partitions);
    std::vector<size_t> bytes(partitions, 0);

    const size_t estimatedRowBytes = EstimateShuffleRowBytes(inputType);

    if (partitions == 1 && !NeedsHashes(partitions)) {
        rows[0].reserve(static_cast<size_t>(input->RowCount));
        for (int64_t row = 0; row < input->RowCount; ++row) {
            if (!RowSelected(*input, row)) {
                continue;
            }
            rows[0].push_back(static_cast<int32_t>(row));
        }
        bytes[0] = rows[0].size() * estimatedRowBytes;
        AddBufferedRows(0, input, std::move(rows[0]), bytes[0], nullptr);
        return;
    }

    Hashes_.assign(static_cast<size_t>(input->RowCount), 0);
    if (Code_->UseInputHash && input->Hash) {
        std::copy_n(input->Hash, input->RowCount, Hashes_.data());
    } else if (!Code_->Hash(input.get(), Hashes_.data())) {
        throw std::runtime_error("hash shuffle hash kernel failed");
    }
    auto hashes = std::make_shared<std::vector<uint64_t>>(Hashes_);
    AccumulateFilter(*input);

    for (int64_t row = 0; row < input->RowCount; ++row) {
        if (!RowSelected(*input, row)) {
            continue;
        }
        if (!FilterKeeps(static_cast<size_t>(row))) {
            continue;
        }
        const size_t dst = partitions == 1
            ? 0
            : static_cast<size_t>(Hashes_[static_cast<size_t>(row)] % partitions);
        rows[dst].push_back(static_cast<int32_t>(row));
    }

    for (size_t dst = 0; dst < partitions; ++dst) {
        bytes[dst] = rows[dst].size() * estimatedRowBytes;
        AddBufferedRows(dst, input, std::move(rows[dst]), bytes[dst], hashes);
    }
}

void THashShuffleTask::ScatterViews(TRowSet& rowSet) {
    const size_t partitions = Output_->DstCount();
    if (partitions == 0 || rowSet.RowCount <= 0) {
        return;
    }

    // Many-to-one shuffle: every row routes to lane 0, so skip hashing and
    // forward the whole rowset (keeping its existing selection) unchanged.
    // A filter still needs the hashes, so it falls through to the general path.
    if (partitions == 1 && !NeedsHashes(partitions)) {
        ClearPending();
        auto input = std::shared_ptr<TRowSet>(new TRowSet(rowSet), DestroySharedRowSet);
        rowSet = {};
        auto* data = new THashShuffleRowSetData;
        data->Input = input;
        Pending_.push_back(TPendingOutput{
            .DstLane = 0,
            .RowSet = TRowSet{
                .Columns = input->Columns,
                .ColumnCount = input->ColumnCount,
                .RowCount = input->RowCount,
                .Selection = input->Selection,
                .Destroy = DestroyHashShuffleRowSet,
                .Private = data,
                .RefCount = 1,
            },
        });
        return;
    }

    Hashes_.assign(static_cast<size_t>(rowSet.RowCount), 0);
    if (Code_->UseInputHash && rowSet.Hash) {
        std::copy_n(rowSet.Hash, rowSet.RowCount, Hashes_.data());
    } else if (!Code_->Hash(&rowSet, Hashes_.data())) {
        throw std::runtime_error("hash shuffle hash kernel failed");
    }
    auto hashes = std::make_shared<std::vector<uint64_t>>(Hashes_);
    AccumulateFilter(rowSet);

    std::vector<std::vector<uint8_t>> selections(partitions);
    std::vector<size_t> counts(partitions, 0);
    for (auto& selection : selections) {
        selection.assign(static_cast<size_t>(rowSet.RowCount), uint8_t{0});
    }
    for (int64_t row = 0; row < rowSet.RowCount; ++row) {
        if (!RowSelected(rowSet, row)) {
            continue;
        }
        if (!FilterKeeps(static_cast<size_t>(row))) {
            continue;
        }
        const size_t dst = partitions == 1
            ? 0
            : static_cast<size_t>(Hashes_[static_cast<size_t>(row)] % partitions);
        selections[dst][static_cast<size_t>(row)] = 0xff;
        ++counts[dst];
    }

    ClearPending();
    auto input = std::shared_ptr<TRowSet>(new TRowSet(rowSet), DestroySharedRowSet);
    rowSet = {};

    Pending_.reserve(partitions);
    for (size_t dst = 0; dst < partitions; ++dst) {
        if (counts[dst] == 0) {
            continue;
        }
        auto* data = new THashShuffleRowSetData;
        data->Input = input;
        data->Selection = std::move(selections[dst]);
        data->Hashes = hashes;
        Pending_.push_back(TPendingOutput{
            .DstLane = dst,
            .RowSet = TRowSet{
                .Columns = input->Columns,
                .ColumnCount = input->ColumnCount,
                .RowCount = input->RowCount,
                .Selection = data->Selection.data(),
                .Hash = data->Hashes->data(),
                .Destroy = DestroyHashShuffleRowSet,
                .Private = data,
                .RefCount = 1,
            },
        });
    }
}

void THashShuffleTask::AddBufferedRows(size_t dst,
    const std::shared_ptr<TRowSet>& input,
    std::vector<int32_t>&& rows,
    size_t bytes,
    const std::shared_ptr<std::vector<uint64_t>>& hashes)
{
    if (rows.empty()) {
        return;
    }
    auto& buffer = *Buffers_[dst];
    buffer.RowCount += rows.size();
    buffer.ByteCount += bytes;
    buffer.Batches.push_back(TBufferedBatch{
        .Input = input,
        .Rows = std::move(rows),
        .Hashes = hashes,
    });

    const size_t targetRows = Code_->TargetOutputBatchRows;
    const size_t maxRows = Code_->MaxOutputBatchRows;
    const size_t targetBytes = Code_->TargetOutputBatchBytes;
    if ((targetRows > 0 && buffer.RowCount >= targetRows) ||
        (maxRows > 0 && buffer.RowCount >= maxRows) ||
        (targetBytes > 0 && buffer.ByteCount >= targetBytes)) {
        FlushBuffer(dst);
    }
}

void THashShuffleTask::FlushBuffer(size_t dst) {
    auto& buffer = *Buffers_[dst];
    if (buffer.RowCount == 0) {
        return;
    }

    if (buffer.Batches.size() == 1) {
        auto& batch = buffer.Batches[0];
        auto* data = new THashShuffleRowSetData;
        data->Input = batch.Input;
        data->Selection.assign(static_cast<size_t>(batch.Input->RowCount),
            uint8_t{0});
        for (int32_t row : batch.Rows) {
            data->Selection[static_cast<size_t>(row)] = 0xff;
        }
        data->Hashes = batch.Hashes;
        Pending_.push_back(TPendingOutput{
            .DstLane = dst,
            .RowSet = TRowSet{
                .Columns = batch.Input->Columns,
                .ColumnCount = batch.Input->ColumnCount,
                .RowCount = batch.Input->RowCount,
                .Selection = data->Selection.data(),
                .Hash = data->Hashes ? data->Hashes->data() : nullptr,
                .Destroy = DestroyHashShuffleRowSet,
                .Private = data,
                .RefCount = 1,
            },
        });
        buffer.Batches.clear();
        buffer.RowCount = 0;
        buffer.ByteCount = 0;
        return;
    }

    const auto& inputType = ShuffleInputStruct(Code_->InputType);
    auto* data = new TMaterializedShuffleRowSetData;
    data->Gathered.resize(inputType.Fields.size());
    data->Columns.resize(inputType.Fields.size());

    for (size_t colIdx = 0; colIdx < inputType.Fields.size(); ++colIdx) {
        const auto& type = inputType.Fields[colIdx].second;
        const auto valueType = ShuffleValueType(type);
        const bool isString = static_cast<bool>(TMaybeType<TStringType>(valueType));
        const bool isBool = static_cast<bool>(TMaybeType<TBoolType>(valueType));
        const size_t width = ShuffleColumnFixedWidth(type);
        auto& out = data->Gathered[colIdx];
        const size_t rowCount = buffer.RowCount;

        out.Data.clear();
        out.Offsets.clear();
        out.Mask.assign((rowCount + 7) / 8, 0xff);
        bool anyNull = false;

        auto markNull = [&](size_t row) {
            ClearBit(out.Mask, row);
            anyNull = true;
        };

        size_t outRow = 0;
        if (isString) {
            out.Offsets.resize(rowCount + 1);
            out.Offsets[0] = 0;
            for (const auto& batch : buffer.Batches) {
                const TColumn& col = batch.Input->Columns[colIdx];
                for (int32_t row : batch.Rows) {
                    int64_t len = 0;
                    if (!SourceValid(col, row)) {
                        markNull(outRow);
                    } else {
                        len = OffsetAt(col, row + 1) - OffsetAt(col, row);
                    }
                    out.Offsets[outRow + 1] = out.Offsets[outRow] + len;
                    ++outRow;
                }
            }
            out.Data.resize(static_cast<size_t>(out.Offsets[rowCount]));
            outRow = 0;
            for (const auto& batch : buffer.Batches) {
                const TColumn& col = batch.Input->Columns[colIdx];
                for (int32_t row : batch.Rows) {
                    if (SourceValid(col, row)) {
                        const int64_t begin = OffsetAt(col, row);
                        const int64_t len = OffsetAt(col, row + 1) - begin;
                        if (len > 0) {
                            std::memcpy(out.Data.data() + out.Offsets[outRow],
                                col.Data + begin,
                                static_cast<size_t>(len));
                        }
                    }
                    ++outRow;
                }
            }
            out.Column = TColumn{
                .Data = out.Data.data(),
                .Mask = anyNull ? out.Mask.data() : nullptr,
                .Offsets = out.Offsets.data(),
                .OffsetWidth = 8,
            };
        } else if (isBool) {
            out.Data.assign((rowCount + 7) / 8, 0);
            for (const auto& batch : buffer.Batches) {
                const TColumn& col = batch.Input->Columns[colIdx];
                for (int32_t row : batch.Rows) {
                    if (!SourceValid(col, row)) {
                        markNull(outRow);
                    } else if (BoolAt(col, row)) {
                        out.Data[outRow / 8] |= char(uint8_t(1) << (outRow % 8));
                    }
                    ++outRow;
                }
            }
            out.Column = TColumn{
                .Data = out.Data.data(),
                .Mask = anyNull ? out.Mask.data() : nullptr,
            };
        } else {
            out.Data.assign(rowCount * width, 0);
            for (const auto& batch : buffer.Batches) {
                const TColumn& col = batch.Input->Columns[colIdx];
                for (int32_t row : batch.Rows) {
                    if (!SourceValid(col, row)) {
                        markNull(outRow);
                    } else {
                        std::memcpy(out.Data.data() + outRow * width,
                            col.Data + static_cast<int64_t>(row) * width,
                            width);
                    }
                    ++outRow;
                }
            }
            out.Column = TColumn{
                .Data = out.Data.data(),
                .Mask = anyNull ? out.Mask.data() : nullptr,
            };
        }

        if (!anyNull) {
            out.Mask.clear();
        }
        data->Columns[colIdx] = out.Column;
    }

    Pending_.push_back(TPendingOutput{
        .DstLane = dst,
        .RowSet = TRowSet{
            .Columns = data->Columns.data(),
            .ColumnCount = static_cast<int64_t>(data->Columns.size()),
            .RowCount = static_cast<int64_t>(buffer.RowCount),
            .Selection = nullptr,
            .Destroy = DestroyMaterializedShuffleRowSet,
            .Private = data,
            .RefCount = 1,
        },
    });

    buffer.Batches.clear();
    buffer.RowCount = 0;
    buffer.ByteCount = 0;
}

void THashShuffleTask::FlushAllBuffers() {
    for (size_t dst = 0; dst < Buffers_.size(); ++dst) {
        FlushBuffer(dst);
    }
}

ETaskResult THashShuffleTask::PushPending() {
    while (PendingIndex_ < Pending_.size()) {
        auto& pending = Pending_[PendingIndex_];
        if (!Output_->CanPushTo(SourceLane_, pending.DstLane)) {
            return ETaskResult::BLOCKED_OUTPUT;
        }
        if (!Output_->PushTo(SourceLane_, pending.DstLane, std::move(pending.RowSet))) {
            return ETaskResult::BLOCKED_OUTPUT;
        }
        ++PendingIndex_;
    }
    ClearPending();
    return ETaskResult::OK;
}

void THashShuffleTask::ClearPending() {
    for (size_t i = PendingIndex_; i < Pending_.size(); ++i) {
        if (Pending_[i].RowSet.RefCount) {
            Release(&Pending_[i].RowSet);
        }
    }
    Pending_.clear();
    PendingIndex_ = 0;
}

TCteConsumerTask::TCteConsumerTask(
    std::shared_ptr<const TCteSharedState> state, TOutputPort output)
    : State_(std::move(state))
    , Output_(output)
{}

ETaskResult TCteConsumerTask::Execute() {
    if (Finished_) {
        return ETaskResult::FINISHED;
    }
    if (State_->Status.load(std::memory_order_acquire) !=
        TCteSharedState::EStatus::Ready)
    {
        return ETaskResult::NEED_DATA;
    }
    if (NextBatch_ == State_->Batches.size()) {
        Finished_ = true;
        Output_.Finish();
        return ETaskResult::FINISHED;
    }
    if (!Output_.CanPush()) {
        return ETaskResult::BLOCKED_OUTPUT;
    }

    TRowSet view = State_->Batches[NextBatch_];
    view.Destroy = nullptr;
    view.Private = nullptr;
    view.RefCount = 1;
    if (!Output_.Push(std::move(view))) {
        return ETaskResult::BLOCKED_OUTPUT;
    }
    ++NextBatch_;
    return ETaskResult::OK;
}

} // namespace NScheduler
} // namespace NQdb
