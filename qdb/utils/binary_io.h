#pragma once

#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace NQdb {

// A record exposes `void Fields(this auto& self, auto& v)` that calls
// v(self.Member) for each member in order; the same function drives writing
// and reading. Integers are little-endian; sizes and counts are u32.
// TODO: no versioning or compatibility checks yet.
class TBinaryWriter {
public:
    static constexpr bool Reading = false;

    void U8(uint8_t value)
    {
        Bytes_.push_back(static_cast<char>(value));
    }

    void U32(uint32_t value)
    {
        Raw(value);
    }

    void U64(uint64_t value)
    {
        Raw(value);
    }

    void I64(int64_t value)
    {
        Raw(value);
    }

    void String(std::string_view value)
    {
        U32(static_cast<uint32_t>(value.size()));
        Bytes_.append(value);
    }

    std::string Take()
    {
        return std::move(Bytes_);
    }

    template <class T>
    void operator()(const T& value)
    {
        if constexpr (std::is_same_v<T, bool>) {
            U8(value ? 1 : 0);
        } else if constexpr (std::is_enum_v<T>) {
            U8(static_cast<uint8_t>(value));
        } else if constexpr (std::is_same_v<T, uint8_t>) {
            U8(value);
        } else if constexpr (std::is_same_v<T, uint32_t>) {
            U32(value);
        } else if constexpr (std::is_same_v<T, uint64_t>) {
            U64(value);
        } else if constexpr (std::is_same_v<T, int64_t>) {
            I64(value);
        } else if constexpr (std::is_same_v<T, std::string>) {
            String(value);
        } else {
            WriteComposite(value);
        }
    }

private:
    template <class T>
    void Raw(T value)
    {
        char buffer[sizeof(T)];
        std::memcpy(buffer, &value, sizeof(T));
        Bytes_.append(buffer, sizeof(T));
    }

    template <class T>
    void WriteComposite(const std::vector<T>& values)
    {
        U32(static_cast<uint32_t>(values.size()));
        for (const auto& value : values) {
            (*this)(value);
        }
    }

    template <class T>
    void WriteComposite(const std::optional<T>& value)
    {
        U8(value.has_value() ? 1 : 0);
        if (value) {
            (*this)(*value);
        }
    }

    template <class... Ts>
    void WriteComposite(const std::variant<Ts...>& value)
    {
        U8(static_cast<uint8_t>(value.index()));
        std::visit([this](const auto& alternative) { (*this)(alternative); }, value);
    }

    template <class T>
    void WriteComposite(const T& record)
    {
        record.Fields(*this);
    }

    std::string Bytes_;
};

class TBinaryReader {
public:
    static constexpr bool Reading = true;

    explicit TBinaryReader(std::string_view bytes)
        : Bytes_(bytes)
    {}

    uint8_t U8()
    {
        return Raw<uint8_t>();
    }

    uint32_t U32()
    {
        return Raw<uint32_t>();
    }

    uint64_t U64()
    {
        return Raw<uint64_t>();
    }

    int64_t I64()
    {
        return Raw<int64_t>();
    }

    std::string String()
    {
        const uint32_t size = U32();
        Need(size);
        std::string value(Bytes_.substr(Offset_, size));
        Offset_ += size;
        return value;
    }

    bool AtEnd() const
    {
        return Offset_ == Bytes_.size();
    }

    template <class T>
    void operator()(T& value)
    {
        if constexpr (std::is_same_v<T, bool>) {
            value = U8() != 0;
        } else if constexpr (std::is_enum_v<T>) {
            value = static_cast<T>(U8());
        } else if constexpr (std::is_same_v<T, uint8_t>) {
            value = U8();
        } else if constexpr (std::is_same_v<T, uint32_t>) {
            value = U32();
        } else if constexpr (std::is_same_v<T, uint64_t>) {
            value = U64();
        } else if constexpr (std::is_same_v<T, int64_t>) {
            value = I64();
        } else if constexpr (std::is_same_v<T, std::string>) {
            value = String();
        } else {
            ReadComposite(value);
        }
    }

private:
    void Need(size_t size) const
    {
        if (Bytes_.size() - Offset_ < size) {
            throw std::runtime_error("binary input is truncated");
        }
    }

    template <class T>
    T Raw()
    {
        Need(sizeof(T));
        T value;
        std::memcpy(&value, Bytes_.data() + Offset_, sizeof(T));
        Offset_ += sizeof(T);
        return value;
    }

    template <class T>
    void ReadComposite(std::vector<T>& values)
    {
        const uint32_t count = U32();
        // Every element takes at least one byte, so a larger count is corrupt.
        Need(count);
        values.clear();
        values.resize(count);
        for (auto& value : values) {
            (*this)(value);
        }
    }

    template <class T>
    void ReadComposite(std::optional<T>& value)
    {
        if (U8() == 0) {
            value.reset();
            return;
        }
        value.emplace();
        (*this)(*value);
    }

    template <class... Ts>
    void ReadComposite(std::variant<Ts...>& value)
    {
        EmplaceAlternative<0>(value, U8());
        std::visit([this](auto& alternative) { (*this)(alternative); }, value);
    }

    template <size_t I, class... Ts>
    void EmplaceAlternative(std::variant<Ts...>& value, size_t index)
    {
        if constexpr (I < sizeof...(Ts)) {
            if (index == I) {
                value.template emplace<I>();
            } else {
                EmplaceAlternative<I + 1>(value, index);
            }
        } else {
            throw std::runtime_error("binary input has an unknown variant tag");
        }
    }

    template <class T>
    void ReadComposite(T& record)
    {
        record.Fields(*this);
    }

    std::string_view Bytes_;
    size_t Offset_ = 0;
};

} // namespace NQdb
