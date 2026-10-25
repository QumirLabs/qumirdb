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
        TRowSet none{};
        Dispatch(ERowJoinKernelOp::Destroy, &none);
    }
    for (auto& batch : BuildBatches_) {
        Release(&batch);
    }
    for (auto& batch : PendingProbe_) {
        Release(&batch);
    }
}

bool TRowJoinProcessor::CanHoldProbe() const {
    return HeldProbeBytes_ < ProbeHoldBytes_;
}

uint64_t TRowJoinProcessor::ProbeBatchBytes(const TRowSet& batch) const {
    const auto rows = static_cast<uint64_t>(batch.RowCount);
    uint64_t bytes = 0;
    for (size_t i = 0; i < Kernels_.ProbeColumnWidths.size() && i < static_cast<size_t>(batch.ColumnCount); ++i) {
        const auto width = Kernels_.ProbeColumnWidths[i];
        const auto& column = batch.Columns[i];
        if (width >= 0) {
            bytes += rows * static_cast<uint64_t>(width);
        } else if (column.Offsets && rows > 0) {
            // Offsets of a string column: rows + 1 entries; the bytes are the span they cover.
            const auto offsetWidth = static_cast<uint64_t>(column.OffsetWidth);
            bytes += (rows + 1) * offsetWidth;
            if (column.OffsetWidth == 8) {
                const auto* offsets = static_cast<const int64_t*>(column.Offsets);
                bytes += static_cast<uint64_t>(offsets[rows] - offsets[0]);
            } else {
                const auto* offsets = static_cast<const int32_t*>(column.Offsets);
                bytes += static_cast<uint64_t>(offsets[rows] - offsets[0]);
            }
        }
    }
    return bytes;
}

void TRowJoinProcessor::SetInstrumentation(TOperatorInstrumentation* instrumentation) {
    Instrumentation_ = instrumentation;
}

void TRowJoinProcessor::ObserveInstrumentation() {
    if (Instrumentation_ && BuildDone_) {
        const auto side = BuildSide_ == EJoinBuildSide::Left ? 0 : 1;
        Instrumentation_->ObserveBuildTable(
            side,
            static_cast<uint64_t>(Table_.Keys),
            static_cast<uint64_t>(Table_.Capacity),
            static_cast<uint64_t>(Table_.Capacity) * sizeof(uint64_t)
                + static_cast<uint64_t>(Arena_.Rows) * static_cast<uint64_t>(Arena_.RowBytes));
    }
}

void TRowJoinProcessor::EnsureInit() {
    if (Initialized_) {
        return;
    }
    Initialized_ = true;
    TRowSet none{};
    if (!Dispatch(ERowJoinKernelOp::Init, &none)) {
        throw std::runtime_error("row join initialization failed");
    }
}

bool TRowJoinProcessor::Dispatch(ERowJoinKernelOp op, TRowSet* batch) {
    return Kernels_.Dispatch(&Arena_, &Table_, &Scratch_, &PairBuffer_, batch, RowJoinOp(op));
}

bool TRowJoinProcessor::DrainReadyOutput(TRowSet& rowSet) {
    if (ReadyOutput_.empty()) {
        return false;
    }
    rowSet = ReadyOutput_.front();
    ReadyOutput_.pop_front();
    return true;
}

bool TRowJoinProcessor::PullBuildBatch(const TFetch& build) {
    TRowSet batch{};
    const auto fetch = build(batch);
    if (fetch == EJoinFetchResult::NO_DATA) {
        return false;
    }
    if (fetch == EJoinFetchResult::FINISHED) {
        TRowSet none{};
        if (!Dispatch(ERowJoinKernelOp::Build, &none)) {
            throw std::runtime_error("row join table build failed");
        }
        BuildDone_ = true;
        ObserveInstrumentation();
        return true;
    }
    if (!Dispatch(ERowJoinKernelOp::Append, &batch)) {
        Release(&batch);
        throw std::runtime_error("row join append failed");
    }
    if (Kernels_.RetainsBuildBatches) {
        if (Instrumentation_) {
            Instrumentation_->RetainInput(batch, BuildSide_ == EJoinBuildSide::Left ? 0 : 1);
        }
        BuildBatches_.push_back(batch);
    } else {
        Release(&batch);
    }
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
    if (!BuildDone_) {
        if (Instrumentation_) {
            Instrumentation_->RetainInput(batch, BuildSide_ == EJoinBuildSide::Left ? 1 : 0);
        }
        HeldProbeBytes_ += ProbeBatchBytes(batch);
        PendingProbe_.push_back(batch);
        return true;
    }
    ProbeBatch(batch);
    return true;
}

void TRowJoinProcessor::ProbeBatch(TRowSet& batch) {
    if (!Dispatch(ERowJoinKernelOp::Probe, &batch)) {
        Release(&batch);
        throw std::runtime_error("row join probe failed");
    }
    // The pairs refer to this batch, so its matches are materialized before
    // it is released.
    int64_t cursor = 0;
    while (cursor < PairBuffer_.Count) {
        TRowSet out{};
        const int64_t produced = Kernels_.Materialize(
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

EJoinProcessorResult TRowJoinProcessor::Process(
    const TFetch& left,
    const TFetch& right,
    TRowSet& output)
{
    EnsureInit();
    if (DrainReadyOutput(output)) {
        return EJoinProcessorResult::OK;
    }
    const TFetch& build = BuildSide_ == EJoinBuildSide::Left ? left : right;
    const TFetch& probe = BuildSide_ == EJoinBuildSide::Left ? right : left;
    if (!BuildDone_) {
        // A probe batch is taken only when the build side has none ready, so
        // the probe side never delays the build, and only below the budget.
        if (!PullBuildBatch(build) && !ProbeDone_ && CanHoldProbe()) {
            PullProbeBatch(probe);
        }
        if (!BuildDone_) {
            return EJoinProcessorResult::NEED_DATA;
        }
    }
    while (!PendingProbe_.empty()) {
        TRowSet batch = PendingProbe_.front();
        PendingProbe_.pop_front();
        HeldProbeBytes_ -= ProbeBatchBytes(batch);
        if (Instrumentation_) {
            const size_t input = BuildSide_ == EJoinBuildSide::Left ? 1 : 0;
            Instrumentation_->SetRetainedInputBytes(
                Instrumentation_->RetainedInputBytes
                - Instrumentation_->InputPayloadBytes(batch, input));
        }
        ProbeBatch(batch);
        if (DrainReadyOutput(output)) {
            return EJoinProcessorResult::OK;
        }
    }
    if (ProbeDone_) {
        return EJoinProcessorResult::FINISHED;
    }
    if (!PullProbeBatch(probe)) {
        return EJoinProcessorResult::NEED_DATA;
    }
    if (DrainReadyOutput(output)) {
        return EJoinProcessorResult::OK;
    }
    return ProbeDone_ ? EJoinProcessorResult::FINISHED : EJoinProcessorResult::NEED_DATA;
}

EJoinBuildSide TRowJoinProcessor::RequiredInputSide() const {
    if (!BuildDone_) {
        return ProbeDone_ || !CanHoldProbe() ? BuildSide_ : EJoinBuildSide::Auto;
    }
    return BuildSide_ == EJoinBuildSide::Left ? EJoinBuildSide::Right : EJoinBuildSide::Left;
}

} // namespace NQdb
