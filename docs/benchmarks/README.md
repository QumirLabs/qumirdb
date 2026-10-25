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

So qdb is shown in its most favourable light: its JIT compilation is left out.
The qdb TPC-H, TPC-DS and ClickBench logs do carry a `plan`/`kbuild`/`llvm` breakdown
if that cost needs to be added back.

## Coverage

- **ClickBench** — 43 queries over `hits.parquet`. Run by qdb, DuckDB and
  `clickhouse-local`.
- **TPC-H** — 22 queries at scale factors 1, 10 and 100.
- **TPC-DS** — all 99 queries completed at scale factors 1, 10 and 100.
  qdb runs 14, 23, 24 and 39 as two parts each; the parts are summed within a run
  here so the query set matches DuckDB.

## Summary

| Benchmark | Scale | qdb (s) | DuckDB (s) | ClickHouse (s) | qdb ÷ DuckDB | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| ClickBench | — | 14.734 | 14.44 | 14.95 | 1.02× | qdb: one SQL run |
| TPC-H | 1 | 0.777 | 1.17 | — | 0.66× | qdb: one SQL run |
| TPC-H | 10 | 3.864 | 4.03 | — | 0.96× | qdb: one SQL run |
| TPC-H | 100 | 36.645 | 32.10 | — | 1.14× | qdb: one SQL run |
| TPC-DS | 1 | 5.120 | 5.13 | — | 1.00× | qdb: one SQL run |
| TPC-DS | 10 | 27.406 | 15.89 | — | 1.72× | qdb: one SQL run |
| TPC-DS | 100 | 311.278 | 117.86 | — | 2.64× | qdb: one SQL run |

Totals cover the queries each engine completed. Rows with failures or skipped
queries compare different subsets, as indicated in the Notes column.
