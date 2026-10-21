# TPC-H benchmark results

See the [benchmark index](README.md) for the test machine, engine builds,
measurement definitions, coverage, and summary across benchmarks.

## Scale 1

qdb re-run 2026-10-21 in SQL mode, one run per query on the Apple M5 Pro.
Command from `build/`:

```sh
MODE=sql ../benchmark/tpch/run_tpch.sh ~/Projects/tpch 1
```

All 22 queries completed with status `OK`; no failures. Runner-reported totals:
processing **0.857 s**, CPU **5.957 s**. Compilation is excluded from processing:
planning **0.192 s**, kernel build **0.567 s**, LLVM **6.644 s**.
The runner rounds cumulative totals to milliseconds after each query.
The DuckDB column is still the 2026-08-22 run.

| Query | qdb (s) | DuckDB (s) | qdb ÷ DuckDB |
| --- | --- | --- | --- |
| q1 | 0.034 | 0.070 | 0.48× |
| q2 | 0.026 | 0.030 | 0.88× |
| q3 | 0.043 | 0.050 | 0.85× |
| q4 | 0.041 | 0.040 | 1.03× |
| q5 | 0.045 | 0.050 | 0.89× |
| q6 | 0.015 | 0.030 | 0.51× |
| q7 | 0.027 | 0.050 | 0.55× |
| q8 | 0.043 | 0.050 | 0.85× |
| q9 | 0.077 | 0.080 | 0.96× |
| q10 | 0.047 | 0.080 | 0.59× |
| q11 | 0.011 | 0.020 | 0.54× |
| q12 | 0.045 | 0.040 | 1.14× |
| q13 | 0.094 | 0.150 | 0.62× |
| q14 | 0.021 | 0.040 | 0.52× |
| q15 | 0.021 | 0.030 | 0.71× |
| q16 | 0.037 | 0.040 | 0.94× |
| q17 | 0.032 | 0.030 | 1.08× |
| q18 | 0.042 | 0.060 | 0.71× |
| q19 | 0.044 | 0.050 | 0.88× |
| q20 | 0.027 | 0.050 | 0.55× |
| q21 | 0.065 | 0.100 | 0.65× |
| q22 | 0.020 | 0.030 | 0.65× |
| **total** | **0.857** | **1.17** | **0.73×** |

## Scale 10

qdb re-run 2026-10-21 in SQL mode, one run per query on the Apple M5 Pro.
Command from `build/`:

```sh
MODE=sql ../benchmark/tpch/run_tpch.sh ~/Projects/tpch 10
```

All 22 queries completed with status `OK`; no failures. Runner-reported totals:
processing **4.733 s**, CPU **63.646 s**. Compilation is excluded from processing:
planning **0.286 s**, kernel build **0.843 s**, LLVM **6.837 s**.
The runner rounds cumulative totals to milliseconds after each query.
The DuckDB column is still the 2026-08-22 run.

| Query | qdb (s) | DuckDB (s) | qdb ÷ DuckDB |
| --- | --- | --- | --- |
| q1 | 0.173 | 0.260 | 0.67× |
| q2 | 0.090 | 0.060 | 1.50× |
| q3 | 0.207 | 0.170 | 1.22× |
| q4 | 0.203 | 0.110 | 1.84× |
| q5 | 0.289 | 0.170 | 1.70× |
| q6 | 0.101 | 0.110 | 0.92× |
| q7 | 0.190 | 0.170 | 1.12× |
| q8 | 0.195 | 0.180 | 1.09× |
| q9 | 0.485 | 0.340 | 1.43× |
| q10 | 0.347 | 0.360 | 0.96× |
| q11 | 0.035 | 0.050 | 0.69× |
| q12 | 0.173 | 0.130 | 1.33× |
| q13 | 0.517 | 0.250 | 2.07× |
| q14 | 0.140 | 0.160 | 0.87× |
| q15 | 0.088 | 0.110 | 0.80× |
| q16 | 0.134 | 0.090 | 1.49× |
| q17 | 0.173 | 0.150 | 1.16× |
| q18 | 0.272 | 0.220 | 1.24× |
| q19 | 0.265 | 0.220 | 1.20× |
| q20 | 0.154 | 0.170 | 0.90× |
| q21 | 0.417 | 0.410 | 1.02× |
| q22 | 0.085 | 0.140 | 0.61× |
| **total** | **4.733** | **4.03** | **1.17×** |

## Scale 100

qdb re-run 2026-10-21 in SQL mode, one run per query on the Apple M5 Pro.
Command from `build/`:

```sh
MODE=sql ../benchmark/tpch/run_tpch.sh ~/Projects/tpch 100
```

All 22 queries completed with status `OK`; no failures. Runner-reported totals:
processing **49.843 s**, CPU **708.112 s**. Compilation is excluded from processing:
planning **0.676 s**, kernel build **1.212 s**, LLVM **7.325 s**.
The runner rounds cumulative totals to milliseconds after each query.
The DuckDB column is still the 2026-08-22 run.

| Query | qdb (s) | DuckDB (s) | qdb ÷ DuckDB |
| --- | --- | --- | --- |
| q1 | 1.481 | 2.200 | 0.67× |
| q2 | 0.560 | 0.420 | 1.33× |
| q3 | 2.120 | 1.390 | 1.53× |
| q4 | 2.553 | 0.930 | 2.75× |
| q5 | 3.417 | 1.450 | 2.36× |
| q6 | 0.922 | 0.870 | 1.06× |
| q7 | 2.115 | 1.350 | 1.57× |
| q8 | 1.665 | 1.570 | 1.06× |
| q9 | 5.125 | 3.090 | 1.66× |
| q10 | 2.697 | 1.770 | 1.52× |
| q11 | 0.314 | 0.260 | 1.21× |
| q12 | 1.755 | 1.090 | 1.61× |
| q13 | 6.566 | 2.190 | 3.00× |
| q14 | 1.162 | 1.130 | 1.03× |
| q15 | 1.011 | 1.080 | 0.94× |
| q16 | 0.705 | 0.460 | 1.53× |
| q17 | 1.428 | 1.280 | 1.12× |
| q18 | 3.285 | 2.300 | 1.43× |
| q19 | 4.019 | 1.610 | 2.50× |
| q20 | 1.411 | 1.160 | 1.22× |
| q21 | 4.575 | 3.890 | 1.18× |
| q22 | 0.957 | 0.610 | 1.57× |
| **total** | **49.843** | **32.10** | **1.55×** |
