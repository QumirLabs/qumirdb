#pragma once

#include <qdb/exec/stage.h>
#include <qdb/kernel/execution_context.h>

#include <qumir/parser/ast.h>

#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace NQumir {
class TLLVMRunner;
} // namespace NQumir

namespace NQdb {

class TRuntimeContext;

// Kernel ABI: arguments are pointers, integers up to 64 bits or bool; the
// result is void, an integer or bool.
template <typename T>
constexpr bool IsKernelArg() {
    if constexpr (std::is_pointer_v<T>) {
        return true;
    } else if constexpr (std::is_integral_v<T>) {
        return sizeof(T) <= sizeof(int64_t);
    } else {
        return false;
    }
}

template <typename T>
constexpr bool IsKernelResult() {
    return std::is_void_v<T> || IsKernelArg<T>();
}

template <typename>
inline constexpr bool IsKernelSignature = false;

template <typename R, typename... TArgs>
inline constexpr bool IsKernelSignature<R (*)(TArgs...)> =
    IsKernelResult<R>() && (IsKernelArg<TArgs>() && ...);

// Machine binding for one generated kernel program. Created empty at
// generation time; filled exactly once by the JIT finalizer.
struct TKernelSlot {
    // Parallel to TGeneratedKernel::Entrypoints; nullptr until bound.
    std::vector<void*> Fns;
    // Owns the JIT'd code: the pointers are valid while this lives.
    std::shared_ptr<NQumir::TLLVMRunner> Runner;
    // Cache path only: keeps the query's LLJIT alive (the runner does not).
    std::shared_ptr<void> JitLifetime;

    // Calls entrypoint `index` on the calling thread; TFn is its native
    // function pointer type.
    template <typename TFn, typename... TArgs>
    auto Call(
        [[maybe_unused]] TExecutionContext& context, size_t index, TArgs... args) const
    {
        static_assert(IsKernelSignature<TFn>, "kernel signature violates the kernel ABI");
        return reinterpret_cast<TFn>(Fns[index])(args...);
    }
};

// One kernel program attached to the physical plan as an AST. A finalizer
// then either JITs it (fills Slot) or compiles it to wasm.
struct TGeneratedKernel {
    std::string Name;                       // "filter", "join", "aggregate.update", ...
    std::string Stage;                      // diagnostic label; never an identity key
    TExecStageId ExecStageId = InvalidExecStageId;
    std::vector<std::string> Entrypoints;
    NQumir::NAst::TExprPtr Ast;
    // Payloads the AST points into (e.g. filter literal strings).
    std::shared_ptr<void> Storage;
    std::shared_ptr<TRuntimeContext> RuntimeContext;
    std::shared_ptr<TKernelSlot> Slot;
    bool ExportArtifacts = true;

    // Sort kernels only: resolved key metadata retained with the lowered plan.
    struct TSortKeyMeta {
        int32_t Index = 0;
        int32_t WidthBytes = 0;
        bool IsString = false;
        bool Desc = false;
    };
    std::vector<TSortKeyMeta> SortKeys;

    // Aggregate kernels only: output layout retained with the lowered plan.
    // Agg-output nullability is a kernel property (sum/min/max over a nullable
    // argument) that the plan's output schema does not carry.
    struct TAggKeyMeta {
        bool IsString = false;
        bool IsNullable = false;
    };
    struct TAggValueMeta {
        bool IsNullable = false;
    };
    std::vector<int64_t> AggStringMetadataBuffers;
    std::vector<TAggKeyMeta> AggKeys;
    std::vector<TAggValueMeta> AggValues;
};

} // namespace NQdb
