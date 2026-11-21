#pragma once

#include <optional>
#include <string_view>

namespace NQdb {

// Where generated kernels run: native code from the LLVM JIT, or the Qumir VM.
// Tpde is an experiment: native code from the TPDE backend, AArch64 Linux only.
enum class EKernelBackend {
    Llvm = 0,
    Vm = 1,
    Tpde = 2,
};

// The names the config accepts.
inline constexpr std::string_view KernelBackendNames = "llvm, vm, tpde";

inline std::optional<EKernelBackend> ParseKernelBackend(std::string_view name) {
    if (name == "llvm") {
        return EKernelBackend::Llvm;
    }
    if (name == "vm") {
        return EKernelBackend::Vm;
    }
    if (name == "tpde") {
        return EKernelBackend::Tpde;
    }
    return std::nullopt;
}

} // namespace NQdb
