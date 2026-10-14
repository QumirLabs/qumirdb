# Working on QumirDB

QumirDB is an analytical SQL engine for Parquet, built on Qumir's compiler and
runtime. The [project principles](README.md#project-principles) guide design and
implementation decisions. Follow the user's task instructions when choosing
scope, validation, and publication steps.

## Project principles

### 1. Modern database design, beautiful code and architecture

Aim for a database at the forefront of the field. Understand the relevant
database ideas and choose coherent abstractions, clear contracts, and precise
names. Beauty, clarity, and architectural coherence are the highest priority.
Solve the underlying design problem rather than accumulating special cases.

Keep changes focused and understandable. Explain design decisions and their
trade-offs. Performance improvements must preserve the quality of the code and
architecture; a benchmark win alone does not settle a design decision.

### 2. Computation in oz-lang through a compiled pipeline

Express operator computation in oz-lang and generate it through the common
kernel pipeline. C++ handles planning, I/O, scheduling, and resource ownership.
Keep new row processing, predicate evaluation, hashing, joins, aggregation, and
sorting computation in the generated kernels.

Preserve the common compilation path for native and browser execution. Integrate
user-defined computation through the existing compiled-module mechanism. Avoid
duplicating operator algorithms in host code or introducing a separate execution
engine for a particular target.

### 3. Operators and tasks form the execution boundaries

The pipeline is split into operators and schedulable tasks so work can eventually
be distributed across processes and nodes. Make data exchange, readiness,
dependencies, and ownership explicit. Kernel fusion must preserve these logical
execution boundaries.

Keep task progress in task/operator state. Pass per-thread execution resources
through an explicit context; do not locate mutable execution state or a VM
instance through thread-local or implicit global state. Keep prepared code and
mutable execution state separate, with clear lifetimes. A task may resume on a
different worker.

### 4. Complex analytical queries are central

Treat joins across many tables, outer/semi/anti joins, residual conditions, and
correlated subqueries as core workloads. Preserve SQL semantics through rewrites:
NULL behavior, duplicate rows, unmatched outer rows, empty inputs, and aggregate
results matter as much as plan shape.

Use focused cases that exercise the affected semantics. Where execution is
enabled, check results alongside plans: successful planning alone cannot detect
an execution bug. Keep optimizer rules general enough to serve real analytical
queries rather than one benchmark query.

### 5. Speed throughout query processing

Optimize planning, compilation, and execution. Measure the stage that changed
before claiming a speedup. Distinguish preparation/JIT time from processing time
and CPU time; a warm dependency cache still leaves query-specific compilation.

Compare equivalent builds, datasets, query parameters, scheduler settings, and
cache conditions. Preserve individual runs and disclose the aggregation method.
Distinguish native runs from sandbox runs. Prefer optimizations with a clear
algorithmic explanation and measured benefit.

## Code style and repository layout

Read and follow the [Qumir code style](externals/qumir/docs/arch/codestyle.md).
It defines naming, braces, formatting, include order, comments, and error handling.
Use C++23, the `N`/`T`/`I`/`E` naming prefixes, and explicit ownership. Write code
comments in English and explain why a decision exists. Avoid unrelated formatting
changes.

The main areas are:

- `qdb/sql/`, `qdb/plan/`: SQL parsing, logical plans, optimizer passes, and costs.
- `qdb/kernel/`, `qdb/modules/`: generated kernels, oz-lang implementations, and
  runtime bindings.
- `qdb/exec/`, `qdb/scheduler/`: operator state, tasks, connections, and execution.
- `qdb/io/`: Parquet and other input/output infrastructure.
- `bin/`: CLI and plan export; `service/`: web workbench.
- `test/`: unit tests and SQL regression suites; `benchmark/`: benchmark runners.
- `externals/qumir/`, `externals/coroio/`: Git submodules. Compiler/VM changes
  belong in Qumir; record their commits and update the QDB submodule pin.

### Source map for agents

Use this map to locate implementation details. Keep source-file inventories here
or in the wiki; architectural chapters should explain the algorithms themselves.

| Area | Starting points |
|---|---|
| Logical operators and schemas | `qdb/plan/ops/` |
| SQL-to-plan construction and decorrelation | `qdb/plan/build.cpp` |
| Optimizer order and CTE decisions | `qdb/plan/pipeline.cpp`, `qdb/plan/passes/cte_reuse.cpp` |
| Join ordering, DPccp, and estimates | `qdb/plan/passes/join_order.cpp`, `qdb/plan/passes/cbo/dpccp.cpp`, `qdb/plan/passes/estimate_stats.cpp` |
| Predicate placement and join keys | `qdb/plan/passes/equijoin.cpp` |
| Physical task lowering and parallel aggregation | `qdb/scheduler/plan_lowerer.cpp`, `qdb/exec/planner_helpers.cpp` |
| Task protocol, graphs, connections, scheduling | `qdb/scheduler/state.h`, `qdb/scheduler/graph.h`, `qdb/scheduler/connection.h`, `qdb/scheduler/threaded_scheduler.cpp` |
| Kernel specifications, generation, finalization | `qdb/kernel/spec.h`, `qdb/kernel/compiler.cpp`, `qdb/kernel/finalize.cpp` |
| Aggregate key types and generated reducers | `qdb/kernel/aggregate_key.cpp`, `qdb/kernel/gen.cpp`, `qdb/kernel/lib.cpp` |
| Join keys and generated matching/materialization | `qdb/kernel/join_key.cpp`, `qdb/kernel/join_gen.cpp` |
| Generic oz-lang algorithms | `qdb/kernel/aggregation/`, `qdb/kernel/join/`, `qdb/kernel/sort/` |
| Operator state and lifetimes | `qdb/exec/aggregate_exec.cpp`, `qdb/exec/join_exec.cpp`, `qdb/exec/sort_exec.cpp` |
| Column values and runtime ABI | `qdb/io/io.h`, `qdb/kernel/column_value.cpp`, `qdb/modules/qumirdb.cpp`, `qdb/modules/qumirdb_runtime.cpp` |
| Runtime filters | `qdb/plan/passes/runtime_filters.cpp`, `qdb/exec/runtime_filter.cpp`, `qdb/kernel/join/runtime_filter.oz` |
| Parquet scans and late lookup | `qdb/io/parquet/source.cpp`, `qdb/io/parquet/row_reader.cpp`, `qdb/plan/passes/late_materialization.cpp` |
| Execution-plan export and browser dispatch | `qdb/exec/plan_builder.cpp`, `bin/plan_export.cpp`, `service/static/` |
| Regression tests and focused operator tests | `test/test_reg.cpp`, `test/test_aggregate.cpp`, `test/test_join_kernel.cpp`, `test/test_join_exec.cpp` |

## Building and running qdb

Use [docs/building.md](docs/building.md) for dependencies and full build/service
instructions. A development build with tests is:

```sh
git submodule update --init --recursive
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DQDB_BUILD_TESTS=ON
cmake --build build --target qdb test_reg -j 6
```

SQL table names resolve to `<table>.parquet` in the data directory. Enable the
persistent JIT cache for normal execution runs. Run a SQL file with phase timings:

```sh
export QDB_JIT_CACHE_DIR="$PWD/build/jit-cache"
./build/bin/qdb --sql --data /path/to/parquet -i query.sql --timing
```

Omit `-i` for the interactive SQL prompt; terminate statements with `;`. Use
`EXPLAIN` in SQL to inspect the optimized plan. For execution diagnostics use
`--verbose` and, when needed, `--scheduler-counters`.

Scheduler selection is explicit when comparing performance:

```sh
./build/bin/qdb --sql --data /path/to/parquet -i query.sql --scheduler single
./build/bin/qdb --sql --data /path/to/parquet -i query.sql \
  --scheduler threaded --scheduler-workers 4 --scan-tasks 4
```

Use `--format console`, `--format '<escape>csv'`, or `--format null` as appropriate,
and keep the same output mode in comparisons. Use the benchmark runners to render
TPC-H templates with their scale parameters:

```sh
MODE=sql QDB_JIT_CACHE_DIR="$PWD/build/jit-cache" \
  OUT_DIR="$PWD/build/results/tpch" \
  bash benchmark/tpch/run_tpch.sh /path/to/tpch 100
QDB_JIT_CACHE_DIR="$PWD/build/jit-cache" OUT_DIR="$PWD/build/results/tpcds" \
  bash benchmark/tpcds/run_tpcds.sh /path/to/tpcds 10
```

Runner base directories contain `pq100/` or `pq10/`. Set `QDB_BIN` to a saved
binary, `QDB_ARGS` to matching scheduler options, and separate `OUT_DIR` values
for each run. Use the same build configuration for baseline and candidate.

Run checks appropriate to the change and the user's instructions. Examples:

```sh
ctest --test-dir build -R '^test_reg$' --output-on-failure
./build/test/test_reg test/regtest --gtest_filter='RegQuery.correlated_aggregate_*'
```

`test_reg` enables its persistent JIT cache automatically; `QDB_JIT_CACHE_DIR`
overrides its location. Use `--canonize` only for understood, intended expectation
changes, then review the golden diff and check the affected cases again. Do not
canonize away an unexplained failure. Documentation-only edits need text/link
checks, not an engine rebuild or benchmark.

## Documentation structure

| Location | Purpose |
|---|---|
| [README.md](README.md) | Project introduction, principles, and entry points |
| [docs/arch/](docs/arch/README.md) | Fundamental architecture and algorithms for human readers |
| [docs/building.md](docs/building.md) | Build requirements and ordinary build/run instructions |
| [docs/benchmarks/](docs/benchmarks/README.md) | Benchmark methodology and results, organized by benchmark |
| [Project wiki](https://github.com/resetius/qumirdb/wiki) | Detailed implementation explanations, local workflows, configuration, test infrastructure, troubleshooting, and operational notes |
| GitHub issues and PRs | Work plans, progress, review, and validation evidence |

Wiki navigation belongs in the wiki. Do not turn the repository README or the
architecture index into a wiki page index. Existing document placement is not a
reason to add new content in the wrong location.

### What belongs in docs/arch

Write fundamental knowledge about how the engine works: the query pipeline,
logical/physical plans, optimizer passes and their ordering, scheduling and data
flow, compilation, joins, aggregation, and ownership contracts. Explain the
ideas, invariants, design decisions, and trade-offs so a human can understand
them without opening the source code.

AI slop is unacceptable. Remove generic filler, repetitive explanations,
marketing claims, invented design rationale, and sprawling generated inventories.
Each paragraph should teach a concrete, durable architectural idea. Use a small
diagram, SQL example, or before/after plan only when it helps explain that idea.

References to research papers, articles, books, and other literature are welcome
and encouraged. Cite the actual source of an algorithm or design, explain the
relevant idea, and describe how QDB applies or adapts it. Verify references and
their claims.

Do not write exhaustive tours of source files, classes, methods, signatures,
fields, or line numbers here. Do not retell a patch or paste detailed traces and
large plan dumps into an architectural chapter. Detailed source-linked
walkthroughs, exact formats/layouts, debugging recipes, and implementation
specifics belong in the project wiki.

### Documentation decisions for a change

Before finishing a change or PR, assess its architectural significance. Judge
the conceptual change, not the number of modified lines.

- A substantial change to the execution model, component responsibilities,
  scheduling, data flow, ownership, or optimizer organization requires an update
  to the relevant `docs/arch` explanation.
- A **new optimization, pass, or algorithm must be mentioned in docs/arch**, even
  if its implementation is small. Explain its purpose, applicability, semantic
  guarantees, and place in the pipeline. Update
  [optimizations.md](docs/arch/optimizations.md) where applicable; add a separate
  chapter only if the concept needs one.
- A small implementation change or local behavior change belongs in the wiki.
  Renames, small refactors, and debugging details do not justify expanding an
  architectural chapter. Update the relevant wiki page when those details are
  worth retaining.
- Benchmark measurements belong in the appropriate `docs/benchmarks` page.
  Reproduction details and local troubleshooting belong in the wiki.

Keep fundamental architectural descriptions concise and readable. Updating
`docs/arch` is not a requirement to document every implementation detail there.
