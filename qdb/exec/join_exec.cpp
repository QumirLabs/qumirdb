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

// This processor streams an outer join only against its preserved side, whose
// matched marks give the unmatched rows. A null-supplying build side, which
// the row join table serves, falls back to the symmetric path here.
EJoinBuildSide StreamableBuildSide(EJoinType type, EJoinBuildSide buildSide) {
    switch (type) {
        case EJoinType::Left:
            return buildSide == EJoinBuildSide::Left
                ? buildSide : EJoinBuildSide::Auto;
        case EJoinType::Right:
            return buildSide == EJoinBuildSide::Right
                ? buildSide : EJoinBuildSide::Auto;
        case EJoinType::Full:
            return EJoinBuildSide::Auto;
        default:
            return buildSide;
    }
}

} // namespace

TInnerJoinProcessor::TInnerJoinProcessor(
    TJoinKernels kernels,
    EJoinType joinType,
    EJoinBuildSide buildSide)
    : Kernels_(std::move(kernels))
    , JoinType_(joinType)
    , BuildSide_(StreamableBuildSide(joinType, buildSide))
{
    // Its markers need the complete right side; no input order is wired yet.
    if (JoinType_ == EJoinType::LeftMark) {
        throw std::runtime_error("left_mark join is not supported by the join processor yet");
    }
}

void TInnerJoinProcessor::SetInstrumentation(TOperatorInstrumentation* instrumentation) {
    Instrumentation_ = instrumentation;
    LeftRows_.SetInstrumentation(instrumentation, 0);
    RightRows_.SetInstrumentation(instrumentation, 1);
}

