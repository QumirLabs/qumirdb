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

TRowJoinProcessor::TRowJoinProcessor(
    TRowJoinKernels kernels,
    EJoinBuildSide buildSide,
    size_t probeHoldBytes)
    : Kernels_(std::move(kernels))
    , BuildSide_(buildSide)
    , ProbeHoldBytes_(probeHoldBytes)
{
    if (BuildSide_ == EJoinBuildSide::Auto) {
        throw std::runtime_error("row join requires a fixed build side");
    }
}

TRowJoinProcessor::~TRowJoinProcessor() {
    while (!ReadyOutput_.empty()) {
        Release(&ReadyOutput_.front());
        ReadyOutput_.pop_front();
    }
    if (Initialized_) {
        // Runs outside any worker, on the thread that releases the plan.
        TExecutionContext context;
        TRowSet none{};
        Dispatch(context, ERowJoinKernelOp::Destroy, &none);
        if (Kernels_.KeepsNullKeyRows) {
            Dispatch(context, ERowJoinKernelOp::Destroy, &none, NullKeyArena_);
        }
    }
    for (auto& batch : BuildBatches_) {
        Release(&batch);
    }
    for (auto& held : PendingProbe_) {
        Release(&held.Batch);
    }
}

bool TRowJoinProcessor::CanHoldProbe() const {
    return HeldProbeBytes_ < ProbeHoldBytes_;
}

uint64_t TRowJoinProcessor::ProbeBatchBytes(
    TExecutionContext& context, const TRowSet& batch) const
{
    // The same measure as the retained_input counters: all rows, no selection.
    auto measure = Kernels_.ProbeMeasure;
    auto allRows = batch;
    allRows.Selection = nullptr;
    measure.Add(context, allRows);
    return measure.Bytes;
}

void TRowJoinProcessor::SetInstrumentation(TOperatorInstrumentation* instrumentation) {
    Instrumentation_ = instrumentation;
}

void TRowJoinProcessor::ObserveInstrumentation(TExecutionContext&) {
    if (Instrumentation_ && BuildDone_) {
        const auto side = BuildSide_ == EJoinBuildSide::Left ? 0 : 1;
        Instrumentation_->ObserveBuildTable(
            side,
            static_cast<uint64_t>(Table_.Keys),
            static_cast<uint64_t>(Table_.Capacity),
            static_cast<uint64_t>(Table_.Capacity) * sizeof(uint64_t)
                + static_cast<uint64_t>(Arena_.Rows + NullKeyArena_.Rows)
                    * static_cast<uint64_t>(Arena_.RowBytes));
    }
}

void TRowJoinProcessor::EnsureInit(TExecutionContext& context) {
    if (Initialized_) {
        return;
    }
    Initialized_ = true;
    TRowSet none{};
    if (!Dispatch(context, ERowJoinKernelOp::Init, &none)
        || (Kernels_.KeepsNullKeyRows
            && !Dispatch(context, ERowJoinKernelOp::Init, &none, NullKeyArena_)))
    {
        throw std::runtime_error("row join initialization failed");
    }
}

bool TRowJoinProcessor::Dispatch(
    TExecutionContext& context, ERowJoinKernelOp op, TRowSet* batch)
{
    return Dispatch(context, op, batch, Arena_);
}

bool TRowJoinProcessor::Dispatch(
    TExecutionContext& context, ERowJoinKernelOp op, TRowSet* batch, TArenaState& arena)
{
    return Kernels_.Dispatch(
        context, &arena, &Table_, &Scratch_, &PairBuffer_, batch, RowJoinOp(op));
}

bool TRowJoinProcessor::DrainReadyOutput(TRowSet& rowSet) {
    if (ReadyOutput_.empty()) {
        return false;
    }
    rowSet = ReadyOutput_.front();
    ReadyOutput_.pop_front();
    return true;
}

bool TRowJoinProcessor::PullBuildBatch(TExecutionContext& context, const TFetch& build) {
    TRowSet batch{};
    const auto fetch = build(batch);
    if (fetch == EJoinFetchResult::NO_DATA) {
        return false;
    }
    if (fetch == EJoinFetchResult::FINISHED) {
        TRowSet none{};
        if (!Dispatch(context, ERowJoinKernelOp::Build, &none)) {
            throw std::runtime_error("row join table build failed");
        }
        BuildDone_ = true;
        ObserveInstrumentation(context);
        return true;
    }
    if (!Dispatch(context, ERowJoinKernelOp::Append, &batch)
        || (Kernels_.KeepsNullKeyRows
            && !Dispatch(context, ERowJoinKernelOp::AppendNullKeys, &batch, NullKeyArena_)))
    {
        Release(&batch);
        throw std::runtime_error("row join append failed");
    }
    if (Kernels_.RetainsBuildBatches) {
        if (Instrumentation_) {
            Instrumentation_->RetainInput(
                context, batch, BuildSide_ == EJoinBuildSide::Left ? 0 : 1);
        }
        BuildBatches_.push_back(batch);
    } else {
        Release(&batch);
    }
    return true;
}

