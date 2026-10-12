#include <gtest/gtest.h>

#include "runtime_filter_test_util.h"

#include <qdb/exec/runtime_filter.h>
#include <qdb/exec/runtime_context.h>
#include <qdb/kernel/compiler.h>
#include <qdb/kernel/finalize.h>

#include <qumir/codegen/llvm/llvm_initializer.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <random>
#include <set>
#include <span>
#include <stdexcept>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace NQdb;
using NQdb::NTest::ProbeRuntimeFilter;
using NQdb::NTest::RuntimeFilterMayContain;

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

size_t Kept(std::span<const uint8_t> output) {
    return static_cast<size_t>(std::count_if(
        output.begin(), output.end(), [](uint8_t value) { return value != 0; }));
}

// Any nonzero byte means true, so masks compare by truth value.
std::vector<bool> Flags(std::span<const uint8_t> output) {
    std::vector<bool> flags;
    flags.reserve(output.size());
    for (uint8_t value : output) {
        flags.push_back(value != 0);
    }
    return flags;
}

// Every row selected, none, every third, and a random half marked with 1
// rather than 0xff: any nonzero byte selects a row.
std::vector<std::vector<uint8_t>> Selections(size_t rows) {
    std::vector<std::vector<uint8_t>> out(4, std::vector<uint8_t>(rows, 0));
    std::mt19937_64 rng(rows);
    for (size_t i = 0; i < rows; ++i) {
        out[0][i] = 0xff;
        out[2][i] = i % 3 == 0 ? 0xff : 0;
        out[3][i] = rng() & 1;
    }
    return out;
}

std::vector<uint64_t> Selected(
    std::span<const uint64_t> hashes, std::span<const uint8_t> selection)
{
    std::vector<uint64_t> out;
    for (size_t i = 0; i < hashes.size(); ++i) {
        if (selection[i]) {
            out.push_back(hashes[i]);
        }
    }
    return out;
}

void PublishKeys(TRuntimeFilter& filter, std::span<const uint64_t> keys) {
    auto builder = filter.MakeBuilder();
    builder.Add(keys);
    filter.FinishProducer(std::move(builder).TakePartial());
}

size_t ProbeBatch(IRuntimeFilterProbe& filter, TRuntimeFilterProbeState& state,
    std::span<const uint64_t> hashes, std::span<uint8_t> output,
    std::span<const uint8_t> selection = {})
{
    const size_t rows = selection.empty() ? hashes.size() : Kept(selection);
    if (filter.BeginBatch(state, rows)) {
        return filter.MayContainBatch(hashes, output, state, selection);
    }
    for (size_t i = 0; i < output.size(); ++i) {
        output[i] = selection.empty() || selection[i] ? 0xff : 0;
    }
    return rows;
}

void ExpectContainsAll(IRuntimeFilterProbe& filter, std::span<const uint64_t> keys) {
    EXPECT_EQ(Kept(ProbeRuntimeFilter(filter, keys)), keys.size());
}

void ExpectSamePartial(const TRuntimeFilterPartial& actual, const TRuntimeFilterPartial& expected) {
    EXPECT_EQ(actual.Hashes, expected.Hashes);
    EXPECT_EQ(actual.BloomWords, expected.BloomWords);
    EXPECT_EQ(actual.ExactSetOverflowed, expected.ExactSetOverflowed);
    EXPECT_EQ(actual.BuildRows, expected.BuildRows);
}

class RuntimeFilter : public NTest::TRuntimeFilterTest {
protected:
    TRuntimeFilterPartial BuildPartial(size_t maxKeys, size_t bloomBytes,
        std::span<const uint64_t> hashes, size_t batchRows,
        std::span<const uint8_t> selection = {})
    {
        TRuntimeFilterBuilder builder(Context_, maxKeys, bloomBytes);
        for (size_t row = 0; row < hashes.size(); row += batchRows) {
            const size_t rows = std::min(batchRows, hashes.size() - row);
            builder.Add(hashes.subspan(row, rows),
                selection.empty() ? selection : selection.subspan(row, rows));
        }
        return std::move(builder).TakePartial();
    }
};

} // namespace

