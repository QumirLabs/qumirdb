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

TRowJoinProcessor::TRowJoinProcessor(TRowJoinKernels kernels, EJoinBuildSide buildSide)
    : Kernels_(std::move(kernels))
    , BuildSide_(buildSide)
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
    return true;
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
    if (ProbeDone_) {
        return EJoinProcessorResult::FINISHED;
    }
    const TFetch& build = BuildSide_ == EJoinBuildSide::Left ? left : right;
    const TFetch& probe = BuildSide_ == EJoinBuildSide::Left ? right : left;
    if (!BuildDone_) {
        PullBuildBatch(build);
        return EJoinProcessorResult::NEED_DATA;
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
        return BuildSide_;
    }
    return BuildSide_ == EJoinBuildSide::Left ? EJoinBuildSide::Right : EJoinBuildSide::Left;
}

} // namespace NQdb
