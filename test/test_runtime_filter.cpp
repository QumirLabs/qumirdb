#include <gtest/gtest.h>

#include <qdb/exec/runtime_filter.h>

#include <random>
#include <thread>
#include <vector>

using namespace NQdb;

namespace {

std::vector<uint64_t> Hashes(size_t count, uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::vector<uint64_t> out;
    out.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        out.push_back(rng());
    }
    return out;
}

} // namespace

TEST(RuntimeFilter, NeverRejectsAKeyItWasGiven) {
    TRuntimeFilter filter;
    auto keys = Hashes(5000, 1);
    auto builder = filter.MakeBuilder();
    for (uint64_t key : keys) {
        builder.Add(key);
    }
    filter.Merge(std::move(builder));
    filter.Publish();

    ASSERT_TRUE(filter.Ready());
    EXPECT_FALSE(filter.ExactSetDisabled());
    for (uint64_t key : keys) {
        EXPECT_TRUE(filter.MayContain(key)) << key;
    }
}

TEST(RuntimeFilter, HandlesZeroHash) {
    TRuntimeFilter filter;
    auto builder = filter.MakeBuilder();
    builder.Add(0);
    builder.Add(7);
    filter.Merge(std::move(builder));
    filter.Publish();

    EXPECT_TRUE(filter.MayContain(0));
    EXPECT_TRUE(filter.MayContain(7));
    EXPECT_FALSE(filter.MayContain(9));
}

TEST(RuntimeFilter, RejectsKeysItWasNotGiven) {
    TRuntimeFilter filter;
    auto builder = filter.MakeBuilder();
    for (uint64_t key : Hashes(1000, 2)) {
        builder.Add(key);
    }
    filter.Merge(std::move(builder));
    filter.Publish();

    size_t passed = 0;
    for (uint64_t probe : Hashes(10000, 99)) {
        passed += filter.MayContain(probe) ? 1 : 0;
    }
    EXPECT_LT(passed, 10u) << passed;
}

TEST(RuntimeFilter, PassesEverythingWhenItCannotAnswer) {
    TRuntimeFilter unpublished;
    EXPECT_FALSE(unpublished.Ready());
    EXPECT_TRUE(unpublished.MayContain(12345));
    EXPECT_TRUE(unpublished.MayContainKey(12345));

    TRuntimeFilter overflowed(/*maxKeys=*/8);
    auto builder = overflowed.MakeBuilder();
    for (uint64_t key : Hashes(64, 3)) {
        builder.Add(key);
    }
    EXPECT_TRUE(builder.Overflowed());
    overflowed.Merge(std::move(builder));
    EXPECT_TRUE(overflowed.ExactSetDisabled());
    EXPECT_FALSE(overflowed.Ready());
    overflowed.Publish();
    EXPECT_TRUE(overflowed.ExactSetDisabled());
    EXPECT_TRUE(overflowed.MayContain(12345));
}

TEST(RuntimeFilter, MergedOverflowDisables) {
    TRuntimeFilter filter(/*maxKeys=*/16);
    for (uint64_t seed = 0; seed < 4; ++seed) {
        auto builder = filter.MakeBuilder();
        for (uint64_t key : Hashes(8, seed + 10)) {
            builder.Add(key);
        }
        EXPECT_FALSE(builder.Overflowed());
        filter.Merge(std::move(builder));
    }
    filter.Publish();
    EXPECT_TRUE(filter.ExactSetDisabled());
    EXPECT_TRUE(filter.MayContain(999));
}

