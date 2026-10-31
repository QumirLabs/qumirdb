#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace NQumir {
namespace NIR {
struct TFunction;
} // namespace NIR
} // namespace NQumir

namespace NQdb {

struct TVmProgram;

// Mutable execution state of one worker. The worker owns it and passes it down
// to every kernel call; tasks never keep it, so a task may resume on another
// worker.
class TExecutionContext {
public:
    TExecutionContext();
    ~TExecutionContext();

    TExecutionContext(const TExecutionContext&) = delete;
    TExecutionContext& operator=(const TExecutionContext&) = delete;

    // Calls a prepared function on this worker's VM instance of `program`,
    // created on first use. Throws on a VM error.
    int64_t Invoke(
        const std::shared_ptr<TVmProgram>& program,
        NQumir::NIR::TFunction& function,
        std::vector<int64_t> args);

private:
    struct TInstance;
    std::vector<std::unique_ptr<TInstance>> Instances_;
};

} // namespace NQdb
