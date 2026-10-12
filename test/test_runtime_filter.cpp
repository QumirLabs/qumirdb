#include <gtest/gtest.h>

#include <qdb/exec/runtime_filter.h>
#include <qdb/exec/runtime_filter_vector.h>
#include <qdb/exec/runtime_context.h>
#include <qdb/kernel/compiler.h>
#include <qdb/kernel/finalize.h>

#include <qumir/codegen/llvm/llvm_initializer.h>

#include <algorithm>
#include <atomic>
#include <random>
#include <thread>
#include <utility>
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

size_t ProbeBatch(IRuntimeFilterProbe& filter, TRuntimeFilterProbeState& state,
    std::span<const uint64_t> hashes, std::span<uint8_t> output,
    std::span<const uint8_t> selection = {})
{
    const size_t rows = selection.empty() ? hashes.size() : static_cast<size_t>(
        std::count_if(selection.begin(), selection.end(), [](uint8_t value) { return value != 0; }));
    if (filter.BeginBatch(state, rows)) {
        return filter.MayContainBatch(hashes, output, state, selection);
    }
    for (size_t i = 0; i < output.size(); ++i) {
        output[i] = selection.empty() || selection[i] ? 0xff : 0;
    }
    return rows;
}

} // namespace

TEST(RuntimeFilter, NeverRejectsAKeyItWasGiven) {
    TRuntimeFilter filter;
    auto keys = Hashes(5000, 1);
    auto builder = filter.MakeBuilder();
    for (uint64_t key : keys) {
        builder.Add(key);
    }
    filter.FinishProducer(std::move(builder).TakePartial());

    ASSERT_TRUE(filter.Ready());
    EXPECT_FALSE(filter.UsesBloom());
    for (uint64_t key : keys) {
        EXPECT_TRUE(filter.MayContain(key)) << key;
    }
}

TEST(RuntimeFilter, HandlesZeroHash) {
    TRuntimeFilter filter;
    auto builder = filter.MakeBuilder();
    builder.Add(0);
    builder.Add(7);
    filter.FinishProducer(std::move(builder).TakePartial());

    EXPECT_TRUE(filter.MayContain(0));
    EXPECT_TRUE(filter.MayContain(7));
    EXPECT_FALSE(filter.MayContain(9));
}

TEST(RuntimeFilter, RegistersProbeStatesFromConcurrentLanes) {
    TRuntimeFilter filter;
    constexpr size_t Lanes = 16;
    constexpr size_t StatesPerLane = 64;
    std::atomic<bool> start{false};
    std::vector<std::thread> lanes;
    lanes.reserve(Lanes);
    for (size_t lane = 0; lane < Lanes; ++lane) {
        lanes.emplace_back([&] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (size_t i = 0; i < StatesPerLane; ++i) {
                auto state = filter.CreateProbeState();
                filter.BeginBatch(*state, 1);
            }
        });
    }
    start.store(true, std::memory_order_release);
    for (auto& lane : lanes) {
        lane.join();
    }
    EXPECT_EQ(filter.StatsSnapshot().ProbeRows, Lanes * StatesPerLane);
}

TEST(RuntimeFilter, BatchMatchesScalarForExactBloomAndUnpublished) {
    auto keys = Hashes(200, 21);
    auto probes = Hashes(10000, 22);
    probes.insert(probes.end(), keys.begin(), keys.end());
    probes.push_back(0);

    auto check = [&](TRuntimeFilter& filter) {
        std::vector<uint8_t> output(probes.size());
        auto state = filter.CreateProbeState();
        ProbeBatch(filter, *state, probes, output);
        for (size_t i = 0; i < probes.size(); ++i) {
            EXPECT_EQ(output[i] != 0, filter.MayContain(probes[i])) << i;
        }
    };

    TRuntimeFilter exact(/*maxKeys=*/256, /*bloomBytes=*/512);
    check(exact);
    auto exactBuilder = exact.MakeBuilder();
    for (uint64_t key : keys) {
        exactBuilder.Add(key);
    }
    exact.FinishProducer(std::move(exactBuilder).TakePartial());

    ASSERT_FALSE(exact.UsesBloom());
    check(exact);

    TRuntimeFilter bloom(/*maxKeys=*/8, /*bloomBytes=*/512);
    auto bloomBuilder = bloom.MakeBuilder();
    for (uint64_t key : keys) {
        bloomBuilder.Add(key);
    }
    bloom.FinishProducer(std::move(bloomBuilder).TakePartial());

    ASSERT_TRUE(bloom.UsesBloom());
    check(bloom);
}