TEST(RuntimeFilter, MergesKeysAndBoundsFromEveryBuilder) {
    TRuntimeFilter filter;
    std::vector<std::vector<uint64_t>> parts;
    for (uint64_t seed = 0; seed < 4; ++seed) {
        parts.push_back(Hashes(100, seed + 20));
        auto builder = filter.MakeBuilder();
        for (uint64_t key : parts.back()) {
            builder.Add(key);
        }
        builder.AddBound(static_cast<int64_t>(seed) * 10);
        builder.AddBound(static_cast<int64_t>(seed) * 10 + 5);
        filter.Merge(std::move(builder));
    }
    filter.Publish();

    for (const auto& part : parts) {
        for (uint64_t key : part) {
            EXPECT_TRUE(filter.MayContain(key));
        }
    }
    auto bounds = filter.Bounds();
    ASSERT_TRUE(bounds.has_value());
    EXPECT_EQ(bounds->first, 0);
    EXPECT_EQ(bounds->second, 35);
    EXPECT_TRUE(filter.MayContainKey(0));
    EXPECT_TRUE(filter.MayContainKey(35));
    EXPECT_FALSE(filter.MayContainKey(-1));
    EXPECT_FALSE(filter.MayContainKey(36));
}

TEST(RuntimeFilter, OneBuilderWithoutBoundsDropsThemAll) {
    TRuntimeFilter filter;
    auto withBounds = filter.MakeBuilder();
    withBounds.Add(1);
    withBounds.AddBound(10);
    filter.Merge(std::move(withBounds));

    auto without = filter.MakeBuilder();
    without.Add(2);
    without.DropBounds();
    filter.Merge(std::move(without));
    filter.Publish();

    EXPECT_FALSE(filter.Bounds().has_value());
    EXPECT_TRUE(filter.MayContainKey(-1000));
    EXPECT_TRUE(filter.MayContain(1));
    EXPECT_TRUE(filter.MayContain(2));
}

