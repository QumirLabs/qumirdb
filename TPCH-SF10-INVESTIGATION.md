# TPC-H SF10 execution gap: QumirDB and DuckDB

On the same SQL and Parquet files, QDB spent **21.983 s** executing the 22
queries and DuckDB spent **15.984 s**. These totals sum the median of three
trials per query. QDB was slower in all three trials on 18 queries. Q4 explains
0.388 s of the 5.999 s gap. The largest gaps were Q5 and Q13.

## Measurement

The original runs recorded 19.837 s of QDB execution and 12.45 s of DuckDB
process wall time. The original DuckDB runner used `/usr/bin/time -p`. It also
used different SQL parameters on some queries, such as the Q9 color, Q13
comment pattern, and Q18 quantity threshold.

The controlled comparison used the exact SQL files from the original QDB
run and the same `/tmp/tpch/pq10` data. It ran in the Linux sandbox on WSL2
with those Parquet files on tmpfs, with 12 scheduler workers for QDB and
`threads=12` for DuckDB. The QDB baseline was a local `RelWithDebInfo` build
from the checkout at `ae7d6e35`. Its binary SHA-256 was
`372a4c93bccf232c1b859d8554cea0cd457ae7ca8ff35a2772eba62cf3226bed`.
DuckDB was 1.5.5 (`d8cdaa33fd`). Each query and engine ran in its own
process. The runner shuffled query order and alternated which engine ran
first. Both emitted CSV. QDB used its persistent JIT cache with operator
instrumentation disabled during timing.

