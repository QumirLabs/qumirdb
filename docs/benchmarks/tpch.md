# TPC-H benchmark results

See the [benchmark index](README.md) for the test machine, engine builds,
measurement definitions, coverage, and summary across benchmarks.

DataFusion CLI 55.2.0 was run on 2026-10-25 with 18 target partitions. Its
column shows the fastest complete run by reported total: the third run at
scales 1 and 10 and the second at scale 100. Before timing, the runner removes
the precision suffix from Q1's day interval and translates qgen's trailing
`ROWNUM` clause to `LIMIT` (or removes its `-1` no-limit sentinel). It also
materializes Q15's `revenue0` aggregate once so both references use the same
floating-point result. Each timing includes CLI startup, Parquet table
registration, query execution, and CSV output. Ratios to DataFusion use the
rounded values shown in the tables. The complete-run totals are listed above
each scale's table.

ClickHouse local 26.9.1.347 (official build) runs were supplied on 2026-11-08
for scales 1, 10 and 100, with 18 threads and one measurement per query over
`pq1`, `pq10` and `pq100`. Times are full process wall-clock time, as defined
in the benchmark index. Ratios to ClickHouse use the rounded qdb values shown
in the tables.

## Scale 1

qdb re-run 2026-10-25 in SQL mode at scale 1, one run per query on the
Apple M5 Pro.

All 22 queries completed with status `OK`; no failures. Runner-reported totals:
processing **0.777 s**, CPU **5.335 s**. Compilation is excluded from processing:
planning **0.282 s**, kernel build **0.555 s**, LLVM **7.190 s**.
The runner rounds cumulative totals to milliseconds after each query.
The DuckDB column is still the 2026-08-22 run.
DataFusion complete-run totals: 0.970, 0.980, and 0.960 s.
ClickHouse complete-run total: 2.610 s.

| Query | qdb (s) | DuckDB (s) | DataFusion (s) | ClickHouse (s) | qdb ÷ DuckDB | qdb ÷ DataFusion | qdb ÷ ClickHouse |
| --- | --- | --- | --- | --- | --- | --- | --- |
| q1 | 0.034 | 0.070 | 0.06 | 0.09 | 0.49× | 0.57× | 0.38× |
| q2 | 0.026 | 0.030 | 0.03 | 0.13 | 0.86× | 0.87× | 0.20× |
| q3 | 0.040 | 0.050 | 0.04 | 0.13 | 0.81× | 1.00× | 0.31× |
| q4 | 0.035 | 0.040 | 0.03 | 0.09 | 0.86× | 1.17× | 0.39× |
| q5 | 0.033 | 0.050 | 0.04 | 0.14 | 0.66× | 0.83× | 0.24× |
| q6 | 0.016 | 0.030 | 0.02 | 0.08 | 0.52× | 0.80× | 0.20× |
| q7 | 0.024 | 0.050 | 0.05 | 0.13 | 0.47× | 0.48× | 0.18× |
| q8 | 0.040 | 0.050 | 0.04 | 0.16 | 0.81× | 1.00× | 0.25× |
| q9 | 0.059 | 0.080 | 0.06 | 0.19 | 0.74× | 0.98× | 0.31× |
| q10 | 0.042 | 0.080 | 0.06 | 0.14 | 0.52× | 0.70× | 0.30× |
| q11 | 0.011 | 0.020 | 0.02 | 0.09 | 0.53× | 0.55× | 0.12× |
| q12 | 0.043 | 0.040 | 0.03 | 0.11 | 1.08× | 1.43× | 0.39× |
| q13 | 0.085 | 0.150 | 0.08 | 0.15 | 0.57× | 1.06× | 0.57× |
| q14 | 0.019 | 0.040 | 0.03 | 0.08 | 0.48× | 0.63× | 0.24× |
| q15 | 0.019 | 0.030 | 0.02 | 0.09 | 0.64× | 0.95× | 0.21× |
| q16 | 0.037 | 0.040 | 0.03 | 0.09 | 0.93× | 1.23× | 0.41× |
| q17 | 0.031 | 0.030 | 0.06 | 0.09 | 1.04× | 0.52× | 0.34× |
| q18 | 0.042 | 0.060 | 0.11 | 0.09 | 0.70× | 0.38× | 0.47× |
| q19 | 0.041 | 0.050 | 0.04 | 0.11 | 0.82× | 1.03× | 0.37× |
| q20 | 0.026 | 0.050 | 0.03 | 0.13 | 0.52× | 0.87× | 0.20× |
| q21 | 0.055 | 0.100 | 0.06 | 0.21 | 0.55× | 0.92× | 0.26× |
| q22 | 0.019 | 0.030 | 0.02 | 0.09 | 0.62× | 0.95× | 0.21× |
| **total** | **0.777** | **1.17** | **0.960** | **2.610** | **0.66×** | **0.81×** | **0.30×** |

