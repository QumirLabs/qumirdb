#include <qdb/scheduler/tasks/scan_morsels.h>

#include <qdb/scheduler/tasks/runtime_adapter.h>

#include <iterator>
#include <utility>

namespace NQdb::NScheduler {

void TScanMorselQueue::Add(std::vector<std::vector<int>> morsels)
{
    Morsels_.insert(
        Morsels_.end(), std::make_move_iterator(morsels.begin()), std::make_move_iterator(morsels.end()));
}

std::optional<std::vector<int>> TScanMorselQueue::Take()
{
    // The morsels are fixed before the graph runs; only the cursor is shared.
    const auto index = Next_.fetch_add(1, std::memory_order_relaxed);
    if (index >= Morsels_.size()) {
        return std::nullopt;
    }
    return Morsels_[index];
}

std::shared_ptr<const TSourceCode> MakeMorselSourceCode()
{
    return std::make_shared<TSourceCode>([](void* state, TRowSet& rowSet) {
        auto& lane = *static_cast<TMorselSourceState*>(state);
        while (true) {
            if (!lane.Source) {
                auto morsel = lane.Queue->Take();
                if (!morsel) {
                    return ETaskResult::FINISHED;
                }
                lane.Source = lane.Table->MakeSource(std::move(morsel), lane.Columns);
            }
            if (lane.Source->Next(rowSet)) {
                return ETaskResult::OK;
            }
            lane.Source.reset();
        }
    });
}

} // namespace NQdb::NScheduler
