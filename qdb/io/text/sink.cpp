#include <qdb/io/text/sink.h>
#include <qdb/io/row_layouts.h>
#include <qdb/plan/types/date.h>

#include <algorithm>
#include <charconv>
#include <cstring>
#include <iomanip>
#include <cstdio>
#include <string_view>
#include <string>

namespace NQdb {

namespace {

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

// Shortest text that reads back as the same value in the column's float width.
std::to_chars_result FloatToChars(char* first, char* last, const TColumn& col, int32_t row, int32_t width)
{
    if (width == sizeof(float)) {
        float value;
        std::memcpy(&value, col.Data + static_cast<int64_t>(row) * sizeof(float), sizeof(float));
        return std::to_chars(first, last, value);
    }
    double value;
    std::memcpy(&value, col.Data + static_cast<int64_t>(row) * sizeof(double), sizeof(double));
    return std::to_chars(first, last, value);
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

std::string FormatInt128(const char* data, bool isSigned) {
    if (isSigned) {
        return FormatDecimal128(data, 0);
    }
    unsigned __int128 raw = 0;
    std::memcpy(&raw, data, sizeof(raw));
    return UnsignedInt128ToString(raw);
}

// A DECIMAL cell holds its unscaled value at the storage width of its type.
std::string FormatDecimalCell(const TColumn& col, int32_t row, const TValueType& value) {
    const char* data = col.Data + static_cast<int64_t>(row) * value.Width;
    if (value.Width == sizeof(int64_t)) {
        int64_t unscaled = 0;
        std::memcpy(&unscaled, data, sizeof(unscaled));
        const __int128 wide = unscaled;
        return FormatDecimal128(reinterpret_cast<const char*>(&wide), value.Scale);
    }
    return FormatDecimal128(data, value.Scale);
}

int64_t SignedCell(const TColumn& col, int32_t row, int32_t width) {
    const char* ptr = col.Data + static_cast<int64_t>(row) * width;
    switch (width) {
        case 1:
            return *reinterpret_cast<const int8_t*>(ptr);
        case 2:
            return *reinterpret_cast<const int16_t*>(ptr);
        case 4:
            return *reinterpret_cast<const int32_t*>(ptr);
    }
    return *reinterpret_cast<const int64_t*>(ptr);
}

uint64_t UnsignedCell(const TColumn& col, int32_t row, int32_t width) {
    const char* ptr = col.Data + static_cast<int64_t>(row) * width;
    switch (width) {
        case 1:
            return *reinterpret_cast<const uint8_t*>(ptr);
        case 2:
            return *reinterpret_cast<const uint16_t*>(ptr);
        case 4:
            return *reinterpret_cast<const uint32_t*>(ptr);
    }
    return *reinterpret_cast<const uint64_t*>(ptr);
}

bool IsInteger(const TValueType& value) {
    return value.Kind == EValueKind::Signed || value.Kind == EValueKind::Unsigned;
}

std::string FormatInt128Cell(const TColumn& col, int32_t row, const TValueType& value) {
    return FormatInt128(col.Data + static_cast<int64_t>(row) * value.Width, value.Kind == EValueKind::Signed);
}

std::string FormatCell(const TColumn& col, int32_t row, const std::optional<TValueType>& value) {
    if (IsMasked(col, row)) {
        return "NULL";
    }
    if (!value) {
        return "?";
    }
    char buf[64];
    switch (value->Kind) {
        case EValueKind::Decimal:
            return FormatDecimalCell(col, row, *value);
        case EValueKind::Date:
            return FormatDate(SignedCell(col, row, value->Width));
        case EValueKind::Signed:
        case EValueKind::Unsigned: {
            if (value->Width == 16) {
                return FormatInt128Cell(col, row, *value);
            }
            const auto r = value->Kind == EValueKind::Signed
                ? std::to_chars(buf, buf + sizeof(buf), SignedCell(col, row, value->Width))
                : std::to_chars(buf, buf + sizeof(buf), UnsignedCell(col, row, value->Width));
            return std::string(buf, r.ptr);
        }
        case EValueKind::Float: {
            const auto r = FloatToChars(buf, buf + sizeof(buf), col, row, value->Width);
            return std::string(buf, r.ptr);
        }
        case EValueKind::Bool:
            return IsBitSet(col, row) ? "true" : "false";
        case EValueKind::String:
            if (col.OffsetWidth == 4) {
                return std::string(StringViewValue<int32_t>(col, row));
            }
            return std::string(StringViewValue<int64_t>(col, row));
    }
    return "?";
}

void WriteText(std::ostream& out, std::string_view text) {
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

template <typename TOffset>
void WriteValue(std::ostream& out, const TColumn& col, int32_t row, const std::optional<TValueType>& value) {
    if (IsMasked(col, row)) {
        out << "NULL";
        return;
    }
    if (!value) {
        out << '?';
        return;
    }

    char buffer[64];
    switch (value->Kind) {
        case EValueKind::Decimal:
            WriteText(out, FormatDecimalCell(col, row, *value));
            return;
        case EValueKind::Date:
            WriteText(out, FormatDate(SignedCell(col, row, value->Width)));
            return;
        case EValueKind::Signed:
        case EValueKind::Unsigned:
            if (value->Width == 16) {
                WriteText(out, FormatInt128Cell(col, row, *value));
            } else if (value->Kind == EValueKind::Signed) {
                WriteText(out, ToCharsView(out, SignedCell(col, row, value->Width), buffer, sizeof(buffer)));
            } else {
                WriteText(out, ToCharsView(out, UnsignedCell(col, row, value->Width), buffer, sizeof(buffer)));
            }
            return;
        case EValueKind::Float: {
            const auto result = FloatToChars(buffer, buffer + sizeof(buffer), col, row, value->Width);
            out.write(buffer, result.ptr - buffer);
            return;
        }
        case EValueKind::Bool:
            out << (IsBitSet(col, row) ? "true" : "false");
            return;
        case EValueKind::String:
            CsvWriteEscapedView<TOffset>(out, StringViewValue<TOffset>(col, row), ',');
            return;
    }
    out << '?';
}

bool IsNumeric(const std::optional<TValueType>& value) {
    return value && (IsInteger(*value) || value->Kind == EValueKind::Float || value->Kind == EValueKind::Decimal);
}

std::vector<std::optional<TValueType>> ColumnValueTypes(const TSchema& schema) {
    std::vector<std::optional<TValueType>> values;
    values.reserve(schema.Columns.size());
    for (const auto& column : schema.Columns) {
        values.push_back(ValueTypeOf(column.Type));
    }
    return values;
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
    , Values_(ColumnValueTypes(schema))
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
            formatted[c] = FormatCell(rowSet.Columns[c], row, Values_[c]);
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
            if (IsNumeric(Values_[c])) {
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
    , Values_(ColumnValueTypes(schema))
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
            const auto& value = Values_[c];
            const bool isString = value && value->Kind == EValueKind::String;
            if (NoEscape_ && isString) {
                if (col.OffsetWidth == 4) {
                    WriteCsvStringRaw<int32_t>(Out_, col, row);
                } else {
                    WriteCsvStringRaw<int64_t>(Out_, col, row);
                }
            } else if (isString) {
                if (col.OffsetWidth == 4) {
                    WriteCsvString<int32_t>(Out_, col, row, Separator_);
                } else {
                    WriteCsvString<int64_t>(Out_, col, row, Separator_);
                }
            } else {
                WriteValue<int64_t>(Out_, col, row, value);
            }
        }
        Out_ << "\n";
    }
}

void TNullSink::Write(const TRowSet& rowSet) {
    Rows_ += rowSet.RowCount;
}

} // namespace NQdb
