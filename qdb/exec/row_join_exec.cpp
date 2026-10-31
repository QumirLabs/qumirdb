#include <qdb/exec/row_join_exec.h>

#include <qdb/exec/kernel_rowset.h>

#include <stdexcept>
#include <utility>

namespace NQdb {

namespace {

// Rows per materialized output batch. Larger than the legacy join's 1024:
// every batch is one hop through the downstream queue.
constexpr int64_t RowJoinOutputBatchRows = 16384;

constexpr int64_t RowJoinOp(ERowJoinKernelOp op) {
    return static_cast<int64_t>(op);
}

} // namespace

TRowJoinTable::TRowJoinTable(TRowJoinKernels kernels)
    : Kernels_(std::move(kernels))
{}

TRowJoinTable::~TRowJoinTable() {
    if (Initialized_) {
        TRowSet none{};
        Dispatch(ERowJoinKernelOp::Destroy, &none, Arena_);
        if (Kernels_.KeepsNullKeyRows) {
            Dispatch(ERowJoinKernelOp::Destroy, &none, NullKeyArena_);
        }
    }
    for (auto& batch : BuildBatches_) {
        Release(&batch);
    }
}

void TRowJoinTable::SetInstrumentation(TOperatorInstrumentation* instrumentation, size_t input) {
    Instrumentation_ = instrumentation;
    InstrumentationInput_ = input;
}

void TRowJoinTable::ObserveInstrumentation() const {
    if (Instrumentation_ && Sealed_) {
        Instrumentation_->ObserveBuildTable(
            InstrumentationInput_,
            static_cast<uint64_t>(Table_.Keys),
            static_cast<uint64_t>(Table_.Capacity),
            static_cast<uint64_t>(Table_.Capacity) * sizeof(uint64_t)
                + static_cast<uint64_t>(Arena_.Rows + NullKeyArena_.Rows)
                    * static_cast<uint64_t>(Arena_.RowBytes));
    }
}

void TRowJoinTable::EnsureInit() {
    if (Initialized_) {
        return;
    }
    Initialized_ = true;
    TRowSet none{};
    if (!Dispatch(ERowJoinKernelOp::Init, &none, Arena_)
        || (Kernels_.KeepsNullKeyRows
            && !Dispatch(ERowJoinKernelOp::Init, &none, NullKeyArena_)))
    {
        throw std::runtime_error("row join initialization failed");
    }
}

bool TRowJoinTable::Dispatch(ERowJoinKernelOp op, TRowSet* batch, TRowArenaState& arena) {
    // Init, Append, Build and Destroy touch only the arena and the table here;
    // the probe lanes own the scratch and pair buffers.
    TRowScratchState noScratch{};
    TJoinPairBuffer noPairs{};
    return Kernels_.Dispatch(&arena, &Table_, &noScratch, &noPairs, batch, RowJoinOp(op));
}

void TRowJoinTable::Add(TRowSet batch) {
    if (Sealed_) {
        Release(&batch);
        throw std::runtime_error("the row join table is already sealed");
    }
    EnsureInit();
    if (!Dispatch(ERowJoinKernelOp::Append, &batch, Arena_)
        || (Kernels_.KeepsNullKeyRows
            && !Dispatch(ERowJoinKernelOp::AppendNullKeys, &batch, NullKeyArena_)))
    {
        Release(&batch);
        throw std::runtime_error("row join append failed");
    }
    if (Kernels_.RetainsBuildBatches) {
        if (Instrumentation_) {
            Instrumentation_->RetainInput(batch, InstrumentationInput_);
        }
        BuildBatches_.push_back(batch);
    } else {
        Release(&batch);
    }
}

void TRowJoinTable::Seal() {
    EnsureInit();
    TRowSet none{};
    if (!Dispatch(ERowJoinKernelOp::Build, &none, Arena_)) {
        throw std::runtime_error("row join table build failed");
    }
    Sealed_ = true;
    ObserveInstrumentation();
}

void* TRowJoinTable::ArenaState() const {
    return const_cast<TRowArenaState*>(&Arena_);
}

void* TRowJoinTable::NullKeyArenaState() const {
    return const_cast<TRowArenaState*>(&NullKeyArena_);
}

void* TRowJoinTable::TableState() const {
    return const_cast<TRowTableState*>(&Table_);
}

TRowJoinProbe::TRowJoinProbe(std::shared_ptr<const TRowJoinTable> table, bool useInputHash)
    : Table_(std::move(table))
    , UseInputHash_(useInputHash)
{}

TRowJoinProbe::~TRowJoinProbe() {
    while (!ReadyOutput_.empty()) {
        Release(&ReadyOutput_.front());
        ReadyOutput_.pop_front();
    }
    if (Used_) {
        // Zeroed in place of the shared table, so Destroy frees only this
        // lane's scratch and pair buffer.
        TRowArenaState noArena{};
        TRowTableState noTable{};
        TRowSet none{};
        Table_->Kernels().Dispatch(
            &noArena, &noTable, &Scratch_, &PairBuffer_, &none, RowJoinOp(ERowJoinKernelOp::Destroy));
    }
}

bool TRowJoinProbe::Dispatch(ERowJoinKernelOp op, TRowSet* batch, void* arena) {
    Used_ = true;
    return Table_->Kernels().Dispatch(
        arena, Table_->TableState(), &Scratch_, &PairBuffer_, batch, RowJoinOp(op));
}

TRowSet TRowJoinProbe::Materialize(TRowSet* stream, int64_t start) {
    TRowSet out{};
    const int64_t produced = Table_->Kernels().Materialize(
        &PairBuffer_, stream, start, RowJoinOutputBatchRows, &out);
    if (produced <= 0) {
        throw std::runtime_error("row join materialize failed");
    }
    out.Destroy = DestroyKernelOwnedRowSet;
    return out;
}

void TRowJoinProbe::Probe(TRowSet batch) {
    if (!Table_->Sealed()) {
        Release(&batch);
        throw std::runtime_error("probe of the row join table before it is sealed");
    }
    // The kernel reads a shallow view; the batch keeps its owner and refcount.
    TRowSet view = batch;
    if (!UseInputHash_) {
        view.Hash = nullptr;
    }
    if (!Dispatch(ERowJoinKernelOp::Probe, &view, Table_->ArenaState())) {
        Release(&batch);
        throw std::runtime_error("row join probe failed");
    }
    // The pairs refer to this batch, so its matches are materialized before
    // it is released.
    int64_t cursor = 0;
    while (cursor < PairBuffer_.Count) {
        TRowSet out{};
        try {
            out = Materialize(&view, cursor);
        } catch (...) {
            Release(&batch);
            throw;
        }
        ReadyOutput_.push_back(out);
        cursor += out.RowCount;
    }
    PairBuffer_.Count = 0;
    Release(&batch);
}

bool TRowJoinProbe::Next(TRowSet& output) {
    if (ReadyOutput_.empty()) {
        return false;
    }
    output = ReadyOutput_.front();
    ReadyOutput_.pop_front();
    return true;
}

// The output of a SEMI/ANTI join is known only once the probe side has ended.
// It is materialized from the pair buffer one batch per call, as the build
// rows stay in the arena until Destroy.
bool TRowJoinProbe::EmitMarked(TRowSet& output) {
    TRowSet none{};
    if (!MarksEmitted_) {
        if (!Dispatch(ERowJoinKernelOp::EmitByMark, &none, Table_->ArenaState())
            || (Table_->Kernels().KeepsNullKeyRows
                && !Dispatch(ERowJoinKernelOp::EmitByMark, &none, Table_->NullKeyArenaState())))
        {
            throw std::runtime_error("row join emit by mark failed");
        }
        MarksEmitted_ = true;
    }
    if (EmitCursor_ >= PairBuffer_.Count) {
        return false;
    }
    output = Materialize(&none, EmitCursor_);
    EmitCursor_ += output.RowCount;
    return true;
}

TRowJoinProcessor::TRowJoinProcessor(
    TRowJoinKernels kernels,
    EJoinBuildSide buildSide,
    size_t probeHoldBytes)
    : BuildSide_(buildSide)
    , Table_(std::make_shared<TRowJoinTable>(std::move(kernels)))
    // Both inputs of a partitioned join come from the hash shuffle on this
    // join's keys, so a probe batch's hash is its key hash.
    , Probe_(Table_, /*useInputHash=*/true)
    , ProbeHoldBytes_(probeHoldBytes)
{
    if (BuildSide_ == EJoinBuildSide::Auto) {
        throw std::runtime_error("row join requires a fixed build side");
    }
}

TRowJoinProcessor::~TRowJoinProcessor() {
    for (auto& held : PendingProbe_) {
        Release(&held.Batch);
    }
}

size_t TRowJoinProcessor::ProbeInput() const {
    return BuildSide_ == EJoinBuildSide::Left ? 1 : 0;
}

bool TRowJoinProcessor::CanHoldProbe() const {
    return HeldProbeBytes_ < ProbeHoldBytes_;
}

uint64_t TRowJoinProcessor::ProbeBatchBytes(const TRowSet& batch) const {
    // The same measure as the retained_input counters: all rows, no selection.
    auto measure = Table_->Kernels().ProbeMeasure;
    auto allRows = batch;
    allRows.Selection = nullptr;
    measure.Add(allRows);
    return measure.Bytes;
}

void TRowJoinProcessor::SetInstrumentation(TOperatorInstrumentation* instrumentation) {
    Instrumentation_ = instrumentation;
    Table_->SetInstrumentation(instrumentation, BuildSide_ == EJoinBuildSide::Left ? 0 : 1);
}

void TRowJoinProcessor::ObserveInstrumentation() {
    Table_->ObserveInstrumentation();
}

bool TRowJoinProcessor::PullBuildBatch(const TFetch& build) {
    TRowSet batch{};
    const auto fetch = build(batch);
    if (fetch == EJoinFetchResult::NO_DATA) {
        return false;
    }
    if (fetch == EJoinFetchResult::FINISHED) {
        Table_->Seal();
        return true;
    }
    Table_->Add(batch);
    return true;
}

bool TRowJoinProcessor::PullProbeBatch(const TFetch& probe) {
    TRowSet batch{};
    const auto fetch = probe(batch);
    if (fetch == EJoinFetchResult::NO_DATA) {
        return false;
    }
    if (fetch == EJoinFetchResult::FINISHED) {
        ProbeDone_ = true;
        return true;
    }
    if (!Table_->Sealed()) {
        if (Instrumentation_) {
            Instrumentation_->RetainInput(batch, ProbeInput());
        }
        const auto bytes = ProbeBatchBytes(batch);
        HeldProbeBytes_ += bytes;
        PendingProbe_.push_back({batch, bytes});
        return true;
    }
    Probe_.Probe(batch);
    return true;
}

EJoinProcessorResult TRowJoinProcessor::Finish(TRowSet& output) {
    if (!Table_->Kernels().EmitsByMark) {
        return EJoinProcessorResult::FINISHED;
    }
    return Probe_.EmitMarked(output)
        ? EJoinProcessorResult::OK
        : EJoinProcessorResult::FINISHED;
}

EJoinProcessorResult TRowJoinProcessor::Process(
    const TFetch& left,
    const TFetch& right,
    TRowSet& output)
{
    if (Probe_.Next(output)) {
        return EJoinProcessorResult::OK;
    }
    const TFetch& build = BuildSide_ == EJoinBuildSide::Left ? left : right;
    const TFetch& probe = BuildSide_ == EJoinBuildSide::Left ? right : left;
    if (!Table_->Sealed()) {
        // A probe batch is taken only when the build side has none ready, so
        // the probe side never delays the build, and only below the budget.
        if (!PullBuildBatch(build) && !ProbeDone_ && CanHoldProbe()) {
            PullProbeBatch(probe);
        }
        if (!Table_->Sealed()) {
            return EJoinProcessorResult::NEED_DATA;
        }
    }
    while (!PendingProbe_.empty()) {
        TRowSet batch = PendingProbe_.front().Batch;
        HeldProbeBytes_ -= PendingProbe_.front().Bytes;
        PendingProbe_.pop_front();
        if (Instrumentation_) {
            Instrumentation_->SetRetainedInputBytes(
                Instrumentation_->RetainedInputBytes
                - Instrumentation_->InputPayloadBytes(batch, ProbeInput()));
        }
        Probe_.Probe(batch);
        if (Probe_.Next(output)) {
            return EJoinProcessorResult::OK;
        }
    }
    if (ProbeDone_) {
        return Finish(output);
    }
    if (!PullProbeBatch(probe)) {
        return EJoinProcessorResult::NEED_DATA;
    }
    if (Probe_.Next(output)) {
        return EJoinProcessorResult::OK;
    }
    return ProbeDone_ ? Finish(output) : EJoinProcessorResult::NEED_DATA;
}

EJoinBuildSide TRowJoinProcessor::RequiredInputSide() const {
    if (!Table_->Sealed()) {
        return ProbeDone_ || !CanHoldProbe() ? BuildSide_ : EJoinBuildSide::Auto;
    }
    return BuildSide_ == EJoinBuildSide::Left ? EJoinBuildSide::Right : EJoinBuildSide::Left;
}

} // namespace NQdb
