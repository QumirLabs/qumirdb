#include <gtest/gtest.h>

#include <qdb/scheduler/utils/mpmc.h>
#include <qdb/scheduler/utils/spsc.h>

#include <atomic>
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
