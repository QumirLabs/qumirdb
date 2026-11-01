#pragma once

#include <optional>
#include <string_view>

namespace NQdb {

// Where generated kernels run: native code from the LLVM JIT, or the Qumir VM.
enum class EKernelBackend {
    Llvm = 0,
    Vm = 1,
};

// The names the config accepts.
inline constexpr std::string_view KernelBackendNames = "llvm, vm";

inline std::optional<EKernelBackend> ParseKernelBackend(std::string_view name) {
    if (name == "llvm") {
        return EKernelBackend::Llvm;
    }
    if (name == "vm") {
        return EKernelBackend::Vm;
    }
    return std::nullopt;
}

} // namespace NQdb
