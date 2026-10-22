# Building, running, benchmarking

## Build

Requirements:

- CMake 3.30+
- C++23 compiler
- LLVM
- Apache Arrow and Parquet development packages discoverable via `pkg-config`
- Readline
- GoogleTest

Configure and build:

```bash
cmake -S . -B build -G Ninja
cmake --build build
```

## Run tests

```bash
cmake -S . -B build -G Ninja -DQDB_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Run one test binary directly:

```bash
./build/test/test_sort
```

When Node.js is installed, CTest also runs the browser WASM runtime checks:
`test_reg_js` runs the shared SQL regression suites (see
[test/regtest/README.md](../test/regtest/README.md)), and
`test_plan_export_integration` covers browser-only contracts.

## External modules

Rust `CREATE MODULE` queries require a local toolchain whose LLVM bitcode is
compatible with QDB's linked LLVM. The current LLVM 22 CI build uses Rust 1.98.1
(the last release on LLVM 22; 1.99 moved to LLVM 23) with `rust-src` for wasm64
standard-library compilation. Service-user setup and toolchain troubleshooting
are documented in the project wiki.

## CLI

Run SQL against a directory of `<table>.parquet` files:

```bash
./build/bin/qdb --sql --data /path/to/pq1 -i query.sql
```

Without `-i`, `qdb` starts an interactive SQL prompt. Statements end with `;`.
The prompt supports `\d <table>` for table description.

Common options:

```bash
--format console
--format csv
--format '<escape>csv'
--format null

--scheduler single
--scheduler threaded
--scheduler-workers 8
--scan-tasks 8
--scheduler-counters
--no-partial-aggregates
--no-runtime-filters
--force-runtime-filters
--verbose
```

Execution defaults to the threaded scheduler with one worker per logical CPU
reported by the system (at least one if the count is unknown). Scheduler options
can be omitted; `--scheduler single` and `--scheduler-workers N` override these
defaults.

When `--scan-tasks` is omitted, the threaded scheduler chooses a limit for each
Parquet source using its surviving row groups, row count and uncompressed bytes
of selected columns. Small scans stay serial. Large scans feeding a join are
capped at the worker count; other scans may use up to twice that count. An
explicit `--scan-tasks N` overrides this choice, including `N=1`. The single
scheduler defaults to one scan task. `--verbose` reports each automatic choice.
See [Scan parallelism](arch/scan_parallelism.md) for the sizing rule and its limits.

The scheduler can add a local grouped aggregate before hash repartition. It
does this when row-count and NDV statistics show a large drop in rows. Use
`--no-partial-aggregates` to keep the raw-row plan for an A/B test. See
[Aggregation](arch/aggregation.md#partial-aggregate-plan) for the rule and the two
physical plans.

Use `--no-runtime-filters` for an A/B run against automatic runtime-filter
selection. `--force-runtime-filters` bypasses the filter benefit estimate;
it is useful for measuring joins that the estimate currently skips.

Example:

```bash
./build/bin/qdb \
  --sql \
  --data ~/Projects/tpch/pq1 \
  --scheduler single \
  --format '<escape>csv' \
  -i benchmark/tpch/queries/sql/q1.sql
```

## Web workbench

The workbench is what [db.qumir.dev](https://db.qumir.dev) serves. To run your
own instance, build and start the web server:

```bash
cmake -S . -B build -G Ninja -DQDB_BUILD_SERVICE=ON
cmake --build build --target qdb_web
./build/service/qdb_web --port 8080 --data /path/to/pq1 --cache /path/to/jit-cache
```

Open:

```text
http://localhost:8080
```

`--data <dir>` registers a server-side dataset. Queries against these datasets
run natively through `qdb` on the server.

`--cache <dir>` enables the symbol-granular wasm object cache used while the
service exports browser execution plans.

The workbench can also run queries in the browser. Browser datasets live in
OPFS. You can create them in the UI by uploading `.parquet` files.

### Downloadable local data

Use `--local-data <dir>` to expose a local parquet directory as a downloadable
dataset in the UI:

```bash
./build/service/qdb_web \
  --port 8080 \
  --data ~/Projects/tpch/pq1 \
  --local-data ~/Projects/tpch/pq1
```

### Dataset map

Instead of listing every directory on the command line, point `--data-dir <dir>`
at a directory holding a `datasets.map` descriptor:

```text
# <kind> <path> [alias]
# kind: server | local

server  data/sample          sample
local   ~/Projects/tpch/pq1  tpch_scale1
```

Lines starting with `#` are comments, relative paths are resolved against the
directory holding the map, and `~` is expanded. The packaged systemd unit uses
`--data-dir /var/lib/qumirdb-data`, so a deployment is configured by editing
`/var/lib/qumirdb-data/datasets.map` rather than the unit file.

`--local-data` does not run queries directly. The UI shows a download action;
pressing it downloads the parquet files from the server into browser OPFS and
creates a normal browser dataset. Download progress is byte-based and downloads
multiple files concurrently. The UI also exposes a delete action for removing
the OPFS copy.

Browser UI state is persisted per saved query and dataset pair. Persisted state
includes graph/plans/details/result rows and the exec plan. Wasm payloads are
not persisted, and result persistence is capped at 1000 data rows plus the
header.

## Plan export

`qdb --plan-export` is the bridge used by the web workbench. It accepts JSON on
stdin and can emit logical/physical graphs plus browser runtime bundles.

The service spawns the same `qdb` binary in this mode, and it is also useful
when debugging plan generation:

```bash
./build/bin/qdb --plan-export \
  --stdin-json --stdout-json --cache /path/to/jit-cache < request.json
```

The cache stores reusable wasm64 objects per cacheable symbol; the
query-specific object and final `.wasm` module are rebuilt for every request.

## TPC-H

The benchmark runner expects a base directory containing `pq<N>/` directories,
for example:

```text
/path/to/tpch/
  pq1/
    lineitem.parquet
    orders.parquet
    ...
  pq10/
    ...
```

Run all discovered scales in s-expression mode:

```bash
benchmark/tpch/run_tpch.sh /path/to/tpch
```

Run SF1 SQL query 21 with the single-threaded scheduler:

```bash
MODE=sql \
QDB_ARGS="--scheduler single" \
benchmark/tpch/run_tpch.sh /path/to/tpch 1 21
```

Run several queries:

```bash
MODE=sql benchmark/tpch/run_tpch.sh /path/to/tpch 1 1,4,21
```

Benchmark logs are written to `./results/` by default. Override with `OUT_DIR`.

## ClickBench

The ClickBench runner expects the official `hits.parquet`, either directly in
the supplied directory or under `pq1/`:

```bash
benchmark/clickbench/run_clickbench.sh /path/to/clickbench
benchmark/clickbench/run_clickbench.sh /path/to/clickbench 0,7,42
```

Use `QDB_ARGS` and `OUT_DIR` as with the other benchmark runners. Queries that
require unsupported string operations are skipped unless
`ALLOW_UNSUPPORTED=1` is set.
