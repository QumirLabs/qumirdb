#include <gtest/gtest.h>

#include "runtime_filter_test_util.h"

#include <qdb/exec/runtime_filter.h>
#include <qdb/exec/runtime_filter_kernels.h>
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
using NQdb::NTest::RuntimeFilterContext;
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

TRuntimeFilterKernels Kernels() {
    return TRuntimeFilterKernels(*RuntimeFilterContext()->RuntimeFilterKernels);
}

size_t BloomProbe(bool vectorized, std::span<const uint64_t> words,
    std::span<const uint64_t> hashes, std::span<const uint8_t> selection,
    std::span<uint8_t> output)
{
    std::vector<uint64_t> selectedHashes;
    std::vector<uint8_t> selectedOutput;
    return vectorized
        ? Kernels().BloomProbeVector(
            words, hashes, selection, output, selectedHashes, selectedOutput)
        : Kernels().BloomProbeScalar(words, hashes, selection, output);
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

TRuntimeFilterPartial BuildPartial(size_t maxKeys, size_t bloomBytes,
    std::span<const uint64_t> hashes, size_t batchRows,
    std::span<const uint8_t> selection = {})
{
    TRuntimeFilterBuilder builder(RuntimeFilterContext(), maxKeys, bloomBytes);
    for (size_t row = 0; row < hashes.size(); row += batchRows) {
        const size_t rows = std::min(batchRows, hashes.size() - row);
        builder.Add(hashes.subspan(row, rows),
            selection.empty() ? selection : selection.subspan(row, rows));
    }
    return std::move(builder).TakePartial();
}

void ExpectSamePartial(const TRuntimeFilterPartial& actual, const TRuntimeFilterPartial& expected) {
    EXPECT_EQ(actual.Hashes, expected.Hashes);
    EXPECT_EQ(actual.BloomWords, expected.BloomWords);
    EXPECT_EQ(actual.ExactSetOverflowed, expected.ExactSetOverflowed);
    EXPECT_EQ(actual.BuildRows, expected.BuildRows);
}

} // namespace

// ---- Kernels ----

TEST(RuntimeFilterKernels, BindWhenTheQueryIsFinalized) {
    auto context = std::make_shared<TRuntimeContext>();
    std::vector<TGeneratedKernel> kernels;
    TKernelCompiler compiler(TKernelCompilerOptions{
        .Sink = &kernels,
        .BindNow = false,
        .RuntimeContext = context,
    });
    context->RuntimeFilterKernels = compiler.CompileRuntimeFilter();
    ASSERT_EQ(kernels.size(), 1u);
    EXPECT_EQ(kernels.front().Entrypoints, TRuntimeFilterKernels::Entrypoints());
    EXPECT_FALSE(kernels.front().Slot->Runner);
    JitFinalizeKernels(kernels);

    TRuntimeFilter filter(context, /*maxKeys=*/8, /*bloomBytes=*/512);
    auto keys = Hashes(200, 23);
    PublishKeys(filter, keys);
    ASSERT_TRUE(filter.UsesBloom());
    ExpectContainsAll(filter, keys);
}

TEST(RuntimeFilterKernels, BloomNeverRejectsAnAddedHash) {
    const auto kernels = Kernels();
    for (size_t blocks : {size_t{1}, size_t{8}, size_t{64}}) {
        std::vector<uint64_t> words(blocks * 8, 0);
        auto keys = Hashes(blocks * 4 + 3, blocks);
        keys.push_back(0);
        kernels.BloomAdd(words, keys);
        for (bool vectorized : {false, true}) {
            std::vector<uint8_t> output(keys.size(), 7);
            EXPECT_EQ(BloomProbe(vectorized, words, keys, {}, output), keys.size());
            EXPECT_EQ(Kept(output), keys.size());
        }
    }
}

TEST(RuntimeFilterKernels, BloomRejectsMostHashesItWasNotGiven) {
    const auto kernels = Kernels();
    std::vector<uint64_t> words(512, 0);
    kernels.BloomAdd(words, Hashes(400, 1));
    auto probes = Hashes(10000, 2);
    std::vector<uint8_t> output(probes.size());
    EXPECT_LT(BloomProbe(true, words, probes, {}, output), 100u);
}

