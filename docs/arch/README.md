# Architecture

## Documentation scope

Keep only fundamental documentation of the engine's architecture and algorithms
in this directory: the query pipeline, scheduler, logical and physical plans,
optimizer runs and pass ordering, kernels, runtime ABI, ownership, and the core
join and aggregation algorithms.

Explain the design decisions, invariants, and trade-offs. Link to the papers,
articles, and books that informed an algorithm or design. Explain how the engine
applies or adapts those ideas.

These pages are for human readers. Explain fundamental concepts without
requiring a source-code tour. AI slop, repetitive filler, exhaustive source
links, and detailed inventories of classes, functions, or fields are not
acceptable here; implementation details belong in the project wiki.

For each architectural change, assess whether it substantially changes the
engine's model. Update these pages for substantial changes; document minor
implementation changes in the wiki. A new optimization, pass, or algorithm
must always be mentioned here, with its purpose and role in the pipeline.

Project-specific guides and local implementation details belong in the project
wiki: test infrastructure, suite and fixture formats, and troubleshooting.
Build and run instructions belong in [docs/building.md](../building.md).
Benchmark reports belong in [docs/benchmarks](../benchmarks/README.md), with
separate pages for each benchmark.

## Query pipeline

SQL → logical plan → typed rewrite passes → physical task graph and kernels
→ compilation → scheduled execution.

1. **Build** — SQL AST → initial operator tree, including aggregate rewrites and
   supported subquery decorrelation.
   [logical_plan_build.md](logical_plan_build.md), [decorrelation.md](decorrelation.md).
2. **AssignSourceAliases / QualifyColumns / AnnotateTypes** — resolve column
   ownership and schemas; re-annotate after structural rewrites.
3. **FlattenInnerJoins** (with CBO), **SimplifySelfEquality**, then
   **PushDownPredicates** — expose join regions and filtered leaf inputs.
4. **EstimateStats / ReorderJoins** — choose a cost-based connected join tree,
   or use the greedy fallback when CBO cannot apply.
   [join_reorder.md](join_reorder.md).
5. **ExtractEquiJoins / PushDownSemiJoins** — place keys, residuals, and safe
   existence filters. [predicate_pushdown.md](predicate_pushdown.md).
6. **PushDownLimits / ApplyTopSort / CoerceSetOpBranches / ApplyColumnPruning** —
   bound work, align set-operation schemas, and establish output demand.
   **RewriteOuterJoinAsAnti** runs after pruning; a successful rewrite repeats
   typing, semi pushdown, and pruning before the final estimate.
7. **CTE optimization** — push common consumer predicates, propagate demand,
   apply late materialization, and choose inlining versus shared reuse.
   [late_materialization.md](late_materialization.md).
8. **AttachRowGroupPredicates / AttachRuntimeFilters** — attach conservative
   scan and join-filter hints after plan restructuring.
9. **Lower / compile / execute** — select scan lanes and exchanges, generate
   kernels, finalize their code, and schedule tasks.
   [scheduler_runtime.md](scheduler_runtime.md),
   [kernel-compilation-phases.md](kernel-compilation-phases.md).

- [Optimizations](optimizations.md) - every rewrite in one place: build-time,
  pipeline passes, the CTE phase and the execution-time ones, each with a
  minimal query and the plan before and after.

Detailed algorithm docs:

- [Logical plan build](logical_plan_build.md) - SQL AST → naive operator tree:
  FROM/WHERE/SELECT/GROUP BY/HAVING mapping, aggregate-argument materialization,
  global-aggregate and `count(distinct)` rewrites, shared CTE definitions and reuse, derived-table
  aliases.
- [Join reorder](join_reorder.md) - rewriting cross-join chains into a connected
  order using CBO or the greedy fallback, with filter equalities available to each chain.
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
- [String values](string_values.md) - borrowed/owned string representations for
  filtering and aggregation, including dual lookup/storage key types.
- [SQL expression pseudo-calls](sql_pseudo_calls.md) - call-shaped parser and
  typing markers for `IN`, concatenation, and SQL NULL semantics.
