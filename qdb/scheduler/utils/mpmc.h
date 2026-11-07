#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace NQdb {
namespace NScheduler {

template <class T>
class TMPMCQueue {
public:
    explicit TMPMCQueue(size_t capacity)
        : Capacity_(capacity)
        , Buffer_(Capacity_)
    {
        assert(Capacity_ > 0);
        for (size_t i = 0; i < Capacity_; ++i) {
            Buffer_[i].Sequence.store(i, std::memory_order_relaxed);
        }
    }

    TMPMCQueue(const TMPMCQueue&) = delete;
    TMPMCQueue& operator=(const TMPMCQueue&) = delete;

    bool TryPush(T&& value) {
        auto pos = PushPos_.load(std::memory_order_relaxed);
        for (;;) {
            auto& cell = Buffer_[pos % Capacity_];
            auto seq = cell.Sequence.load(std::memory_order_acquire);
            auto diff = static_cast<std::intptr_t>(seq) -
                static_cast<std::intptr_t>(pos);
            if (diff == 0) {
                if (PushPos_.compare_exchange_weak(
                        pos,
                        pos + 1,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed)) {
                    cell.Value.emplace(std::move(value));
                    cell.Sequence.store(pos + 1, std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) {
                return false;
            } else {
                pos = PushPos_.load(std::memory_order_relaxed);
            }
        }
    }

    bool TryPop(T& value) {
        auto pos = PopPos_.load(std::memory_order_relaxed);
        for (;;) {
            auto& cell = Buffer_[pos % Capacity_];
            auto seq = cell.Sequence.load(std::memory_order_acquire);
            auto diff = static_cast<std::intptr_t>(seq) -
                static_cast<std::intptr_t>(pos + 1);
            if (diff == 0) {
                if (PopPos_.compare_exchange_weak(
                        pos,
                        pos + 1,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed)) {
                    value = std::move(*cell.Value);
                    cell.Value.reset();
                    cell.Sequence.store(pos + Capacity_, std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) {
                return false;
            } else {
                pos = PopPos_.load(std::memory_order_relaxed);
            }
        }
    }

private:
    struct TCell {
        std::atomic<size_t> Sequence = 0;
        std::optional<T> Value;
    };

    size_t Capacity_;
    std::vector<TCell> Buffer_;
    std::atomic<size_t> PushPos_ = 0;
    std::atomic<size_t> PopPos_ = 0;
};

} // namespace NScheduler
} // namespace NQdb
