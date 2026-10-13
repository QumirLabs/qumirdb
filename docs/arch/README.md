# Architecture

## Documentation scope

Keep only fundamental documentation of the engine's architecture and algorithms
in this directory: the query pipeline, scheduler, logical and physical plans,
optimizer runs and pass ordering, kernels, runtime ABI, ownership, and the core
join and aggregation algorithms.

Explain the design decisions, invariants, and trade-offs. Link to the papers and
articles that informed an algorithm or design, and describe how the engine
applies or adapts those ideas.

Project-specific guides and local implementation details belong in the project
wiki: test infrastructure, suite and fixture formats, and troubleshooting.
Build and run instructions belong in [docs/building.md](../building.md).
Benchmark reports belong in [docs/benchmarks](../benchmarks/README.md), with
separate pages for each benchmark.

## Query pipeline

SQL → logical plan → idempotent rewrite passes → physical plan. Entry point
`bin/cli.cpp` (`RunQuery`); builder `qdb/plan/build.cpp`; passes
`qdb/plan/passes/`.

1. **Build** — SQL AST → naive operator tree (no keys, no pushdown, no reorder).
   [logical_plan_build.md](logical_plan_build.md), [decorrelation.md](decorrelation.md).
2. **AssignSourceAliases** — unique alias per source.
3. **QualifyColumns** — rewrite refs to `alias.col`; set source/join schemas.
4. **AnnotateTypes** — attach `TFunctionType` (input/output schema) to every op;
   idempotent, re-run after each structural rewrite.
5. **ReorderJoins** — cross-join chains → connected order.
   [join_reorder.md](join_reorder.md).
6. **ExtractEquiJoins** — lift equi-keys (equivalence classes), push predicates
   per side, leave residuals. [predicate_pushdown.md](predicate_pushdown.md).
7. **ApplyColumnPruning** — narrow each op's required input columns.
8. **ApplyLateMaterialization** — after CTE output demand is known, move wide
   output columns after a small limit when the cost is lower.
   [late_materialization.md](late_materialization.md).
9. **AttachRowGroupPredicates** — attach a conservative Parquet row-group
   pruning hint without removing the row-level filter.
   [predicate_pushdown.md](predicate_pushdown.md#parquet-row-group-pruning).

- [Optimizations](optimizations.md) - every rewrite in one place: build-time,
  pipeline passes, the CTE phase and the execution-time ones, each with a
  minimal query and the plan before and after.

Detailed algorithm docs:

- [Logical plan build](logical_plan_build.md) - SQL AST → naive operator tree:
  FROM/WHERE/SELECT/GROUP BY/HAVING mapping, aggregate-argument materialization,
  global-aggregate and `count(distinct)` rewrites, CTE inlining, derived-table
  aliases.
- [Join reorder](join_reorder.md) - rewriting cross-join chains into a connected
  order by threading filter equalities down to each chain.
- [Predicate pushdown](predicate_pushdown.md) - equi-key extraction via
  equivalence classes, per-side predicate pushdown, disjunction factoring and
  derived side filters, outer-join residual handling.
- [Decorrelation](decorrelation.md) - EXISTS/IN → semi/anti joins; scalar
  subqueries → broadcast cross join (uncorrelated) or group-by + LEFT join
  (correlated, Apply elimination).

Physical execution:

- [Scan parallelism](scan_parallelism.md) - automatic per-source scan sizing,
  join fan-in, explicit limits, and the comparison with DuckDB.
- [Aggregation](aggregation.md) - aggregation planning, generated Qumir AST,
  generic Oz kernels, type-specific hash injection, runtime ABI and memory.
- [Join](join.md) - supported join forms, adaptive symmetric/probe-only hash
  join scheduling, kernel split, residual predicates, and runtime lifetime.
- [Scheduler runtime](scheduler_runtime.md) - scheduled physical graph, task
  nodes, connections, compiled kernels, lowering patterns, and JS/WASM runtime
  boundary.
- [Late materialization](late_materialization.md) - narrow Parquet scans, row
  locators, lookup tasks, and safety rules.
- [Dispatch](dispatch.md) - runtime shell dispatch model, node kinds, and
  kernel/dispatcher ownership split.
- [String values](string_values.md) - proposed borrowed/owned string ABI for
  filtering and aggregation, including dual lookup/storage key types.
- [SQL expression pseudo-calls](sql_pseudo_calls.md) - call-shaped parser and
  typing markers for `IN`, concatenation, and SQL NULL semantics.

Deployment:

- [Building, running, benchmarking](../building.md) - build/test commands, the
  CLI, the web workbench, plan export, and the TPC-H/ClickBench runners.
- [Benchmark results](../benchmarks/README.md) - measured ClickBench, TPC-H and
  TPC-DS numbers for qdb, DuckDB and YQL, per query and per scale, with what
  each engine's metric does and does not include. A dated snapshot: qumirdb is
  under development and the numbers are expected to move.
- [Rust toolchain](rust_toolchain.md) - toolchain external `CREATE MODULE`
  compilation needs, why the Rust release is pinned to the linked LLVM, and how
  to configure it for the packaged service user.
