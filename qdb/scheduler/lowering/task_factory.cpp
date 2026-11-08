#include <qdb/scheduler/lowering/task_factory.h>

namespace NQdb::NScheduler::NLowering {

std::shared_ptr<const NScheduler::TSourceCode> MakeSourceCode()
{
    return std::make_shared<NScheduler::TSourceCode>(
        [](void* state, TRowSet& rowSet) {
            auto* sourceState = static_cast<TSchedulerSourceState*>(state);
            return sourceState->Source->Next(rowSet)
                ? NScheduler::ETaskResult::OK
                : NScheduler::ETaskResult::FINISHED;
        });
}

EJoinFetchResult MapJoinFetch(NScheduler::EFetchResult result)
{
    switch (result) {
        case NScheduler::EFetchResult::OK:
            return EJoinFetchResult::OK;
        case NScheduler::EFetchResult::NO_DATA:
            return EJoinFetchResult::NO_DATA;
        case NScheduler::EFetchResult::FINISHED:
            return EJoinFetchResult::FINISHED;
    }
    return EJoinFetchResult::FINISHED;
}

NScheduler::ETaskResult MapJoinProcessResult(EJoinProcessorResult result)
{
    switch (result) {
        case EJoinProcessorResult::OK:
            return NScheduler::ETaskResult::OK;
        case EJoinProcessorResult::NEED_DATA:
            return NScheduler::ETaskResult::NEED_DATA;
        case EJoinProcessorResult::FINISHED:
            return NScheduler::ETaskResult::FINISHED;
    }
    return NScheduler::ETaskResult::FINISHED;
}

TBlockingTail MakeSortTail(
    const NQumir::NAst::TTypePtr& childType,
    const std::vector<TSortKey>& keys,
    std::vector<TSortColumnRef> keyColumns,
    TSortRadixKernel radixKernel,
    std::optional<int64_t> topLimit)
{
    if (topLimit) {
        const int64_t limit = *topLimit;
        return TBlockingTail{
            .Code = MakeDrainBlockingCode<TTopSortBlockingState>(),
            .MakeState = [childType, keys, keyColumns, radixKernel, limit]()
                -> std::shared_ptr<NScheduler::ITaskState>
            {
                return std::make_shared<TTopSortBlockingState>(
                    childType, keys, keyColumns, radixKernel, limit);
            },
            .OutputType = childType,
        };
    }
    return TBlockingTail{
        .Code = MakeDrainBlockingCode<TSortBlockingState>(),
        .MakeState = [childType, keys, keyColumns, radixKernel]()
            -> std::shared_ptr<NScheduler::ITaskState>
        {
            return std::make_shared<TSortBlockingState>(
                childType, keys, keyColumns, radixKernel);
        },
        .OutputType = childType,
    };
}

// Blocking code for a limit/offset task.
std::shared_ptr<const NScheduler::TBlockingCode> MakeLimitBlockingCode()
{
    return std::make_shared<NScheduler::TBlockingCode>(
        [](NScheduler::ITaskState& state, NScheduler::TInputPort& input, TRowSet& output) {
            auto* s = &static_cast<TLimitBlockingState&>(state);
            if (s->Processor.Finished()) {
                return NScheduler::ETaskResult::FINISHED;
            }
            TRowSet rowSet{};
            while (!s->Processor.Finished()) {
                auto fetch = input.Fetch(rowSet);
                if (fetch == NScheduler::EFetchResult::NO_DATA) {
                    return NScheduler::ETaskResult::NEED_DATA;
                }
                if (fetch == NScheduler::EFetchResult::FINISHED) {
                    return NScheduler::ETaskResult::FINISHED;
                }
                if (s->Processor.Process(rowSet, output)) {
                    return NScheduler::ETaskResult::OK;
                }
                rowSet = {};
            }
            return NScheduler::ETaskResult::FINISHED;
        });
}

} // namespace NQdb::NScheduler::NLowering
