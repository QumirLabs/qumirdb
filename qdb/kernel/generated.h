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
namespace NIR {
struct TFunction;
} // namespace NIR
} // namespace NQumir

namespace NQdb {

class TRuntimeContext;
struct TVmProgram;

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

// The VM passes every argument and result as one 64-bit word.
template <typename T>
int64_t ToKernelWord(T value) {
    if constexpr (std::is_pointer_v<T>) {
        return reinterpret_cast<intptr_t>(value);
    } else {
        return static_cast<int64_t>(value);
    }
}

template <typename R, typename... TParams, typename... TArgs>
std::vector<int64_t> ToKernelWords(R (*)(TParams...), TArgs... args) {
    return {ToKernelWord<TParams>(args)...};
}

template <typename T>
T FromKernelWord(int64_t word) {
    if constexpr (std::is_same_v<T, bool>) {
        // Only the low byte of a bool register is defined.
        return (word & 0xff) != 0;
    } else if constexpr (std::is_pointer_v<T>) {
        return reinterpret_cast<T>(word);
    } else {
        return static_cast<T>(word);
    }
}

// Machine binding for one generated kernel program. Created empty at
// generation time; filled exactly once by FinalizeKernels for one backend.
struct TKernelSlot {
    // LLVM: parallel to TGeneratedKernel::Entrypoints; nullptr until bound.
    std::vector<void*> Fns;
    // Owns the JIT'd code: the pointers are valid while this lives.
    std::shared_ptr<NQumir::TLLVMRunner> Runner;
    // Cache path only: keeps the query's LLJIT alive (the runner does not).
    std::shared_ptr<void> JitLifetime;
    // VM: the query's prepared program and the function of each entrypoint.
    std::shared_ptr<TVmProgram> Vm;
    std::vector<NQumir::NIR::TFunction*> VmFns;

    // Calls entrypoint `index` on the calling thread; TFn is its native
    // function pointer type. The VM runs it on the context's instance.
    template <typename TFn, typename... TArgs>
    auto Call(TExecutionContext& context, size_t index, TArgs... args) const {
        static_assert(IsKernelSignature<TFn>, "kernel signature violates the kernel ABI");
        using TResult = std::invoke_result_t<TFn, TArgs...>;
        if (!Vm) {
            return reinterpret_cast<TFn>(Fns[index])(args...);
        }
        if constexpr (std::is_void_v<TResult>) {
            CallVm<TFn>(context, index, args...);
        } else {
            return FromKernelWord<TResult>(CallVm<TFn>(context, index, args...));
        }
    }

private:
    // Out of line so the native call sites stay small.
    template <typename TFn, typename... TArgs>
    [[gnu::noinline, gnu::cold]] int64_t CallVm(
        TExecutionContext& context, size_t index, TArgs... args) const
    {
        return context.Invoke(Vm, *VmFns[index], ToKernelWords(TFn{}, args...));
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
