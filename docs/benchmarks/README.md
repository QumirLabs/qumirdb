# Benchmarks

Results are organized by benchmark:

- [ClickBench](clickbench.md) — 43 queries over `hits.parquet`.
- [TPC-H](tpch.md) — 22 queries at scale factors 1, 10, and 100.
- [TPC-DS](tpcds.md) — 99 queries at scale factors 1, 10, and 100.

All runs on one machine: an Apple M5 Pro with 18 cores and 64 GB of memory,
with every engine set to 18 threads. Unless a benchmark section states
otherwise, each query reports the **best** of the available runs. Benchmarks are
re-run independently, so each section on the benchmark pages carries its own
run date.

Engine builds: DuckDB v1.5.5 (Variegata) `d8cdaa33fd`; ClickHouse
`clickhouse-local` 26.9.1.347 (official build); qdb built from this repository.

> **qumirdb is under active development.** These numbers are a snapshot taken
> on the dates recorded on each benchmark page, not a fixed characterisation of the engine. Query
> execution, the hash table and Parquet scanning are all being worked on, so
> the qdb column is expected to move — treat it as a baseline to measure the
> next change against, and re-run the benchmarks rather than citing these
> figures later.

## What each number measures

The engines log different quantities, which matters when reading the tables:

| Engine | Metric | Includes |
|---|---|---|
| qdb | `exec` | execution only — planning, kernel build and JIT compilation are **excluded** |
| DuckDB | `real` | full wall-clock time |
| ClickHouse | wall clock | full wall-clock time of `clickhouse-local` per query |
| YQL | `proc` | processing time, without the fixed per-query startup cost (~0.6–0.8 s for YQL) |

So qdb is shown in its most favourable light: its JIT compilation is left out.
The qdb TPC-H, TPC-DS and ClickBench logs do carry a `plan`/`kbuild`/`llvm` breakdown
if that cost needs to be added back.

## Coverage

- **ClickBench** — 43 queries over `hits.parquet`. Run by qdb, DuckDB and
  `clickhouse-local`; YQL was not run.
- **TPC-H** — 22 queries at scale factors 1, 10 and 100.
- **TPC-DS** — 99 queries. qdb runs 14, 23, 24 and 39 as two parts each; the parts
  are summed within a run here so the query set matches DuckDB and YQL.
  YQL has no scale-100 run. qdb has no result for q78 and q80 at scale 100:
  both exhausted the machine's 64 GB.

## About the YQL numbers

[YQL](https://ydb.tech/docs/en/yql/reference/) is the query language and engine
of [YDB](https://ydb.tech/). These numbers come from the `dqrun` utility in the
[YDB repository](https://github.com/ydb-platform/ydb), reading the same Parquet
files as qdb and DuckDB. `dqrun` is a standalone query runner, not a YDB cluster:
a real YDB deployment reads its own storage and distributes work differently, so
its results may differ from what is shown here.

## Summary

| Benchmark | Scale | qdb (s) | DuckDB (s) | ClickHouse (s) | YQL (s) | qdb ÷ DuckDB | Notes |
|---|---|---|---|---|---|---|---|
| ClickBench | — | 15.31 | 14.44 | 14.95 | — | 1.06× | — |
| TPC-H | 1 | 0.857 | 1.17 | — | 4.17 | 0.73× | qdb: one SQL run |
| TPC-H | 10 | 4.733 | 4.03 | — | 29.21 | 1.17× | qdb: one SQL run |
| TPC-H | 100 | 49.843 | 32.10 | — | 338.38 | 1.55× | qdb: one SQL run |
| TPC-DS | 1 | 7.08 | 5.13 | — | 55.27 | 1.38× | YQL: 7 failed |
| TPC-DS | 10 | 51.90 | 15.89 | — | 420.05 | 3.27× | YQL: 10 failed |
| TPC-DS | 100 | 465.22 | 117.86 | — | — | 3.95× | qdb: 2 missing |

Totals cover the queries each engine completed, so rows with failures compare
different subsets — flagged in the Notes column.
