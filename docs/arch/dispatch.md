# Dispatch architecture

This document describes the execution-dispatch model after the node-lowering
work. It is intentionally about dispatch and ownership, not about ABI details.
The motivating question is whether a non-C++ dispatcher, such as a JavaScript
dispatcher, can drive execution while C++ still builds and compiles kernels.

## Node kinds at a glance

| Node kind | Examples | Dispatcher job | Kernel job |
|---|---|---|---|
| `source` | table/parquet/text source | Produce rowsets | None |
| `non_compute` | zero-copy project, limit-like views | Rewrite/view rowsets | None |
| `unary_stream` | filter, computed project | Pull one batch, call kernel | Predicate/expression eval |
| `unary_block` | aggregate, sort, top-sort | Drain input, own state | Update/finalize/key work |
| `binary_join` | inner/semi/anti/outer equi-join | Schedule two inputs, drain output | Key/hash/probe/residual |
| `sink` | CLI/result writer/future materializer | Consume rowsets | Usually none |

The dispatch question is mostly about implementing the `Dispatcher job` column
outside C++. The `Kernel job` column should stay in compiled kernels.

## Current split

The current physical executor has three responsibilities:

- plan lowering: choose a runtime shell and build `TOperatorKernelSpec`;
- kernel compilation: generate and JIT the query-specific Oz entrypoints;
- dispatch: pull rowsets through runtime nodes and call compiled kernels.

After lowering, most compute-specific behavior is no longer embedded directly
in per-operator runtime nodes. The runtime shell owns rowset lifetimes, state
objects, scratch buffers, and output publication. Kernels own expression
evaluation, key extraction, hash-table update/probe, and residual predicates.

```text
logical plan
  -> physical planner
      -> TOperatorKernelSpec
      -> compiled kernel bundle
      -> runtime shell
          -> pull inputs
          -> call kernel entrypoints
          -> publish TRowSet output
```

## Operator ownership

Source:

- dispatcher-owned: file/source scan, column restriction, rowset production;
- kernel-owned: none.

Filter:

- dispatcher-owned: input pull, selection buffer lifetime;
- kernel-owned: predicate evaluation, selection update.

Computed project:

- dispatcher-owned: input pull, output buffers, borrowed/owned column assembly;
- kernel-owned: expression evaluation.

Zero-copy project:

- dispatcher-owned: column view construction;
- kernel-owned: none.

Aggregate:

- dispatcher-owned: input drain, hash-table buffer lifetime, output publication;
- kernel-owned: key extraction, state update, finalize materialization helpers.

Sort/top-sort:

- dispatcher-owned: input drain, sort state, bounded top state;
- kernel-owned: numeric key extraction/normalization where supported.

Join:

- dispatcher-owned: two-input scheduling, row stores, hash-table buffers,
  pair/output draining;
- kernel-owned: key extraction, hash/probe/update, residual filter,
  semi/anti/outer finalize kernels.

Limit:

- dispatcher-owned: offset/limit counters;
- kernel-owned: none.

The important boundary is that the dispatcher does not need to understand the
meaning of a filter predicate, project expression, aggregate reducer, join key,
or join residual. It only chooses when to call already-compiled entrypoints.

## Dispatch graphs

Unary streaming operators are already close to a JS-friendly model:

```text
dispatcher
  pull child batch
  call kernel(batch, shell_state)
  return batch/view
```

Unary blocking operators have one materialization boundary:

```text
dispatcher
  while child.Next(batch):
      retain/read batch as needed
      call update kernel/state process
      release batch when consumed
  call finalize/materialize
  return output batches
```

Join has the richest dispatch policy:

```text
dispatcher
  initialize left/right hash table state

  inner/outer symmetric phase:
      choose side
      pull batch
      retain batch in that side row store
      call ProcessLeft/ProcessRight
      drain pair buffer into output builder

  probe-only phase:
      pull stream batch
      call ProbeLeftStream/ProbeRightStream
      materialize output while stream batch is alive
      release stream batch

  semi/anti finalization:
      drain/build retained side state
      call FinalizeAntiSemi
      publish left-side output

  outer finalization:
      call FinalizeOuter
      publish null-padded unmatched rows
```

Nothing in that loop requires C++ language dispatch specifically. It requires a
dispatcher capable of owning state handles, preserving rowset lifetimes, and
calling the correct compiled kernel entrypoint at each transition.