## Scale 10

qdb re-run 2026-10-25 in SQL mode at scale 10, one run per query on the
Apple M5 Pro.

All 22 queries completed with status `OK`; no failures. Runner-reported totals:
processing **3.864 s**, CPU **53.211 s**. Compilation is excluded from processing:
planning **0.381 s**, kernel build **0.856 s**, LLVM **7.340 s**.
The runner rounds cumulative totals to milliseconds after each query.
The DuckDB column is still the 2026-08-22 run.
DataFusion complete-run totals: 4.220, 4.250, and 4.180 s.
ClickHouse complete-run total: 8.510 s.

| Query | qdb (s) | DuckDB (s) | DataFusion (s) | ClickHouse (s) | qdb ÷ DuckDB | qdb ÷ DataFusion | qdb ÷ ClickHouse |
| --- | --- | --- | --- | --- | --- | --- | --- |
| q1 | 0.175 | 0.260 | 0.21 | 0.38 | 0.67× | 0.83× | 0.46× |
| q2 | 0.090 | 0.060 | 0.08 | 0.20 | 1.50× | 1.13× | 0.45× |
| q3 | 0.171 | 0.170 | 0.13 | 0.50 | 1.01× | 1.32× | 0.34× |
| q4 | 0.166 | 0.110 | 0.08 | 0.24 | 1.51× | 2.08× | 0.69× |
| q5 | 0.225 | 0.170 | 0.19 | 0.65 | 1.32× | 1.18× | 0.35× |
| q6 | 0.107 | 0.110 | 0.10 | 0.19 | 0.97× | 1.07× | 0.56× |
| q7 | 0.185 | 0.170 | 0.25 | 0.37 | 1.09× | 0.74× | 0.50× |
| q8 | 0.172 | 0.180 | 0.21 | 0.52 | 0.95× | 0.82× | 0.33× |
| q9 | 0.320 | 0.340 | 0.32 | 1.11 | 0.94× | 1.00× | 0.29× |
| q10 | 0.311 | 0.360 | 0.30 | 0.48 | 0.86× | 1.04× | 0.65× |
| q11 | 0.033 | 0.050 | 0.04 | 0.12 | 0.67× | 0.83× | 0.28× |
| q12 | 0.155 | 0.130 | 0.10 | 0.33 | 1.20× | 1.55× | 0.47× |
| q13 | 0.283 | 0.250 | 0.14 | 0.40 | 1.13× | 2.02× | 0.71× |
| q14 | 0.098 | 0.160 | 0.10 | 0.23 | 0.61× | 0.98× | 0.43× |
| q15 | 0.089 | 0.110 | 0.09 | 0.32 | 0.81× | 0.99× | 0.28× |
| q16 | 0.117 | 0.090 | 0.07 | 0.15 | 1.30× | 1.67× | 0.78× |
| q17 | 0.158 | 0.150 | 0.29 | 0.31 | 1.05× | 0.54× | 0.51× |
| q18 | 0.245 | 0.220 | 0.66 | 0.29 | 1.11× | 0.37× | 0.84× |
| q19 | 0.211 | 0.220 | 0.15 | 0.37 | 0.96× | 1.41× | 0.57× |
| q20 | 0.129 | 0.170 | 0.14 | 0.34 | 0.76× | 0.92× | 0.38× |
| q21 | 0.355 | 0.410 | 0.43 | 0.82 | 0.87× | 0.83× | 0.43× |
| q22 | 0.069 | 0.140 | 0.10 | 0.19 | 0.49× | 0.69× | 0.36× |
| **total** | **3.864** | **4.03** | **4.180** | **8.510** | **0.96×** | **0.92×** | **0.45×** |

## Scale 100

qdb re-run 2026-10-29 in SQL mode at scale 100, one run per query on the
Apple M5 Pro.

All 22 queries completed with status `OK`; no failures. Runner-reported totals:
processing **32.843 s**, CPU **533.078 s**. Compilation is excluded from processing:
planning **0.621 s**, kernel build **1.191 s**, LLVM **8.154 s**.
The runner rounds cumulative totals to milliseconds after each query.
The DuckDB column is still the 2026-08-22 run.
DataFusion complete-run totals: 65.210 and 62.680 s.
ClickHouse complete-run total: 360.240 s.

