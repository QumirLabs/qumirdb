#include <qdb/io/parquet/decimal_storage.h>
#include <qdb/plan/types/decimal.h>

#include <cstring>
#include <stdexcept>

namespace NQdb {

namespace {

__int128 LoadUnscaled(const uint8_t* data, int32_t width) {
    switch (width) {
        case 4: {
            int32_t value;
            std::memcpy(&value, data, sizeof(value));
            return value;
        }
        case 8: {
            int64_t value;
            std::memcpy(&value, data, sizeof(value));
            return value;
        }
        case 16: {
            __int128 value;
            std::memcpy(&value, data, sizeof(value));
            return value;
        }
    }
    throw std::runtime_error("unsupported decimal width: " + std::to_string(width));
}

} // namespace

std::shared_ptr<arrow::Buffer> DecimalStorageValues(
    const arrow::Array& array,
    arrow::MemoryPool* memoryPool)
{
    const auto& type = static_cast<const arrow::DecimalType&>(*array.type());
    const int32_t from = type.byte_width();
    const int32_t to = DecimalStorageBytes({
        .Precision = type.precision(),
        .Scale = type.scale(),
    });
    const auto& values = array.data()->buffers[1];
    if (from == to) {
        return arrow::SliceBuffer(values, array.offset() * from, array.length() * from);
    }
    auto allocated = arrow::AllocateBuffer(array.length() * to, memoryPool);
    if (!allocated.ok()) {
        throw std::runtime_error(allocated.status().ToString());
    }
    std::shared_ptr<arrow::Buffer> converted = std::move(*allocated);
    const uint8_t* src = values->data() + array.offset() * from;
    uint8_t* dst = converted->mutable_data();
    // Little-endian: the low `to` bytes of the sign-extended value are its narrower form.
    for (int64_t i = 0; i < array.length(); ++i) {
        const __int128 value = LoadUnscaled(src + i * from, from);
        std::memcpy(dst + i * to, &value, static_cast<size_t>(to));
    }
    return converted;
}

} // namespace NQdb
