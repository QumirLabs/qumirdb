#include <qdb/scheduler/connection.h>

#include <utility>

namespace NQdb {
namespace NScheduler {

void IConnection::SetDebugName(std::string name) {
    DebugName_ = std::move(name);
}

const std::string& IConnection::DebugName() const {
    return DebugName_;
}

void IConnection::SetStatsEnabled(bool enabled) {
    StatsEnabled_ = enabled;
}

void IConnection::SetRowLayouts(std::vector<TValueLayout> layouts) {
    RowLayouts_ = std::move(layouts);
}

std::span<const TValueLayout> IConnection::RowLayouts() const {
    return RowLayouts_;
}

TConnectionStats IConnection::Stats() const {
    return TConnectionStats{
        .Pushed = Pushed_.load(std::memory_order_relaxed),
        .Popped = Popped_.load(std::memory_order_relaxed),
        .Finished = Finished_.load(std::memory_order_relaxed),
        .BlockedPush = BlockedPush_.load(std::memory_order_relaxed),
        .EmptyFetch = EmptyFetch_.load(std::memory_order_relaxed),
        .FinishedFetch = FinishedFetch_.load(std::memory_order_relaxed),
    };
}

void IConnection::CountPushed(uint64_t count) const {
    if (!StatsEnabled_) {
        return;
    }
    Pushed_.fetch_add(count, std::memory_order_relaxed);
}

void IConnection::CountPopped(uint64_t count) const {
    if (!StatsEnabled_) {
        return;
    }
    Popped_.fetch_add(count, std::memory_order_relaxed);
}

void IConnection::CountFinished(uint64_t count) const {
    if (!StatsEnabled_) {
        return;
    }
    Finished_.fetch_add(count, std::memory_order_relaxed);
}

void IConnection::CountBlockedPush(uint64_t count) const {
    if (!StatsEnabled_) {
        return;
    }
    BlockedPush_.fetch_add(count, std::memory_order_relaxed);
}

void IConnection::CountEmptyFetch(uint64_t count) const {
    if (!StatsEnabled_) {
        return;
    }
    EmptyFetch_.fetch_add(count, std::memory_order_relaxed);
}

void IConnection::CountFinishedFetch(uint64_t count) const {
    if (!StatsEnabled_) {
        return;
    }
    FinishedFetch_.fetch_add(count, std::memory_order_relaxed);
}

} // namespace NScheduler
} // namespace NQdb