bool TRowJoinProcessor::PullProbeBatch(TExecutionContext& context, const TFetch& probe) {
    TRowSet batch{};
    const auto fetch = probe(batch);
    if (fetch == EJoinFetchResult::NO_DATA) {
        return false;
    }
    if (fetch == EJoinFetchResult::FINISHED) {
        ProbeDone_ = true;
        return true;
    }
    if (!BuildDone_) {
        if (Instrumentation_) {
            Instrumentation_->RetainInput(
                context, batch, BuildSide_ == EJoinBuildSide::Left ? 1 : 0);
        }
        const auto bytes = ProbeBatchBytes(context, batch);
        HeldProbeBytes_ += bytes;
        PendingProbe_.push_back({batch, bytes});
        return true;
    }
    ProbeBatch(context, batch);
    return true;
}

void TRowJoinProcessor::ProbeBatch(TExecutionContext& context, TRowSet& batch) {
    if (!Dispatch(context, ERowJoinKernelOp::Probe, &batch)) {
        Release(&batch);
        throw std::runtime_error("row join probe failed");
    }
    // The pairs refer to this batch, so its matches are materialized before
    // it is released.
    int64_t cursor = 0;
    while (cursor < PairBuffer_.Count) {
        TRowSet out{};
        const int64_t produced = Kernels_.Materialize(
            context,
            &PairBuffer_, &batch, cursor, RowJoinOutputBatchRows, &out);
        if (produced <= 0) {
            Release(&batch);
            throw std::runtime_error("row join materialize failed");
        }
        out.Destroy = DestroyKernelOwnedRowSet;
        ReadyOutput_.push_back(out);
        cursor += produced;
    }
    PairBuffer_.Count = 0;
    Release(&batch);
}

// The output of a SEMI/ANTI join is known only once the probe side has ended.
// It is materialized from the pair buffer one batch per call, as the build
// rows stay in the arena until Destroy.
EJoinProcessorResult TRowJoinProcessor::Finish(TExecutionContext& context, TRowSet& output) {
    if (!Kernels_.EmitsByMark) {
        return EJoinProcessorResult::FINISHED;
    }
    TRowSet none{};
    if (!MarksEmitted_) {
        if (!Dispatch(context, ERowJoinKernelOp::EmitByMark, &none)
            || (Kernels_.KeepsNullKeyRows
                && !Dispatch(context, ERowJoinKernelOp::EmitByMark, &none, NullKeyArena_)))
        {
            throw std::runtime_error("row join emit by mark failed");
        }
        MarksEmitted_ = true;
    }
    if (EmitCursor_ >= PairBuffer_.Count) {
        return EJoinProcessorResult::FINISHED;
    }
    TRowSet out{};
    const int64_t produced = Kernels_.Materialize(
        context,
        &PairBuffer_, &none, EmitCursor_, RowJoinOutputBatchRows, &out);
    if (produced <= 0) {
        throw std::runtime_error("row join materialize failed");
    }
    out.Destroy = DestroyKernelOwnedRowSet;
    EmitCursor_ += produced;
    output = out;
    return EJoinProcessorResult::OK;
}

EJoinProcessorResult TRowJoinProcessor::Process(
    TExecutionContext& context,
    const TFetch& left,
    const TFetch& right,
    TRowSet& output)
{
    EnsureInit(context);
    if (DrainReadyOutput(output)) {
        return EJoinProcessorResult::OK;
    }
    const TFetch& build = BuildSide_ == EJoinBuildSide::Left ? left : right;
    const TFetch& probe = BuildSide_ == EJoinBuildSide::Left ? right : left;
    if (!BuildDone_) {
        // A probe batch is taken only when the build side has none ready, so
        // the probe side never delays the build, and only below the budget.
        if (!PullBuildBatch(context, build) && !ProbeDone_ && CanHoldProbe()) {
            PullProbeBatch(context, probe);
        }
        if (!BuildDone_) {
            return EJoinProcessorResult::NEED_DATA;
        }
    }
    while (!PendingProbe_.empty()) {
        TRowSet batch = PendingProbe_.front().Batch;
        HeldProbeBytes_ -= PendingProbe_.front().Bytes;
        PendingProbe_.pop_front();
        if (Instrumentation_) {
            const size_t input = BuildSide_ == EJoinBuildSide::Left ? 1 : 0;
            Instrumentation_->SetRetainedInputBytes(
                Instrumentation_->RetainedInputBytes
                - Instrumentation_->InputPayloadBytes(context, batch, input));
        }
        ProbeBatch(context, batch);
        if (DrainReadyOutput(output)) {
            return EJoinProcessorResult::OK;
        }
    }
    if (ProbeDone_) {
        return Finish(context, output);
    }
    if (!PullProbeBatch(context, probe)) {
        return EJoinProcessorResult::NEED_DATA;
    }
    if (DrainReadyOutput(output)) {
        return EJoinProcessorResult::OK;
    }
    return ProbeDone_ ? Finish(context, output) : EJoinProcessorResult::NEED_DATA;
}

EJoinBuildSide TRowJoinProcessor::RequiredInputSide() const {
    if (!BuildDone_) {
        return ProbeDone_ || !CanHoldProbe() ? BuildSide_ : EJoinBuildSide::Auto;
    }
    return BuildSide_ == EJoinBuildSide::Left ? EJoinBuildSide::Right : EJoinBuildSide::Left;
}

} // namespace NQdb
