#include <qdb/scheduler/runtime_adapter.h>

#include <qdb/scheduler/graph.h>
#include <qdb/scheduler/instrumentation/task.h>
#include <qdb/plan/types/decimal.h>
#include <qdb/plan/types/nullable.h>

#include <qumir/error.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <unordered_map>
#include <unordered_set>
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
    std::shared_ptr<uint64_t> Hashes; // nullptr if not computed for this batch
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
    std::vector<uint64_t> Hashes;
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
    if (auto decimal = DecimalSpecOf(type)) {
        return static_cast<size_t>(DecimalStorageBytes(*decimal));
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

void AddPayloadBytes(size_t& total, size_t count, size_t width = 1) {
    const size_t limit = std::numeric_limits<size_t>::max();
    if (width != 0 && count > (limit - total) / width) {
        total = limit;
    } else {
        total += count * width;
    }
}

// A sparse view retains the full visible input, not just its selected rows.
// Count each destination's reference conservatively: shared input storage may
// be charged more than once, and opaque source ancestors are not visible here.
size_t ReferencedInputBytes(
    const TRowSet& input,
    const TStructType& type,
    const std::shared_ptr<uint64_t>& hashes)
{
    const size_t rows = static_cast<size_t>(input.RowCount);
    size_t bytes = 0;
    for (size_t index = 0; index < type.Fields.size(); ++index) {
        const auto& column = input.Columns[index];
        const auto& fieldType = type.Fields[index].second;
        const auto valueType = ShuffleValueType(fieldType);
        if (TMaybeType<TStringType>(valueType)) {
            AddPayloadBytes(bytes, rows + 1, column.OffsetWidth);
            AddPayloadBytes(bytes, static_cast<size_t>(
                OffsetAt(column, input.RowCount) - OffsetAt(column, 0)));
        } else if (TMaybeType<TBoolType>(valueType)) {
            AddPayloadBytes(bytes, (rows + column.DataBitOffset % 8 + 7) / 8);
        } else {
            AddPayloadBytes(bytes, rows, ShuffleColumnFixedWidth(fieldType));
        }
        if (column.Mask) {
            AddPayloadBytes(bytes, (rows + column.MaskBitOffset % 8 + 7) / 8);
        }
    }
    if (input.Selection) {
        AddPayloadBytes(bytes, rows);
    }
    if (input.Hash) {
        AddPayloadBytes(bytes, rows, sizeof(uint64_t));
    }
    if (hashes && hashes.get() != input.Hash) {
        AddPayloadBytes(bytes, rows, sizeof(uint64_t));
    }
    return bytes;
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
    const auto result = Connection
        ? Connection->Fetch(Lane, rowSet)
        : EFetchResult::FINISHED;
    if (result == EFetchResult::OK && Instrumentation) {
        Instrumentation->Add(rowSet);
    }
    return result;
}

bool TOutputPort::CanPush() const {
    return !Connection || Connection->CanPush(Lane);
}

bool TOutputPort::Push(TRowSet&& rowSet) const {
    const auto rows = Instrumentation ? Instrumentation->Rows : 0;
    const auto bytes = Instrumentation ? Instrumentation->Bytes : 0;
    if (Instrumentation) {
        Instrumentation->Add(rowSet);
    }
    const bool pushed = !Connection || Connection->Push(Lane, std::move(rowSet));
    if (!pushed && Instrumentation) {
        Instrumentation->Rows = rows;
        Instrumentation->Bytes = bytes;
    }
    return pushed;
}

void TOutputPort::Finish() const {
    if (Connection) {
        Connection->Finish(Lane);
    }
}

void TAuxInput::AddFilter(std::shared_ptr<IRuntimeFilterProbe> filter) {
    Filters_.push_back(std::move(filter));
}

void TAuxInput::AddProducer(TTaskNode& producer) {
    Producers_.push_back(&producer);
}

bool TAuxInput::Ready() const {
    return std::all_of(Filters_.begin(), Filters_.end(),
        [](const auto& filter) { return filter->Ready(); });
}

EAuxInputStatus TAuxInput::Poll() const {
    if (Ready()) {
        return EAuxInputStatus::Ready;
    }
    if (Producers_.empty()) {
        return EAuxInputStatus::Failed;
    }
    if (std::any_of(Producers_.begin(), Producers_.end(), [](const auto* producer) {
            return producer->State.load(std::memory_order_acquire) == ETaskState::Idle;
        })) {
        return EAuxInputStatus::NeedProducer;
    }
    if (std::all_of(Producers_.begin(), Producers_.end(), [](const auto* producer) {
            return producer->State.load(std::memory_order_acquire) == ETaskState::Finished;
        })) {
        // Publication and task completion are separate atomic operations.
        return Ready() ? EAuxInputStatus::Ready : EAuxInputStatus::Failed;
    }
    return EAuxInputStatus::Waiting;
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

namespace {

struct TRuntimeFilterRowSetData {
    TRowSet Input;
    std::vector<uint8_t> Selection;
    std::vector<uint64_t> Hashes;
};

void DestroyRuntimeFilterRowSet(TRowSet* rowSet) {
    auto* data = static_cast<TRuntimeFilterRowSetData*>(rowSet->Private);
    Release(&data->Input);
    delete data;
}

} // namespace

TRuntimeFilterTask::TRuntimeFilterTask(
    std::shared_ptr<const TRuntimeFilterCode> code,
    TInputPort input,
    TOutputPort output)
    : Code_(std::move(code))
    , Input_(input)
    , Output_(output)
{}

TRuntimeFilterTask::~TRuntimeFilterTask() {
    if (HasInput_) {
        Release(&CurrentInput_);
    }
}

bool TRuntimeFilterTask::WantsInput(const TTaskEdge& edge) const noexcept {
    switch (Code_->AuxInput->Poll()) {
        case EAuxInputStatus::Ready:
            return edge.Kind == ETaskEdgeKind::Rowset;
        case EAuxInputStatus::NeedProducer:
            return edge.Kind == ETaskEdgeKind::Dependency;
        case EAuxInputStatus::Waiting:
        case EAuxInputStatus::Failed:
            return false;
    }
    return false;
}

bool TRuntimeFilterTask::TryNotifyDependency() noexcept {
    const auto status = Code_->AuxInput->Poll();
    if (status != EAuxInputStatus::Ready
        && status != EAuxInputStatus::Failed) {
        return false;
    }
    bool expected = false;
    return DependencyNotified_.compare_exchange_strong(expected, true,
        std::memory_order_acq_rel);
}

ETaskResult TRuntimeFilterTask::Execute() {
    if (OutputFinished_) {
        return ETaskResult::FINISHED;
    }
    switch (Code_->AuxInput->Poll()) {
        case EAuxInputStatus::Ready: break;
        case EAuxInputStatus::NeedProducer:
        case EAuxInputStatus::Waiting: return ETaskResult::NEED_DATA;
        case EAuxInputStatus::Failed:
            throw std::runtime_error(
                "auxiliary producers finished without publishing input");
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

    if (!Prepared_) {
        if (!Code_->Probe || !Code_->Hash) {
            throw std::runtime_error("runtime filter consumer is not bound");
        }
        if (!ProbeState_) {
            ProbeState_ = Code_->Probe->CreateProbeState();
        }
        const size_t rows = static_cast<size_t>(CurrentInput_.RowCount);
        const std::span<const uint8_t> selection = CurrentInput_.Selection
            ? std::span<const uint8_t>(CurrentInput_.Selection, rows)
            : std::span<const uint8_t>{};
        const size_t selected = selection.empty() ? rows : static_cast<size_t>(
            std::count_if(selection.begin(), selection.end(),
                [](uint8_t value) { return value != 0; }));
        if (Code_->Probe->BeginBatch(*ProbeState_, selected)) {
            auto data = std::make_unique<TRuntimeFilterRowSetData>();
            data->Hashes.resize(rows);
            if (!Code_->Hash(&CurrentInput_, data->Hashes.data())) {
                throw std::runtime_error("runtime filter hash kernel failed");
            }
            data->Selection.resize(rows);
            const size_t kept = Code_->Probe->MayContainBatch(
                data->Hashes, data->Selection, *ProbeState_, selection);
            if (!kept) {
                Release(&CurrentInput_);
                CurrentInput_ = {};
                HasInput_ = false;
                return ETaskResult::OK;
            }
            data->Input = CurrentInput_;
            CurrentInput_.Selection = data->Selection.data();
            CurrentInput_.Hash = data->Hashes.data();
            CurrentInput_.Destroy = DestroyRuntimeFilterRowSet;
            CurrentInput_.Private = data.release();
            CurrentInput_.RefCount = 1;
        } else {
            // A previous filter may have hashed a different key.
            CurrentInput_.Hash = nullptr;
        }
        Prepared_ = true;
    }
    if (!Output_.Push(std::move(CurrentInput_))) {
        return ETaskResult::BLOCKED_OUTPUT;
    }
    CurrentInput_ = {};
    HasInput_ = false;
    Prepared_ = false;
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
    std::shared_ptr<ITaskState> state,
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

    auto result = Code_->Process(*State_, Input_, CurrentOutput_);
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
    std::shared_ptr<ITaskState> state,
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

    auto result = Code_->Process(*State_, Left_, Right_, CurrentOutput_);
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

bool TBinaryBlockingTask::WantsInput(const TTaskEdge& edge) const noexcept {
    if (!Code_->RequiredSide) {
        return true;
    }
    const auto side = Code_->RequiredSide(*State_);
    if (side == ERequiredSide::Both) {
        return true;
    }
    const TInputPort& wanted = side == ERequiredSide::Left ? Left_ : Right_;
    return edge.Connection == wanted.Connection && edge.DstLane == wanted.Lane;
}

TMergeTask::TMergeTask(
    std::shared_ptr<const TMergeCode> code,
    std::shared_ptr<ITaskState> state,
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

    auto result = Code_->Process(*State_, Inputs_, CurrentOutput_);
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
    std::shared_ptr<uint64_t> Hashes; // nullptr if not computed for this batch
};

struct THashShuffleTask::TInstrumentationState {
    TOperatorInstrumentation* Counters = nullptr;
    std::unordered_map<const void*, uint64_t> PayloadBytes;
};

struct THashShuffleTask::TDestinationBuffer {
    std::vector<TBufferedBatch> Batches;
    size_t RowCount = 0;
    size_t ByteCount = 0;
    size_t ReferencedInputBytes = 0;
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
    auto fetch = InputFinished_ ? EFetchResult::FINISHED : Input_.Fetch(rowSet);
    if (fetch == EFetchResult::NO_DATA) {
        return ETaskResult::NEED_DATA;
    }
    if (fetch == EFetchResult::FINISHED) {
        if (!InputFinished_) {
            InputFinished_ = true;
            if (Code_->ProducedFilter) {
                // All input keys have been collected. Publish before waiting
                // for buffered output, which cannot change this contribution.
                auto builder = FilterBuilder_
                    ? std::move(*FilterBuilder_)
                    : Code_->ProducedFilter->MakeBuilder();
                Code_->ProducedFilter->FinishProducer(std::move(builder).TakePartial());
                FilterBuilder_.reset();
            }
            FlushAllBuffers();
        }
        auto result = PushPending();
        if (result != ETaskResult::OK) {
            return result;
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

void THashShuffleTask::ObserveInstrumentation() {
    if (!ResourceInstrumentation_) {
        return;
    }
    auto& state = *ResourceInstrumentation_;
    std::unordered_set<const void*> owners;
    uint64_t bytes = 0;
    auto retain = [&](const void* owner, const TRowSet& rowSet) {
        if (!owners.insert(owner).second) {
            return;
        }
        auto found = state.PayloadBytes.find(owner);
        if (found == state.PayloadBytes.end()) {
            found = state.PayloadBytes.emplace(owner,
                state.Counters->InputPayloadBytes(rowSet, 0)).first;
        }
        bytes += found->second;
    };
    for (const auto& buffer : Buffers_) {
        for (const auto& batch : buffer->Batches) {
            retain(batch.Input.get(), *batch.Input);
        }
    }
    for (size_t i = PendingIndex_; i < Pending_.size(); ++i) {
        const auto& rowSet = Pending_[i].RowSet;
        if (rowSet.Destroy == DestroyHashShuffleRowSet) {
            const auto* view = static_cast<const THashShuffleRowSetData*>(rowSet.Private);
            retain(view->Input.get(), *view->Input);
        } else {
            retain(rowSet.Private, rowSet);
        }
    }
    std::erase_if(state.PayloadBytes, [&](const auto& entry) {
        return !owners.contains(entry.first);
    });
    state.Counters->SetRetainedInputBytes(bytes);
}

bool THashShuffleTask::BatchingEnabled() const {
    return Code_->InputType && Code_->TargetOutputBatchRows > 0;
}

std::shared_ptr<uint64_t> THashShuffleTask::ComputeHashes(
    const std::shared_ptr<TRowSet>& input)
{
    if (Code_->UseInputHash && input->Hash) {
        // Share the input owner without copying its already computed hashes.
        return std::shared_ptr<uint64_t>(input, input->Hash);
    }
    auto hashes = std::make_shared<std::vector<uint64_t>>(input->RowCount);
    if (!Code_->Hash(input.get(), hashes->data())) {
        throw std::runtime_error("hash shuffle hash kernel failed");
    }
    return std::shared_ptr<uint64_t>(hashes, hashes->data());
}

void THashShuffleTask::AccumulateFilter(
    const TRowSet& rowSet, std::span<const uint64_t> hashes)
{
    if (!Code_->ProducedFilter) {
        return;
    }
    if (!FilterBuilder_) {
        FilterBuilder_.emplace(Code_->ProducedFilter->MakeBuilder());
    }
    FilterBuilder_->Add(hashes, rowSet.Selection
        ? std::span<const uint8_t>(rowSet.Selection, hashes.size())
        : std::span<const uint8_t>{});
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
    if (Output_->DstCount() == 0 || rowSet.RowCount <= 0) {
        return;
    }
    auto input = std::shared_ptr<TRowSet>(new TRowSet(rowSet), DestroySharedRowSet);
    rowSet = {};
    if (BatchingEnabled()) {
        ScatterBuffered(input);
    } else {
        ScatterViews(input);
    }
    ObserveInstrumentation();
}

void THashShuffleTask::ScatterBuffered(const std::shared_ptr<TRowSet>& input) {
    const size_t partitions = Output_->DstCount();
    EnsureBuffers(partitions);

    const auto& inputType = ShuffleInputStruct(Code_->InputType);
    if (input->ColumnCount < static_cast<int64_t>(inputType.Fields.size())) {
        throw std::runtime_error("hash shuffle rowset has fewer columns than schema");
    }

    std::vector<std::vector<int32_t>> rows(partitions);
    std::vector<size_t> bytes(partitions, 0);

    const size_t estimatedRowBytes = EstimateShuffleRowBytes(inputType);

    if (partitions == 1 && !Code_->ProducedFilter) {
        rows[0].reserve(input->RowCount);
        for (int64_t row = 0; row < input->RowCount; ++row) {
            if (!RowSelected(*input, row)) {
                continue;
            }
            rows[0].push_back(static_cast<int32_t>(row));
        }
        bytes[0] = rows[0].size() * estimatedRowBytes;
        const auto hashes = Code_->UseInputHash && input->Hash
            ? std::shared_ptr<uint64_t>(input, input->Hash) : nullptr;
        const size_t inputBytes = ReferencedInputBytes(*input, inputType, hashes);
        AddBufferedRows(0, input, std::move(rows[0]), bytes[0], hashes, inputBytes);
        return;
    }

    auto hashes = ComputeHashes(input);
    const std::span<const uint64_t> hashValues(hashes.get(), input->RowCount);
    AccumulateFilter(*input, hashValues);

    for (int64_t row = 0; row < input->RowCount; ++row) {
        if (!RowSelected(*input, row)) {
            continue;
        }
        const size_t dst = partitions == 1 ? 0 : hashValues[row] % partitions;
        rows[dst].push_back(static_cast<int32_t>(row));
    }

    const size_t inputBytes = ReferencedInputBytes(*input, inputType, hashes);
    for (size_t dst = 0; dst < partitions; ++dst) {
        bytes[dst] = rows[dst].size() * estimatedRowBytes;
        AddBufferedRows(dst, input, std::move(rows[dst]), bytes[dst], hashes, inputBytes);
    }
}

void THashShuffleTask::ScatterViews(const std::shared_ptr<TRowSet>& input) {
    const size_t partitions = Output_->DstCount();

    // Many-to-one shuffle: every row routes to lane 0, so skip hashing and
    // forward the whole rowset (keeping its existing selection) unchanged.
    if (partitions == 1 && !Code_->ProducedFilter) {
        ClearPending();
        auto* data = new THashShuffleRowSetData;
        data->Input = input;
        Pending_.push_back(TPendingOutput{
            .DstLane = 0,
            .RowSet = TRowSet{
                .Columns = input->Columns,
                .ColumnCount = input->ColumnCount,
                .RowCount = input->RowCount,
                .Selection = input->Selection,
                .Hash = Code_->UseInputHash ? input->Hash : nullptr,
                .Destroy = DestroyHashShuffleRowSet,
                .Private = data,
                .RefCount = 1,
            },
        });
        return;
    }

    auto hashes = ComputeHashes(input);
    const std::span<const uint64_t> hashValues(hashes.get(), input->RowCount);
    AccumulateFilter(*input, hashValues);

    std::vector<std::vector<uint8_t>> selections(partitions);
    std::vector<size_t> counts(partitions, 0);
    for (auto& selection : selections) {
        selection.assign(input->RowCount, uint8_t{0});
    }
    for (int64_t row = 0; row < input->RowCount; ++row) {
        if (!RowSelected(*input, row)) {
            continue;
        }
        const size_t dst = partitions == 1 ? 0 : hashValues[row] % partitions;
        selections[dst][row] = 0xff;
        ++counts[dst];
    }

    ClearPending();

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
                .Hash = data->Hashes.get(),
                .Destroy = DestroyHashShuffleRowSet,
                .Private = data,
                .RefCount = 1,
            },
        });
    }
}

void THashShuffleTask::AddBufferedRows(
    size_t dst,
    const std::shared_ptr<TRowSet>& input,
    std::vector<int32_t>&& rows,
    size_t bytes,
    const std::shared_ptr<uint64_t>& hashes,
    size_t inputBytes)
{
    if (rows.empty()) {
        return;
    }
    auto& buffer = *Buffers_[dst];
    buffer.RowCount += rows.size();
    buffer.ByteCount += bytes;
    AddPayloadBytes(buffer.ReferencedInputBytes, inputBytes);
    buffer.Batches.push_back(TBufferedBatch{
        .Input = input,
        .Rows = std::move(rows),
        .Hashes = hashes,
    });
    ObserveInstrumentation();

    const size_t targetRows = Code_->TargetOutputBatchRows;
    const size_t maxRows = Code_->MaxOutputBatchRows;
    const size_t targetBytes = Code_->TargetOutputBatchBytes;
    const bool releaseInputs = targetBytes > 0
        && buffer.ReferencedInputBytes >= targetBytes;
    if ((targetRows > 0 && buffer.RowCount >= targetRows) ||
        (maxRows > 0 && buffer.RowCount >= maxRows) ||
        (targetBytes > 0 && buffer.ByteCount >= targetBytes) || releaseInputs) {
        FlushBuffer(dst, releaseInputs);
    }
}

void THashShuffleTask::FlushBuffer(size_t dst, bool releaseInputs) {
    auto& buffer = *Buffers_[dst];
    if (buffer.RowCount == 0) {
        return;
    }

    // Forwarding a sparse view at the retention limit would move the large
    // owner into a downstream blocking operator instead of releasing it.
    if (buffer.Batches.size() == 1 && (!releaseInputs
            || buffer.Batches[0].Rows.size()
                == static_cast<size_t>(buffer.Batches[0].Input->RowCount))) {
        auto& batch = buffer.Batches[0];
        auto* data = new THashShuffleRowSetData;
        data->Input = batch.Input;
        data->Selection.assign(batch.Input->RowCount, uint8_t{0});
        for (int32_t row : batch.Rows) {
            data->Selection[row] = 0xff;
        }
        data->Hashes = batch.Hashes;
        Pending_.push_back(TPendingOutput{
            .DstLane = dst,
            .RowSet = TRowSet{
                .Columns = batch.Input->Columns,
                .ColumnCount = batch.Input->ColumnCount,
                .RowCount = batch.Input->RowCount,
                .Selection = data->Selection.data(),
                .Hash = data->Hashes.get(),
                .Destroy = DestroyHashShuffleRowSet,
                .Private = data,
                .RefCount = 1,
            },
        });
        buffer.Batches.clear();
        buffer.RowCount = 0;
        buffer.ByteCount = 0;
        buffer.ReferencedInputBytes = 0;
        ObserveInstrumentation();
        return;
    }

    const auto& inputType = ShuffleInputStruct(Code_->InputType);
    auto* data = new TMaterializedShuffleRowSetData;
    data->Gathered.resize(inputType.Fields.size());
    data->Columns.resize(inputType.Fields.size());
    if (std::all_of(buffer.Batches.begin(), buffer.Batches.end(),
            [](const auto& batch) { return static_cast<bool>(batch.Hashes); })) {
        data->Hashes.reserve(buffer.RowCount);
        for (const auto& batch : buffer.Batches) {
            for (int32_t row : batch.Rows) {
                data->Hashes.push_back(batch.Hashes.get()[row]);
            }
        }
    }

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
            .Hash = data->Hashes.empty() ? nullptr : data->Hashes.data(),
            .Destroy = DestroyMaterializedShuffleRowSet,
            .Private = data,
            .RefCount = 1,
        },
    });

    buffer.Batches.clear();
    buffer.RowCount = 0;
    buffer.ByteCount = 0;
    buffer.ReferencedInputBytes = 0;
    ObserveInstrumentation();
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
        const auto rows = OutputInstrumentation_ ? OutputInstrumentation_->Rows : 0;
        const auto bytes = OutputInstrumentation_ ? OutputInstrumentation_->Bytes : 0;
        if (OutputInstrumentation_) {
            OutputInstrumentation_->Add(pending.RowSet);
        }
        if (!Output_->PushTo(SourceLane_, pending.DstLane, std::move(pending.RowSet))) {
            if (OutputInstrumentation_) {
                OutputInstrumentation_->Rows = rows;
                OutputInstrumentation_->Bytes = bytes;
            }
            return ETaskResult::BLOCKED_OUTPUT;
        }
        ++PendingIndex_;
        ObserveInstrumentation();
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
    ObserveInstrumentation();
}

