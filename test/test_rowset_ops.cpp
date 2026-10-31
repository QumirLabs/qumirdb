#include <gtest/gtest.h>

#include <qdb/io/rowset_ops.h>

#include <qumir/error.h>

#include <cstring>
#include <random>
#include <span>
#include <string>
#include <vector>

using namespace NQdb;

namespace {

enum class EKind { Fixed, Bool, String4, String8 };

// Owns one column; random padding around the data exposes out-of-range writes.
struct TTestColumn {
    EKind Kind;
    int32_t Width = 0;
    bool Nullable = false;
    int32_t DataBitOffset = 0;
    int32_t MaskBitOffset = 0;
    std::vector<char> Data;
    std::vector<uint8_t> Mask;
    std::vector<char> Offsets;
};

struct TExpectedRow {
    std::string Value; // fixed bytes, "0"/"1" for bool, string chars
    bool Valid = true;
};

bool Bit(const uint8_t* bits, int64_t i) {
    return (bits[i / 8] >> (i % 8)) & 1;
}

int64_t Offset(const TTestColumn& c, int64_t i) {
    return c.Kind == EKind::String8
        ? reinterpret_cast<const int64_t*>(c.Offsets.data())[i]
        : reinterpret_cast<const int32_t*>(c.Offsets.data())[i];
}

class TRowSetFixture {
public:
    TRowSetFixture(std::vector<EKind> kinds, int64_t rows, bool nullable, bool withHash, uint32_t seed)
        : Rng_(seed)
    {
        for (EKind kind : kinds) {
            Columns_.push_back(MakeColumn(kind, rows, nullable));
        }
        Hash_.resize(rows);
        for (auto& h : Hash_) {
            h = Rng_();
            h = (h << 32) ^ Rng_();
        }
        for (const auto& c : Columns_) {
            TColumn col{};
            col.Data = const_cast<char*>(c.Data.data());
            col.DataBitOffset = c.DataBitOffset;
            col.Mask = c.Nullable ? c.Mask.data() : nullptr;
            col.MaskBitOffset = c.MaskBitOffset;
            col.Offsets = const_cast<char*>(c.Offsets.data());
            col.OffsetWidth = c.Kind == EKind::String4 ? 4 : c.Kind == EKind::String8 ? 8 : 0;
            if (c.Offsets.empty()) {
                col.Offsets = nullptr;
            }
            Views_.push_back(col);
            Layouts_.push_back(c.Kind == EKind::Bool ? TValueLayout{0, true} : TValueLayout{c.Width, false});
        }
        RowSet_ = TRowSet{};
        RowSet_.Columns = Views_.data();
        RowSet_.ColumnCount = static_cast<int64_t>(Views_.size());
        RowSet_.RowCount = rows;
        RowSet_.Hash = withHash ? Hash_.data() : nullptr;
        RowSet_.RefCount = 1;
    }

    std::vector<uint8_t> MakeSelection(const std::string& pattern) {
        const int64_t rows = RowSet_.RowCount;
        std::vector<uint8_t> sel(rows);
        for (int64_t i = 0; i < rows; ++i) {
            if (pattern == "none") sel[i] = 0;
            else if (pattern == "all") sel[i] = 1;
            else if (pattern == "alt") sel[i] = i % 2;
            else if (pattern == "first") sel[i] = i == 0;
            else if (pattern == "last") sel[i] = i == rows - 1;
            else if (pattern == "runs") sel[i] = (i / 5) % 3 != 1;
            else sel[i] = Rng_() % 3 != 0;
        }
        return sel;
    }

    // Reference: row-by-row copy of the selected rows.
    std::vector<std::vector<TExpectedRow>> Expected(const std::vector<uint8_t>& sel) const {
        std::vector<std::vector<TExpectedRow>> out(Columns_.size());
        for (size_t c = 0; c < Columns_.size(); ++c) {
            for (int64_t i = 0; i < RowSet_.RowCount; ++i) {
                if (sel[i]) {
                    out[c].push_back(Row(c, i));
                }
            }
        }
        return out;
    }