void TInnerJoinProcessor::ObserveInstrumentation(TExecutionContext& context) {
    if (Instrumentation_ && Initialized_) {
        Instrumentation_->ObserveHash(context, LeftTable_.data(), 0, true);
        Instrumentation_->ObserveHash(context, RightTable_.data(), 1, true);
    }
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
        // Runs outside any worker, on the thread that releases the plan.
        TExecutionContext context{TExecutionContext::TReleaseTag{}};
        Kernels_.Dispatch(
            context,
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

void TInnerJoinProcessor::EnsureInit(TExecutionContext& context) {
    if (Initialized_) {
        return;
    }
    Initialized_ = true;
    if (!Kernels_.Dispatch(
            context,
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
    ObserveInstrumentation(context);
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
bool TInnerJoinProcessor::DrainMaterialized(TExecutionContext& context, TRowSet& rowSet) {
    if (MaterializeCursor_ >= PairBuffer_.Count) {
        return false;
    }
    auto* leftStore = const_cast<TRowSet*>(LeftRows_.Data());
    auto* rightStore = const_cast<TRowSet*>(RightRows_.Data());
    TRowSet out{};
    const int64_t produced = Kernels_.Materialize(
        context,
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
    TExecutionContext& context,
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
            context,
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
    TExecutionContext& context,
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
        const int32_t batchIdx = LeftRows_.PushBatch(context, batch);
        if (!Kernels_.Dispatch(
                context,
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
            batchIdx = RightRows_.PushBatch(context, batch);
            input = const_cast<TRowSet*>(&RightRows_.Batch(batchIdx));
        }
        if (!Kernels_.Dispatch(
                context,
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
                context,
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
        DrainStreamingPairs(context, batch, EJoinSide::Left);
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
                context,
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
        DrainStreamingPairs(context, batch, EJoinSide::Right);
        Release(&batch);
        return true;
    };

    // Without a preserved build side an outer join keeps both sides for the
    // unmatched scan.
    const bool allowStreaming =
        !IsOuter() || BuildSide_ != EJoinBuildSide::Auto;

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

// The preserved rows an outer join leaves unmatched, and the left rows of a
// SEMI/ANTI join that built them, are known only once both inputs end.
bool TInnerJoinProcessor::FinalizesPreservedRows() const {
    return IsOuter() || StreamsSemiAntiRight();
}

void TInnerJoinProcessor::FinalizePreservedRows(TExecutionContext& context) {
    // A streamed join has no table of the other side to look keys up in; its
    // build rows carry matched marks instead.
    const auto op = BuildSide_ == EJoinBuildSide::Auto
        ? EJoinKernelOp::Finalize : EJoinKernelOp::FinalizeStream;
    if (!Kernels_.Dispatch(
            context,
            LeftTable_.data(),
            RightTable_.data(),
            nullptr,
            0,
            &PairBuffer_,
            const_cast<TRowSet*>(LeftRows_.Data()),
            const_cast<TRowSet*>(RightRows_.Data()),
            0,
            JoinOp(op))) {
        throw std::runtime_error("join finalize of preserved rows failed");
    }
    PreservedFinalized_ = true;
}

bool TInnerJoinProcessor::StreamsSemiAntiLeft() const {
    return IsSemiAnti() && BuildSide_ == EJoinBuildSide::Right;
}

bool TInnerJoinProcessor::StreamsSemiAntiRight() const {
    return IsSemiAnti() && BuildSide_ == EJoinBuildSide::Left;
}

// The symmetric path stores the left rows and the distinct right keys; the left
// rows must survive until the final membership scan.
bool TInnerJoinProcessor::PullSemiAntiBatch(
    TExecutionContext& context,
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
        const int32_t batchIdx = LeftRows_.PushBatch(context, batch);
        if (!Kernels_.Dispatch(
                context,
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
                context,
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

void TInnerJoinProcessor::FinalizeSemiAntiJoin(TExecutionContext& context) {
    if (!Kernels_.Dispatch(
            context,
            LeftTable_.data(),
            RightTable_.data(),
            nullptr,
            0,
            &PairBuffer_,
            const_cast<TRowSet*>(LeftRows_.Data()),
            const_cast<TRowSet*>(RightRows_.Data()),
            0,
            JoinOp(EJoinKernelOp::Finalize))) {
        throw std::runtime_error("join semi/anti finalize failed");
    }
    SemiAntiFinalized_ = true;
}

EJoinProcessorResult TInnerJoinProcessor::ProcessSemiAnti(
    TExecutionContext& context,
    const TFetch& left,
    const TFetch& right,
    TRowSet& output)
{
    if (DrainMaterialized(context, output)) {
        return EJoinProcessorResult::OK;
    }
    if (SemiAntiFinalized_) {
        return EJoinProcessorResult::FINISHED;
    }
    while (!BothDone_) {
        if (!PullSemiAntiBatch(context, left, right)) {
            return EJoinProcessorResult::NEED_DATA;
        }
    }
    FinalizeSemiAntiJoin(context);
    if (DrainMaterialized(context, output)) {
        return EJoinProcessorResult::OK;
    }
    return EJoinProcessorResult::FINISHED;
}

EJoinProcessorResult TInnerJoinProcessor::Process(
    TExecutionContext& context,
    const TFetch& left,
    const TFetch& right,
    TRowSet& output)
{
    EnsureInit(context);

    if (IsSemiAnti() && BuildSide_ == EJoinBuildSide::Auto) {
        return ProcessSemiAnti(context, left, right, output);
    }

    // Once both inputs are drained, outer joins emit their unmatched rows and
    // SEMI/ANTI joins that built their left side the rows its marks select.
    auto finishedResult = [&]() -> EJoinProcessorResult {
        if (FinalizesPreservedRows() && !PreservedFinalized_) {
            FinalizePreservedRows(context);
            if (DrainReadyOutput(output) || DrainMaterialized(context, output)) {
                return EJoinProcessorResult::OK;
            }
        }
        return EJoinProcessorResult::FINISHED;
    };

    if (DrainReadyOutput(output) || DrainMaterialized(context, output)) {
        return EJoinProcessorResult::OK;
    }
    if (BothDone_) {
        return finishedResult();
    }
    if (!PullOneInputBatch(context, left, right)) {
        return EJoinProcessorResult::NEED_DATA;
    }
    if (DrainReadyOutput(output) || DrainMaterialized(context, output)) {
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

void TCrossJoinProcessor::SetInstrumentation(TOperatorInstrumentation* instrumentation) {
    LeftRows_.SetInstrumentation(instrumentation, 0);
    RightRows_.SetInstrumentation(instrumentation, 1);
}

TCrossJoinProcessor::TCrossJoinProcessor(
    TCrossJoinKernels kernels,
    EJoinType joinType)
    : Kernels_(std::move(kernels))
    , JoinType_(joinType)
{
    // The nested-loop kernel has no marker emission.
    if (JoinType_ == EJoinType::LeftMark) {
        throw std::runtime_error("left_mark join is not supported by the nested-loop join processor");
    }
}

TCrossJoinProcessor::~TCrossJoinProcessor() {
    // The pair buffer is only allocated once the kernel actually runs (pb_push
    // during Process). When nothing ran - e.g. plan export, which compiles the
    // kernels to WASM but never JIT-finalizes them, so Kernels_.Dispatch wraps
    // a null function pointer - Data stays null and there is nothing to free.
    if (PairBuffer_.Data) {
        // Runs outside any worker, on the thread that releases the plan.
        TExecutionContext context{TExecutionContext::TReleaseTag{}};
        Kernels_.Dispatch(
            context,
            nullptr,
            0,
            nullptr,
            0,
            nullptr,
            &PairBuffer_,
            CrossJoinOp(ECrossJoinKernelOp::Destroy));
    }
}

// Left rows without a match still produce output: NULL padding or ANTI rows.
bool TCrossJoinProcessor::KeepsUnmatchedLeft() const {
    return JoinType_ == EJoinType::Left || JoinType_ == EJoinType::Full
        || JoinType_ == EJoinType::LeftAnti;
}

bool TCrossJoinProcessor::KeepsUnmatchedRight() const {
    return JoinType_ == EJoinType::Right || JoinType_ == EJoinType::Full;
}

bool TCrossJoinProcessor::Dispatch(
    TExecutionContext& context, int32_t leftBatch, ECrossJoinKernelOp op)
{
    return Kernels_.Dispatch(
        context,
        const_cast<TRowSet*>(LeftRows_.Data()),
        leftBatch,
        const_cast<TRowSet*>(RightRows_.Data()),
        RightRows_.BatchCount(),
        RightMatched_.data(),
        &PairBuffer_,
        CrossJoinOp(op));
}

bool TCrossJoinProcessor::DrainMaterialized(TExecutionContext& context, TRowSet& rowSet) {
    if (MaterializeCursor_ >= PairBuffer_.Count) {
        return false;
    }
    auto* leftStore = const_cast<TRowSet*>(LeftRows_.Data());
    auto* rightStore = const_cast<TRowSet*>(RightRows_.Data());
    TRowSet out{};
    const int64_t produced = Kernels_.Materialize(
        context,
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
    TExecutionContext& context,
    const TFetch& left,
    const TFetch& right,
    TRowSet& output)
{
    if (DrainMaterialized(context, output)) {
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
                if (KeepsUnmatchedRight()) {
                    RightMatched_.assign(RightTotalRows_, 0);
                }
                break;
            }
            RightTotalRows_ += batch.RowCount;
            RightRows_.PushBatch(context, batch);
        }
    }

    // Phase 2: stream left, comparing each selected left row with every
    // selected right row.
    for (;;) {
        if (DrainMaterialized(context, output)) {
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
            if (KeepsUnmatchedRight() &&
                !Dispatch(context, 0, ECrossJoinKernelOp::EmitUnmatchedRight)) {
                throw std::runtime_error("cross join unmatched-right emission failed");
            }
            continue;
        }
        if (RightTotalRows_ == 0 && !KeepsUnmatchedLeft()) {
            Release(&batch);
            continue;
        }
        const int32_t li = LeftRows_.PushBatch(context, batch);
        if (!Dispatch(context, li, ECrossJoinKernelOp::Emit)) {
            throw std::runtime_error("cross join kernel update failed");
        }
        if (DrainMaterialized(context, output)) {
            return EJoinProcessorResult::OK;
        }
    }
}

} // namespace NQdb
