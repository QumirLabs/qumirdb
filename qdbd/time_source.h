#pragma once

#include <coroio/base.hpp>

namespace NQdb {
namespace NRemote {

struct ITimeSource {
    virtual ~ITimeSource() = default;
    virtual NNet::TTime CurrentTime() const = 0;
};

struct TTimeSource : ITimeSource {
    NNet::TTime CurrentTime() const override {
        return NNet::TClock::now();
    }
};

} // namespace NRemote
} // namespace NQdb
