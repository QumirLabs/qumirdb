#pragma once

#include <optional>
#include <string_view>

namespace NQdb {

// The network poller of the daemon; Auto is coroio's TDefaultPoller.
// Which backends exist on the platform is the daemon's business.
enum class EPollerBackend {
    Auto = 0,
    Epoll,
    Uring,
    Kqueue,
    Poll,
    Select,
};

inline constexpr std::string_view PollerBackendNames = "auto, epoll, uring, kqueue, poll, select";

inline std::optional<EPollerBackend> ParsePollerBackend(std::string_view name) {
    if (name == "auto") {
        return EPollerBackend::Auto;
    }
    if (name == "epoll") {
        return EPollerBackend::Epoll;
    }
    if (name == "uring") {
        return EPollerBackend::Uring;
    }
    if (name == "kqueue") {
        return EPollerBackend::Kqueue;
    }
    if (name == "poll") {
        return EPollerBackend::Poll;
    }
    if (name == "select") {
        return EPollerBackend::Select;
    }
    return std::nullopt;
}

} // namespace NQdb
