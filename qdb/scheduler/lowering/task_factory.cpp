#include <qdb/scheduler/lowering/task_factory.h>

#include <algorithm>
#include <stdexcept>

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

std::shared_ptr<const NScheduler::TBlockingCode> MakePartialAggregateBlockingCode()
{
    return std::make_shared<NScheduler::TBlockingCode>(
        [](NScheduler::ITaskState& state, NScheduler::TInputPort& input, TRowSet& output) {
            auto* s = &static_cast<TPartialAggregateBlockingState&>(state);
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
                    return s->Processor.Finish(output)
                        ? NScheduler::ETaskResult::OK
                        : NScheduler::ETaskResult::FINISHED;
                }
                s->Processor.Add(rowSet);
                if (s->Processor.Next(output)) {
                    return NScheduler::ETaskResult::OK;
                }
            }
        });
}

std::shared_ptr<const NScheduler::TBinaryBlockingCode> MakeWindowRangeCode()
{
    auto code = std::make_shared<NScheduler::TBinaryBlockingCode>(
        [](NScheduler::ITaskState& state,
           NScheduler::TInputPort& carries,
           NScheduler::TInputPort& rows,
           TRowSet& output)
        {
            auto* s = &static_cast<TWindowRangeBlockingState&>(state);
            TRowSet rowSet{};
            // A route lane sends its carries after all its rows, so read rows first.
            while (!s->RowsFinished) {
                auto fetch = rows.Fetch(rowSet);
                if (fetch == NScheduler::EFetchResult::NO_DATA) {
                    return NScheduler::ETaskResult::NEED_DATA;
                }
                if (fetch == NScheduler::EFetchResult::FINISHED) {
                    s->RowsFinished = true;
                    break;
                }
                s->Processor.Add(rowSet);
                rowSet = {};
            }
            while (!s->CarriesFinished) {
                auto fetch = carries.Fetch(rowSet);
                if (fetch == NScheduler::EFetchResult::NO_DATA) {
                    return NScheduler::ETaskResult::NEED_DATA;
                }
                if (fetch == NScheduler::EFetchResult::FINISHED) {
                    s->CarriesFinished = true;
                    break;
                }
                s->Processor.AddCarry(rowSet);
                rowSet = {};
            }
            if (s->Processor.Next(output)) {
                return NScheduler::ETaskResult::OK;
            }
            return NScheduler::ETaskResult::FINISHED;
        });
    code->RequiredSide = [](NScheduler::ITaskState& state) {
        return static_cast<TWindowRangeBlockingState&>(state).RowsFinished
            ? NScheduler::ERequiredSide::Left
            : NScheduler::ERequiredSide::Right;
    };
    return code;
}

std::shared_ptr<const NScheduler::TFanOutCode> MakeWindowSampleCode()
{
    using TEmit = NScheduler::TFanOutCode::TEmit;
    static constexpr size_t RowsOutput = 0;
    static constexpr size_t SampleOutput = 1;
    return std::make_shared<NScheduler::TFanOutCode>(
        [](NScheduler::ITaskState& state, std::span<NScheduler::TInputPort> inputs, TEmit& emit) {
            auto* s = &static_cast<TWindowSampleState&>(state);
            TRowSet rowSet{};
            while (!s->InputFinished) {
                auto fetch = inputs[0].Fetch(rowSet);
                if (fetch == NScheduler::EFetchResult::NO_DATA) {
                    return NScheduler::ETaskResult::NEED_DATA;
                }
                if (fetch == NScheduler::EFetchResult::FINISHED) {
                    s->InputFinished = true;
                    break;
                }
                s->Processor.Add(rowSet);
                rowSet = {};
            }
            // The sample goes first and its output finishes before any row is
            // sent: the splitters need every lane's sample, and the route lane
            // reads the rows only after the splitters.
            if (!s->SampleSent) {
                s->SampleSent = true;
                if (s->Processor.Sample(emit.RowSet)) {
                    emit.Output = SampleOutput;
                    return NScheduler::ETaskResult::OK;
                }
            }
            if (!s->SampleFinished) {
                s->SampleFinished = true;
                emit.Output = SampleOutput;
                emit.Finish = true;
                return NScheduler::ETaskResult::OK;
            }
            if (s->Processor.Next(emit.RowSet)) {
                emit.Output = RowsOutput;
                return NScheduler::ETaskResult::OK;
            }
            return NScheduler::ETaskResult::FINISHED;
        },
        [](NScheduler::ITaskState&) -> std::optional<size_t> {
            return 0;
        });
}