TEST(RuntimeFilterKernel, BindsWhenTheQueryIsFinalized) {
    auto context = std::make_shared<TRuntimeContext>();
    std::vector<TGeneratedKernel> kernels;
    TKernelCompiler compiler(TKernelCompilerOptions{
        .Sink = &kernels,
        .BindNow = false,
        .RuntimeContext = context,
    });
    context->RuntimeFilterKernels = compiler.CompileRuntimeFilter();
    ASSERT_EQ(kernels.size(), 1u);
    EXPECT_EQ(kernels.front().Entrypoints, (std::vector<std::string>(
        RuntimeFilterKernelEntrypoints.begin(), RuntimeFilterKernelEntrypoints.end())));
    EXPECT_FALSE(kernels.front().Slot->Runner);
    JitFinalizeKernels(kernels);

    TRuntimeFilter filter(context, /*maxKeys=*/8, /*bloomBytes=*/512);
    auto keys = Hashes(200, 23);
    PublishKeys(filter, keys);
    ASSERT_TRUE(filter.UsesBloom());
    ExpectContainsAll(filter, keys);
}

TEST_F(RuntimeFilter, RequiresTheKernel) {
    EXPECT_THROW(TRuntimeFilter(nullptr), std::invalid_argument);
    EXPECT_THROW(TRuntimeFilter(std::make_shared<TRuntimeContext>()), std::invalid_argument);
    EXPECT_THROW(TRuntimeFilterBuilder(nullptr, 8, 512), std::invalid_argument);
}

TEST_F(RuntimeFilter, NeverRejectsAKeyItWasGiven) {
    TRuntimeFilter filter(Context_);
    auto keys = Hashes(5000, 1);
    PublishKeys(filter, keys);

    ASSERT_TRUE(filter.Ready());
    EXPECT_FALSE(filter.UsesBloom());
    ExpectContainsAll(filter, keys);
}

TEST_F(RuntimeFilter, HandlesZeroHash) {
    // 0 and 8 share a home slot in the smallest table, so one is displaced.
    for (const auto& keys : {std::vector<uint64_t>{0, 7}, std::vector<uint64_t>{8, 0}}) {
        TRuntimeFilter filter(Context_);
        PublishKeys(filter, keys);

        const std::vector<uint64_t> probes = {keys[0], keys[1], 9, 16};
        EXPECT_EQ(Flags(ProbeRuntimeFilter(filter, probes)),
            (std::vector<bool>{true, true, false, false}));
    }
}