    std::vector<uint64_t> ExpectedHash(const std::vector<uint8_t>& sel) const {
        std::vector<uint64_t> out;
        for (int64_t i = 0; i < RowSet_.RowCount; ++i) {
            if (sel[i]) {
                out.push_back(Hash_[i]);
            }
        }
        return out;
    }

    TExpectedRow Row(size_t c, int64_t i) const {
        const TTestColumn& col = Columns_[c];
        TExpectedRow r;
        if (col.Nullable) {
            r.Valid = Bit(col.Mask.data(), col.MaskBitOffset + i);
        }
        switch (col.Kind) {
        case EKind::Fixed:
            r.Value.assign(col.Data.data() + i * col.Width, col.Width);
            break;
        case EKind::Bool:
            r.Value = Bit(reinterpret_cast<const uint8_t*>(col.Data.data()), col.DataBitOffset + i) ? "1" : "0";
            break;
        default:
            r.Value.assign(col.Data.data() + Offset(col, i), Offset(col, i + 1) - Offset(col, i));
        }
        return r;
    }

    TRowSet& RowSet() { return RowSet_; }
    const std::vector<TValueLayout>& Layouts() const { return Layouts_; }
    const std::vector<TTestColumn>& Columns() const { return Columns_; }
    const std::vector<uint64_t>& Hash() const { return Hash_; }

private:
    TTestColumn MakeColumn(EKind kind, int64_t rows, bool nullable) {
        TTestColumn c;
        c.Kind = kind;
        c.Nullable = nullable;
        c.MaskBitOffset = nullable ? static_cast<int32_t>(Rng_() % 13) : 0;
        if (nullable) {
            c.Mask.resize((c.MaskBitOffset + rows) / 8 + 2);
            for (auto& b : c.Mask) {
                b = static_cast<uint8_t>(Rng_());
            }
        }
        switch (kind) {
        case EKind::Fixed: {
            static const int widths[] = {1, 2, 4, 8, 16};
            c.Width = widths[Rng_() % 5];
            c.Data.resize(rows * c.Width);
            for (auto& b : c.Data) {
                b = static_cast<char>(Rng_());
            }
            break;
        }
        case EKind::Bool:
            c.DataBitOffset = static_cast<int32_t>(Rng_() % 13);
            c.Data.resize((c.DataBitOffset + rows) / 8 + 2);
            for (auto& b : c.Data) {
                b = static_cast<char>(Rng_());
            }
            break;
        default: {
            // Non-zero first offset (like a sliced array); some strings empty or long.
            std::string chars(5, 'x');
            std::vector<int64_t> offs{static_cast<int64_t>(chars.size())};
            for (int64_t i = 0; i < rows; ++i) {
                const uint32_t pick = Rng_() % 8;
                const size_t len = pick == 0 ? 0 : pick == 1 ? 200 + Rng_() % 300 : Rng_() % 12;
                for (size_t k = 0; k < len; ++k) {
                    chars.push_back(static_cast<char>('a' + Rng_() % 26));
                }
                offs.push_back(static_cast<int64_t>(chars.size()));
            }
            c.Data.assign(chars.begin(), chars.end());
            if (kind == EKind::String8) {
                c.Offsets.resize(offs.size() * 8);
                std::memcpy(c.Offsets.data(), offs.data(), c.Offsets.size());
            } else {
                c.Offsets.resize(offs.size() * 4);
                for (size_t i = 0; i < offs.size(); ++i) {
                    const int32_t v = static_cast<int32_t>(offs[i]);
                    std::memcpy(c.Offsets.data() + i * 4, &v, 4);
                }
            }
        }
        }
        return c;
    }

