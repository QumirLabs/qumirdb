#pragma once

#include <optional>
#include <string_view>

namespace NQdb {

// How the daemon reads inbound frames: V1 is coroio's TZeroCopyEnvelopeReader,
// V2 its TZeroCopyEnvelopeReaderV2.
enum class EEnvelopeReader {
    V1 = 0,
    V2,
};

inline constexpr std::string_view EnvelopeReaderNames = "v1, v2";

inline std::optional<EEnvelopeReader> ParseEnvelopeReader(std::string_view name) {
    if (name == "v1") {
        return EEnvelopeReader::V1;
    }
    if (name == "v2") {
        return EEnvelopeReader::V2;
    }
    return std::nullopt;
}

} // namespace NQdb