TEST(RuntimeFilterKernels, EmptyBloomRejectsAndFullBloomAccepts) {
    auto probes = Hashes(8195, 3);
    for (bool vectorized : {false, true}) {
        std::vector<uint8_t> output(probes.size(), 7);
        std::vector<uint64_t> words(64, 0);
        EXPECT_EQ(BloomProbe(vectorized, words, probes, {}, output), 0u);
        EXPECT_EQ(Kept(output), 0u);

        words.assign(64, ~uint64_t{0});
        EXPECT_EQ(BloomProbe(vectorized, words, probes, {}, output), probes.size());
        EXPECT_EQ(Kept(output), probes.size());
    }
}

TEST(RuntimeFilterKernels, VectorBloomProbeMatchesScalar) {
    std::vector<size_t> sizes = {8191, 8192, 8195, 16387};
    for (size_t rows = 0; rows < 38; ++rows) {
        sizes.push_back(rows);
    }
    std::mt19937_64 rng(4);
    for (size_t blocks : {size_t{1}, size_t{16}}) {
        // About 10% and 60% of the probes pass.
        for (int density : {2, 4}) {
            std::vector<uint64_t> words(blocks * 8, 0);
            for (auto& word : words) {
                for (int i = 0; i < density; ++i) {
                    word |= rng();
                }
            }
            for (size_t rows : sizes) {
                auto probes = Hashes(rows, rows + blocks);
                auto selections = Selections(rows);
                selections.emplace_back();
                for (const auto& selection : selections) {
                    std::vector<uint8_t> scalar(rows, 7);
                    std::vector<uint8_t> vector(rows, 7);
                    const size_t kept = BloomProbe(
                        false, words, probes, selection, scalar);
                    ASSERT_EQ(BloomProbe(true, words, probes, selection, vector), kept)
                        << rows;
                    ASSERT_EQ(Flags(vector), Flags(scalar)) << rows;
                    ASSERT_EQ(Kept(scalar), kept) << rows;
                    for (size_t i = 0; i < rows; ++i) {
                        if (!selection.empty() && !selection[i]) {
                            ASSERT_EQ(scalar[i], 0) << i;
                        }
                    }
                }
            }
        }
    }
}

TEST(RuntimeFilterKernels, BloomAddHonoursSelection) {
    const auto kernels = Kernels();
    auto hashes = Hashes(1000, 5);
    for (const auto& selection : Selections(hashes.size())) {
        std::vector<uint64_t> masked(128, 0);
        std::vector<uint64_t> compacted(128, 0);
        kernels.BloomAdd(masked, hashes, selection);
        kernels.BloomAdd(compacted, Selected(hashes, selection));
        EXPECT_EQ(masked, compacted);
    }
}

TEST(RuntimeFilterKernels, BloomFragmentsMergeByOr) {
    const auto kernels = Kernels();
    auto first = Hashes(300, 6);
    auto second = Hashes(300, 7);
    std::vector<uint64_t> merged(256, 0);
    std::vector<uint64_t> fragment(256, 0);
    std::vector<uint64_t> whole(256, 0);
    kernels.BloomAdd(merged, first);
    kernels.BloomAdd(fragment, second);
    kernels.BloomAdd(whole, first);
    kernels.BloomAdd(whole, second);
    kernels.BloomOr(merged, fragment);
    EXPECT_EQ(merged, whole);
}