TEST(RuntimeFilter, QumirVectorBloomKernel) {
    auto context = std::make_shared<TRuntimeContext>();
    {
        std::vector<TGeneratedKernel> kernels;
        TKernelCompiler compiler(TKernelCompilerOptions{
            .Sink = &kernels,
            .BindNow = false,
            .RuntimeContext = context,
        });
        context->BloomProbe = compiler.CompileBloomProbe();
        ASSERT_EQ(kernels.size(), 1u);
        EXPECT_FALSE(kernels.front().Slot->Runner);
        JitFinalizeKernels(kernels);
    }

    auto probes = Hashes(8195, 23);
    std::vector<uint8_t> output(probes.size(), 7);
    std::vector<uint64_t> words(64, ~uint64_t{0});

    const size_t processed = ProbeBloomVectorBatch(*context->BloomProbe, words, probes, output);
    ASSERT_EQ(processed, 8192u);
    EXPECT_TRUE(std::all_of(output.begin(), output.begin() + processed,
        [](uint8_t value) { return value != 0; }));
    EXPECT_EQ(output.back(), 7);

    std::fill(words.begin(), words.end(), 0);
    ProbeBloomVectorBatch(*context->BloomProbe, words, probes, output);
    EXPECT_TRUE(std::all_of(output.begin(), output.begin() + processed,
        [](uint8_t value) { return value == 0; }));
}