    std::mt19937 Rng_;
    std::vector<TTestColumn> Columns_;
    std::vector<TColumn> Views_;
    std::vector<TValueLayout> Layouts_;
    std::vector<uint64_t> Hash_;
    TRowSet RowSet_;
};

// Bits outside [offset, offset + rows) must stay unchanged; the tail past `kept` is unspecified.
void ExpectBitPadding(const std::vector<uint8_t>& before, const std::vector<uint8_t>& after,
                      int64_t offset, int64_t rows, const std::string& what)
{
    ASSERT_EQ(before.size(), after.size());
    for (int64_t i = 0; i < static_cast<int64_t>(before.size()) * 8; ++i) {
        if (i >= offset && i < offset + rows) {
            continue;
        }
        ASSERT_EQ(Bit(after.data(), i), Bit(before.data(), i)) << what << " bit " << i;
    }
}

void CheckCompaction(std::vector<EKind> kinds, int64_t rows, bool nullable, bool withHash, const std::string& pattern, uint32_t seed) {
    SCOPED_TRACE(pattern + " rows=" + std::to_string(rows) + " seed=" + std::to_string(seed));
    TRowSetFixture fx(kinds, rows, nullable, withHash, seed);
    auto sel = fx.MakeSelection(pattern);
    const auto expected = fx.Expected(sel);
    const auto expectedHash = fx.ExpectedHash(sel);
    const std::vector<TTestColumn> before = fx.Columns();
    fx.RowSet().Selection = sel.data();

    const int64_t kept = CompactRowSet(fx.RowSet(), fx.Layouts());
    int64_t expectedKept = 0;
    for (uint8_t s : sel) {
        expectedKept += s != 0;
    }
    ASSERT_EQ(kept, expectedKept);
    EXPECT_EQ(fx.RowSet().RowCount, kept);
    EXPECT_EQ(fx.RowSet().Selection, nullptr);
    for (size_t c = 0; c < expected.size(); ++c) {
        for (int64_t i = 0; i < kept; ++i) {
            const TExpectedRow got = fx.Row(c, i);
            ASSERT_EQ(got.Valid, expected[c][i].Valid) << "col " << c << " row " << i;
            ASSERT_EQ(got.Value, expected[c][i].Value) << "col " << c << " row " << i;
        }
    }
    for (size_t c = 0; c < before.size(); ++c) {
        const TTestColumn& was = before[c];
        const TTestColumn& now = fx.Columns()[c];
        const std::string name = "col " + std::to_string(c);
        if (was.Nullable) {
            ExpectBitPadding(was.Mask, now.Mask, was.MaskBitOffset, rows, name + " mask");
        }
        if (was.Kind == EKind::Bool) {
            ExpectBitPadding({was.Data.begin(), was.Data.end()}, {now.Data.begin(), now.Data.end()},
                             was.DataBitOffset, rows, name + " data");
        }
    }
    if (withHash) {
        for (int64_t i = 0; i < kept; ++i) {
            ASSERT_EQ(fx.Hash()[i], expectedHash[i]) << "hash row " << i;
        }
    }
}

const char* const Patterns[] = {"none", "all", "alt", "first", "last", "runs", "random"};
const int64_t RowCounts[] = {0, 1, 7, 8, 9, 17, 100, 1000};

void RunAll(std::vector<EKind> kinds, bool nullable, bool withHash) {
    uint32_t seed = 1;
    for (int64_t rows : RowCounts) {
        for (const char* pattern : Patterns) {
            CheckCompaction(kinds, rows, nullable, withHash, pattern, seed++);
        }
    }
}

} // namespace

TEST(RowSetCompact, FixedWidths) {
    for (int rep = 0; rep < 8; ++rep) { // widths are random per column
        RunAll({EKind::Fixed, EKind::Fixed}, false, false);
    }
}

TEST(RowSetCompact, Bool) {
    RunAll({EKind::Bool}, false, false);
}

TEST(RowSetCompact, Strings) {
    RunAll({EKind::String4}, false, false);
    RunAll({EKind::String8}, false, false);
}

TEST(RowSetCompact, NullsWithMaskOffset) {
    RunAll({EKind::Fixed, EKind::Bool, EKind::String4, EKind::String8}, true, false);
}

TEST(RowSetCompact, HashColumn) {
    RunAll({EKind::Fixed, EKind::String4}, true, true);
}

TEST(RowSetCompact, MixedRowSet) {
    RunAll({EKind::Fixed, EKind::Bool, EKind::String8, EKind::Fixed, EKind::String4}, true, true);
}

TEST(RowSetCompact, NoSelectionIsNoOp) {
    TRowSetFixture fx({EKind::Fixed}, 10, false, false, 7);
    EXPECT_EQ(CompactRowSet(fx.RowSet(), fx.Layouts()), 10);
    EXPECT_EQ(fx.RowSet().RowCount, 10);
}

