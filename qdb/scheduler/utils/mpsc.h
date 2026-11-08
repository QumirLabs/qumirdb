#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace NQdb {
namespace NScheduler {

template <class T>
class TMPSCQueue {
    // A reserved slot cannot be rolled back once other producers reserve later slots.
    static_assert(std::is_nothrow_move_constructible_v<T>);
    static_assert(std::is_nothrow_move_assignable_v<T>);

public:
    explicit TMPSCQueue(size_t capacity)
        : Capacity_(capacity)
        , Buffer_(Capacity_)
    {
        assert(Capacity_ > 0);
    }

    TMPSCQueue(const TMPSCQueue&) = delete;
    TMPSCQueue& operator=(const TMPSCQueue&) = delete;

    bool TryPush(T&& value) {
        for (;;) {
            // PopPos_ first, so pos - popPos cannot wrap around.
            const auto popPos = PopPos_.load(std::memory_order_acquire);
            auto pos = PushPos_.load(std::memory_order_relaxed);
            if (pos - popPos >= Capacity_) {
                // If the consumer moved since, popPos was stale: the queue may not be full.
                // The fence keeps this re-load from being satisfied before the relaxed PushPos_ load.
                std::atomic_thread_fence(std::memory_order_acquire);
                if (PopPos_.load(std::memory_order_acquire) == popPos) {
                    return false;
                }
                continue;
            }
            if (PushPos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                auto& cell = Buffer_[pos % Capacity_];
                cell.Value.emplace(std::move(value));
                cell.Ready.store(true, std::memory_order_release);
                return true;
            }
        }
    }

    bool TryPop(T& value) {
        auto& cell = Buffer_[ReadPos_ % Capacity_];
        // A reserved but unpublished slot must not be skipped.
        if (!cell.Ready.load(std::memory_order_acquire)) {
            return false;
        }
        value = std::move(*cell.Value);
        cell.Value.reset();
        cell.Ready.store(false, std::memory_order_relaxed);
        PopPos_.store(++ReadPos_, std::memory_order_release);
        return true;
    }

private:
    struct TCell {
        std::atomic<bool> Ready = false;
        std::optional<T> Value;
    };

    size_t Capacity_;
    std::vector<TCell> Buffer_;
    // Each counter gets its own cache line, away from the read-only fields. Fixed 128:
    // Apple M-series lines are 128 bytes; std::hardware_destructive_interference_size
    // varies with the compiler and -mcpu (64-256 on aarch64).
    alignas(128) std::atomic<size_t> PushPos_ = 0;
    alignas(128) std::atomic<size_t> PopPos_ = 0;
    size_t ReadPos_ = 0;
};

} // namespace NScheduler
} // namespace NQdb
