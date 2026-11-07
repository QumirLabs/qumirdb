#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace NQdb {
namespace NScheduler {

template <class T>
class TSPSC {
public:
    explicit TSPSC(size_t capacity)
        : Capacity_(capacity + 1)
        , Buffer_(Capacity_)
    {
        assert(capacity > 0);
    }

    TSPSC(const TSPSC&) = delete;
    TSPSC& operator=(const TSPSC&) = delete;

    bool CanPush() const {
        auto tail = Tail_.load(std::memory_order_relaxed);
        auto next = Next(tail);
        auto head = Head_.load(std::memory_order_acquire);
        return next != head;
    }

    bool Empty() const {
        auto head = Head_.load(std::memory_order_relaxed);
        auto tail = Tail_.load(std::memory_order_acquire);
        return head == tail;
    }

    bool TryPush(T&& value) {
        auto tail = Tail_.load(std::memory_order_relaxed);
        auto next = Next(tail);
        if (next == Head_.load(std::memory_order_acquire)) {
            return false;
        }

        Buffer_[tail].emplace(std::move(value));
        Tail_.store(next, std::memory_order_release);
        return true;
    }

    bool TryPop(T& value) {
        auto head = Head_.load(std::memory_order_relaxed);
        if (head == Tail_.load(std::memory_order_acquire)) {
            return false;
        }

        value = std::move(*Buffer_[head]);
        Buffer_[head].reset();
        Head_.store(Next(head), std::memory_order_release);
        return true;
    }

private:
    size_t Next(size_t index) const {
        ++index;
        return (index == Capacity_)
            ? 0
            : index;
    }

    size_t Capacity_;
    std::vector<std::optional<T>> Buffer_;
    std::atomic<size_t> Head_ = 0;
    std::atomic<size_t> Tail_ = 0;
};

} // namespace NScheduler
} // namespace NQdb
