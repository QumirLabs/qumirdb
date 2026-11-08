#include <gtest/gtest.h>

#include <qdb/scheduler/utils/mpmc.h>
#include <qdb/scheduler/utils/mpsc.h>
#include <qdb/scheduler/utils/spsc.h>

#include <atomic>
#include <chrono>
#include <latch>
#include <memory>
#include <thread>
#include <vector>

using namespace NQdb::NScheduler;

namespace {

TEST(SPSC, PreservesOrder) {
    TSPSC<int> queue(3);
    EXPECT_TRUE(queue.Empty());
    EXPECT_TRUE(queue.CanPush());

    int one = 1;
    int two = 2;
    int three = 3;
    ASSERT_TRUE(queue.TryPush(std::move(one)));
    ASSERT_TRUE(queue.TryPush(std::move(two)));
    ASSERT_TRUE(queue.TryPush(std::move(three)));
    EXPECT_FALSE(queue.CanPush());

    int value = 0;
    ASSERT_TRUE(queue.TryPop(value));
    EXPECT_EQ(value, 1);
    ASSERT_TRUE(queue.TryPop(value));
    EXPECT_EQ(value, 2);
    ASSERT_TRUE(queue.TryPop(value));
    EXPECT_EQ(value, 3);
    EXPECT_TRUE(queue.Empty());
    EXPECT_FALSE(queue.TryPop(value));
}

TEST(SPSC, RejectsPushWhenFull) {
    TSPSC<int> queue(1);
    int first = 10;
    int second = 20;
    ASSERT_TRUE(queue.TryPush(std::move(first)));
    EXPECT_FALSE(queue.TryPush(std::move(second)));

    int value = 0;
    ASSERT_TRUE(queue.TryPop(value));
    EXPECT_EQ(value, 10);
    ASSERT_TRUE(queue.TryPush(std::move(second)));
    ASSERT_TRUE(queue.TryPop(value));
    EXPECT_EQ(value, 20);
}

TEST(SPSC, SupportsMoveOnlyValues) {
    TSPSC<std::unique_ptr<int>> queue(1);
    ASSERT_TRUE(queue.TryPush(std::make_unique<int>(42)));

    std::unique_ptr<int> value;
    ASSERT_TRUE(queue.TryPop(value));
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(*value, 42);
}

TEST(MPSC, PreservesCapacityAndOrderAcrossWraparound) {
    for (size_t capacity : {1, 2, 3, 7}) {
        SCOPED_TRACE(capacity);
        TMPSCQueue<int> queue(capacity);
        int value = -1;
        EXPECT_FALSE(queue.TryPop(value));
        EXPECT_EQ(value, -1);
        for (int round = 0; round < 200; ++round) {
            for (size_t i = 0; i < capacity; ++i) {
                int item = round * capacity + i;
                ASSERT_TRUE(queue.TryPush(std::move(item)));
            }
            int extra = -2;
            EXPECT_FALSE(queue.TryPush(std::move(extra)));
            for (size_t i = 0; i < capacity; ++i) {
                ASSERT_TRUE(queue.TryPop(value));
                EXPECT_EQ(value, round * capacity + i);
            }
            value = -1;
            EXPECT_FALSE(queue.TryPop(value));
            EXPECT_EQ(value, -1);
        }
    }
}

TEST(MPSC, SupportsMoveOnlyAndPreservesRejectedValues) {
    TMPSCQueue<std::unique_ptr<int>> queue(1);
    auto first = std::make_unique<int>(42);
    auto second = std::make_unique<int>(17);
    ASSERT_TRUE(queue.TryPush(std::move(first)));
    EXPECT_EQ(first, nullptr);
    EXPECT_FALSE(queue.TryPush(std::move(second)));
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(*second, 17);
    std::unique_ptr<int> value;
    ASSERT_TRUE(queue.TryPop(value));
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(*value, 42);
    ASSERT_TRUE(queue.TryPush(std::move(second)));
    ASSERT_TRUE(queue.TryPop(value));
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(*value, 17);
    EXPECT_FALSE(queue.TryPop(value));
    EXPECT_EQ(*value, 17);
}

TEST(MPSC, DestroysRemainingValues) {
    struct TDeleter {
        int* Destroyed;
        void operator()(int* value) const {
            ++*Destroyed;
            delete value;
        }
    };
    using TValue = std::unique_ptr<int, TDeleter>;
    int destroyed = 0;
    {
        TMPSCQueue<TValue> queue(2);
        ASSERT_TRUE(queue.TryPush(TValue(new int(1), TDeleter{&destroyed})));
        ASSERT_TRUE(queue.TryPush(TValue(new int(2), TDeleter{&destroyed})));
        EXPECT_EQ(destroyed, 0);
    }
    EXPECT_EQ(destroyed, 2);
}

TEST(MPSC, WaitsForReservedSlotPublication) {
    struct TValue {
        int Value;
        std::latch* Reserved = nullptr;
        std::latch* Publish = nullptr;

        explicit TValue(int value, std::latch* reserved = nullptr, std::latch* publish = nullptr)
            : Value(value)
            , Reserved(reserved)
            , Publish(publish)
        {}

        TValue(TValue&& other) noexcept
            : Value(other.Value)
        {
            if (other.Reserved) {
                other.Reserved->count_down();
                other.Publish->wait();
            }
            other.Value = -1;
        }

        TValue& operator=(TValue&& other) noexcept {
            Value = std::exchange(other.Value, -1);
            return *this;
        }
    };
    TMPSCQueue<TValue> queue(2);
    std::latch reserved(1);
    std::latch publish(1);
    TValue first(10, &reserved, &publish);
    bool firstPushed = false;
    std::thread producer([&] { firstPushed = queue.TryPush(std::move(first)); });
    reserved.wait();
    TValue second(20);
    const bool secondPushed = queue.TryPush(std::move(second));
    TValue extra(30);
    const bool extraPushed = queue.TryPush(std::move(extra));
    TValue value(-1);
    const bool poppedBeforePublication = queue.TryPop(value);
    publish.count_down();
    producer.join();

    ASSERT_TRUE(firstPushed);
    ASSERT_TRUE(secondPushed);
    EXPECT_FALSE(extraPushed);
    EXPECT_EQ(extra.Value, 30);
    EXPECT_FALSE(poppedBeforePublication);
    EXPECT_EQ(value.Value, -1);
    ASSERT_TRUE(queue.TryPop(value));
    EXPECT_EQ(value.Value, 10);
    ASSERT_TRUE(queue.TryPop(value));
    EXPECT_EQ(value.Value, 20);
    EXPECT_FALSE(queue.TryPop(value));
}

TEST(MPSC, RejectsPushOnlyWhenFull) {
    constexpr int producerCount = 8;
    constexpr int itemsPerProducer = 100000;
    constexpr int itemCount = producerCount * itemsPerProducer;
    // The queue never fills up, so every rejected push is spurious.
    TMPSCQueue<int> queue(itemCount);
    std::latch start(1);
    std::atomic<int> rejected = 0;
    std::vector<std::thread> producers;
    for (int producer = 0; producer < producerCount; ++producer) {
        producers.emplace_back([&] {
            start.wait();
            for (int i = 0; i < itemsPerProducer; ++i) {
                int item = i;
                while (!queue.TryPush(std::move(item))) {
                    rejected.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    start.count_down();
    int consumed = 0;
    int value = 0;
    while (consumed < itemCount) {
        if (queue.TryPop(value)) {
            ++consumed;
        }
    }
    for (auto& producer : producers) {
        producer.join();
    }
    EXPECT_EQ(rejected.load(), 0);
}

TEST(MPSC, TransfersAllItemsWithMultipleProducersInProducerOrder) {
    struct TItem {
        int Producer;
        int Index;
    };
    constexpr int producerCount = 4;
    constexpr int itemsPerProducer = 1000;
    constexpr int itemCount = producerCount * itemsPerProducer;
    for (size_t capacity : {1, 3, 32}) {
        SCOPED_TRACE(capacity);
        TMPSCQueue<TItem> queue(capacity);
        std::latch start(1);
        std::atomic<bool> stop = false;
        std::vector<std::thread> producers;
        for (int producer = 0; producer < producerCount; ++producer) {
            producers.emplace_back([&, producer] {
                start.wait();
                for (int i = 0; i < itemsPerProducer; ++i) {
                    TItem item{producer, i};
                    while (!queue.TryPush(std::move(item))) {
                        if (stop.load(std::memory_order_relaxed)) {
                            return;
                        }
                        std::this_thread::yield();
                    }
                }
            });
        }
        start.count_down();
        // Detects a stall, not slowness: with capacity 1 every item is a handoff, and on
        // a loaded machine each yield gives a whole time slice to another process.
        constexpr auto stallTimeout = std::chrono::seconds(10);
        auto deadline = std::chrono::steady_clock::now() + stallTimeout;
        std::vector<int> next(producerCount, 0);
        int consumed = 0;
        while (consumed < itemCount && std::chrono::steady_clock::now() < deadline) {
            TItem item{-1, -1};
            if (!queue.TryPop(item)) {
                std::this_thread::yield();
                continue;
            }
            EXPECT_GE(item.Producer, 0);
            EXPECT_LT(item.Producer, producerCount);
            if (item.Producer >= 0 && item.Producer < producerCount) {
                EXPECT_EQ(item.Index, next[item.Producer]++);
            }
            ++consumed;
            deadline = std::chrono::steady_clock::now() + stallTimeout;
        }
        stop.store(true, std::memory_order_relaxed);
        for (auto& producer : producers) {
            producer.join();
        }
        ASSERT_EQ(consumed, itemCount);
        for (const auto count : next) {
            EXPECT_EQ(count, itemsPerProducer);
        }
        TItem item{-1, -1};
        EXPECT_FALSE(queue.TryPop(item));
    }
}

TEST(MPMC, PreservesSingleProducerOrder) {
    TMPMCQueue<int> queue(4);
    for (int i = 0; i < 4; ++i) {
        int value = i;
        ASSERT_TRUE(queue.TryPush(std::move(value)));
    }

    int extra = 100;
    EXPECT_FALSE(queue.TryPush(std::move(extra)));

    for (int i = 0; i < 4; ++i) {
        int value = -1;
        ASSERT_TRUE(queue.TryPop(value));
        EXPECT_EQ(value, i);
    }

    int value = -1;
    EXPECT_FALSE(queue.TryPop(value));
}

TEST(MPMC, TransfersAllItemsWithMultipleProducersAndConsumers) {
    constexpr int producerCount = 4;
    constexpr int consumerCount = 4;
    constexpr int itemsPerProducer = 1000;
    constexpr int itemCount = producerCount * itemsPerProducer;

    TMPMCQueue<int> queue(32);
    std::atomic<int> consumed = 0;
    std::vector<std::atomic<int>> seen(itemCount);
    std::vector<std::thread> threads;

    for (int producer = 0; producer < producerCount; ++producer) {
        threads.emplace_back([producer, &queue]() {
            for (int i = 0; i < itemsPerProducer; ++i) {
                int value = producer * itemsPerProducer + i;
                while (!queue.TryPush(std::move(value))) {
                    std::this_thread::yield();
                }
            }
        });
    }

    for (int consumer = 0; consumer < consumerCount; ++consumer) {
        threads.emplace_back([&queue, &consumed, &seen, itemCount]() {
            while (consumed.load(std::memory_order_acquire) < itemCount) {
                int value = -1;
                if (!queue.TryPop(value)) {
                    std::this_thread::yield();
                    continue;
                }
                ASSERT_GE(value, 0);
                ASSERT_LT(value, itemCount);
                seen[value].fetch_add(1, std::memory_order_relaxed);
                consumed.fetch_add(1, std::memory_order_release);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(consumed.load(std::memory_order_acquire), itemCount);
    for (int i = 0; i < itemCount; ++i) {
        EXPECT_EQ(seen[i].load(std::memory_order_relaxed), 1) << i;
    }
}

} // namespace

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
