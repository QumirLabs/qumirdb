# Kernel compilation phases

Status: Implemented.

Kernel handling is split into two phases; adding an operator only requires
teaching generation to emit its kernels, and JIT (CLI) vs wasm (JSON/browser)
is the only fork.

```
logical plan
  │  physical compilation (AST level)
  ▼
TLoweredPlan  =  TTaskGraph + Kernels: every kernel as TGeneratedKernel
                 { Name, Stage, ExecStageId, Entrypoints, Ast, Storage, Slot,
                   SortKeys }  (AST only, not compiled)
  │  machine finalization (the ONLY fork)
  ├── JitFinalizeKernels  → deduplicate + fuse all ASTs, compile one native
  │                          module, fill every kernel's TKernelSlot
  └── WasmFinalizeKernels → use the same deduplication/fusion, compile one
                             query-level wasm64 module, publish its entrypoint
                             mapping and base64 artifact
```

## Generation (qdb/kernel/compiler.{h,cpp})

Every operator's kernels go through `TKernelCompiler::EmitKernel`: build the
program AST, create an empty `TKernelSlot`, append the `TGeneratedKernel` to
`TKernelCompilerOptions::Sink`, and — only when `BindNow` (the default, used by
direct callers such as unit tests) — JIT immediately. The returned dispatches
are closures over the slot (`slot->Fns[entryIndex]`), so filling one slot
late-binds every copy of the dispatch, including those already captured in
per-partition scheduler states.

Kernels per operator:
- filter / project: one kernel, one entry (`<kernel>` / `<project>`), with
  `Storage` holding the literal strings the AST points into.
- sort: `sort.radix.fused` + `sort.radix.nullable.fused`, with resolved
  `SortKeys` `{Index, WidthBytes, Desc}` metadata for the exec exporter.
- aggregate: three kernels (`aggregate.update`/`.measure`/`.finalize`), one
  entry each (the three programs are built independently).
- join: one multi-entry kernel (jt_init, jt_process_*, jt_probe_*_stream,
  jt_destroy, pb_destroy, plus semi/anti/outer finalizers), resolved via
  LookupMany; `join_hash` (jt_hash_left/right) for the partitioned path.
- runtime filters: one `runtime-filter` kernel per query, shared by all
  filters; its entries are listed by `TRuntimeFilterKernels::Entrypoints()`.

Lowering (`plan_lowerer.cpp`) creates compilers via
`KernelOptions(stage, execStageId)`: `Sink = &TLoweredPlan::Kernels`,
`BindNow = false`, `ExecStageId` is the stable association with the executable
stage, and `Stage` is a deterministic `exec:<id>:<kind>` diagnostic label. No
operator pointer, debug label, or target is an identity in this phase.

## Finalization

- JIT: `JitFinalizeKernels` (qdb/kernel/finalize.cpp) — deduplicate kernels by
  AST + entrypoints, build one `TFusedProgram`, compile it through one
  `TLLVMRunner`, then fill every `Slot->Fns` in original entrypoint order. All
  slots retain the runner (and cached-JIT lifetime when enabled). Idempotent.
  `RunPlanIntoSink` always finalizes before scheduling; cli.cpp finalizes
  earlier so the reported query time excludes kernel compilation.
- Wasm: `WasmFinalizeKernels` (bin/plan_export.cpp) — register an `ast` artifact
  per exported kernel, apply the same deduplication and `BuildFusedProgram`, and
  compile one query-level module through `CompileKernelAstToWasm64` with
  `TargetTriple=wasm64-unknown-unknown`, linked by `wasm-ld -mwasm64`. The
  exporter records fused entrypoint names per `ExecStageId`; slots stay unbound
  and the exporter never executes the native graph.

Both finalizers share the frontend (`EnsureQumirDbUse` + the runner's
`EmitKernelArtifacts`); only emission differs. The browser runtime instantiates
WebAssembly Memory64, matching the exporter and the kernel ABI's 8-byte
pointers; no wasm32 fallback is supported.

## Consumers

- Physical graph JSON attaches kernel artifacts to semantic task groups by
  `ExecStageId`. Scheduler-only plumbing has separate monotonically assigned
  `task:<id>` groups; debug strings are display-only.
- `BuildExecPlan` collapses physical lanes and contracts plumbing using stable
  stage/input IDs, producing an exporter-neutral typed DAG.
- The exec JSON codec indexes kernels by `ExecStageId`; stage entrypoints and
  physical graph artifacts therefore refer to kernels emitted by the same
  scheduler lowering. Sort stages take `radixKeys` directly from retained
  kernel metadata. Nothing is lowered or compiled twice.