// Fragments from different producers merge by OR, so the bit positions are
// part of the partial's format.
TEST(RuntimeFilterKernels, BloomLayoutIsStable) {
    std::vector<uint64_t> words(16, 0);
    Kernels().BloomAdd(words, std::vector<uint64_t>{0, 1, 2, 0xfedcba9876543210ULL});
    EXPECT_EQ(words, (std::vector<uint64_t>{
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

TEST(RuntimeFilterKernels, BloomSetBitsCountsEveryBit) {
    const auto kernels = Kernels();
    auto words = Hashes(64, 8);
    size_t expected = 0;
    for (uint64_t word : words) {
        expected += std::popcount(word);
    }
    EXPECT_EQ(kernels.BloomSetBits(words), expected);
    EXPECT_EQ(kernels.BloomSetBits(std::vector<uint64_t>(64, 0)), 0u);
    EXPECT_EQ(kernels.BloomSetBits(std::vector<uint64_t>(64, ~uint64_t{0})), 64u * 64);
}

TEST(RuntimeFilterKernels, ExactTableMatchesTheKeySet) {
    const auto kernels = Kernels();
    for (size_t count : {size_t{0}, size_t{1}, size_t{5}, size_t{200}, size_t{5000}}) {
        auto keys = Hashes(count, count + 9);
        const std::unordered_set<uint64_t> expected(keys.begin(), keys.end());
        std::vector<uint64_t> table(std::bit_ceil(std::max<size_t>(8, keys.size() * 2)), 0);
        kernels.ExactBuild(table, keys);

        auto probes = Hashes(3000, count + 10);
        probes.insert(probes.end(), keys.begin(), keys.end());
        probes.push_back(0);
        auto selections = Selections(probes.size());
        selections.emplace_back();
        for (const auto& selection : selections) {
            std::vector<uint8_t> output(probes.size(), 7);
            const size_t kept = kernels.ExactProbe(table, probes, selection, output);
            EXPECT_EQ(Kept(output), kept);
            for (size_t i = 0; i < probes.size(); ++i) {
                const bool selected = selection.empty() || selection[i];
                ASSERT_EQ(output[i] != 0, selected && expected.contains(probes[i]))
                    << count << " " << i;
            }
        }
    }
}

TEST(RuntimeFilterKernels, ExactTableKeepsZeroHash) {
    const auto kernels = Kernels();
    // 0 and 8 share a home slot, so one of them is displaced.
    for (const auto& keys : {std::vector<uint64_t>{0, 7}, std::vector<uint64_t>{8, 0}}) {
        std::vector<uint64_t> table(8, 0);
        kernels.ExactBuild(table, keys);
        const std::vector<uint64_t> probes = {keys[0], keys[1], 9, 16};
        std::vector<uint8_t> output(probes.size());
        EXPECT_EQ(kernels.ExactProbe(table, probes, {}, output), 2u);
        EXPECT_EQ(Flags(output), (std::vector<bool>{true, true, false, false}));
    }
}

TEST(RuntimeFilterKernels, SortUniqueMatchesAnOrderedSet) {
    const auto kernels = Kernels();
    std::vector<std::vector<uint64_t>> cases = {
        {},
        {42},
        std::vector<uint64_t>(100, 42),
        {~uint64_t{0}, 0, 1, 0, ~uint64_t{0}},
        Hashes(5000, 11),
    };
    // Repeats in random order.
    const auto distinct = Hashes(700, 12);
    std::vector<uint64_t> repeated;
    for (int i = 0; i < 4; ++i) {
        repeated.insert(repeated.end(), distinct.begin(), distinct.end());
    }
    std::ranges::shuffle(repeated, std::mt19937_64(13));
    cases.push_back(std::move(repeated));

    for (auto& keys : cases) {
        const std::set<uint64_t> ordered(keys.begin(), keys.end());
        kernels.SortUnique(keys);
        EXPECT_EQ(keys, std::vector<uint64_t>(ordered.begin(), ordered.end()));
    }
}

TEST(RuntimeFilterKernels, AppendKeysStopsAtCapacity) {
    const auto kernels = Kernels();
    auto hashes = Hashes(100, 14);

    std::vector<uint64_t> keys = {1, 2, 3};
    EXPECT_EQ(kernels.AppendKeys(keys, 10, hashes, {}), 7u);
    ASSERT_EQ(keys.size(), 10u);
    EXPECT_TRUE(std::equal(keys.begin() + 3, keys.end(), hashes.begin()));
    EXPECT_EQ(kernels.AppendKeys(keys, 10, hashes, {}), 0u);
    EXPECT_EQ(keys.size(), 10u);

    // Rows 0, 2, ..., 12 fill the seven free entries.
    keys = {1, 2, 3};
    std::vector<uint8_t> everyOther(hashes.size(), 0);
    for (size_t i = 0; i < everyOther.size(); i += 2) {
        everyOther[i] = 1;
    }
    EXPECT_EQ(kernels.AppendKeys(keys, 10, hashes, everyOther), 13u);
    ASSERT_EQ(keys.size(), 10u);
    EXPECT_EQ(keys[3], hashes[0]);
    EXPECT_EQ(keys[9], hashes[12]);

    keys.clear();
    EXPECT_EQ(kernels.AppendKeys(keys, 1000, hashes, everyOther), hashes.size());
    EXPECT_EQ(keys, Selected(hashes, everyOther));
}

TEST(RuntimeFilterKernels, AppendDistinctKeysKeepsFirstOccurrences) {
    const auto kernels = Kernels();
    constexpr size_t MaxKeys = 4096;
    auto initial = Hashes(500, 15);
    auto fresh = Hashes(2000, 16);
    fresh.push_back(0);

    std::vector<uint64_t> hashes;
    for (size_t i = 0; i < fresh.size(); ++i) {
        hashes.push_back(fresh[i]);
        hashes.push_back(initial[i % initial.size()]);
        hashes.push_back(fresh[i / 2]);
    }
    auto selections = Selections(hashes.size());
    selections.emplace_back();
    for (const auto& selection : selections) {
        TRuntimeFilterKeySet set;
        kernels.FillSet(set, MaxKeys, initial);
        auto keys = initial;
        auto expected = initial;
        std::unordered_set<uint64_t> seen(initial.begin(), initial.end());
        for (size_t i = 0; i < hashes.size(); ++i) {
            if ((selection.empty() || selection[i]) && seen.insert(hashes[i]).second) {
                expected.push_back(hashes[i]);
            }
        }
        EXPECT_EQ(kernels.AppendDistinctKeys(set, keys, MaxKeys, hashes, selection),
            hashes.size());
        EXPECT_EQ(keys, expected);
    }
}

TEST(RuntimeFilterKernels, AppendDistinctKeysStopsRightAfterTheCap) {
    const auto kernels = Kernels();
    constexpr size_t MaxKeys = 8;
    auto distinct = Hashes(20, 17);
    const std::vector<uint64_t> firstNine(distinct.begin(), distinct.begin() + MaxKeys + 1);

    TRuntimeFilterKeySet set;
    std::vector<uint64_t> keys;
    kernels.FillSet(set, MaxKeys, keys);
    EXPECT_EQ(kernels.AppendDistinctKeys(set, keys, MaxKeys, distinct, {}), MaxKeys + 1);
    EXPECT_EQ(keys, firstNine);

    // The ninth distinct key is at row 16.
    std::vector<uint64_t> doubled;
    for (uint64_t key : distinct) {
        doubled.push_back(key);
        doubled.push_back(key);
    }
    keys.clear();
    kernels.FillSet(set, MaxKeys, keys);
    EXPECT_EQ(kernels.AppendDistinctKeys(set, keys, MaxKeys, doubled, {}), 17u);
    EXPECT_EQ(keys, firstNine);
}

// ---- Filter ----

TEST(RuntimeFilter, RequiresTheKernel) {
    EXPECT_THROW(TRuntimeFilter(nullptr), std::invalid_argument);
    EXPECT_THROW(TRuntimeFilter(std::make_shared<TRuntimeContext>()), std::invalid_argument);
    EXPECT_THROW(TRuntimeFilterBuilder(nullptr, 8, 512), std::invalid_argument);
}

TEST(RuntimeFilter, NeverRejectsAKeyItWasGiven) {
    TRuntimeFilter filter(RuntimeFilterContext());
    auto keys = Hashes(5000, 1);
    PublishKeys(filter, keys);

    ASSERT_TRUE(filter.Ready());
    EXPECT_FALSE(filter.UsesBloom());
    ExpectContainsAll(filter, keys);
}

TEST(RuntimeFilter, HandlesZeroHash) {
    TRuntimeFilter filter(RuntimeFilterContext());
    PublishKeys(filter, std::vector<uint64_t>{0, 7});

    EXPECT_TRUE(RuntimeFilterMayContain(filter, 0));
    EXPECT_TRUE(RuntimeFilterMayContain(filter, 7));
    EXPECT_FALSE(RuntimeFilterMayContain(filter, 9));
}

TEST(RuntimeFilter, RegistersProbeStatesFromConcurrentLanes) {
    TRuntimeFilter filter(RuntimeFilterContext());
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

TEST(RuntimeFilter, ExactFilterMatchesTheKeySet) {
    auto keys = Hashes(200, 21);
    const std::unordered_set<uint64_t> expected(keys.begin(), keys.end());
    auto probes = Hashes(10000, 22);
    probes.insert(probes.end(), keys.begin(), keys.end());
    probes.push_back(0);

    TRuntimeFilter filter(RuntimeFilterContext(), /*maxKeys=*/256, /*bloomBytes=*/512);
    EXPECT_EQ(Kept(ProbeRuntimeFilter(filter, probes)), probes.size());
    PublishKeys(filter, keys);

    ASSERT_FALSE(filter.UsesBloom());
    const auto output = ProbeRuntimeFilter(filter, probes);
    for (size_t i = 0; i < probes.size(); ++i) {
        ASSERT_EQ(output[i] != 0, expected.contains(probes[i])) << i;
    }
}

TEST(RuntimeFilter, BloomAnswersDoNotDependOnBatchSize) {
    TRuntimeFilter filter(RuntimeFilterContext(), /*maxKeys=*/8, /*bloomBytes=*/2048);
    auto keys = Hashes(2000, 24);
    PublishKeys(filter, keys);
    ASSERT_TRUE(filter.UsesBloom());

    auto probes = Hashes(16'387, 25);
    for (size_t i = 0; i < probes.size(); i += 1000) {
        probes[i] = keys[i % keys.size()];
    }
    auto selections = Selections(probes.size());
    selections.emplace_back();
    for (const auto& selection : selections) {
        // One batch is large enough for the vector probe; the pieces are not.
        std::vector<uint8_t> whole(probes.size(), 7);
        std::vector<uint8_t> pieces(probes.size(), 7);
        auto state = filter.CreateProbeState();
        const std::span<const uint64_t> hashes(probes);
        const std::span<const uint8_t> mask(selection);
        ProbeBatch(filter, *state, hashes, whole, mask);
        for (size_t row = 0; row < probes.size(); row += 1000) {
            const size_t rows = std::min<size_t>(1000, probes.size() - row);
            ProbeBatch(filter, *filter.CreateProbeState(), hashes.subspan(row, rows),
                std::span(pieces).subspan(row, rows),
                mask.empty() ? mask : mask.subspan(row, rows));
        }
        ASSERT_EQ(Flags(whole), Flags(pieces));
        for (size_t i = 0; i < probes.size(); i += 1000) {
            ASSERT_EQ(whole[i] != 0, selection.empty() || selection[i] != 0) << i;
        }
    }
}

TEST(RuntimeFilter, BatchRespectsSelectionAndCountsOnlySelectedRows) {
    for (size_t cap : {size_t{8}, size_t{256}}) {
        TRuntimeFilter filter(RuntimeFilterContext(), cap, /*bloomBytes=*/512);
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

TEST(RuntimeFilter, WeakBloomPausesAndRechecks) {
    TRuntimeFilter filter(RuntimeFilterContext(), /*maxKeys=*/8, /*bloomBytes=*/512);
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

TEST(RuntimeFilter, RejectsKeysItWasNotGiven) {
    TRuntimeFilter filter(RuntimeFilterContext());
    PublishKeys(filter, Hashes(1000, 2));

    EXPECT_LT(Kept(ProbeRuntimeFilter(filter, Hashes(10000, 99))), 10u);
}

TEST(RuntimeFilter, PassesEverythingWhenItCannotAnswer) {
    TRuntimeFilter unpublished(RuntimeFilterContext());
    EXPECT_FALSE(unpublished.Ready());
    EXPECT_TRUE(RuntimeFilterMayContain(unpublished, 12345));

    TRuntimeFilter overflowed(RuntimeFilterContext(), /*maxKeys=*/8, /*bloomBytes=*/0);
    auto builder = overflowed.MakeBuilder();
    builder.Add(Hashes(64, 3));
    EXPECT_TRUE(builder.Overflowed());
    overflowed.FinishProducer(std::move(builder).TakePartial());
    EXPECT_TRUE(overflowed.Ready());
    EXPECT_TRUE(overflowed.StatsSnapshot().BloomDisabled);
    EXPECT_TRUE(RuntimeFilterMayContain(overflowed, 12345));
}

TEST(RuntimeFilter, MergedOverflowUsesBloom) {
    TRuntimeFilter filter(RuntimeFilterContext(), /*maxKeys=*/16, /*bloomBytes=*/512);
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

TEST(RuntimeFilter, RepeatsAtTheCapStayExact) {
    constexpr size_t Cap = 4096;
    TRuntimeFilter filter(RuntimeFilterContext(), Cap);
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

TEST(RuntimeFilter, PromotesAfterDuplicateHeavyExactBuildGetsNewKey) {
    constexpr size_t Cap = 64;
    TRuntimeFilter filter(RuntimeFilterContext(), Cap, /*bloomBytes=*/512);
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

TEST(RuntimeFilter, BuilderResultDoesNotDependOnBatchBoundaries) {
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
        repeated(40, 4, std::span(late).first(10)), // deduplicates and stays exact
        repeated(Cap, 10, late),                    // deduplicates, then overflows
        Hashes(1000, 43),                           // overflows at the first compaction
    };
    EXPECT_FALSE(BuildPartial(Cap, 512, inputs[1], 1).ExactSetOverflowed);
    EXPECT_EQ(BuildPartial(Cap, 512, inputs[1], 1).Hashes.size(), 50u);
    EXPECT_TRUE(BuildPartial(Cap, 512, inputs[2], 1).ExactSetOverflowed);

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

TEST(RuntimeFilter, CapIsEnforcedAtPublication) {
    constexpr size_t Cap = 8;
    for (size_t distinct : {Cap, Cap + 1}) {
        TRuntimeFilter filter(RuntimeFilterContext(), Cap, /*bloomBytes=*/512);
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

TEST(RuntimeFilter, CapIsEnforcedAcrossBuildersAtPublication) {
    constexpr size_t Cap = 8;
    TRuntimeFilter filter(RuntimeFilterContext(), Cap, /*bloomBytes=*/512);
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

TEST(RuntimeFilter, BloomFragmentsMergeFromSeveralProducers) {
    TRuntimeFilter filter(RuntimeFilterContext(), /*maxKeys=*/32, /*bloomBytes=*/4096);
    constexpr size_t Producers = 4;
    filter.SetProducerCount(Producers);
    std::vector<std::vector<uint64_t>> parts;
    for (size_t i = 0; i < Producers; ++i) {
        parts.push_back(Hashes(100, i + 70));
        auto builder = filter.MakeBuilder();
        builder.Add(parts.back());
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
        ExpectContainsAll(filter, part);
    }
    EXPECT_LT(Kept(ProbeRuntimeFilter(filter, Hashes(1000, 99))), 100u);
}

TEST(RuntimeFilter, MergesExactEmptyAndBloomContributionsWithoutLosingZeroHash) {
    TRuntimeFilter filter(RuntimeFilterContext(), /*maxKeys=*/8, /*bloomBytes=*/1000);
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

TEST(RuntimeFilter, IncompatibleBloomContributionPassesEverything) {
    TRuntimeFilter filter(RuntimeFilterContext(), /*maxKeys=*/8, /*bloomBytes=*/512);
    TRuntimeFilterBuilder builder(RuntimeFilterContext(), /*maxKeys=*/8, /*bloomBytes=*/64);
    builder.Add(Hashes(64, 72));
    filter.FinishProducer(std::move(builder).TakePartial());

    ASSERT_TRUE(filter.Ready());
    EXPECT_TRUE(filter.StatsSnapshot().BloomDisabled);
    EXPECT_TRUE(RuntimeFilterMayContain(filter, 0));
    EXPECT_TRUE(RuntimeFilterMayContain(filter, 1234567));
}

TEST(RuntimeFilter, SaturatedBloomPassesEverything) {
    TRuntimeFilter filter(RuntimeFilterContext(), /*maxKeys=*/8, /*bloomBytes=*/64);
    PublishKeys(filter, Hashes(10000, 88));

    EXPECT_TRUE(filter.StatsSnapshot().BloomDisabled);
    EXPECT_FALSE(filter.UsesBloom());
    EXPECT_TRUE(RuntimeFilterMayContain(filter, 1234567));
}

TEST(RuntimeFilter, RepeatedKeysDoNotConsumeTheCap) {
    TRuntimeFilter filter(RuntimeFilterContext(), /*maxKeys=*/64);
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

TEST(RuntimeFilter, RepeatedKeysAcrossBuildersDoNotConsumeTheCap) {
    TRuntimeFilter filter(RuntimeFilterContext(), /*maxKeys=*/64);
    filter.SetProducerCount(20);
    auto keys = Hashes(50, 8);
    for (int producer = 0; producer < 20; ++producer) {
        PublishKeys(filter, keys);
    }

    ASSERT_FALSE(filter.UsesBloom());
    EXPECT_EQ(filter.StatsSnapshot().ExactKeys, keys.size());
}

TEST(RuntimeFilter, PublishesWhenTheLastProducerFinishes) {
    TRuntimeFilter filter(RuntimeFilterContext());
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

TEST(RuntimeFilter, PublishesOnceUnderContention) {
    TRuntimeFilter filter(RuntimeFilterContext());
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

TEST(RuntimeFilter, PartialCanBeMergedAfterLeavingTheProducer) {
    TRuntimeFilter filter(RuntimeFilterContext(), /*maxKeys=*/8);
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

TEST(RuntimeFilter, LocalBindingKeepsProducerAndProbeInSync) {
    TLocalRuntimeFilterBindingFactory factory(RuntimeFilterContext());
    auto binding = factory.Create(/*id=*/7, /*producerCount=*/2,
        TRuntimeFilter::DefaultBloomBytes);
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