TEST(RuntimeFilter, MergesConcurrently) {
    TRuntimeFilter filter;
    constexpr size_t Threads = 8;
    constexpr size_t PerThread = 500;

    std::vector<std::thread> workers;
    for (size_t t = 0; t < Threads; ++t) {
        workers.emplace_back([&filter, t] {
            auto builder = filter.MakeBuilder();
            for (uint64_t key : Hashes(PerThread, t + 100)) {
                builder.Add(key);
            }
            builder.AddBound(static_cast<int64_t>(t));
            filter.Merge(std::move(builder));
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    filter.Publish();

    ASSERT_FALSE(filter.ExactSetDisabled());
    EXPECT_EQ(filter.KeyCount(), Threads * PerThread);
    for (size_t t = 0; t < Threads; ++t) {
        for (uint64_t key : Hashes(PerThread, t + 100)) {
            EXPECT_TRUE(filter.MayContain(key));
        }
    }
    auto bounds = filter.Bounds();
    ASSERT_TRUE(bounds.has_value());
    EXPECT_EQ(bounds->first, 0);
    EXPECT_EQ(bounds->second, static_cast<int64_t>(Threads) - 1);
}

TEST(RuntimeFilter, RepeatsAtTheCapDoNotResortEveryRow) {
    constexpr size_t Cap = 4096;
    TRuntimeFilter filter(Cap);
    auto keys = Hashes(Cap, 11);
    auto builder = filter.MakeBuilder();
    for (uint64_t key : keys) {
        builder.Add(key);
    }
    for (int repeat = 0; repeat < 200; ++repeat) {
        for (uint64_t key : keys) {
            builder.Add(key);
        }
    }
    EXPECT_FALSE(builder.Overflowed());
    filter.Merge(std::move(builder));
    filter.Publish();

    ASSERT_FALSE(filter.ExactSetDisabled());
    EXPECT_EQ(filter.KeyCount(), Cap);
    for (uint64_t key : keys) {
        EXPECT_TRUE(filter.MayContain(key));
    }
}

TEST(RuntimeFilter, OverflowKeepsBounds) {
    TRuntimeFilter filter(/*maxKeys=*/8);
    auto builder = filter.MakeBuilder();
    for (uint64_t key : Hashes(64, 12)) {
        builder.Add(key);
    }
    builder.AddBound(100);
    builder.AddBound(200);
    EXPECT_TRUE(builder.Overflowed());
    filter.Merge(std::move(builder));
    filter.Publish();

    ASSERT_TRUE(filter.ExactSetDisabled());
    EXPECT_TRUE(filter.MayContain(12345)) << "the key set is gone";
    auto bounds = filter.Bounds();
    ASSERT_TRUE(bounds.has_value()) << "bounds outlive the key set";
    EXPECT_EQ(bounds->first, 100);
    EXPECT_EQ(bounds->second, 200);
    EXPECT_TRUE(filter.MayContainKey(150));
    EXPECT_FALSE(filter.MayContainKey(99));
    EXPECT_FALSE(filter.MayContainKey(201));
}

TEST(RuntimeFilter, CapIsEnforcedAtPublication) {
    constexpr size_t Cap = 8;
    for (size_t distinct : {Cap, Cap + 1}) {
        TRuntimeFilter filter(Cap);
        auto keys = Hashes(distinct, 13);
        auto builder = filter.MakeBuilder();
        for (uint64_t key : keys) {
            builder.Add(key);
        }
        EXPECT_FALSE(builder.Overflowed()) << distinct;
        filter.Merge(std::move(builder));
        filter.Publish();

        if (distinct > Cap) {
            EXPECT_TRUE(filter.ExactSetDisabled()) << distinct;
            EXPECT_EQ(filter.KeyCount(), 0u) << distinct;
            EXPECT_TRUE(filter.MayContain(keys.front())) << distinct;
            EXPECT_TRUE(filter.MayContain(~keys.front())) << distinct;
        } else {
            EXPECT_FALSE(filter.ExactSetDisabled()) << distinct;
            EXPECT_EQ(filter.KeyCount(), Cap) << distinct;
            for (uint64_t key : keys) {
                EXPECT_TRUE(filter.MayContain(key)) << distinct;
            }
        }
    }
}

TEST(RuntimeFilter, CapIsEnforcedAcrossBuildersAtPublication) {
    constexpr size_t Cap = 8;
    TRuntimeFilter filter(Cap);
    for (uint64_t producer = 0; producer < 3; ++producer) {
        auto builder = filter.MakeBuilder();
        for (uint64_t key : Hashes(3, producer + 30)) {
            builder.Add(key);
        }
        EXPECT_FALSE(builder.Overflowed());
        filter.Merge(std::move(builder));
    }
    filter.Publish();
    EXPECT_TRUE(filter.ExactSetDisabled());
    EXPECT_EQ(filter.KeyCount(), 0u);
}

TEST(RuntimeFilter, RepeatedKeysDoNotConsumeTheCap) {
    TRuntimeFilter filter(/*maxKeys=*/64);
    auto keys = Hashes(50, 7);
    auto builder = filter.MakeBuilder();
    for (int repeat = 0; repeat < 1000; ++repeat) {
        for (uint64_t key : keys) {
            builder.Add(key);
        }
    }
    EXPECT_FALSE(builder.Overflowed());
    filter.Merge(std::move(builder));
    filter.Publish();

    ASSERT_FALSE(filter.ExactSetDisabled());
    EXPECT_EQ(filter.KeyCount(), keys.size());
    for (uint64_t key : keys) {
        EXPECT_TRUE(filter.MayContain(key));
    }
}

TEST(RuntimeFilter, RepeatedKeysAcrossBuildersDoNotConsumeTheCap) {
    TRuntimeFilter filter(/*maxKeys=*/64);
    auto keys = Hashes(50, 8);
    for (int producer = 0; producer < 20; ++producer) {
        auto builder = filter.MakeBuilder();
        for (uint64_t key : keys) {
            builder.Add(key);
        }
        filter.Merge(std::move(builder));
    }
    filter.Publish();

    ASSERT_FALSE(filter.ExactSetDisabled());
    EXPECT_EQ(filter.KeyCount(), keys.size());
}

TEST(RuntimeFilter, DeduplicatesRepeatedKeys) {
    TRuntimeFilter filter;
    auto builder = filter.MakeBuilder();
    for (int i = 0; i < 100; ++i) {
        builder.Add(42);
    }
    filter.Merge(std::move(builder));
    filter.Publish();
    EXPECT_EQ(filter.KeyCount(), 1u);
    EXPECT_TRUE(filter.MayContain(42));
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