std::shared_ptr<const NScheduler::TFanOutCode> MakeWindowRouteCode()
{
    using TEmit = NScheduler::TFanOutCode::TEmit;
    static constexpr size_t SampleInput = 0;
    static constexpr size_t RowsInput = 1;
    return std::make_shared<NScheduler::TFanOutCode>(
        [](NScheduler::ITaskState& state, std::span<NScheduler::TInputPort> inputs, TEmit& emit) {
            auto* s = &static_cast<TWindowRouteState&>(state);
            TRowSet rowSet{};
            while (!s->SampleFinished) {
                auto fetch = inputs[SampleInput].Fetch(rowSet);
                if (fetch == NScheduler::EFetchResult::NO_DATA) {
                    return NScheduler::ETaskResult::NEED_DATA;
                }
                if (fetch == NScheduler::EFetchResult::FINISHED) {
                    s->SampleFinished = true;
                    break;
                }
                s->Processor.AddSample(rowSet);
                rowSet = {};
            }
            while (s->Pending.empty() && !s->RowsFinished) {
                auto fetch = inputs[RowsInput].Fetch(rowSet);
                if (fetch == NScheduler::EFetchResult::NO_DATA) {
                    return NScheduler::ETaskResult::NEED_DATA;
                }
                if (fetch == NScheduler::EFetchResult::FINISHED) {
                    s->RowsFinished = true;
                    break;
                }
                s->Processor.Route(rowSet, s->Pending);
                std::ranges::reverse(s->Pending);
            }
            if (!s->Pending.empty()) {
                emit.Output = s->Pending.back().first;
                emit.RowSet = s->Pending.back().second;
                s->Pending.pop_back();
                return NScheduler::ETaskResult::OK;
            }
            // Every range has all rows of this lane before its carry, so the
            // range task can read its rows to the end first.
            if (s->NextRowOutput < s->Ranges) {
                emit.Output = s->NextRowOutput++;
                emit.Finish = true;
                return NScheduler::ETaskResult::OK;
            }
            while (s->NextCarry < s->Ranges) {
                const size_t range = s->NextCarry++;
                if (s->Processor.Carry(range, emit.RowSet)) {
                    emit.Output = s->Ranges + range;
                    return NScheduler::ETaskResult::OK;
                }
            }
            return NScheduler::ETaskResult::FINISHED;
        },
        [](NScheduler::ITaskState& state) -> std::optional<size_t> {
            return static_cast<TWindowRouteState&>(state).SampleFinished ? RowsInput : SampleInput;
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

std::shared_ptr<const NScheduler::TMergeCode> MakeLateMergeCode()
{
    return std::make_shared<NScheduler::TMergeCode>(
        [](NScheduler::ITaskState& state,
           std::vector<NScheduler::TInputPort>& inputs,
           TRowSet& output)
        {
            auto* merge = &static_cast<TLateMaterializeMergeState&>(state);
            if (merge->Emitted) {
                return NScheduler::ETaskResult::FINISHED;
            }

            for (size_t i = 0; i < inputs.size(); ++i) {
                if (merge->Received[i] || merge->Finished[i]) {
                    continue;
                }
                TRowSet partition{};
                switch (inputs[i].Fetch(partition)) {
                    case NScheduler::EFetchResult::OK:
                        merge->Partitions[i] = partition;
                        merge->Received[i] = true;
                        break;
                    case NScheduler::EFetchResult::NO_DATA:
                        break;
                    case NScheduler::EFetchResult::FINISHED:
                        merge->Finished[i] = true;
                        break;
                }
            }

            const bool allReceived = std::ranges::all_of(
                merge->Received, [](bool value) { return value; });
            if (allReceived) {
                MergeLateMaterializedColumns(merge->Partitions, output);
                std::ranges::fill(merge->Received, false);
                merge->Emitted = true;
                return NScheduler::ETaskResult::OK;
            }

            const bool allFinished = std::ranges::all_of(
                merge->Finished, [](bool value) { return value; });
            const bool anyReceived = std::ranges::any_of(
                merge->Received, [](bool value) { return value; });
            const bool anyFinished = std::ranges::any_of(
                merge->Finished, [](bool value) { return value; });
            if (allFinished && !anyReceived) {
                return NScheduler::ETaskResult::FINISHED;
            }
            if (anyReceived && anyFinished) {
                throw std::runtime_error(
                    "late materialize: lookup partitions returned "
                    "different result counts");
            }
            return NScheduler::ETaskResult::NEED_DATA;
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
