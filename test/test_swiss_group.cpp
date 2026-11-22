#include <gtest/gtest.h>
#include "test_types.h"

#include <qdb/kernel/lib.h>
#include "qumirdb_source_module.h"
#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/runner/runner_llvm.h>

#include <array>
#include <cstdint>
#include <memory>
#include <random>
#include <string>

namespace {

constexpr uint8_t Empty = 0x80;

class SwissGroup : public testing::Test {
protected:
    void SetUp() override {
        auto library = NQdb::NKernel::ParseFunctionLibrary(NQdb::TestTypes(),
            NQdb::NKernel::ReadAggregationKernel("swiss_group.oz"));
        ASSERT_TRUE(library) << library.error().ToString();
        NQumir::TLLVMRunnerOptions options;
        options.CoreInput = true;
        options.NativeCode = true;
        options.AllowOverloads = true;
        options.OptLevel = 3;
        NQdb::NTest::ConfigureQumirDbSourceModule(options);
        Runner_ = std::make_unique<NQumir::TLLVMRunner>(options, nullptr, &NQdb::TestTypes());
        auto program = NQumir::MakeIntrusive<NQumir::NAst::TBlockExpr>(
            NQumir::TLocation{}, std::move(*library));
        NQdb::NTest::AddQumirDbUse(program);
        std::string error;
        auto entries = Runner_->CompileKernelAst(program, {
            "swiss_match", "swiss_match_empty", "swiss_lowest_index",
            "swiss_clear_first", "swiss_group_width"}, &error);
        ASSERT_EQ(entries.size(), 5u) << error;
        Match_ = reinterpret_cast<uint64_t(*)(const uint8_t*, uint64_t)>(entries.at("swiss_match"));
        MatchEmpty_ = reinterpret_cast<uint64_t(*)(const uint8_t*)>(entries.at("swiss_match_empty"));
        First_ = reinterpret_cast<int64_t(*)(uint64_t)>(entries.at("swiss_lowest_index"));
        Clear_ = reinterpret_cast<uint64_t(*)(uint64_t)>(entries.at("swiss_clear_first"));
        Width_ = reinterpret_cast<int64_t(*)()>(entries.at("swiss_group_width"))();
        ASSERT_TRUE(Width_ == 8 || Width_ == 16);
    }

    uint64_t SlotBit(int slot) const {
        return Width_ == 16
            ? uint64_t{1} << slot
            : uint64_t{0x80} << (8 * slot);
    }

    std::unique_ptr<NQumir::TLLVMRunner> Runner_;
    uint64_t (*Match_)(const uint8_t*, uint64_t) = nullptr;
    uint64_t (*MatchEmpty_)(const uint8_t*) = nullptr;
    int64_t (*First_)(uint64_t) = nullptr;
    uint64_t (*Clear_)(uint64_t) = nullptr;
    int Width_ = 0;
};

TEST_F(SwissGroup, LowestIndexRecoversSlotWithinGroup) {
    for (int low = 0; low < Width_; ++low) {
        EXPECT_EQ(First_(SlotBit(low)), low);
        for (int high = low + 1; high < Width_; ++high) {
            EXPECT_EQ(First_(SlotBit(low) | SlotBit(high)), low);
        }
    }
}

TEST_F(SwissGroup, MatchEmptyFindsExactlyTheEmptySlots) {
    std::mt19937_64 rng(1234);
    std::uniform_int_distribution<int> h2Dist(0, 0x7F);
    for (int iteration = 0; iteration < 512; ++iteration) {
        std::array<uint8_t, 16> bytes{};
        uint64_t expected = 0;
        for (int k = 0; k < Width_; ++k) {
            const bool empty = (rng() & 1) != 0;
            bytes[k] = empty
                ? Empty
                : static_cast<uint8_t>(h2Dist(rng));
            if (empty) {
                expected |= SlotBit(k);
            }
        }
        EXPECT_EQ(MatchEmpty_(bytes.data()), expected) << iteration;
    }
}

// Portable SWAR can produce false positives, resolved by the key comparison.
// All implementations must report every actual match and exclude empty slots.
TEST_F(SwissGroup, MatchNeverMissesAndNeverHitsEmptySlots) {
    std::mt19937_64 rng(4321);
    std::uniform_int_distribution<int> h2Dist(0, 0x7F);
    for (int iteration = 0; iteration < 2048; ++iteration) {
        std::array<uint8_t, 16> bytes{};
        for (int k = 0; k < Width_; ++k) {
            bytes[k] = (rng() & 3) == 0
                ? Empty
                : static_cast<uint8_t>(h2Dist(rng));
        }
        const auto h2 = static_cast<uint64_t>(h2Dist(rng));
        const uint64_t mask = Match_(bytes.data(), h2);
        if (Width_ == 16) {
            EXPECT_EQ(mask >> 16, 0u);
        }
        for (int k = 0; k < Width_; ++k) {
            const bool reported = (mask & SlotBit(k)) != 0;
            if (bytes[k] == h2) {
                EXPECT_TRUE(reported) << "slot " << k;
            }
            if (bytes[k] == Empty) {
                EXPECT_FALSE(reported) << "slot " << k;
            }
        }
    }
}

TEST_F(SwissGroup, MatchFindsEveryH2Value) {
    for (uint64_t h2 = 0; h2 <= 0x7F; ++h2) {
        for (int k = 0; k < Width_; ++k) {
            std::array<uint8_t, 16> bytes;
            bytes.fill(Empty);
            bytes[k] = static_cast<uint8_t>(h2);
            EXPECT_NE(Match_(bytes.data(), h2) & SlotBit(k), 0u);
        }
    }
}

TEST_F(SwissGroup, CandidateIterationVisitsEachMatchingSlotOnce) {
    // In particular, a NEON FF byte must not produce eight visits to one slot.
    for (unsigned subset = 0; subset < (1u << Width_); ++subset) {
        std::array<uint8_t, 16> bytes;
        bytes.fill(Empty);
        for (int k = 0; k < Width_; ++k) {
            if (subset & (1u << k)) {
                bytes[k] = 37;
            }
        }
        auto mask = Match_(bytes.data(), 37);
        unsigned visited = 0;
        while (mask) {
            const auto slot = First_(mask);
            ASSERT_GE(slot, 0);
            ASSERT_LT(slot, Width_);
            ASSERT_EQ(visited & (1u << slot), 0u) << subset;
            visited |= 1u << slot;
            mask = Clear_(mask);
        }
        ASSERT_EQ(visited, subset);
    }
}

} // namespace

int main(int argc, char** argv) {
    NQumir::NCodeGen::TLLVMInitializer initializer;
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