TEST(RuntimeFilter, BloomBatchMatchesScalarWithQueryKernel) {
    auto context = std::make_shared<TRuntimeContext>();
    context->BloomProbe = TKernelCompiler().CompileBloomProbe();
    TRuntimeFilter filter(/*maxKeys=*/8, /*bloomBytes=*/2048, context);
    auto keys = Hashes(2000, 24);
    auto builder = filter.MakeBuilder();
    for (uint64_t key : keys) {
        builder.Add(key);
    }
    filter.FinishProducer(std::move(builder).TakePartial());

    ASSERT_TRUE(filter.UsesBloom());

    auto probes = Hashes(16'387, 25);
    for (size_t i = 0; i < probes.size(); i += 1000) {
        probes[i] = keys[i % keys.size()];
    }
    std::vector<uint8_t> output(probes.size());
    auto state = filter.CreateProbeState();
    ProbeBatch(filter, *state, probes, output);
    for (size_t i = 0; i < probes.size(); ++i) {
        ASSERT_EQ(output[i] != 0, filter.MayContain(probes[i])) << i;
    }
    std::vector<uint8_t> selection(probes.size(), 0xff);
    for (size_t i = 0; i < probes.size(); i += 3) {
        selection[i] = 0;
    }
    ProbeBatch(filter, *state, probes, output, selection);
    for (size_t i = 0; i < probes.size(); ++i) {
        ASSERT_EQ(output[i] != 0, selection[i] && filter.MayContain(probes[i])) << i;
    }
}

TEST(RuntimeFilter, BatchRespectsSelectionAndCountsOnlySelectedRows) {
    for (size_t cap : {size_t{8}, size_t{256}}) {
        TRuntimeFilter filter(cap, /*bloomBytes=*/512);
        auto state = filter.CreateProbeState();
        auto keys = Hashes(200, 26);
        auto builder = filter.MakeBuilder();
        for (uint64_t key : keys) {
            builder.Add(key);
        }
        filter.FinishProducer(std::move(builder).TakePartial());

        auto probes = Hashes(1000, 27);
        std::vector<uint8_t> selection(probes.size(), 0);
        std::vector<uint8_t> output(probes.size(), 7);
        size_t accepted = 0;
        size_t selected = 0;
        for (size_t i = 0; i < probes.size(); i += 3) {
            selection[i] = 0xff;
            ++selected;
            if (i % 2 == 0) {
                probes[i] = keys[i % keys.size()];
            }
            accepted += filter.MayContain(probes[i]);
        }
        EXPECT_EQ(ProbeBatch(filter, *state, probes, output, selection), accepted);
        for (size_t i = 0; i < probes.size(); ++i) {
            EXPECT_EQ(output[i] != 0, selection[i] && filter.MayContain(probes[i])) << i;
        }
        const auto stats = filter.StatsSnapshot();
        EXPECT_EQ(stats.ProbeRows, selected);
        EXPECT_EQ(stats.RejectedRows, selected - accepted);
    }
}

TEST(RuntimeFilter, WeakBloomPausesAndRechecks) {
    TRuntimeFilter filter(/*maxKeys=*/8, /*bloomBytes=*/512);
    auto keys = Hashes(200, 31);
    auto builder = filter.MakeBuilder();
    for (uint64_t key : keys) {
        builder.Add(key);
    }
    filter.FinishProducer(std::move(builder).TakePartial());

    ASSERT_TRUE(filter.UsesBloom());

    std::vector<uint64_t> matching(8192, keys[0]);
    std::vector<uint8_t> output(matching.size());
    auto state = filter.CreateProbeState();
    auto otherLane = filter.CreateProbeState();
    for (size_t batch = 0; batch < 6; ++batch) {
        ProbeBatch(filter, *state, matching, output);
    }
    // A weak sample on one lane must not pause another lane's probe.
    EXPECT_TRUE(filter.BeginBatch(*otherLane, matching.size()));
    filter.MayContainBatch(matching, output, *otherLane);
    for (size_t batch = 0; batch < 4; ++batch) {
        ProbeBatch(filter, *state, matching, output);
    }
    auto stats = filter.StatsSnapshot();
    EXPECT_EQ(stats.BuildRows, keys.size());
    EXPECT_EQ(stats.ProbeRows, matching.size() * 11);
    EXPECT_EQ(stats.SkippedProbeRows, matching.size() * 4);
    EXPECT_EQ(stats.PausedProbeRows, matching.size() * 4);
    EXPECT_EQ(stats.RejectedRows, 0u);
    EXPECT_EQ(stats.BloomBytes, 512u);
    EXPECT_GT(stats.BloomSetBits, 0u);

    auto random = Hashes(8192, 32);
    ProbeBatch(filter, *state, random, output);
    stats = filter.StatsSnapshot();
    EXPECT_GT(stats.RejectedRows, 0u);
    for (size_t i = 0; i < random.size(); ++i) {
        ASSERT_EQ(output[i] != 0, filter.MayContain(random[i])) << i;
    }
}

TEST(RuntimeFilter, RejectsKeysItWasNotGiven) {
    TRuntimeFilter filter;
    auto builder = filter.MakeBuilder();
    for (uint64_t key : Hashes(1000, 2)) {
        builder.Add(key);
    }
    filter.FinishProducer(std::move(builder).TakePartial());

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

    TRuntimeFilter overflowed(/*maxKeys=*/8, /*bloomBytes=*/0);
    auto builder = overflowed.MakeBuilder();
    for (uint64_t key : Hashes(64, 3)) {
        builder.Add(key);
    }
    EXPECT_TRUE(builder.Overflowed());
    overflowed.FinishProducer(std::move(builder).TakePartial());
    EXPECT_TRUE(overflowed.Ready());
    EXPECT_TRUE(overflowed.StatsSnapshot().BloomDisabled);
    EXPECT_TRUE(overflowed.MayContain(12345));
}

TEST(RuntimeFilter, MergedOverflowUsesBloom) {
    TRuntimeFilter filter(/*maxKeys=*/16, /*bloomBytes=*/512);
    filter.SetProducerCount(4);
    std::vector<uint64_t> allKeys;
    for (uint64_t seed = 0; seed < 4; ++seed) {
        auto builder = filter.MakeBuilder();
        for (uint64_t key : Hashes(8, seed + 10)) {
            builder.Add(key);
            allKeys.push_back(key);
        }
        EXPECT_FALSE(builder.Overflowed());
        filter.FinishProducer(std::move(builder).TakePartial());
    }

    EXPECT_TRUE(filter.UsesBloom());
    for (uint64_t key : allKeys) {
        EXPECT_TRUE(filter.MayContain(key));
    }
}

TEST(RuntimeFilter, MergesKeysFromEveryBuilder) {
    TRuntimeFilter filter;
    filter.SetProducerCount(4);
    std::vector<std::vector<uint64_t>> parts;
    for (uint64_t seed = 0; seed < 4; ++seed) {
        parts.push_back(Hashes(100, seed + 20));
        auto builder = filter.MakeBuilder();
        for (uint64_t key : parts.back()) {
            builder.Add(key);
        }
        filter.FinishProducer(std::move(builder).TakePartial());
    }

    for (const auto& part : parts) {
        for (uint64_t key : part) {
            EXPECT_TRUE(filter.MayContain(key));
        }
    }
}

TEST(RuntimeFilter, MergesConcurrently) {
    TRuntimeFilter filter;
    constexpr size_t Threads = 8;
    constexpr size_t PerThread = 500;
    filter.SetProducerCount(Threads);

    std::vector<std::thread> workers;
    for (size_t t = 0; t < Threads; ++t) {
        workers.emplace_back([&filter, t] {
            auto builder = filter.MakeBuilder();
            for (uint64_t key : Hashes(PerThread, t + 100)) {
                builder.Add(key);
            }
            filter.FinishProducer(std::move(builder).TakePartial());
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }

    ASSERT_FALSE(filter.UsesBloom());
    EXPECT_EQ(filter.StatsSnapshot().ExactKeys, Threads * PerThread);
    for (size_t t = 0; t < Threads; ++t) {
        for (uint64_t key : Hashes(PerThread, t + 100)) {
            EXPECT_TRUE(filter.MayContain(key));
        }
    }
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
    filter.FinishProducer(std::move(builder).TakePartial());

    ASSERT_FALSE(filter.UsesBloom());
    EXPECT_EQ(filter.StatsSnapshot().ExactKeys, Cap);
    for (uint64_t key : keys) {
        EXPECT_TRUE(filter.MayContain(key));
    }
}

TEST(RuntimeFilter, PromotesAfterDuplicateHeavyExactBuildGetsNewKey) {
    constexpr size_t Cap = 64;
    TRuntimeFilter filter(Cap, /*bloomBytes=*/512);
    auto keys = Hashes(Cap, 21);
    auto builder = filter.MakeBuilder();
    for (int repeat = 0; repeat < 100; ++repeat) {
        for (uint64_t key : keys) {
            builder.Add(key);
        }
    }
    EXPECT_FALSE(builder.Overflowed());
    const uint64_t newKey = 0xfedcba9876543210ULL;
    builder.Add(newKey);
    EXPECT_TRUE(builder.Overflowed());
    filter.FinishProducer(std::move(builder).TakePartial());
    EXPECT_TRUE(filter.UsesBloom());
    for (uint64_t key : keys) {
        EXPECT_TRUE(filter.MayContain(key));
    }
    EXPECT_TRUE(filter.MayContain(newKey));
}

TEST(RuntimeFilter, CapIsEnforcedAtPublication) {
    constexpr size_t Cap = 8;
    for (size_t distinct : {Cap, Cap + 1}) {
        TRuntimeFilter filter(Cap, /*bloomBytes=*/512);
        auto keys = Hashes(distinct, 13);
        auto builder = filter.MakeBuilder();
        for (uint64_t key : keys) {
            builder.Add(key);
        }
        EXPECT_FALSE(builder.Overflowed()) << distinct;
        filter.FinishProducer(std::move(builder).TakePartial());

        if (distinct > Cap) {
            EXPECT_TRUE(filter.UsesBloom()) << distinct;
            EXPECT_EQ(filter.StatsSnapshot().ExactKeys, 0u) << distinct;
            for (uint64_t key : keys) {
                EXPECT_TRUE(filter.MayContain(key)) << distinct;
            }
        } else {
            EXPECT_FALSE(filter.UsesBloom()) << distinct;
            EXPECT_EQ(filter.StatsSnapshot().ExactKeys, Cap) << distinct;
            for (uint64_t key : keys) {
                EXPECT_TRUE(filter.MayContain(key)) << distinct;
            }
        }
    }
}

TEST(RuntimeFilter, CapIsEnforcedAcrossBuildersAtPublication) {
    constexpr size_t Cap = 8;
    TRuntimeFilter filter(Cap, /*bloomBytes=*/512);
    filter.SetProducerCount(3);
    std::vector<uint64_t> allKeys;
    for (uint64_t producer = 0; producer < 3; ++producer) {
        auto builder = filter.MakeBuilder();
        for (uint64_t key : Hashes(3, producer + 30)) {
            builder.Add(key);
            allKeys.push_back(key);
        }
        EXPECT_FALSE(builder.Overflowed());
        filter.FinishProducer(std::move(builder).TakePartial());
    }

    EXPECT_TRUE(filter.UsesBloom());
    EXPECT_EQ(filter.StatsSnapshot().ExactKeys, 0u);
    for (uint64_t key : allKeys) {
        EXPECT_TRUE(filter.MayContain(key));
    }
}

TEST(RuntimeFilter, BloomFragmentsMergeFromSeveralProducers) {
    TRuntimeFilter filter(/*maxKeys=*/32, /*bloomBytes=*/4096);
    constexpr size_t Producers = 4;
    filter.SetProducerCount(Producers);
    std::vector<std::vector<uint64_t>> parts;
    for (size_t i = 0; i < Producers; ++i) {
        parts.push_back(Hashes(100, i + 70));
        auto builder = filter.MakeBuilder();
        for (uint64_t key : parts.back()) {
            builder.Add(key);
        }
        auto partial = std::move(builder).TakePartial();
        ASSERT_TRUE(partial.ExactSetOverflowed);
        ASSERT_FALSE(partial.BloomWords.empty());
        filter.FinishProducer(std::move(partial));
        if (i + 1 < Producers) {
            EXPECT_FALSE(filter.Ready());
        }
    }
    ASSERT_TRUE(filter.UsesBloom());
    EXPECT_EQ(filter.StatsSnapshot().BuildRows, Producers * 100);
    EXPECT_EQ(filter.StatsSnapshot().BloomBytes, 4096u);
    for (const auto& part : parts) {
        for (uint64_t key : part) {
            EXPECT_TRUE(filter.MayContain(key));
        }
    }
    size_t passed = 0;
    for (uint64_t key : Hashes(1000, 99)) {
        passed += filter.MayContain(key);
    }
    EXPECT_LT(passed, 100u);
}

TEST(RuntimeFilter, MergesExactEmptyAndBloomContributionsWithoutLosingZeroHash) {
    TRuntimeFilter filter(/*maxKeys=*/8, /*bloomBytes=*/1000);
    filter.SetProducerCount(3);
    auto exact = filter.MakeBuilder();
    exact.Add(0);
    exact.Add(1);
    filter.FinishProducer(std::move(exact).TakePartial());
    filter.FinishProducer(filter.MakeBuilder().TakePartial());

    auto keys = Hashes(200, 71);
    auto bloom = filter.MakeBuilder();
    for (uint64_t key : keys) {
        bloom.Add(key);
    }
    filter.FinishProducer(std::move(bloom).TakePartial());

    ASSERT_TRUE(filter.UsesBloom());
    EXPECT_EQ(filter.StatsSnapshot().BloomBytes, 512u);
    EXPECT_EQ(filter.StatsSnapshot().BuildRows, keys.size() + 2);
    EXPECT_TRUE(filter.MayContain(0));
    EXPECT_TRUE(filter.MayContain(1));
    for (uint64_t key : keys) {
        EXPECT_TRUE(filter.MayContain(key));
    }
}

TEST(RuntimeFilter, IncompatibleBloomContributionPassesEverything) {
    TRuntimeFilter filter(/*maxKeys=*/8, /*bloomBytes=*/512);
    TRuntimeFilterBuilder builder(/*maxKeys=*/8, /*bloomBytes=*/64);
    for (uint64_t key : Hashes(64, 72)) {
        builder.Add(key);
    }
    filter.FinishProducer(std::move(builder).TakePartial());

    ASSERT_TRUE(filter.Ready());
    EXPECT_TRUE(filter.StatsSnapshot().BloomDisabled);
    EXPECT_TRUE(filter.MayContain(0));
    EXPECT_TRUE(filter.MayContain(1234567));
}

TEST(RuntimeFilter, SaturatedBloomPassesEverything) {
    TRuntimeFilter filter(/*maxKeys=*/8, /*bloomBytes=*/64);
    auto builder = filter.MakeBuilder();
    for (uint64_t key : Hashes(10000, 88)) {
        builder.Add(key);
    }
    filter.FinishProducer(std::move(builder).TakePartial());

    EXPECT_TRUE(filter.StatsSnapshot().BloomDisabled);
    EXPECT_FALSE(filter.UsesBloom());
    EXPECT_TRUE(filter.MayContain(1234567));
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
    filter.FinishProducer(std::move(builder).TakePartial());

    ASSERT_FALSE(filter.UsesBloom());
    EXPECT_EQ(filter.StatsSnapshot().ExactKeys, keys.size());
    for (uint64_t key : keys) {
        EXPECT_TRUE(filter.MayContain(key));
    }
}

TEST(RuntimeFilter, RepeatedKeysAcrossBuildersDoNotConsumeTheCap) {
    TRuntimeFilter filter(/*maxKeys=*/64);
    filter.SetProducerCount(20);
    auto keys = Hashes(50, 8);
    for (int producer = 0; producer < 20; ++producer) {
        auto builder = filter.MakeBuilder();
        for (uint64_t key : keys) {
            builder.Add(key);
        }
        filter.FinishProducer(std::move(builder).TakePartial());
    }

    ASSERT_FALSE(filter.UsesBloom());
    EXPECT_EQ(filter.StatsSnapshot().ExactKeys, keys.size());
}

TEST(RuntimeFilter, DeduplicatesRepeatedKeys) {
    TRuntimeFilter filter;
    auto builder = filter.MakeBuilder();
    for (int i = 0; i < 100; ++i) {
        builder.Add(42);
    }
    filter.FinishProducer(std::move(builder).TakePartial());

    EXPECT_EQ(filter.StatsSnapshot().ExactKeys, 1u);
    EXPECT_TRUE(filter.MayContain(42));
}

TEST(RuntimeFilter, PublishesWhenTheLastProducerFinishes) {
    TRuntimeFilter filter;
    filter.SetProducerCount(3);

    std::vector<std::vector<uint64_t>> parts;
    for (uint64_t producer = 0; producer < 3; ++producer) {
        parts.push_back(Hashes(50, producer + 40));
        auto builder = filter.MakeBuilder();
        for (uint64_t key : parts.back()) {
            builder.Add(key);
        }
        EXPECT_FALSE(filter.Ready()) << "published before the last producer";
        filter.FinishProducer(std::move(builder).TakePartial());
    }

    ASSERT_TRUE(filter.Ready());
    for (const auto& part : parts) {
        for (uint64_t key : part) {
            EXPECT_TRUE(filter.MayContain(key));
        }
    }
    EXPECT_FALSE(filter.MayContain(~parts[0][0]));
}

TEST(RuntimeFilter, PublishesOnceUnderContention) {
    TRuntimeFilter filter;
    constexpr size_t Producers = 8;
    filter.SetProducerCount(Producers);

    std::vector<std::thread> workers;
    for (size_t t = 0; t < Producers; ++t) {
        workers.emplace_back([&filter, t] {
            auto builder = filter.MakeBuilder();
            for (uint64_t key : Hashes(200, t + 50)) {
                builder.Add(key);
            }
            filter.FinishProducer(std::move(builder).TakePartial());
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }

    ASSERT_TRUE(filter.Ready());
    ASSERT_FALSE(filter.UsesBloom());
    EXPECT_EQ(filter.StatsSnapshot().ExactKeys, Producers * 200);
    for (size_t t = 0; t < Producers; ++t) {
        for (uint64_t key : Hashes(200, t + 50)) {
            EXPECT_TRUE(filter.MayContain(key));
        }
    }
}

TEST(RuntimeFilter, PartialCanBeMergedAfterLeavingTheProducer) {
    TRuntimeFilter filter(/*maxKeys=*/8);
    filter.SetProducerCount(2);

    auto first = filter.MakeBuilder();
    first.Add(10);
    auto firstPartial = std::move(first).TakePartial();
    ASSERT_EQ(firstPartial.Hashes, (std::vector<uint64_t>{10}));
    filter.FinishProducer(std::move(firstPartial));
    EXPECT_FALSE(filter.Ready());

    auto second = filter.MakeBuilder();
    second.Add(20);
    filter.FinishProducer(std::move(second).TakePartial());

    ASSERT_TRUE(filter.Ready());
    EXPECT_TRUE(filter.MayContain(10));
    EXPECT_TRUE(filter.MayContain(20));
    EXPECT_FALSE(filter.MayContain(30));
}

TEST(RuntimeFilter, LocalBindingKeepsProducerAndProbeInSync) {
    TLocalRuntimeFilterBindingFactory factory;
    auto binding = factory.Create(/*id=*/7, /*producerCount=*/2);
    ASSERT_TRUE(binding.Producer);
    ASSERT_TRUE(binding.Probe);

    for (uint64_t key : {11, 22}) {
        auto builder = binding.Producer->MakeBuilder();
        builder.Add(key);
        binding.Producer->FinishProducer(std::move(builder).TakePartial());
        if (key == 11) {
            EXPECT_TRUE(binding.Probe->MayContain(33));
        }
    }
    EXPECT_TRUE(binding.Probe->MayContain(11));
    EXPECT_TRUE(binding.Probe->MayContain(22));
    EXPECT_FALSE(binding.Probe->MayContain(33));
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    NQumir::NCodeGen::TLLVMInitializer llvmInit;
    return RUN_ALL_TESTS();
}