void TSourceTask::ConfigureInstrumentation(TTaskInstrumentationSetup& setup) {
    setup.Output(Output_);
}

void TRuntimeFilterTask::ConfigureInstrumentation(TTaskInstrumentationSetup& setup) {
    setup.Input(Input_);
    setup.Output(Output_);
}

void TUnaryTask::ConfigureInstrumentation(TTaskInstrumentationSetup& setup) {
    setup.Input(Input_);
    setup.Output(Output_);
}

void TSinkTask::ConfigureInstrumentation(TTaskInstrumentationSetup& setup) {
    setup.Input(Input_);
}

void TBlockingTask::ConfigureInstrumentation(TTaskInstrumentationSetup& setup) {
    setup.Input(Input_);
    setup.Output(Output_);
    State_->ConfigureInstrumentation(&setup.Counters());
}

void TBinaryBlockingTask::ConfigureInstrumentation(TTaskInstrumentationSetup& setup) {
    setup.Input(Left_);
    setup.Input(Right_);
    setup.Output(Output_);
    State_->ConfigureInstrumentation(&setup.Counters());
}

void TMergeTask::ConfigureInstrumentation(TTaskInstrumentationSetup& setup) {
    setup.Output(Output_);
    State_->ConfigureInstrumentation(&setup.Counters());
    for (auto& input : Inputs_) {
        setup.Input(input);
    }
}

void THashShuffleTask::ConfigureInstrumentation(TTaskInstrumentationSetup& setup) {
    setup.Input(Input_);
    OutputInstrumentation_ = &setup.Output(*Output_);
    ResourceInstrumentation_ = std::make_unique<TInstrumentationState>();
    ResourceInstrumentation_->Counters = &setup.Counters();
}

} // namespace NScheduler
} // namespace NQdb
