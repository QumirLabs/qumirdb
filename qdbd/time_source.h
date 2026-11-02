#pragma once

#include <cstdint>
#include <coroio/base.hpp>

namespace NQdb {
namespace NRemote {

struct ITimeSource {
    virtual uint64_t CurrentTime() const = 0;
};

struct TTimeSource : ITimeSource {
    uint64_t CurrentTime() const override {
        return std::chrono::duration_cast<std::chrono::microseconds>(NNet::TClock::now().time_since_epoch()).count();
    }
};

} // namespace NRemote
} // namespace NQdb