QDB time is the CLI's execution interval after planning, kernel generation
and JIT. DuckDB time is an estimate from detailed JSON profiling:
`latency - (planner + all_optimizers + physical_planner)`. View registration
and process startup happened outside that interval. The two timers differ in
their accounting for result delivery and profiling overhead. [QDB timer](bin/cli.cpp) ·
[DuckDB timing phases](https://github.com/duckdb/duckdb/blob/d8cdaa33fd/src/main/client_context.cpp#L364-L422)

Regenerate the QDB SQL for the same dataset layout with:

```sh
MODE=sql RENDER_ONLY=1 OUT_DIR=build/tpch-investigation/rendered \
  benchmark/tpch/run_tpch.sh /tmp/tpch 10
```

For DuckDB, create a view over `read_parquet('/tmp/tpch/pq10/<table>.parquet')`
for each TPC-H table before profiling the rendered SQL. The original local
artifacts are under `/tmp/qdb-tpch-debug`: `matched/all-runs.csv` records
individual measurements and `qdb-operator-stages.csv` records instrumented
operator counts and CPU time.

## Execution times

Times are seconds. The trial columns preserve the run order within each query.

| Query | QDB trials | DuckDB trials | QDB median | DuckDB median | Gap |
| --- | --- | --- | ---: | ---: | ---: |
| Q1 | 1.203, 1.848, 1.317 | 0.883, 0.937, 0.638 | 1.317 | 0.883 | +0.434 |
| Q2 | 0.406, 0.448, 0.332 | 0.157, 0.230, 0.172 | 0.406 | 0.172 | +0.234 |
| Q3 | 1.044, 0.982, 1.059 | 0.672, 0.809, 0.742 | 1.044 | 0.742 | +0.301 |
| Q4 | 1.015, 0.857, 0.871 | 0.532, 0.483, 0.405 | 0.871 | 0.483 | +0.388 |
| Q5 | 1.649, 1.524, 1.322 | 0.815, 0.945, 0.806 | 1.524 | 0.815 | +0.708 |
| Q6 | 0.511, 0.494, 0.456 | 0.234, 0.268, 0.255 | 0.494 | 0.255 | +0.239 |
| Q7 | 1.078, 1.252, 1.064 | 0.619, 0.845, 0.609 | 1.078 | 0.619 | +0.459 |
| Q8 | 0.763, 0.878, 0.769 | 0.775, 0.836, 0.579 | 0.769 | 0.775 | −0.006 |
| Q9 | 2.062, 1.929, 1.732 | 1.702, 1.969, 1.924 | 1.929 | 1.924 | +0.005 |
| Q10 | 1.460, 1.330, 1.589 | 1.121, 1.063, 1.014 | 1.460 | 1.063 | +0.396 |
| Q11 | 0.172, 0.244, 0.169 | 0.115, 0.148, 0.103 | 0.172 | 0.115 | +0.057 |
| Q12 | 0.841, 1.011, 0.818 | 0.529, 0.471, 0.393 | 0.841 | 0.471 | +0.370 |
| Q13 | 1.903, 2.033, 1.837 | 1.681, 1.246, 1.196 | 1.903 | 1.246 | +0.657 |
| Q14 | 0.451, 0.579, 0.464 | 0.446, 0.681, 0.421 | 0.464 | 0.446 | +0.018 |
| Q15 | 0.504, 0.628, 0.453 | 0.385, 0.421, 0.367 | 0.504 | 0.385 | +0.119 |
| Q16 | 0.309, 0.322, 0.322 | 0.272, 0.287, 0.285 | 0.322 | 0.285 | +0.037 |
| Q17 | 0.855, 0.810, 0.662 | 0.475, 0.548, 0.455 | 0.810 | 0.475 | +0.335 |
| Q18 | 1.579, 1.550, 1.525 | 1.272, 1.205, 1.267 | 1.550 | 1.267 | +0.283 |
| Q19 | 1.776, 1.360, 1.262 | 0.864, 0.995, 0.649 | 1.360 | 0.864 | +0.495 |
| Q20 | 0.579, 0.723, 0.635 | 0.404, 0.583, 0.435 | 0.635 | 0.435 | +0.199 |
| Q21 | 2.195, 2.233, 1.965 | 1.823, 1.911, 1.671 | 2.195 | 1.823 | +0.372 |
| Q22 | 0.336, 0.353, 0.303 | 0.438, 0.439, 0.412 | 0.336 | 0.438 | −0.102 |
| **Sum of medians** | | | **21.983** | **15.984** | **+5.999** |

Comparing CSV values as multisets, with numeric formatting normalized, gave
exact matches on 20 queries in all three runs. Q1 differed in the final digit
of one `AVG`. Q17 differed in the printed precision of a decimal division
(`3186135.45` versus `3186135.455714286`).

## Where the extra work comes from

- **Q5, join order.** QDB joins `lineitem` with date-filtered orders before
  restricting customers to the region. That join emits 9.11 million rows
  versus 1.82 million with the earlier region restriction in DuckDB.
- **Q13, outer join and exchange.** Both plans emit 15.34 million rows from the
  outer join. The join is the most expensive QDB stage in the operator profile.
  QDB then partially aggregates and sends 10.04 million rows through another
  hash exchange on the customer key, which already partitioned the join.
- **Q4, filter direction.** The baseline builds a left semi join on 574,000
  filtered orders. Its runtime filter rule only permits publishing from the
  right. The large `lineitem` stream sends 37.93 million rows through the
  exchange and the semi join returns 525,915 orders. In an equivalent SQL
  experiment an inner join supplies the filter from the small orders input.
  The `lineitem` exchange shrinks to 1.45 million rows and the median goes
  from 0.834 s to 0.604 s in three paired trials with identical results.
- **Q6 and Q12, scan and filtering.** QDB decodes projected columns for the
  whole scan batch and applies the row predicate in a separate operator.
  In Q6 it also computes the projection over the entire batch, even though
  just 1.14 million of 59.99 million rows pass. In Q12 DuckDB applies part of
  the predicate during decoding and emits 9.11 million rows from the scan.
  Disabling DuckDB's `filter_pushdown` raised the Q12 median from 0.361 s to
  0.750 s in a separate three-trial comparison.
  [DuckDB's Parquet scan](https://github.com/duckdb/duckdb/blob/d8cdaa33fd/extension/parquet/parquet_reader.cpp#L1444-L1474)
- **Q18, aggregation exchange.** The baseline moves 59.99 million rows
  (720 MB of logical payload) before grouping by order key. Reducing
  `aggregate:partial_min_reduction_factor` to 2 cuts this to 15.00 million
  rows (300 MB). The extra grouping work offsets that saving in three paired
  trials (1.480 s versus 1.524 s median). Disabling runtime filters made Q18
  slower, since they already cut the inputs of the later joins.

Six workers instead of 12 left a clear gap in the selected Q4, Q6, Q7 and Q12
checks. A single shuffle partition increased the median for all six tested
queries.

## Small change tested: publish semi-join filters from either side

An equi-semi join can discard a right row whose key is absent from the left
input. The optimizer now considers the left side for filter publication
using the same cost model as the right. With a left build, a possible match
from the filter still goes through the existing join kernel, which marks
matching left rows and emits each once. The change is confined to equi-semi
joins without residual predicates and uses the existing filter transport
and join kernels. [Planner rule](qdb/plan/passes/runtime_filters.cpp) ·
[Join contract](docs/arch/join.md)

Q4 was the only changed optimized plan among the 22 SF10 queries. With the
original Q4 SQL, the `lineitem` exchange fell from **37,929,348** to
**1,450,471** rows (151.7 MB to 5.8 MB of logical payload). The join emitted
525,915 orders in both profiles. The CSV results were identical in all
seven timed pairs.

Each binary ran once to warm up. Both used the same build configuration,
SQL, 12 workers, CSV sink and JIT cache. The run order alternated within
each pair. Operator instrumentation ran separately from the timed trials.

| Pair | Baseline execution (s) | Left filter execution (s) |
| --- | ---: | ---: |
| 1 | 0.850 | 0.538 |
| 2 | 0.785 | 0.550 |
| 3 | 0.782 | 0.498 |
| 4 | 0.839 | 0.672 |
| 5 | 0.773 | 0.597 |
| 6 | 0.816 | 0.514 |
| 7 | 0.820 | 0.498 |
| **Median** | **0.816** | **0.538** |

This cuts Q4 execution time by **34%**. Median execution CPU time fell from
8.961 s to 5.792 s. The individual timings and operator logs are under
`build/tpch-investigation/`. There was no full-suite timing rerun of the
candidate.

The join tests cover filter placement, duplicate left and right keys, and
ordinary equality on nullable keys. The full native SQL regression run found
one expected plan change in TPC-DS Q95. The updated case passed with its
existing result expectation. Browser plan-export integration passed.
The browser SQL suite passed 797 cases and failed the Rust external-module
case, which also fails on the baseline because the local toolchain is
missing the standard-library sources needed to build that fixture.
