#include <qdb/exec/join_exec.h>

#include <qdb/exec/kernel_rowset.h>

#include <stdexcept>
#include <utility>

namespace NQdb {

namespace {

constexpr int64_t JoinInitialCapacity = 256;

constexpr int64_t JoinOp(EJoinKernelOp op) {
    return static_cast<int64_t>(op);
}

constexpr int64_t CrossJoinOp(ECrossJoinKernelOp op) {
    return static_cast<int64_t>(op);
}

} // namespace

TInnerJoinProcessor::TInnerJoinProcessor(
    TJoinKernels kernels,
    EJoinType joinType,
    EJoinBuildSide buildSide)
    : Kernels_(std::move(kernels))
    , JoinType_(joinType)
    // Asymmetry needs the streaming (drop-one-side) path, which outer joins skip.
    , BuildSide_(IsOuter() ? EJoinBuildSide::Auto : buildSide)
{
}

bool TInnerJoinProcessor::IsOuter() const {
    return JoinType_ == EJoinType::Left || JoinType_ == EJoinType::Right
        || JoinType_ == EJoinType::Full;
}

bool TInnerJoinProcessor::IsSemiAnti() const {
    return JoinType_ == EJoinType::LeftSemi || JoinType_ == EJoinType::LeftAnti;
}

TInnerJoinProcessor::~TInnerJoinProcessor() {
    while (!ReadyOutput_.empty()) {
        Release(&ReadyOutput_.front());
        ReadyOutput_.pop_front();
    }
    if (Initialized_) {
        Kernels_.Dispatch(
            LeftTable_.data(),
            RightTable_.data(),
            nullptr,
            0,
            &PairBuffer_,
            nullptr,
            nullptr,
            0,
            JoinOp(EJoinKernelOp::Destroy));
    }
}

void TInnerJoinProcessor::EnsureInit() {
    if (Initialized_) {
        return;
    }
    Initialized_ = true;
    if (!Kernels_.Dispatch(
            LeftTable_.data(),
            RightTable_.data(),
            nullptr,
            0,
            &PairBuffer_,
            nullptr,
            nullptr,
            JoinInitialCapacity,
            JoinOp(EJoinKernelOp::Init))) {
        throw std::runtime_error("join hash table initialization failed");
    }
}

bool TInnerJoinProcessor::DrainReadyOutput(TRowSet& rowSet) {
    if (ReadyOutput_.empty()) {
        return false;
    }
    rowSet = ReadyOutput_.front();
    ReadyOutput_.pop_front();
    return true;
}

// Materializes at most one output batch of pending pairs through the kernel;
// resets the buffer once the cursor catches up so later kernel ops (Finalize)
// always see an empty buffer.
bool TInnerJoinProcessor::DrainMaterialized(TRowSet& rowSet) {
    if (MaterializeCursor_ >= PairBuffer_.Count) {
        return false;
    }
    auto* leftStore = const_cast<TRowSet*>(LeftRows_.Data());
    auto* rightStore = const_cast<TRowSet*>(RightRows_.Data());
    TRowSet out{};
    const int64_t produced = Kernels_.Materialize(
        &PairBuffer_, leftStore, rightStore, leftStore, rightStore,
        MaterializeCursor_, JoinOutputBatchRows, &out);
    if (produced <= 0) {
        throw std::runtime_error("join materialize failed");
    }
    out.Destroy = DestroyKernelOwnedRowSet;
    MaterializeCursor_ += produced;
    if (MaterializeCursor_ >= PairBuffer_.Count) {
        PairBuffer_.Count = 0;
        MaterializeCursor_ = 0;
    }
    rowSet = out;
    return true;
}

void TInnerJoinProcessor::DrainStreamingPairs(
    const TRowSet& streamBatch,
    EJoinSide streamSide)
{
    auto* stream = const_cast<TRowSet*>(&streamBatch);
    auto* leftStore = const_cast<TRowSet*>(LeftRows_.Data());
    auto* rightStore = const_cast<TRowSet*>(RightRows_.Data());
    TRowSet* streamLeft = streamSide == EJoinSide::Left ? stream : leftStore;
    TRowSet* streamRight = streamSide == EJoinSide::Right ? stream : rightStore;

    // The stream batch is released right after this call, so every pair that
    // references it (packed batch index -1) must be materialized now. Pairs
    // left over from the stored phase materialize fine with any stream pointer.
    while (MaterializeCursor_ < PairBuffer_.Count) {
        TRowSet out{};
        const int64_t produced = Kernels_.Materialize(
            &PairBuffer_, leftStore, rightStore, streamLeft, streamRight,
            MaterializeCursor_, JoinOutputBatchRows, &out);
        if (produced <= 0) {
            throw std::runtime_error("join materialize failed");
        }
        out.Destroy = DestroyKernelOwnedRowSet;
        ReadyOutput_.push_back(out);
        MaterializeCursor_ += produced;
    }
    PairBuffer_.Count = 0;
    MaterializeCursor_ = 0;
}

EJoinSide TInnerJoinProcessor::ChooseSymmetricPullSide() const {
    if (StoredLeftRows_ == 0 && StoredRightRows_ == 0) {
        return EJoinSide::Left;
    }
    if (StoredLeftRows_ <= StoredRightRows_) {
        if (LastLeftBatchRows_ > 0 &&
            StoredLeftRows_ + LastLeftBatchRows_ >= StoredRightRows_) {
            return EJoinSide::Right;
        }
        return EJoinSide::Left;
    }
    if (LastRightBatchRows_ > 0 &&
        StoredRightRows_ + LastRightBatchRows_ >= StoredLeftRows_) {
        return EJoinSide::Left;
    }
    return EJoinSide::Right;
}

bool TInnerJoinProcessor::PullOneInputBatch(
    const TFetch& left,
    const TFetch& right)
{
    auto processLeft = [&]() {
        TRowSet batch{};
        auto fetch = left(batch);
        if (fetch == EJoinFetchResult::NO_DATA) {
            return false;
        }
        if (fetch == EJoinFetchResult::FINISHED) {
            LeftDone_ = true;
            return true;
        }

        StoredLeftRows_ += batch.RowCount;
        LastLeftBatchRows_ = batch.RowCount;
        const int32_t batchIdx = LeftRows_.PushBatch(batch);
        if (!Kernels_.Dispatch(
                LeftTable_.data(),
                RightTable_.data(),
                const_cast<TRowSet*>(&LeftRows_.Batch(batchIdx)),
                batchIdx,
                &PairBuffer_,
                const_cast<TRowSet*>(LeftRows_.Data()),
                const_cast<TRowSet*>(RightRows_.Data()),
                0,
                JoinOp(EJoinKernelOp::UpdateLeft))) {
            throw std::runtime_error("join kernel update failed");
        }
        return true;
    };

    auto processRight = [&]() {
        TRowSet batch{};
        auto fetch = right(batch);
        if (fetch == EJoinFetchResult::NO_DATA) {
            return false;
        }
        if (fetch == EJoinFetchResult::FINISHED) {
            RightDone_ = true;
            return true;
        }

        const bool keyOnly = StreamsSemiAntiLeft();
        int32_t batchIdx = -1;
        TRowSet* input = &batch;
        if (!keyOnly) {
            StoredRightRows_ += batch.RowCount;
            LastRightBatchRows_ = batch.RowCount;
            batchIdx = RightRows_.PushBatch(batch);
            input = const_cast<TRowSet*>(&RightRows_.Batch(batchIdx));
        }
        if (!Kernels_.Dispatch(
                LeftTable_.data(),
                RightTable_.data(),
                input,
                batchIdx,
                &PairBuffer_,
                const_cast<TRowSet*>(LeftRows_.Data()),
                const_cast<TRowSet*>(RightRows_.Data()),
                0,
                JoinOp(EJoinKernelOp::UpdateRight))) {
            if (keyOnly) {
                Release(&batch);
            }
            throw std::runtime_error("join kernel update failed");
        }
        if (keyOnly) {
            Release(&batch);
        }
        return true;
    };

    auto streamLeft = [&]() {
        TRowSet batch{};
        auto fetch = left(batch);
        if (fetch == EJoinFetchResult::NO_DATA) {
            return false;
        }
        if (fetch == EJoinFetchResult::FINISHED) {
            LeftDone_ = true;
            BothDone_ = true;
            return true;
        }

        if (!Kernels_.Dispatch(
                LeftTable_.data(),
                RightTable_.data(),
                &batch,
                -1,
                &PairBuffer_,
                const_cast<TRowSet*>(LeftRows_.Data()),
                const_cast<TRowSet*>(RightRows_.Data()),
                0,
                JoinOp(EJoinKernelOp::StreamLeft))) {
            Release(&batch);
            throw std::runtime_error("join kernel update failed");
        }
        DrainStreamingPairs(batch, EJoinSide::Left);
        Release(&batch);
        return true;
    };

    auto streamRight = [&]() {
        TRowSet batch{};
        auto fetch = right(batch);
        if (fetch == EJoinFetchResult::NO_DATA) {
            return false;
        }
        if (fetch == EJoinFetchResult::FINISHED) {
            RightDone_ = true;
            BothDone_ = true;
            return true;
        }

        if (!Kernels_.Dispatch(
                LeftTable_.data(),
                RightTable_.data(),
                &batch,
                -1,
                &PairBuffer_,
                const_cast<TRowSet*>(LeftRows_.Data()),
                const_cast<TRowSet*>(RightRows_.Data()),
                0,
                JoinOp(EJoinKernelOp::StreamRight))) {
            Release(&batch);
            throw std::runtime_error("join kernel update failed");
        }
        DrainStreamingPairs(batch, EJoinSide::Right);
        Release(&batch);
        return true;
    };

    // Outer joins need both sides for unmatched rows.
    const bool allowStreaming = !IsOuter();

    for (;;) {
        if (allowStreaming &&
            StreamMode_ == EJoinStreamMode::StreamLeftAgainstRight) {
            return streamLeft();
        }
        if (allowStreaming &&
            StreamMode_ == EJoinStreamMode::StreamRightAgainstLeft) {
            return streamRight();
        }

        if (LeftDone_ && RightDone_) {
            BothDone_ = true;
            return true;
        }
        if (LeftDone_) {
            if (allowStreaming) {
                StreamMode_ = EJoinStreamMode::StreamRightAgainstLeft;
                continue;
            }
            return processRight();
        }
        if (RightDone_) {
            if (allowStreaming) {
                StreamMode_ = EJoinStreamMode::StreamLeftAgainstRight;
                continue;
            }
            return processLeft();
        }

        if (BuildSide_ == EJoinBuildSide::Right) {
            return processRight();
        }
        if (BuildSide_ == EJoinBuildSide::Left) {
            return processLeft();
        }

        const auto first = ChooseSymmetricPullSide();
        if (first == EJoinSide::Left) {
            if (processLeft()) {
                return true;
            }
            return processRight();
        }
        if (processRight()) {
            return true;
        }
        return processLeft();
    }
}

void TInnerJoinProcessor::FinalizeOuterJoin() {
    if (!Kernels_.Dispatch(
            LeftTable_.data(),
            RightTable_.data(),
            nullptr,
            0,
            &PairBuffer_,
            const_cast<TRowSet*>(LeftRows_.Data()),
            const_cast<TRowSet*>(RightRows_.Data()),
            0,
            JoinOp(EJoinKernelOp::Finalize))) {
        throw std::runtime_error("join outer finalize failed");
    }
    OuterFinalized_ = true;
}

bool TInnerJoinProcessor::StreamsSemiAntiLeft() const {
    return IsSemiAnti() && BuildSide_ == EJoinBuildSide::Right;
}

// Left rows must survive until the final membership scan.
bool TInnerJoinProcessor::PullSemiAntiBatch(
    const TFetch& left,
    const TFetch& right)
{
    if (!LeftDone_) {
        TRowSet batch{};
        auto fetch = left(batch);
        if (fetch == EJoinFetchResult::NO_DATA) {
            return false;
        }
        if (fetch == EJoinFetchResult::FINISHED) {
            LeftDone_ = true;
            return true;
        }
        const int32_t batchIdx = LeftRows_.PushBatch(batch);
        if (!Kernels_.Dispatch(
                LeftTable_.data(),
                RightTable_.data(),
                const_cast<TRowSet*>(&LeftRows_.Batch(batchIdx)),
                batchIdx,
                &PairBuffer_,
                const_cast<TRowSet*>(LeftRows_.Data()),
                const_cast<TRowSet*>(RightRows_.Data()),
                0,
                JoinOp(EJoinKernelOp::UpdateLeft))) {
            throw std::runtime_error("join kernel update failed");
        }
        PairBuffer_.Count = 0;
        return true;
    }
    if (!RightDone_) {
        TRowSet batch{};
        auto fetch = right(batch);
        if (fetch == EJoinFetchResult::NO_DATA) {
            return false;
        }
        if (fetch == EJoinFetchResult::FINISHED) {
            RightDone_ = true;
            BothDone_ = true;
            return true;
        }
        if (!Kernels_.Dispatch(
                LeftTable_.data(),
                RightTable_.data(),
                &batch,
                -1,
                &PairBuffer_,
                const_cast<TRowSet*>(LeftRows_.Data()),
                const_cast<TRowSet*>(RightRows_.Data()),
                0,
                JoinOp(EJoinKernelOp::UpdateRight))) {
            Release(&batch);
            throw std::runtime_error("join kernel update failed");
        }
        PairBuffer_.Count = 0;
        Release(&batch);
        return true;
    }
    BothDone_ = true;
    return true;
}

void TInnerJoinProcessor::FinalizeSemiAntiJoin() {
    if (!Kernels_.Dispatch(
            LeftTable_.data(),
            RightTable_.data(),
            nullptr,
            0,
            &PairBuffer_,
            const_cast<TRowSet*>(LeftRows_.Data()),
            const_cast<TRowSet*>(RightRows_.Data()),
            LeftRows_.BatchCount(),
            JoinOp(EJoinKernelOp::Finalize))) {
        throw std::runtime_error("join semi/anti finalize failed");
    }
    SemiAntiFinalized_ = true;
}

EJoinProcessorResult TInnerJoinProcessor::ProcessSemiAnti(
    const TFetch& left,
    const TFetch& right,
    TRowSet& output)
{
    if (DrainMaterialized(output)) {
        return EJoinProcessorResult::OK;
    }
    if (SemiAntiFinalized_) {
        return EJoinProcessorResult::FINISHED;
    }
    while (!BothDone_) {
        if (!PullSemiAntiBatch(left, right)) {
            return EJoinProcessorResult::NEED_DATA;
        }
    }
    FinalizeSemiAntiJoin();
    if (DrainMaterialized(output)) {
        return EJoinProcessorResult::OK;
    }
    return EJoinProcessorResult::FINISHED;
}

EJoinProcessorResult TInnerJoinProcessor::Process(
    const TFetch& left,
    const TFetch& right,
    TRowSet& output)
{
    EnsureInit();

    if (IsSemiAnti() && !StreamsSemiAntiLeft()) {
        return ProcessSemiAnti(left, right, output);
    }

    // Once both inputs are drained, outer joins emit their unmatched rows.
    auto finishedResult = [&]() -> EJoinProcessorResult {
        if (IsOuter() && !OuterFinalized_) {
            FinalizeOuterJoin();
            if (DrainReadyOutput(output) || DrainMaterialized(output)) {
                return EJoinProcessorResult::OK;
            }
        }
        return EJoinProcessorResult::FINISHED;
    };

    if (DrainReadyOutput(output) || DrainMaterialized(output)) {
        return EJoinProcessorResult::OK;
    }
    if (BothDone_) {
        return finishedResult();
    }
    if (!PullOneInputBatch(left, right)) {
        return EJoinProcessorResult::NEED_DATA;
    }
    if (DrainReadyOutput(output) || DrainMaterialized(output)) {
        return EJoinProcessorResult::OK;
    }
    return BothDone_ ? finishedResult() : EJoinProcessorResult::NEED_DATA;
}

EJoinBuildSide TInnerJoinProcessor::RequiredInputSide() const {
    // While streaming, the streamed side is the probe: it is the one being read.
    if (StreamMode_ == EJoinStreamMode::StreamLeftAgainstRight) {
        return EJoinBuildSide::Left;
    }
    if (StreamMode_ == EJoinStreamMode::StreamRightAgainstLeft) {
        return EJoinBuildSide::Right;
    }
    if (BuildSide_ == EJoinBuildSide::Left && !LeftDone_) {
        return EJoinBuildSide::Left;
    }
    if (BuildSide_ == EJoinBuildSide::Right && !RightDone_) {
        return EJoinBuildSide::Right;
    }
    return EJoinBuildSide::Auto;
}

TCrossJoinProcessor::TCrossJoinProcessor(TCrossJoinKernels kernels)
    : Kernels_(std::move(kernels))
{
}

TCrossJoinProcessor::~TCrossJoinProcessor() {
    // The pair buffer is only allocated once the kernel actually runs (pb_push
    // during Process). When nothing ran - e.g. plan export, which compiles the
    // kernels to WASM but never JIT-finalizes them, so Kernels_.Dispatch wraps
    // a null function pointer - Data stays null and there is nothing to free.
    if (PairBuffer_.Data) {
        Kernels_.Dispatch(
            nullptr,
            0,
            nullptr,
            0,
            &PairBuffer_,
            CrossJoinOp(ECrossJoinKernelOp::Destroy));
    }
}

bool TCrossJoinProcessor::DrainMaterialized(TRowSet& rowSet) {
    if (MaterializeCursor_ >= PairBuffer_.Count) {
        return false;
    }
    auto* leftStore = const_cast<TRowSet*>(LeftRows_.Data());
    auto* rightStore = const_cast<TRowSet*>(RightRows_.Data());
    TRowSet out{};
    const int64_t produced = Kernels_.Materialize(
        &PairBuffer_, leftStore, rightStore, leftStore, rightStore,
        MaterializeCursor_, JoinOutputBatchRows, &out);
    if (produced <= 0) {
        throw std::runtime_error("cross join materialize failed");
    }
    out.Destroy = DestroyKernelOwnedRowSet;
    MaterializeCursor_ += produced;
    if (MaterializeCursor_ >= PairBuffer_.Count) {
        PairBuffer_.Count = 0;
        MaterializeCursor_ = 0;
    }
    rowSet = out;
    return true;
}

EJoinProcessorResult TCrossJoinProcessor::Process(
    const TFetch& left,
    const TFetch& right,
    TRowSet& output)
{
    if (DrainMaterialized(output)) {
        return EJoinProcessorResult::OK;
    }

    // Phase 1: fully buffer the (broadcast) right side.
    if (!RightDrained_) {
        for (;;) {
            TRowSet batch{};
            auto fetch = right(batch);
            if (fetch == EJoinFetchResult::NO_DATA) {
                return EJoinProcessorResult::NEED_DATA;
            }
            if (fetch == EJoinFetchResult::FINISHED) {
                RightDrained_ = true;
                break;
            }
            RightTotalRows_ += batch.RowCount;
            RightRows_.PushBatch(batch);
        }
    }

    // Phase 2: stream left, pairing each selected left row with every selected
    // right row.
    for (;;) {
        if (DrainMaterialized(output)) {
            return EJoinProcessorResult::OK;
        }
        if (LeftDone_) {
            return EJoinProcessorResult::FINISHED;
        }
        TRowSet batch{};
        auto fetch = left(batch);
        if (fetch == EJoinFetchResult::NO_DATA) {
            return EJoinProcessorResult::NEED_DATA;
        }
        if (fetch == EJoinFetchResult::FINISHED) {
            LeftDone_ = true;
            continue;
        }
        if (RightTotalRows_ == 0) {
            Release(&batch);
            continue;
        }
        const int32_t li = LeftRows_.PushBatch(batch);
        if (!Kernels_.Dispatch(
                const_cast<TRowSet*>(&LeftRows_.Batch(li)),
                li,
                const_cast<TRowSet*>(RightRows_.Data()),
                RightRows_.BatchCount(),
                &PairBuffer_,
                CrossJoinOp(ECrossJoinKernelOp::Emit))) {
            throw std::runtime_error("cross join kernel update failed");
        }
        if (DrainMaterialized(output)) {
            return EJoinProcessorResult::OK;
        }
    }
}

} // namespace NQdb
