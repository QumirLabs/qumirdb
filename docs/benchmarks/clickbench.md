# ClickBench benchmark results

See the [benchmark index](README.md) for the test machine, engine builds,
measurement definitions, coverage, and summary across benchmarks.

## Results

qdb re-run 2026-10-28 in SQL mode, one run per query on the Apple M5 Pro.

All 43 queries completed with status `OK`; no failures or skipped queries.
Runner-reported totals: processing **13.499 s**, CPU **186.567 s**.
Compilation is excluded from processing: planning **1.522 s**, kernel build
**0.787 s**, LLVM **3.757 s**. The runner rounds cumulative totals to
milliseconds after each query.
The DuckDB column is still the 2026-08-22 run, reporting the best of several
measurements. The ClickHouse column is still the 2026-08-30 run, with one
measurement per query.

| Query | qdb (s) | DuckDB (s) | ClickHouse (s) | qdb ÷ DuckDB | qdb ÷ ClickHouse |
|---|---|---|---|---|---|
| q0 | 0.109 | 0.040 | 0.130 | 2.74× | 0.84× |
| q1 | 0.047 | 0.050 | 0.120 | 0.94× | 0.39× |
| q2 | 0.258 | 0.070 | 0.130 | 3.69× | 1.99× |
| q3 | 0.191 | 0.070 | 0.130 | 2.73× | 1.47× |
| q4 | 0.194 | 0.160 | 0.200 | 1.21× | 0.97× |
| q5 | 0.255 | 0.230 | 0.250 | 1.11× | 1.02× |
| q6 | 0.154 | 0.090 | 0.120 | 1.71× | 1.29× |
| q7 | 0.012 | 0.050 | 0.120 | 0.23× | 0.10× |
| q8 | 0.250 | 0.200 | 0.280 | 1.25× | 0.89× |
| q9 | 0.334 | 0.270 | 0.330 | 1.24× | 1.01× |
| q10 | 0.101 | 0.100 | 0.200 | 1.01× | 0.50× |
| q11 | 0.118 | 0.110 | 0.210 | 1.08× | 0.56× |
| q12 | 0.285 | 0.230 | 0.270 | 1.24× | 1.06× |
| q13 | 0.478 | 0.360 | 0.370 | 1.33× | 1.29× |
| q14 | 0.253 | 0.260 | 0.340 | 0.97× | 0.75× |
| q15 | 0.206 | 0.190 | 0.200 | 1.08× | 1.03× |
| q16 | 0.500 | 0.430 | 0.500 | 1.16× | 1.00× |
| q17 | 0.529 | 0.350 | 0.290 | 1.51× | 1.82× |
| q18 | 1.201 | 0.680 | 0.850 | 1.77× | 1.41× |
| q19 | 0.078 | 0.050 | 0.120 | 1.57× | 0.65× |
| q20 | 0.378 | 0.410 | 0.580 | 0.92× | 0.65× |
| q21 | 0.442 | 0.350 | 0.650 | 1.26× | 0.68× |
| q22 | 0.759 | 0.620 | 0.960 | 1.22× | 0.79× |
| q23 | 0.482 | 0.530 | 0.680 | 0.91× | 0.71× |
| q24 | 0.150 | 0.200 | 0.320 | 0.75× | 0.47× |
| q25 | 0.123 | 0.170 | 0.230 | 0.72× | 0.53× |
| q26 | 0.149 | 0.210 | 0.330 | 0.71× | 0.45× |
| q27 | 0.362 | 0.340 | 0.630 | 1.07× | 0.58× |
| q28 | 1.052 | 3.560 | 0.680 | 0.30× | 1.55× |
| q29 | 0.176 | 0.060 | 0.130 | 2.93× | 1.35× |
| q30 | 0.229 | 0.280 | 0.430 | 0.82× | 0.53× |
| q31 | 0.268 | 0.310 | 0.470 | 0.86× | 0.57× |
| q32 | 0.660 | 0.780 | 0.870 | 0.85× | 0.76× |
| q33 | 1.098 | 0.880 | 0.900 | 1.25× | 1.22× |
| q34 | 1.124 | 0.920 | 0.900 | 1.22× | 1.25× |
| q35 | 0.180 | 0.250 | 0.180 | 0.72× | 1.00× |
| q36 | 0.035 | 0.100 | 0.130 | 0.35× | 0.27× |
| q37 | 0.026 | 0.090 | 0.120 | 0.28× | 0.21× |
| q38 | 0.029 | 0.070 | 0.110 | 0.41× | 0.26× |
| q39 | 0.192 | 0.140 | 0.150 | 1.37× | 1.28× |
| q40 | 0.013 | 0.060 | 0.120 | 0.22× | 0.11× |
| q41 | 0.011 | 0.060 | 0.120 | 0.18× | 0.09× |
| q42 | 0.008 | 0.060 | 0.100 | 0.14× | 0.08× |
| **total** | **13.499** | **14.44** | **14.95** | **0.93×** | **0.90×** |

## Global aggregate cascade

A/B of 2026-10-29 on the queries without `GROUP BY` that end in a global
aggregate (q5 needs a string aggregate qdb lacks). Four arms: master
`b723dbf7`; the same build with `[aggregate] cascade_global` at its new default
`auto`, forced `true` and forced `false` (`8d9a2ea3`). Each arm has its own
persistent JIT cache. One warm-up run per query and arm, then five timed
rounds with the arms alternated; the table shows the **median** processing
time of the five. Every arm returned identical rows.

| Query | master (s) | auto (s) | true (s) | false (s) |
|---|---|---|---|---|
| q0 | 0.108 | 0.014 | 0.014 | 0.107 |
| q1 | 0.047 | 0.015 | 0.015 | 0.045 |
| q2 | 0.252 | 0.045 | 0.045 | 0.252 |
| q3 | 0.188 | 0.038 | 0.038 | 0.186 |
| q4 | 0.187 | 0.167 | 0.168 | 0.185 |
| q6 | 0.154 | 0.023 | 0.022 | 0.154 |
| q20 | 0.369 | 0.369 | 0.368 | 0.372 |
| q29 | 0.170 | 0.031 | 0.031 | 0.172 |
| **sum** | **1.475** | **0.704** | **0.702** | **1.474** |

`auto` chooses the cascade for every query here (the estimated input of each
global aggregate is far above the 1,048,576-row bound) and matches forced
`true`. q4 and q20 are bound by the `COUNT(DISTINCT)` dedup and the `LIKE`
scan, so the serial fold was not their bottleneck.

On a cold JIT cache the cascade compiles a partial and a combine program
instead of one: LLVM time rose from about 0.20 s to 0.33 s on q2 and from
0.40 s to 0.52 s on q29 (three runs each). Processing excludes compilation,
and the persistent cache amortizes it across runs.
