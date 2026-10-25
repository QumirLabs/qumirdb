# TPC-H benchmark results

See the [benchmark index](README.md) for the test machine, engine builds,
measurement definitions, coverage, and summary across benchmarks.

## Scale 1

qdb re-run 2026-10-25 in SQL mode, one run per query on the Apple M5 Pro.
Command from `build/`:

```sh
MODE=sql ../benchmark/tpch/run_tpch.sh ~/Projects/tpch 1
```

All 22 queries completed with status `OK`; no failures. Runner-reported totals:
processing **0.777 s**, CPU **5.335 s**. Compilation is excluded from processing:
planning **0.282 s**, kernel build **0.555 s**, LLVM **7.190 s**.
The runner rounds cumulative totals to milliseconds after each query.
The DuckDB column is still the 2026-08-22 run.

| Query | qdb (s) | DuckDB (s) | qdb ÷ DuckDB |
| --- | --- | --- | --- |
| q1 | 0.034 | 0.070 | 0.49× |
| q2 | 0.026 | 0.030 | 0.86× |
| q3 | 0.040 | 0.050 | 0.81× |
| q4 | 0.035 | 0.040 | 0.86× |
| q5 | 0.033 | 0.050 | 0.66× |
| q6 | 0.016 | 0.030 | 0.52× |
| q7 | 0.024 | 0.050 | 0.47× |
| q8 | 0.040 | 0.050 | 0.81× |
| q9 | 0.059 | 0.080 | 0.74× |
| q10 | 0.042 | 0.080 | 0.52× |
| q11 | 0.011 | 0.020 | 0.53× |
| q12 | 0.043 | 0.040 | 1.08× |
| q13 | 0.085 | 0.150 | 0.57× |
| q14 | 0.019 | 0.040 | 0.48× |
| q15 | 0.019 | 0.030 | 0.64× |
| q16 | 0.037 | 0.040 | 0.93× |
| q17 | 0.031 | 0.030 | 1.04× |
| q18 | 0.042 | 0.060 | 0.70× |
| q19 | 0.041 | 0.050 | 0.82× |
| q20 | 0.026 | 0.050 | 0.52× |
| q21 | 0.055 | 0.100 | 0.55× |
| q22 | 0.019 | 0.030 | 0.62× |
| **total** | **0.777** | **1.17** | **0.66×** |

## Scale 10

qdb re-run 2026-10-25 in SQL mode, one run per query on the Apple M5 Pro.
Command from `build/`:

```sh
MODE=sql ../benchmark/tpch/run_tpch.sh ~/Projects/tpch 10
```

All 22 queries completed with status `OK`; no failures. Runner-reported totals:
processing **3.864 s**, CPU **53.211 s**. Compilation is excluded from processing:
planning **0.381 s**, kernel build **0.856 s**, LLVM **7.340 s**.
The runner rounds cumulative totals to milliseconds after each query.
The DuckDB column is still the 2026-08-22 run.

| Query | qdb (s) | DuckDB (s) | qdb ÷ DuckDB |
| --- | --- | --- | --- |
| q1 | 0.175 | 0.260 | 0.67× |
| q2 | 0.090 | 0.060 | 1.50× |
| q3 | 0.171 | 0.170 | 1.01× |
| q4 | 0.166 | 0.110 | 1.51× |
| q5 | 0.225 | 0.170 | 1.32× |
| q6 | 0.107 | 0.110 | 0.97× |
| q7 | 0.185 | 0.170 | 1.09× |
| q8 | 0.172 | 0.180 | 0.95× |
| q9 | 0.320 | 0.340 | 0.94× |
| q10 | 0.311 | 0.360 | 0.86× |
| q11 | 0.033 | 0.050 | 0.67× |
| q12 | 0.155 | 0.130 | 1.20× |
| q13 | 0.283 | 0.250 | 1.13× |
| q14 | 0.098 | 0.160 | 0.61× |
| q15 | 0.089 | 0.110 | 0.81× |
| q16 | 0.117 | 0.090 | 1.30× |
| q17 | 0.158 | 0.150 | 1.05× |
| q18 | 0.245 | 0.220 | 1.11× |
| q19 | 0.211 | 0.220 | 0.96× |
| q20 | 0.129 | 0.170 | 0.76× |
| q21 | 0.355 | 0.410 | 0.87× |
| q22 | 0.069 | 0.140 | 0.49× |
| **total** | **3.864** | **4.03** | **0.96×** |

## Scale 100

qdb re-run 2026-10-25 in SQL mode, one run per query on the Apple M5 Pro.
Command from `build/`:

```sh
MODE=sql ../benchmark/tpch/run_tpch.sh ~/Projects/tpch 100
```

All 22 queries completed with status `OK`; no failures. Runner-reported totals:
processing **36.645 s**, CPU **580.401 s**. Compilation is excluded from processing:
planning **0.731 s**, kernel build **1.140 s**, LLVM **7.603 s**.
The runner rounds cumulative totals to milliseconds after each query.
The DuckDB column is still the 2026-08-22 run.

| Query | qdb (s) | DuckDB (s) | qdb ÷ DuckDB |
| --- | --- | --- | --- |
| q1 | 1.542 | 2.200 | 0.70× |
| q2 | 0.567 | 0.420 | 1.35× |
| q3 | 1.682 | 1.390 | 1.21× |
| q4 | 1.993 | 0.930 | 2.14× |
| q5 | 2.721 | 1.450 | 1.88× |
| q6 | 0.863 | 0.870 | 0.99× |
| q7 | 1.823 | 1.350 | 1.35× |
| q8 | 1.325 | 1.570 | 0.84× |
| q9 | 2.973 | 3.090 | 0.96× |
| q10 | 2.158 | 1.770 | 1.22× |
| q11 | 0.284 | 0.260 | 1.09× |
| q12 | 1.462 | 1.090 | 1.34× |
| q13 | 3.263 | 2.190 | 1.49× |
| q14 | 0.789 | 1.130 | 0.70× |
| q15 | 0.932 | 1.080 | 0.86× |
| q16 | 0.567 | 0.460 | 1.23× |
| q17 | 1.073 | 1.280 | 0.84× |
| q18 | 3.075 | 2.300 | 1.34× |
| q19 | 2.143 | 1.610 | 1.33× |
| q20 | 0.898 | 1.160 | 0.77× |
| q21 | 3.845 | 3.890 | 0.99× |
| q22 | 0.667 | 0.610 | 1.09× |
| **total** | **36.645** | **32.10** | **1.14×** |