TEST_F(RuntimeFilter, RegistersProbeStatesFromConcurrentLanes) {
    TRuntimeFilter filter(Context_);
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

TEST_F(RuntimeFilter, ExactFilterMatchesTheKeySet) {
    // A power-of-two key count must still leave empty slots in the table.
    for (size_t count : {size_t{0}, size_t{1}, size_t{8}, size_t{200}, size_t{4096}, size_t{5000}}) {
        auto keys = Hashes(count, count + 21);
        const std::unordered_set<uint64_t> expected(keys.begin(), keys.end());
        auto probes = Hashes(10000, count + 22);
        probes.insert(probes.end(), keys.begin(), keys.end());
        probes.push_back(0);

        TRuntimeFilter filter(Context_, /*maxKeys=*/8192, /*bloomBytes=*/512);
        EXPECT_EQ(Kept(ProbeRuntimeFilter(filter, probes)), probes.size());
        PublishKeys(filter, keys);
        ASSERT_FALSE(filter.UsesBloom());
        EXPECT_EQ(filter.StatsSnapshot().ExactKeys, keys.size());

        auto selections = Selections(probes.size());
        selections.emplace_back();
        for (const auto& selection : selections) {
            std::vector<uint8_t> output(probes.size(), 7);
            auto state = filter.CreateProbeState();
            const size_t kept = ProbeBatch(filter, *state, probes, output, selection);
            EXPECT_EQ(Kept(output), kept);
            for (size_t i = 0; i < probes.size(); ++i) {
                const bool selected = selection.empty() || selection[i];
                ASSERT_EQ(output[i] != 0, selected && expected.contains(probes[i]))
                    << count << " " << i;
            }
        }
    }
}

// Large batches take the vector probe, small ones the scalar probe. The whole
// batch sizes cover every tail length, with and without a packed selection.
TEST_F(RuntimeFilter, BloomAnswersDoNotDependOnBatchSize) {
    TRuntimeFilter filter(Context_, /*maxKeys=*/8, /*bloomBytes=*/2048);
    auto keys = Hashes(2000, 24);
    PublishKeys(filter, keys);
    ASSERT_TRUE(filter.UsesBloom());

    for (size_t rows : {size_t{30000}, size_t{30001}, size_t{30002}, size_t{30003}}) {
        auto probes = Hashes(rows, rows);
        for (size_t i = 0; i < probes.size(); i += 1000) {
            probes[i] = keys[i % keys.size()];
        }
        auto selections = Selections(probes.size());
        selections.emplace_back();
        for (const auto& selection : selections) {
            std::vector<uint8_t> whole(probes.size(), 7);
            std::vector<uint8_t> pieces(probes.size(), 7);
            const std::span<const uint64_t> hashes(probes);
            const std::span<const uint8_t> mask(selection);
            const size_t kept = ProbeBatch(
                filter, *filter.CreateProbeState(), hashes, whole, mask);
            for (size_t row = 0; row < probes.size(); row += 1000) {
                const size_t count = std::min<size_t>(1000, probes.size() - row);
                ProbeBatch(filter, *filter.CreateProbeState(), hashes.subspan(row, count),
                    std::span(pieces).subspan(row, count),
                    mask.empty() ? mask : mask.subspan(row, count));
            }
            ASSERT_EQ(Flags(whole), Flags(pieces)) << rows;
            ASSERT_EQ(Kept(whole), kept) << rows;
            for (size_t i = 0; i < probes.size(); ++i) {
                const bool selected = selection.empty() || selection[i] != 0;
                if (!selected) {
                    ASSERT_EQ(whole[i], 0) << i;
                } else if (i % 1000 == 0) {
                    ASSERT_NE(whole[i], 0) << i;
                }
            }
        }
    }
}

TEST_F(RuntimeFilter, BatchRespectsSelectionAndCountsOnlySelectedRows) {
    for (size_t cap : {size_t{8}, size_t{256}}) {
        TRuntimeFilter filter(Context_, cap, /*bloomBytes=*/512);
        auto state = filter.CreateProbeState();
        auto keys = Hashes(200, 26);
        PublishKeys(filter, keys);

        auto probes = Hashes(1000, 27);
        std::vector<uint8_t> selection(probes.size(), 0);
        size_t selected = 0;
        for (size_t i = 0; i < probes.size(); i += 3) {
            selection[i] = 0xff;
            ++selected;
            if (i % 2 == 0) {
                probes[i] = keys[i % keys.size()];
            }
        }
        const auto unmasked = ProbeRuntimeFilter(filter, probes);
        std::vector<uint8_t> output(probes.size(), 7);
        const size_t accepted = ProbeBatch(filter, *state, probes, output, selection);
        EXPECT_EQ(Kept(output), accepted);
        for (size_t i = 0; i < probes.size(); ++i) {
            EXPECT_EQ(output[i] != 0, selection[i] && unmasked[i]) << i;
            if (i % 6 == 0) {
                EXPECT_NE(output[i], 0) << i;
            }
        }
        // ProbeRuntimeFilter used a state of its own for all the rows.
        const auto stats = filter.StatsSnapshot();
        EXPECT_EQ(stats.ProbeRows, probes.size() + selected);
        EXPECT_EQ(state->ProbeRows, selected);
        EXPECT_EQ(state->RejectedRows, selected - accepted);
    }
}

TEST_F(RuntimeFilter, WeakBloomPausesAndRechecks) {
    TRuntimeFilter filter(Context_, /*maxKeys=*/8, /*bloomBytes=*/512);
    auto keys = Hashes(200, 31);
    PublishKeys(filter, keys);

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
    EXPECT_GT(state->RejectedRows, 0u);
    EXPECT_EQ(state->RejectedRows, random.size() - Kept(output));
}

TEST_F(RuntimeFilter, RejectsKeysItWasNotGiven) {
    TRuntimeFilter filter(Context_);
    PublishKeys(filter, Hashes(1000, 2));

    EXPECT_LT(Kept(ProbeRuntimeFilter(filter, Hashes(10000, 99))), 10u);
}

TEST_F(RuntimeFilter, PassesEverythingWhenItCannotAnswer) {
    TRuntimeFilter unpublished(Context_);
    EXPECT_FALSE(unpublished.Ready());
    EXPECT_TRUE(RuntimeFilterMayContain(unpublished, 12345));

    TRuntimeFilter overflowed(Context_, /*maxKeys=*/8, /*bloomBytes=*/0);
    auto builder = overflowed.MakeBuilder();
    builder.Add(Hashes(64, 3));
    EXPECT_TRUE(builder.Overflowed());
    overflowed.FinishProducer(std::move(builder).TakePartial());
    EXPECT_TRUE(overflowed.Ready());
    EXPECT_TRUE(overflowed.StatsSnapshot().BloomDisabled);
    EXPECT_TRUE(RuntimeFilterMayContain(overflowed, 12345));

    // Too many keys for the exact set appear only at publication.
    TRuntimeFilter merged(Context_, /*maxKeys=*/8, /*bloomBytes=*/0);
    PublishKeys(merged, Hashes(9, 4));
    EXPECT_TRUE(merged.StatsSnapshot().BloomDisabled);
    EXPECT_TRUE(RuntimeFilterMayContain(merged, 12345));
}

TEST_F(RuntimeFilter, MergedOverflowUsesBloom) {
    TRuntimeFilter filter(Context_, /*maxKeys=*/16, /*bloomBytes=*/512);
    filter.SetProducerCount(4);
    std::vector<uint64_t> allKeys;
    for (uint64_t seed = 0; seed < 4; ++seed) {
        auto keys = Hashes(8, seed + 10);
        allKeys.insert(allKeys.end(), keys.begin(), keys.end());
        auto builder = filter.MakeBuilder();
        builder.Add(keys);
        EXPECT_FALSE(builder.Overflowed());
        filter.FinishProducer(std::move(builder).TakePartial());
    }

    EXPECT_TRUE(filter.UsesBloom());
    ExpectContainsAll(filter, allKeys);
}

TEST_F(RuntimeFilter, RepeatsAtTheCapStayExact) {
    constexpr size_t Cap = 4096;
    TRuntimeFilter filter(Context_, Cap);
    auto keys = Hashes(Cap, 11);
    auto builder = filter.MakeBuilder();
    for (int repeat = 0; repeat < 201; ++repeat) {
        builder.Add(keys);
    }
    EXPECT_FALSE(builder.Overflowed());
    filter.FinishProducer(std::move(builder).TakePartial());

    ASSERT_FALSE(filter.UsesBloom());
    EXPECT_EQ(filter.StatsSnapshot().ExactKeys, Cap);
    ExpectContainsAll(filter, keys);
}

// The first compaction leaves the distinct hashes sorted. Afterwards new
// hashes are appended as they come and repeats are dropped.
TEST_F(RuntimeFilter, CompactionSortsAndLaterKeysKeepTheirOrder) {
    constexpr size_t Cap = 64;
    auto early = Hashes(38, 51);
    early.push_back(0);
    early.push_back(~uint64_t{0});
    // Exactly the rows that trigger the compaction, with every early hash.
    std::vector<uint64_t> input;
    for (int repeat = 0; repeat < 3; ++repeat) {
        input.insert(input.end(), early.begin(), early.end());
    }
    input.insert(input.end(), early.begin(), early.begin() + 8);
    std::ranges::shuffle(input, std::mt19937_64(52));
    ASSERT_EQ(input.size(), Cap * 2);
    const auto late = Hashes(10, 53);
    for (uint64_t key : late) {
        input.push_back(key);
        input.push_back(early[key % early.size()]);
        input.push_back(key);
    }

    const std::set<uint64_t> sorted(early.begin(), early.end());
    std::vector<uint64_t> expected(sorted.begin(), sorted.end());
    expected.insert(expected.end(), late.begin(), late.end());
    const auto partial = BuildPartial(Cap, 512, input, input.size());
    EXPECT_FALSE(partial.ExactSetOverflowed);
    EXPECT_EQ(partial.Hashes, expected);
    EXPECT_EQ(partial.BuildRows, input.size());
}

TEST_F(RuntimeFilter, PromotesAfterDuplicateHeavyExactBuildGetsNewKey) {
    constexpr size_t Cap = 64;
    TRuntimeFilter filter(Context_, Cap, /*bloomBytes=*/512);
    auto keys = Hashes(Cap, 21);
    auto builder = filter.MakeBuilder();
    for (int repeat = 0; repeat < 100; ++repeat) {
        builder.Add(keys);
    }
    EXPECT_FALSE(builder.Overflowed());
    // The new key overflows the exact set in the middle of a batch; the rows
    // after it must still reach the Bloom filter.
    auto tail = Hashes(50, 22);
    std::vector<uint64_t> batch(keys.begin(), keys.begin() + 10);
    batch.push_back(0xfedcba9876543210ULL);
    batch.insert(batch.end(), tail.begin(), tail.end());
    builder.Add(batch);
    EXPECT_TRUE(builder.Overflowed());
    filter.FinishProducer(std::move(builder).TakePartial());
    EXPECT_TRUE(filter.UsesBloom());
    ExpectContainsAll(filter, keys);
    ExpectContainsAll(filter, batch);
}

TEST_F(RuntimeFilter, BuilderResultDoesNotDependOnBatchBoundaries) {
    constexpr size_t Cap = 64;
    const auto late = Hashes(30, 41);
    auto repeated = [](size_t distinct, int times, std::span<const uint64_t> tail) {
        const auto keys = Hashes(distinct, distinct);
        std::vector<uint64_t> out;
        for (int i = 0; i < times; ++i) {
            out.insert(out.end(), keys.begin(), keys.end());
        }
        out.insert(out.end(), tail.begin(), tail.end());
        return out;
    };
    const std::vector<std::vector<uint64_t>> inputs = {
        Hashes(100, 42),                            // stays raw
        Hashes(Cap * 2, 43),                        // fills up exactly, then overflows
        repeated(40, 4, std::span(late).first(10)), // deduplicates and stays exact
        repeated(Cap, 10, late),                    // deduplicates, then overflows
        Hashes(1000, 44),                           // overflows at the first compaction
    };
    EXPECT_FALSE(BuildPartial(Cap, 512, inputs[0], 1).ExactSetOverflowed);
    EXPECT_TRUE(BuildPartial(Cap, 512, inputs[1], 1).ExactSetOverflowed);
    EXPECT_FALSE(BuildPartial(Cap, 512, inputs[2], 1).ExactSetOverflowed);
    EXPECT_EQ(BuildPartial(Cap, 512, inputs[2], 1).Hashes.size(), 50u);
    EXPECT_TRUE(BuildPartial(Cap, 512, inputs[3], 1).ExactSetOverflowed);

    for (const auto& hashes : inputs) {
        const auto rowByRow = BuildPartial(Cap, 512, hashes, 1);
        for (size_t batchRows : {size_t{7}, size_t{128}, size_t{129}, hashes.size()}) {
            ExpectSamePartial(BuildPartial(Cap, 512, hashes, batchRows), rowByRow);
        }
        for (const auto& selection : Selections(hashes.size())) {
            const auto selected = Selected(hashes, selection);
            ExpectSamePartial(BuildPartial(Cap, 512, hashes, 129, selection),
                BuildPartial(Cap, 512, selected, 1));
        }
    }
}

// Fragments from different producers merge by OR, so the bit positions are
// part of the partial's format.
TEST_F(RuntimeFilter, BloomLayoutIsStable) {
    const std::vector<uint64_t> keys = {0, 1, 2, 0xfedcba9876543210ULL};
    const auto partial = BuildPartial(/*maxKeys=*/0, /*bloomBytes=*/128, keys, keys.size());
    EXPECT_TRUE(partial.ExactSetOverflowed);
    EXPECT_EQ(partial.BloomWords, (std::vector<uint64_t>{
        0x0000800000004000ULL, 0x0000000000402000ULL,
        0x0000000020800000ULL, 0x0800000010000000ULL,
        0x0200000040000000ULL, 0x0020010000000000ULL,
        0x0000000101000000ULL, 0x0000000400800000ULL,
        0x0000000100020000ULL, 0x0000100000000200ULL,
        0x000000a000000000ULL, 0x0000000008800000ULL,
        0x0080000000000040ULL, 0x0002000080000000ULL,
        0x0000000002000002ULL, 0x0000011000000000ULL,
    }));
}

TEST_F(RuntimeFilter, CapIsEnforcedAtPublication) {
    constexpr size_t Cap = 8;
    for (size_t distinct : {Cap, Cap + 1}) {
        TRuntimeFilter filter(Context_, Cap, /*bloomBytes=*/512);
        auto keys = Hashes(distinct, 13);
        auto builder = filter.MakeBuilder();
        builder.Add(keys);
        EXPECT_FALSE(builder.Overflowed()) << distinct;
        filter.FinishProducer(std::move(builder).TakePartial());

        EXPECT_EQ(filter.UsesBloom(), distinct > Cap) << distinct;
        EXPECT_EQ(filter.StatsSnapshot().ExactKeys, distinct > Cap ? 0 : Cap) << distinct;
        ExpectContainsAll(filter, keys);
    }
}

TEST_F(RuntimeFilter, CapIsEnforcedAcrossBuildersAtPublication) {
    constexpr size_t Cap = 8;
    TRuntimeFilter filter(Context_, Cap, /*bloomBytes=*/512);
    filter.SetProducerCount(3);
    std::vector<uint64_t> allKeys;
    for (uint64_t producer = 0; producer < 3; ++producer) {
        auto keys = Hashes(3, producer + 30);
        allKeys.insert(allKeys.end(), keys.begin(), keys.end());
        auto builder = filter.MakeBuilder();
        builder.Add(keys);
        EXPECT_FALSE(builder.Overflowed());
        filter.FinishProducer(std::move(builder).TakePartial());
    }

    EXPECT_TRUE(filter.UsesBloom());
    EXPECT_EQ(filter.StatsSnapshot().ExactKeys, 0u);
    ExpectContainsAll(filter, allKeys);
}

TEST_F(RuntimeFilter, BloomFragmentsMergeFromSeveralProducers) {
    constexpr size_t Producers = 4;
    TRuntimeFilter filter(Context_, /*maxKeys=*/32, /*bloomBytes=*/4096);
    TRuntimeFilter single(Context_, /*maxKeys=*/32, /*bloomBytes=*/4096);
    filter.SetProducerCount(Producers);
    std::vector<uint64_t> allKeys;
    for (size_t i = 0; i < Producers; ++i) {
        const auto part = Hashes(100, i + 70);
        allKeys.insert(allKeys.end(), part.begin(), part.end());
        auto builder = filter.MakeBuilder();
        builder.Add(part);
        auto partial = std::move(builder).TakePartial();
        ASSERT_TRUE(partial.ExactSetOverflowed);
        ASSERT_FALSE(partial.BloomWords.empty());
        filter.FinishProducer(std::move(partial));
        if (i + 1 < Producers) {
            EXPECT_FALSE(filter.Ready());
        }
    }
    // One producer with every key builds the same filter.
    auto builder = single.MakeBuilder();
    builder.Add(allKeys);
    auto whole = std::move(builder).TakePartial();
    size_t setBits = 0;
    for (uint64_t word : whole.BloomWords) {
        setBits += std::popcount(word);
    }
    single.FinishProducer(std::move(whole));

    ASSERT_TRUE(filter.UsesBloom());
    EXPECT_EQ(filter.StatsSnapshot().BuildRows, Producers * 100);
    EXPECT_EQ(filter.StatsSnapshot().BloomBytes, 4096u);
    EXPECT_EQ(filter.StatsSnapshot().BloomSetBits, setBits);
    EXPECT_EQ(single.StatsSnapshot().BloomSetBits, setBits);
    ExpectContainsAll(filter, allKeys);
    const auto probes = Hashes(20000, 99);
    const auto answers = ProbeRuntimeFilter(filter, probes);
    EXPECT_EQ(Flags(answers), Flags(ProbeRuntimeFilter(single, probes)));
    EXPECT_LT(Kept(answers), 2000u);
}

TEST_F(RuntimeFilter, MergesExactEmptyAndBloomContributionsWithoutLosingZeroHash) {
    TRuntimeFilter filter(Context_, /*maxKeys=*/8, /*bloomBytes=*/1000);
    filter.SetProducerCount(3);
    PublishKeys(filter, std::vector<uint64_t>{0, 1});
    filter.FinishProducer(filter.MakeBuilder().TakePartial());

    auto keys = Hashes(200, 71);
    PublishKeys(filter, keys);

    ASSERT_TRUE(filter.UsesBloom());
    EXPECT_EQ(filter.StatsSnapshot().BloomBytes, 512u);
    EXPECT_EQ(filter.StatsSnapshot().BuildRows, keys.size() + 2);
    EXPECT_TRUE(RuntimeFilterMayContain(filter, 0));
    EXPECT_TRUE(RuntimeFilterMayContain(filter, 1));
    ExpectContainsAll(filter, keys);
}

TEST_F(RuntimeFilter, IncompatibleBloomContributionPassesEverything) {
    TRuntimeFilter filter(Context_, /*maxKeys=*/8, /*bloomBytes=*/512);
    TRuntimeFilterBuilder builder(Context_, /*maxKeys=*/8, /*bloomBytes=*/64);
    builder.Add(Hashes(64, 72));
    filter.FinishProducer(std::move(builder).TakePartial());

    ASSERT_TRUE(filter.Ready());
    EXPECT_TRUE(filter.StatsSnapshot().BloomDisabled);
    EXPECT_TRUE(RuntimeFilterMayContain(filter, 0));
    EXPECT_TRUE(RuntimeFilterMayContain(filter, 1234567));
}

TEST_F(RuntimeFilter, SaturatedBloomPassesEverything) {
    TRuntimeFilter filter(Context_, /*maxKeys=*/8, /*bloomBytes=*/64);
    PublishKeys(filter, Hashes(10000, 88));

    EXPECT_TRUE(filter.StatsSnapshot().BloomDisabled);
    EXPECT_FALSE(filter.UsesBloom());
    EXPECT_TRUE(RuntimeFilterMayContain(filter, 1234567));
}

TEST_F(RuntimeFilter, RepeatedKeysDoNotConsumeTheCap) {
    TRuntimeFilter filter(Context_, /*maxKeys=*/64);
    auto keys = Hashes(50, 7);
    auto builder = filter.MakeBuilder();
    for (int repeat = 0; repeat < 1000; ++repeat) {
        builder.Add(keys);
    }
    EXPECT_FALSE(builder.Overflowed());
    filter.FinishProducer(std::move(builder).TakePartial());

    ASSERT_FALSE(filter.UsesBloom());
    EXPECT_EQ(filter.StatsSnapshot().ExactKeys, keys.size());
    ExpectContainsAll(filter, keys);
}

TEST_F(RuntimeFilter, RepeatedKeysAcrossBuildersDoNotConsumeTheCap) {
    TRuntimeFilter filter(Context_, /*maxKeys=*/64);
    filter.SetProducerCount(20);
    auto keys = Hashes(50, 8);
    for (int producer = 0; producer < 20; ++producer) {
        PublishKeys(filter, keys);
    }

    ASSERT_FALSE(filter.UsesBloom());
    EXPECT_EQ(filter.StatsSnapshot().ExactKeys, keys.size());
    ExpectContainsAll(filter, keys);
    EXPECT_LT(Kept(ProbeRuntimeFilter(filter, Hashes(1000, 9))), 2u);
}

TEST_F(RuntimeFilter, PublishesWhenTheLastProducerFinishes) {
    TRuntimeFilter filter(Context_);
    filter.SetProducerCount(3);

    std::vector<std::vector<uint64_t>> parts;
    for (uint64_t producer = 0; producer < 3; ++producer) {
        parts.push_back(Hashes(50, producer + 40));
        EXPECT_FALSE(filter.Ready()) << "published before the last producer";
        PublishKeys(filter, parts.back());
    }

    ASSERT_TRUE(filter.Ready());
    for (const auto& part : parts) {
        ExpectContainsAll(filter, part);
    }
    EXPECT_FALSE(RuntimeFilterMayContain(filter, ~parts[0][0]));
}

TEST_F(RuntimeFilter, PublishesOnceUnderContention) {
    TRuntimeFilter filter(Context_);
    constexpr size_t Producers = 8;
    constexpr size_t KeysPerProducer = 500;
    filter.SetProducerCount(Producers);

    std::vector<std::thread> workers;
    for (size_t t = 0; t < Producers; ++t) {
        workers.emplace_back([&filter, t] {
            PublishKeys(filter, Hashes(KeysPerProducer, t + 50));
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }

    ASSERT_TRUE(filter.Ready());
    ASSERT_FALSE(filter.UsesBloom());
    EXPECT_EQ(filter.StatsSnapshot().ExactKeys, Producers * KeysPerProducer);
    for (size_t t = 0; t < Producers; ++t) {
        ExpectContainsAll(filter, Hashes(KeysPerProducer, t + 50));
    }
}

TEST_F(RuntimeFilter, PartialCanBeMergedAfterLeavingTheProducer) {
    TRuntimeFilter filter(Context_, /*maxKeys=*/8);
    filter.SetProducerCount(2);

    auto first = filter.MakeBuilder();
    first.Add(std::vector<uint64_t>{10});
    auto firstPartial = std::move(first).TakePartial();
    ASSERT_EQ(firstPartial.Hashes, (std::vector<uint64_t>{10}));
    filter.FinishProducer(std::move(firstPartial));
    EXPECT_FALSE(filter.Ready());

    PublishKeys(filter, std::vector<uint64_t>{20});

    ASSERT_TRUE(filter.Ready());
    EXPECT_TRUE(RuntimeFilterMayContain(filter, 10));
    EXPECT_TRUE(RuntimeFilterMayContain(filter, 20));
    EXPECT_FALSE(RuntimeFilterMayContain(filter, 30));
}

TEST_F(RuntimeFilter, LocalBindingKeepsProducerAndProbeInSync) {
    TLocalRuntimeFilterBindingFactory factory;
    auto binding = factory.Create(/*id=*/7, /*producerCount=*/2,
        TRuntimeFilter::DefaultBloomBytes, Context_);
    ASSERT_TRUE(binding.Producer);
    ASSERT_TRUE(binding.Probe);

    for (uint64_t key : {11, 22}) {
        auto builder = binding.Producer->MakeBuilder();
        builder.Add({&key, 1});
        binding.Producer->FinishProducer(std::move(builder).TakePartial());
        if (key == 11) {
            EXPECT_TRUE(RuntimeFilterMayContain(*binding.Probe, 33));
        }
    }
    EXPECT_TRUE(RuntimeFilterMayContain(*binding.Probe, 11));
    EXPECT_TRUE(RuntimeFilterMayContain(*binding.Probe, 22));
    EXPECT_FALSE(RuntimeFilterMayContain(*binding.Probe, 33));
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    NQumir::NCodeGen::TLLVMInitializer llvmInit;
    return RUN_ALL_TESTS();
}
