# Kernel compilation phases

Status: Implemented.

Kernel handling is split into generation and finalization. Operators emit the
same oz-lang programs for LLVM, the Qumir VM, and browser WebAssembly; the target
is selected after generation.

```
logical plan
  │  physical compilation (AST level)
  ▼
TLoweredPlan  =  TTaskGraph + Kernels: every kernel as TGeneratedKernel
                 (program ASTs, named entries, and bindings; not compiled)
  │  machine finalization (the ONLY fork)
  ├── FinalizeKernels     → deduplicate + fuse all ASTs, prepare one LLVM
  │                          module or VM program, fill every kernel's slot
  └── WasmFinalizeKernels → use the same deduplication/fusion, compile one
                             query-level wasm64 module, publish its entrypoint
                             mapping and base64 artifact
```

## Generation

Every operator's kernels go through `TKernelCompiler::EmitKernel`: build the
program AST, create an empty `TKernelSlot`, append the `TGeneratedKernel` to
`TKernelCompilerOptions::Sink`, and — only when `BindNow` (the default, used by
direct callers such as unit tests) — finalize immediately for the chosen
backend. The returned dispatches are closures over the slot, so filling one slot
late-binds every copy of the dispatch, including those already captured in
per-partition scheduler states.

Kernels per operator:
- filter / project: one kernel, one entry (`<kernel>` / `<project>`), with
  `Storage` holding the literal strings the AST points into.
- sort: generated sort/top-sort programs, with resolved key metadata for
  execution and export.
- aggregate: one program with `agg_dispatch` and `agg_finish_rowset` for
  update/lifecycle and output finalization.
- join: one program with dispatch and materialization entries, sharing
  probe/insert/finalization helpers; a separate hash kernel supports repartition.
- runtime filters: one `runtime-filter` kernel per query, shared by all
  filters, with the entries `rf_build`, `rf_publish` and `rf_probe`.

Lowering creates compilers via
`KernelOptions(stage, execStageId)`: `Sink = &TLoweredPlan::Kernels`,
`BindNow = false`, `ExecStageId` is the stable association with the executable
stage, and `Stage` is a deterministic `exec:<id>:<kind>` diagnostic label. No
operator pointer, debug label, or target is an identity in this phase.

## Finalization

- LLVM: `FinalizeKernels` with the LLVM backend — deduplicate kernels by
  AST + entrypoints, build one `TFusedProgram`, compile it through one
  `TLLVMRunner`, then fill every `Slot->Fns` in original entrypoint order. All
  slots retain the runner (and cached-JIT lifetime when enabled). Idempotent.
  `RunPlanIntoSink` always finalizes before scheduling; cli.cpp finalizes
  earlier so the reported query time excludes kernel compilation.
- VM: the same finalizer deduplicates and fuses the same ASTs, lowers them to
  Qumir IR, and prepares reachable bytecode before scheduling. Slots retain the
  prepared program and its entries. The experimental VM backend executes
  generated computation through the interpreter; LLVM remains the default.
  External modules that require linked LLVM bitcode or native code are rejected
  before their compilation begins.
- Wasm: `WasmFinalizeKernels`  — register an `ast` artifact
  per exported kernel, apply the same deduplication and `BuildFusedProgram`, and
  compile one query-level module through `CompileKernelAstToWasm64` with
  `TargetTriple=wasm64-unknown-unknown`, linked by `wasm-ld -mwasm64`. The
  exporter records fused entrypoint names per `ExecStageId`; slots stay unbound
  and the exporter never executes the native graph.

All targets share source-module composition, name resolution, and oz-lang
transforms. LLVM and WebAssembly then use the LLVM runner; the VM lowers the
transformed program to Qumir IR. The browser runtime instantiates
WebAssembly Memory64, matching the exporter and the kernel ABI's 8-byte
pointers; no wasm32 fallback is supported.

Compilation fusion leaves task boundaries and mutable operator state intact.
It does not imply a single row loop through the whole query. A VM program is
shared prepared code; each worker's execution context lazily owns its separate
interpreter registers and stack. A migrating task uses the next worker's
interpreter while retaining the same task heap. The scoped scheduler context
selects both resources at a kernel call; dispatch closures retain neither.
VM externs use the same tracked allocator as LLVM, so intermediate frees and
detached output ownership follow the same lifetime rules on both backends.

For the query-compilation motivation, see Neumann's [Efficiently Compiling
Efficient Query Plans for Modern Hardware (PVLDB 2011)](https://www.vldb.org/pvldb/vol4/p539-neumann.pdf).
QDB specializes computation through oz-lang while keeping explicit scheduled
operator boundaries.