TEST(RowSetCompact, SharedRowSetIsRejected) {
    TRowSetFixture fx({EKind::Fixed}, 10, false, false, 7);
    auto sel = fx.MakeSelection("alt");
    fx.RowSet().Selection = sel.data();
    fx.RowSet().RefCount = 2;
    EXPECT_THROW(CompactRowSet(fx.RowSet(), fx.Layouts()), NQumir::TError);
    EXPECT_EQ(fx.RowSet().RowCount, 10);
    EXPECT_EQ(fx.RowSet().Selection, sel.data());
}

// A failed validation must leave the row set exactly as it was.
void ExpectRejectedUntouched(TRowSetFixture& fx, std::span<const TValueLayout> layouts) {
    auto sel = fx.MakeSelection("alt");
    fx.RowSet().Selection = sel.data();
    const std::vector<TTestColumn> before = fx.Columns();
    EXPECT_THROW(CompactRowSet(fx.RowSet(), layouts), NQumir::TError);
    EXPECT_EQ(fx.RowSet().RowCount, 10);
    EXPECT_EQ(fx.RowSet().Selection, sel.data());
    for (size_t c = 0; c < before.size(); ++c) {
        EXPECT_EQ(fx.Columns()[c].Data, before[c].Data) << c;
        EXPECT_EQ(fx.Columns()[c].Mask, before[c].Mask) << c;
        EXPECT_EQ(fx.Columns()[c].Offsets, before[c].Offsets) << c;
    }
}

TEST(RowSetCompact, InvalidLayoutIsRejectedBeforeMutation) {
    for (int32_t bad : {-1, 0, 3, 5, 32}) {
        // The valid first column must not be touched before the bad one is seen.
        TRowSetFixture fx({EKind::Fixed, EKind::Fixed}, 10, true, true, 7);
        const TValueLayout layouts[] = {fx.Layouts()[0], {bad, false}};
        ExpectRejectedUntouched(fx, layouts);
    }
}

TEST(RowSetCompact, InvalidBitmapLayoutIsRejectedBeforeMutation) {
    TRowSetFixture fx({EKind::Fixed, EKind::Bool}, 10, false, false, 7);
    const TValueLayout layouts[] = {fx.Layouts()[0], {1, true}};
    ExpectRejectedUntouched(fx, layouts);
}

TEST(RowSetCompact, WrongLayoutCountIsRejectedBeforeMutation) {
    TRowSetFixture fx({EKind::Fixed, EKind::Fixed}, 10, false, false, 7);
    const TValueLayout layouts[] = {fx.Layouts()[0]};
    ExpectRejectedUntouched(fx, layouts);
}

TEST(RowSetCompact, BadOffsetWidthIsRejectedBeforeMutation) {
    TRowSetFixture fx({EKind::Fixed, EKind::String4}, 10, false, false, 7);
    fx.RowSet().Columns[1].OffsetWidth = 2;
    ExpectRejectedUntouched(fx, fx.Layouts());
}

TEST(RowSetCompact, BitPaddingIsUntouched) {
    std::vector<uint8_t> bits = {0xA5, 0x5A, 0xC3, 0x3C};
    const std::vector<uint8_t> before = bits;
    const uint8_t sel[8] = {0, 1, 0, 1, 0, 1, 0, 1};
    TColumn col{};
    col.Data = reinterpret_cast<char*>(bits.data());
    col.DataBitOffset = 3;
    TRowSet rowSet{};
    rowSet.Columns = &col;
    rowSet.ColumnCount = 1;
    rowSet.RowCount = 8;
    rowSet.Selection = const_cast<uint8_t*>(sel);
    rowSet.RefCount = 1;
    const TValueLayout layouts[] = {{0, true}};
    EXPECT_EQ(CompactRowSet(rowSet, layouts), 4);
    for (int64_t i = 0; i < 32; ++i) {
        const int64_t from = i >= 3 && i < 7 ? 3 + 1 + 2 * (i - 3) : i;
        EXPECT_EQ(Bit(bits.data(), i), Bit(before.data(), from)) << i;
    }
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