| Query | qdb (s) | DuckDB (s) | DataFusion (s) | ClickHouse (s) | qdb ÷ DuckDB | qdb ÷ DataFusion | qdb ÷ ClickHouse |
| --- | --- | --- | --- | --- | --- | --- | --- |
| q1 | 1.542 | 2.200 | 1.65 | 3.30 | 0.70× | 0.93× | 0.47× |
| q2 | 0.552 | 0.420 | 0.56 | 0.77 | 1.31× | 0.99× | 0.72× |
| q3 | 1.683 | 1.390 | 1.19 | 30.18 | 1.21× | 1.41× | 0.06× |
| q4 | 1.082 | 0.930 | 0.59 | 1.10 | 1.16× | 1.83× | 0.98× |
| q5 | 2.512 | 1.450 | 1.87 | 54.38 | 1.73× | 1.34× | 0.05× |
| q6 | 0.591 | 0.870 | 0.65 | 1.43 | 0.68× | 0.91× | 0.41× |
| q7 | 1.786 | 1.350 | 3.08 | 4.05 | 1.32× | 0.58× | 0.44× |
| q8 | 1.328 | 1.570 | 1.68 | 110.26 | 0.85× | 0.79× | 0.01× |
| q9 | 2.963 | 3.090 | 3.23 | 120.44 | 0.96× | 0.92× | 0.02× |
| q10 | 2.123 | 1.770 | 1.74 | 3.88 | 1.20× | 1.22× | 0.55× |
| q11 | 0.272 | 0.260 | 0.42 | 0.65 | 1.05× | 0.65× | 0.42× |
| q12 | 1.383 | 1.090 | 0.92 | 3.42 | 1.27× | 1.50× | 0.40× |
| q13 | 3.198 | 2.190 | 1.75 | 3.02 | 1.46× | 1.83× | 1.06× |
| q14 | 0.783 | 1.130 | 0.78 | 1.66 | 0.69× | 1.00× | 0.47× |
| q15 | 0.696 | 1.080 | 0.71 | 3.07 | 0.64× | 0.98× | 0.23× |
| q16 | 0.554 | 0.460 | 0.41 | 0.52 | 1.20× | 1.35× | 1.07× |
| q17 | 0.998 | 1.280 | 3.76 | 2.14 | 0.78× | 0.27× | 0.47× |
| q18 | 2.040 | 2.300 | 29.72 | 1.93 | 0.89× | 0.07× | 1.06× |
| q19 | 1.956 | 1.610 | 1.00 | 2.85 | 1.21× | 1.96× | 0.69× |
| q20 | 0.848 | 1.160 | 1.15 | 1.86 | 0.73× | 0.74× | 0.46× |
| q21 | 3.409 | 3.890 | 5.46 | 8.74 | 0.88× | 0.62× | 0.39× |
| q22 | 0.544 | 0.610 | 0.36 | 0.59 | 0.89× | 1.51× | 0.92× |
| **total** | **32.843** | **32.10** | **62.680** | **360.240** | **1.02×** | **0.52×** | **0.09×** |

## Distributed runs (4 nodes)

Distributed runs on a 4-node cluster were made on 2026-11-14 at scales 10 and
100. They are separate from the single-machine results above and are not
included in the benchmark index summary.

### Cluster

- **Hardware:** 4 × Hetzner Cloud CCX13 (2 dedicated vCPU, 8 GB RAM, about
  75 GB usable local disk each), location fsn1, private network between the
  nodes (MTU 1450), image ubuntu-26.04.
- **CPU and OS:** AMD EPYC Milan (`lscpu`: `AMD EPYC-Milan Processor`). The
  2 vCPU are one core with 2 SMT threads, with 512 KiB L2 and 32 MiB L3; no
  AVX-512. Ubuntu 26.04.1 LTS, kernel 7.0.0-30-generic. The CPU model was not
  recorded during the runs. These values come from another CCX13 created in
  fsn1 on 2026-11-15; Hetzner places CCX servers on either Milan or Genoa
  hosts.
- **Data:** TPC-H Parquet with decimals stored as INT64 (the decimal datasets),
  a full copy on every node.
