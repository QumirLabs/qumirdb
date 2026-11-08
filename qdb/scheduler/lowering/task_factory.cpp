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

std::shared_ptr<const NScheduler::TBlockingCode> MakeAggregateBlockingCode()
{
    return std::make_shared<NScheduler::TBlockingCode>(
        [](NScheduler::ITaskState& state, NScheduler::TInputPort& input, TRowSet& output) {
            auto* s = &static_cast<TAggregateBlockingState&>(state);
            if (s->Done) {
                return NScheduler::ETaskResult::FINISHED;
            }
            TRowSet rowSet{};
            while (true) {
                auto fetch = input.Fetch(rowSet);
                if (fetch == NScheduler::EFetchResult::NO_DATA) {
                    return NScheduler::ETaskResult::NEED_DATA;
                }
                if (fetch == NScheduler::EFetchResult::FINISHED) {
                    s->Done = true;
                    s->Processor.Finish(output);
                    return NScheduler::ETaskResult::OK;
                }
                s->Processor.Add(rowSet);
                Release(&rowSet);
                rowSet = {};
            }
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

std::shared_ptr<const NScheduler::TMergeCode> MakeMergeCode()
{
    return std::make_shared<NScheduler::TMergeCode>(
        [](NScheduler::ITaskState& state,
           std::vector<NScheduler::TInputPort>& inputs,
           TRowSet& output)
        {
            auto* s = &static_cast<TMergeState&>(state);
            if (!s->InputsDone) {
                bool allDone = true;
                for (size_t i = 0; i < inputs.size(); ++i) {
                    if (s->RunFinished[i]) {
                        continue;
                    }
                    for (;;) {
                        TRowSet batch{};
                        auto fetch = inputs[i].Fetch(batch);
                        if (fetch == NScheduler::EFetchResult::NO_DATA) {
                            allDone = false;
                            break;
                        }
                        if (fetch == NScheduler::EFetchResult::FINISHED) {
                            s->RunFinished[i] = true;
                            break;
                        }
                        s->Processor.Add(batch, i);
                    }
                }
                if (!allDone) {
                    return NScheduler::ETaskResult::NEED_DATA;
                }
                s->Processor.Finish();
                s->InputsDone = true;
            }
            if (s->Processor.Next(output)) {
                return NScheduler::ETaskResult::OK;
            }
            return NScheduler::ETaskResult::FINISHED;
        });
}

} // namespace NQdb::NScheduler::NLowering
