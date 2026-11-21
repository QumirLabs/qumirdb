#pragma once

#include <optional>
#include <string_view>

namespace NQdb {

// Where generated kernels run: native code from the LLVM JIT, or the Qumir VM.
// Tpde is native code from the TPDE backend of Qumir, in QDB_ENABLE_TPDE builds.
enum class EKernelBackend {
    Llvm = 0,
    Vm = 1,
#ifdef QUMIR_ENABLE_TPDE
    Tpde = 2,
#endif
};

// The names the config accepts.
#ifdef QUMIR_ENABLE_TPDE
inline constexpr std::string_view KernelBackendNames = "llvm, vm, tpde";
#else
inline constexpr std::string_view KernelBackendNames = "llvm, vm";
#endif

inline std::optional<EKernelBackend> ParseKernelBackend(std::string_view name) {
    if (name == "llvm") {
        return EKernelBackend::Llvm;
    }
    if (name == "vm") {
        return EKernelBackend::Vm;
    }
#ifdef QUMIR_ENABLE_TPDE
    if (name == "tpde") {
        return EKernelBackend::Tpde;
    }
#endif
    return std::nullopt;
}

} // namespace NQdb