- **qdb:** `qdbd` from master `b54159d0`, Debian packages 0.5.1-22 from
  Actions run 43645114019, one 4-node cluster, network poller epoll
  (`--net:poller=epoll`). Queries are the `benchmark/tpch` templates with
  Q11 FRACTION = 0.0001/SF (#621). qdb runs the original TPC-H SQL.
- **ClickHouse:** 26.8.37, MergeTree tables, set up following the
  [ClickHouse TPC-H guide](https://clickhouse.com/docs/getting-started/example-datasets/tpch)
  and the DDL from ClickHouse's `tests/benchmarks/tpc-h` (UInt32/Int32,
  Decimal(12,2), FixedString, PRIMARY KEY per spec). The 4-shard layout is
  ours; the guide does not describe one:
  - `lineitem` and `orders` are sharded by `orderkey`; `nation`, `region`,
    `supplier`, `customer`, `part` and `partsupp` are fully copied to every
    node.
  - `distributed_product_mode=local`, `join_use_nulls=1`; q17 and q20 use
    `GLOBAL JOIN`.
  - After loading, `OPTIMIZE TABLE ... FINAL`.
  - ClickHouse 26.8 does not run correlated subqueries on Distributed tables,
    so q2, q4, q17, q20, q21 and q22 (marked \*) are decorrelated rewrites
    for ClickHouse only.
- **Measurement:** execution time only: qdb `X-Qdb-Execute-Seconds`,
  ClickHouse `clickhouse-client --time`. Each query is the best of 3 hot runs
  after 1 cold run; the page cache is dropped before each query's cold run.
  Loading into MergeTree, planning, JIT compilation and wall time are
  excluded. Totals are the sums of the per-query values. Ratios use the
  rounded values shown.

### Scale 10

All 22 queries completed on both systems. 21 of 22 qdb answers match DuckDB
on the same Parquet and queries; q17 differs in the last printed digit
(decimal quotient scale #405 and truncation #623).

| Query | qdb (s) | ClickHouse (s) | qdb ÷ ClickHouse |
| --- | --- | --- | --- |
| q1 | 0.863 | 0.335 | 2.58× |
| q2\* | 0.227 | 0.421 | 0.54× |
| q3 | 0.654 | 4.601 | 0.14× |
| q4\* | 0.430 | 0.521 | 0.83× |
| q5 | 0.884 | 1.657 | 0.53× |
| q6 | 0.201 | 0.143 | 1.41× |
| q7 | 0.634 | 0.973 | 0.65× |
| q8 | 0.576 | 1.235 | 0.47× |
| q9 | 1.425 | 23.279 | 0.06× |
| q10 | 1.470 | 1.554 | 0.95× |
| q11 | 0.140 | 0.175 | 0.80× |
| q12 | 0.679 | 0.180 | 3.77× |
| q13 | 1.069 | 1.363 | 0.78× |
| q14 | 0.323 | 0.168 | 1.92× |
| q15 | 0.246 | 0.424 | 0.58× |
| q16 | 0.445 | 0.277 | 1.61× |
| q17\* | 0.606 | 0.933 | 0.65× |
| q18 | 0.940 | 4.489 | 0.21× |
| q19 | 0.950 | 0.341 | 2.79× |
| q20\* | 0.377 | 0.431 | 0.87× |
| q21\* | 1.468 | 2.382 | 0.62× |
| q22\* | 0.265 | 0.226 | 1.17× |
| **total** | **14.87** | **46.11** | **0.32×** |

\* Decorrelated rewrite for ClickHouse; qdb runs the original SQL.

### Scale 100

No answer check was made at scale 100. Peak memory: `qdbd` about 4 GB per
node, `clickhouse-server` 5.8-6.3 GB RSS per node. Loading into MergeTree took
282 s, plus 80 s for `OPTIMIZE TABLE ... FINAL`; both are excluded from the
times.

| Query | qdb (s) | ClickHouse (s) | qdb ÷ ClickHouse |
| --- | --- | --- | --- |
| q1 | 8.401 | 3.234 | 2.60× |
| q2\* | 2.153 | 5.110 | 0.42× |
| q3 | 6.999 | 67.810 | 0.10× |
| q4\* | 4.605 | 7.908 | 0.58× |
| q5 | 17.994 | 16.768 | 1.07× |
| q6 | 1.905 | 1.406 | 1.35× |
| q7 | 8.204 | 29.908 | 0.27× |
| q8 | 6.104 | 13.575 | 0.45× |
| q9 | 15.840 | `MEMORY_LIMIT_EXCEEDED`† | — |
| q10 | 13.591 | 55.289 | 0.25× |
| q11 | 1.312 | 2.004 | 0.65× |
| q12 | 6.483 | 2.072 | 3.13× |
| q13 | 13.787 | 46.637 | 0.30× |
| q14 | 2.824 | 1.872 | 1.51× |
| q15 | 2.635 | 2.705 | 0.97× |
| q16 | 3.093 | 3.437 | 0.90× |
| q17\* | 5.279 | 18.793 | 0.28× |
| q18 | 11.334 | `MEMORY_LIMIT_EXCEEDED`† | — |
| q19 | 9.034 | 3.611 | 2.50× |
| q20\* | 3.795 | 4.322 | 0.88× |
| q21\* | 18.309 | 90.729 | 0.20× |
| q22\* | 3.041 | 3.895 | 0.78× |
| **total, 20 queries completed by both** | **139.55** | **381.08** | **0.37×** |

\* Decorrelated rewrite for ClickHouse; qdb runs the original SQL.

† ClickHouse failed with `MEMORY_LIMIT_EXCEEDED` (server limit 5.71 GiB, 90% of
RAM). For q9 the cold run passed in 81.2 s and all 3 hot runs failed; q18
failed in all runs. The totals cover the 20 queries both systems completed;
qdb over all 22 queries: 166.72 s.
