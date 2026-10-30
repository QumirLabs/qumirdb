#include <qdb/io/text/sink.h>
#include <qdb/plan/types/decimal.h>
#include <qdb/plan/types/nullable.h>

#include <algorithm>
#include <charconv>
#include <cstring>
#include <iomanip>
#include <cstdio>
#include <string_view>
#include <string>

#include <qumir/parser/type.h>

namespace NQdb {

namespace {

using namespace NQumir::NAst;

bool IsMasked(const TColumn& col, int32_t row) {
    if (!col.Mask) {
        return false;
    }
    const int64_t bitIndex = static_cast<int64_t>(col.MaskBitOffset) + row;
    const uint8_t byte = col.Mask[bitIndex >> 3];
    return ((byte >> (bitIndex & 7)) & 1) == 0;
}

bool IsBitSet(const TColumn& col, int32_t row) {
    const auto* bits = reinterpret_cast<const uint8_t*>(col.Data);
    const int64_t bitIndex = static_cast<int64_t>(col.DataBitOffset) + row;
    return ((bits[bitIndex >> 3] >> (bitIndex & 7)) & 1) != 0;
}

template <typename TOffset>
std::string_view StringViewValue(const TColumn& col, int32_t row) {
    const auto* offsets = static_cast<const TOffset*>(col.Offsets);
    const TOffset base = offsets[0];
    const TOffset start = offsets[row] - base;
    const TOffset end = offsets[row + 1] - base;
    return std::string_view(col.Data + start, static_cast<size_t>(end - start));
}

template <typename TOffset>
void CsvWriteEscapedView(std::ostream& out, std::string_view view, char sep) {
    bool needsQuoting = view.find(sep) != std::string_view::npos
        || view.find('"') != std::string_view::npos
        || view.find('\n') != std::string_view::npos;
    if (!needsQuoting) {
        out.write(view.data(), static_cast<std::streamsize>(view.size()));
        return;
    }
    out.put('"');
    for (char c : view) {
        if (c == '"') {
            out.write("\"\"", 2);
        } else {
            out.put(c);
        }
    }
    out.put('"');
}

template <typename T>
std::string_view ToCharsView(std::ostream& out, T value, char* buffer, size_t size) {
    auto [ptr, ec] = std::to_chars(buffer, buffer + size, value);
    if (ec == std::errc{}) {
        return std::string_view(buffer, static_cast<size_t>(ptr - buffer));
    }
    auto tmp = std::snprintf(buffer, size, "%g", static_cast<double>(value));
    if (tmp < 0) {
        return {};
    }
    return std::string_view(buffer, static_cast<size_t>(tmp));
}

std::string UnsignedInt128ToString(unsigned __int128 value) {
    if (value == 0) {
        return "0";
    }
    std::string out;
    while (value != 0) {
        const unsigned digit = static_cast<unsigned>(value % 10);
        out.push_back(static_cast<char>('0' + digit));
        value /= 10;
    }
    std::reverse(out.begin(), out.end());
    return out;
}

std::string FormatDecimal128(const char* data, int32_t scale) {
    uint64_t lo = 0;
    uint64_t hi = 0;
    std::memcpy(&lo, data, sizeof(lo));
    std::memcpy(&hi, data + sizeof(lo), sizeof(hi));
    unsigned __int128 raw = static_cast<unsigned __int128>(lo) |
        (static_cast<unsigned __int128>(hi) << 64);
    const bool negative = (hi >> 63) != 0;
    unsigned __int128 magnitude = negative ? (~raw + 1) : raw;
    std::string digits = UnsignedInt128ToString(magnitude);
    if (scale > 0) {
        if (digits.size() <= static_cast<size_t>(scale)) {
            digits.insert(digits.begin(),
                static_cast<size_t>(scale) + 1 - digits.size(), '0');
        }
        digits.insert(digits.end() - scale, '.');
    }
    if (negative) {
        digits.insert(digits.begin(), '-');
    }
    return digits;
}

std::string FormatCell(const TColumn& col, int32_t row, const TTypePtr& type) {
    if (IsMasked(col, row)) {
        return "NULL";
    }
    if (!type) {
        return "?";
    }
    char buf[64];
    const auto valueType = UnwrapNullableType(type);
    if (auto decimal = DecimalSpecOfValueType(valueType)) {
        return FormatDecimal128(col.Data + row * 16, decimal->Scale);
    }
    if (auto t = TMaybeType<TIntegerType>(valueType)) {
        auto intType = t.Cast();
        const char* ptr = col.Data + row * (intType->BitWidth() / 8);
        std::to_chars_result r;
        if (intType->IsSigned()) {
            int64_t v = 0;
            switch (intType->BitWidth()) {
                case 8:  v = *reinterpret_cast<const int8_t*>(ptr); break;
                case 16: v = *reinterpret_cast<const int16_t*>(ptr); break;
                case 32: v = *reinterpret_cast<const int32_t*>(ptr); break;
                case 64: v = *reinterpret_cast<const int64_t*>(ptr); break;
            }
            r = std::to_chars(buf, buf + sizeof(buf), v);
        } else {
            uint64_t v = 0;
            switch (intType->BitWidth()) {
                case 8:  v = *reinterpret_cast<const uint8_t*>(ptr); break;
                case 16: v = *reinterpret_cast<const uint16_t*>(ptr); break;
                case 32: v = *reinterpret_cast<const uint32_t*>(ptr); break;
                case 64: v = *reinterpret_cast<const uint64_t*>(ptr); break;
            }
            r = std::to_chars(buf, buf + sizeof(buf), v);
        }
        return std::string(buf, r.ptr);
    }
    if (TMaybeType<TFloatType>(valueType)) {
        double v = reinterpret_cast<const double*>(col.Data)[row];
        auto r = std::to_chars(buf, buf + sizeof(buf), v);
        return std::string(buf, r.ptr);
    }
    if (TMaybeType<TBoolType>(valueType)) {
        return IsBitSet(col, row) ? "true" : "false";
    }
    if (TMaybeType<TStringType>(valueType)) {
        if (col.OffsetWidth == 4) {
            auto sv = StringViewValue<int32_t>(col, row);
            return std::string(sv);
        }
        auto sv = StringViewValue<int64_t>(col, row);
        return std::string(sv);
    }
    if (TMaybeType<TSymbolType>(valueType)) {
        return std::string(1, col.Data[row]);
    }
    return "?";
}

template <typename TOffset>
void WriteValue(std::ostream& out, const TColumn& col, int32_t row, const TTypePtr& type) {
    if (IsMasked(col, row)) {
        out << "NULL";
        return;
    }
    if (!type) {
        out << '?';
        return;
    }

    const auto valueType = UnwrapNullableType(type);
    if (auto decimal = DecimalSpecOfValueType(valueType)) {
        const auto formatted = FormatDecimal128(col.Data + row * 16, decimal->Scale);
        out.write(formatted.data(), static_cast<std::streamsize>(formatted.size()));
        return;
    }
    if (auto t = TMaybeType<TIntegerType>(valueType)) {
        auto intType = t.Cast();
        const char* ptr = col.Data + row * (intType->BitWidth() / 8);
        char buffer[64];
        switch (intType->BitWidth()) {
            case 8:
                if (intType->IsSigned()) {
                    auto view = ToCharsView(out, static_cast<int64_t>(*reinterpret_cast<const int8_t*>(ptr)), buffer, sizeof(buffer));
                    out.write(view.data(), static_cast<std::streamsize>(view.size()));
                } else {
                    auto view = ToCharsView(out, static_cast<uint64_t>(*reinterpret_cast<const uint8_t*>(ptr)), buffer, sizeof(buffer));
                    out.write(view.data(), static_cast<std::streamsize>(view.size()));
                }
                return;
            case 16:
                if (intType->IsSigned()) {
                    auto view = ToCharsView(out, static_cast<int64_t>(*reinterpret_cast<const int16_t*>(ptr)), buffer, sizeof(buffer));
                    out.write(view.data(), static_cast<std::streamsize>(view.size()));
                } else {
                    auto view = ToCharsView(out, static_cast<uint64_t>(*reinterpret_cast<const uint16_t*>(ptr)), buffer, sizeof(buffer));
                    out.write(view.data(), static_cast<std::streamsize>(view.size()));
                }
                return;
            case 32:
                if (intType->IsSigned()) {
                    auto view = ToCharsView(out, static_cast<int64_t>(*reinterpret_cast<const int32_t*>(ptr)), buffer, sizeof(buffer));
                    out.write(view.data(), static_cast<std::streamsize>(view.size()));
                } else {
                    auto view = ToCharsView(out, static_cast<uint64_t>(*reinterpret_cast<const uint32_t*>(ptr)), buffer, sizeof(buffer));
                    out.write(view.data(), static_cast<std::streamsize>(view.size()));
                }
                return;
            case 64:
                if (intType->IsSigned()) {
                    auto view = ToCharsView(out, static_cast<int64_t>(*reinterpret_cast<const int64_t*>(ptr)), buffer, sizeof(buffer));
                    out.write(view.data(), static_cast<std::streamsize>(view.size()));
                } else {
                    auto view = ToCharsView(out, static_cast<uint64_t>(*reinterpret_cast<const uint64_t*>(ptr)), buffer, sizeof(buffer));
                    out.write(view.data(), static_cast<std::streamsize>(view.size()));
                }
                return;
        }
    } else if (TMaybeType<TFloatType>(valueType)) {
        char buffer[64];
        auto view = ToCharsView(out, reinterpret_cast<const double*>(col.Data)[row], buffer, sizeof(buffer));
        out.write(view.data(), static_cast<std::streamsize>(view.size()));
        return;
    } else if (TMaybeType<TBoolType>(valueType)) {
        out << (IsBitSet(col, row) ? "true" : "false");
        return;
    } else if (TMaybeType<TStringType>(valueType)) {
        if constexpr (std::is_same_v<TOffset, int32_t>) {
            CsvWriteEscapedView<int32_t>(out, StringViewValue<int32_t>(col, row), ',');
        } else {
            CsvWriteEscapedView<int64_t>(out, StringViewValue<int64_t>(col, row), ',');
        }
        return;
    } else if (TMaybeType<TSymbolType>(valueType)) {
        out.put(col.Data[row]);
        return;
    }

    out << '?';
}

bool IsNumeric(const TTypePtr& type) {
    const auto valueType = UnwrapNullableType(type);
    return IsDecimalType(valueType) ||
           static_cast<bool>(TMaybeType<TIntegerType>(valueType)) ||
           static_cast<bool>(TMaybeType<TFloatType>(valueType));
}

template <typename TOffset>
void WriteCsvString(std::ostream& out, const TColumn& col, int32_t row, char sep) {
    const auto view = StringViewValue<TOffset>(col, row);
    bool needsQuoting = view.find(sep) != std::string_view::npos
        || view.find('"') != std::string_view::npos
        || view.find('\n') != std::string_view::npos;
    if (!needsQuoting) {
        out.write(view.data(), static_cast<std::streamsize>(view.size()));
        return;
    }
    out.put('"');
    for (char c : view) {
        if (c == '"') {
            out.write("\"\"", 2);
        } else {
            out.put(c);
        }
    }
    out.put('"');
}

template <typename TOffset>
void WriteCsvStringRaw(std::ostream& out, const TColumn& col, int32_t row) {
    const auto view = StringViewValue<TOffset>(col, row);
    out.write(view.data(), static_cast<std::streamsize>(view.size()));
}

} // namespace

// TConsoleSink

TConsoleSink::TConsoleSink(const TSchema& schema, std::ostream& out)
    : Schema_(schema)
    , Out_(out)
{
    Widths_.resize(schema.Columns.size());
    for (size_t i = 0; i < schema.Columns.size(); ++i) {
        Widths_[i] = schema.Columns[i].Name.size();
    }
}

void TConsoleSink::Write(const TRowSet& rowSet) {
    for (int32_t row = 0; row < rowSet.RowCount; ++row) {
        if (rowSet.Selection && !rowSet.Selection[row]) {
            continue;
        }
        std::vector<std::string> formatted(rowSet.ColumnCount);
        for (int32_t c = 0; c < rowSet.ColumnCount; ++c) {
            formatted[c] = FormatCell(rowSet.Columns[c], row, Schema_.Columns[c].Type);
            if (formatted[c].size() > Widths_[c]) {
                Widths_[c] = formatted[c].size();
            }
        }
        Rows_.push_back(std::move(formatted));
    }
}

void TConsoleSink::Flush() {
    size_t numCols = Schema_.Columns.size();

    for (size_t c = 0; c < numCols; ++c) {
        if (c > 0) {
            Out_ << " | ";
        }
        Out_ << std::left << std::setw(static_cast<int>(Widths_[c])) << Schema_.Columns[c].Name;
    }
    Out_ << "\n";

    for (size_t c = 0; c < numCols; ++c) {
        if (c > 0) {
            Out_ << "-+-";
        }
        Out_ << std::string(Widths_[c], '-');
    }
    Out_ << "\n";

    for (const auto& row : Rows_) {
        for (size_t c = 0; c < numCols; ++c) {
            if (c > 0) {
                Out_ << " | ";
            }
            if (IsNumeric(Schema_.Columns[c].Type)) {
                Out_ << std::right << std::setw(static_cast<int>(Widths_[c])) << row[c];
            } else {
                Out_ << std::left << std::setw(static_cast<int>(Widths_[c])) << row[c];
            }
        }
        Out_ << "\n";
    }
}

// TCsvSink

TCsvSink::TCsvSink(const TSchema& schema, std::ostream& out, char separator, bool noEscape)
    : Schema_(schema)
    , Out_(out)
    , Separator_(separator)
    , NoEscape_(noEscape)
{}

void TCsvSink::Write(const TRowSet& rowSet) {
    if (!HeaderPrinted_) {
        for (size_t c = 0; c < Schema_.Columns.size(); ++c) {
            if (c > 0) {
                Out_ << Separator_;
            }
            Out_ << Schema_.Columns[c].Name;
        }
        Out_ << "\n";
        HeaderPrinted_ = true;
    }

    for (int32_t row = 0; row < rowSet.RowCount; ++row) {
        if (rowSet.Selection && !rowSet.Selection[row]) {
            continue;
        }
        for (int32_t c = 0; c < rowSet.ColumnCount; ++c) {
            if (c > 0) {
                Out_ << Separator_;
            }
            const auto& col = rowSet.Columns[c];
            const auto type = UnwrapNullableType(Schema_.Columns[c].Type);
            if (NoEscape_ && TMaybeType<TStringType>(type)) {
                if (col.OffsetWidth == 4) {
                    WriteCsvStringRaw<int32_t>(Out_, col, row);
                } else {
                    WriteCsvStringRaw<int64_t>(Out_, col, row);
                }
            } else if (TMaybeType<TStringType>(type)) {
                if (col.OffsetWidth == 4) {
                    WriteCsvString<int32_t>(Out_, col, row, Separator_);
                } else {
                    WriteCsvString<int64_t>(Out_, col, row, Separator_);
                }
            } else {
                if (TMaybeType<TStringType>(type)) {
                    if (col.OffsetWidth == 4) {
                        WriteCsvString<int32_t>(Out_, col, row, Separator_);
                    } else {
                        WriteCsvString<int64_t>(Out_, col, row, Separator_);
                    }
                } else {
                    WriteValue<int64_t>(Out_, col, row, type);
                }
            }
        }
        Out_ << "\n";
    }
}

void TNullSink::Write(const TRowSet& rowSet) {
    Rows_ += rowSet.RowCount;
}

} // namespace NQdb
