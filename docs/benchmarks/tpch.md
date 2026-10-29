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

## Scale 1

qdb re-run 2026-10-25 in SQL mode at scale 1, one run per query on the
Apple M5 Pro.

All 22 queries completed with status `OK`; no failures. Runner-reported totals:
processing **0.777 s**, CPU **5.335 s**. Compilation is excluded from processing:
planning **0.282 s**, kernel build **0.555 s**, LLVM **7.190 s**.
The runner rounds cumulative totals to milliseconds after each query.
The DuckDB column is still the 2026-08-22 run.
DataFusion complete-run totals: 0.970, 0.980, and 0.960 s.

| Query | qdb (s) | DuckDB (s) | DataFusion (s) | qdb ÷ DuckDB | qdb ÷ DataFusion |
| --- | --- | --- | --- | --- | --- |
| q1 | 0.034 | 0.070 | 0.06 | 0.49× | 0.57× |
| q2 | 0.026 | 0.030 | 0.03 | 0.86× | 0.87× |
| q3 | 0.040 | 0.050 | 0.04 | 0.81× | 1.00× |
| q4 | 0.035 | 0.040 | 0.03 | 0.86× | 1.17× |
| q5 | 0.033 | 0.050 | 0.04 | 0.66× | 0.83× |
| q6 | 0.016 | 0.030 | 0.02 | 0.52× | 0.80× |
| q7 | 0.024 | 0.050 | 0.05 | 0.47× | 0.48× |
| q8 | 0.040 | 0.050 | 0.04 | 0.81× | 1.00× |
| q9 | 0.059 | 0.080 | 0.06 | 0.74× | 0.98× |
| q10 | 0.042 | 0.080 | 0.06 | 0.52× | 0.70× |
| q11 | 0.011 | 0.020 | 0.02 | 0.53× | 0.55× |
| q12 | 0.043 | 0.040 | 0.03 | 1.08× | 1.43× |
| q13 | 0.085 | 0.150 | 0.08 | 0.57× | 1.06× |
| q14 | 0.019 | 0.040 | 0.03 | 0.48× | 0.63× |
| q15 | 0.019 | 0.030 | 0.02 | 0.64× | 0.95× |
| q16 | 0.037 | 0.040 | 0.03 | 0.93× | 1.23× |
| q17 | 0.031 | 0.030 | 0.06 | 1.04× | 0.52× |
| q18 | 0.042 | 0.060 | 0.11 | 0.70× | 0.38× |
| q19 | 0.041 | 0.050 | 0.04 | 0.82× | 1.03× |
| q20 | 0.026 | 0.050 | 0.03 | 0.52× | 0.87× |
| q21 | 0.055 | 0.100 | 0.06 | 0.55× | 0.92× |
| q22 | 0.019 | 0.030 | 0.02 | 0.62× | 0.95× |
| **total** | **0.777** | **1.17** | **0.960** | **0.66×** | **0.81×** |

## Scale 10

qdb re-run 2026-10-25 in SQL mode at scale 10, one run per query on the
Apple M5 Pro.

All 22 queries completed with status `OK`; no failures. Runner-reported totals:
processing **3.864 s**, CPU **53.211 s**. Compilation is excluded from processing:
planning **0.381 s**, kernel build **0.856 s**, LLVM **7.340 s**.
The runner rounds cumulative totals to milliseconds after each query.
The DuckDB column is still the 2026-08-22 run.
DataFusion complete-run totals: 4.220, 4.250, and 4.180 s.

| Query | qdb (s) | DuckDB (s) | DataFusion (s) | qdb ÷ DuckDB | qdb ÷ DataFusion |
| --- | --- | --- | --- | --- | --- |
| q1 | 0.175 | 0.260 | 0.21 | 0.67× | 0.83× |
| q2 | 0.090 | 0.060 | 0.08 | 1.50× | 1.13× |
| q3 | 0.171 | 0.170 | 0.13 | 1.01× | 1.32× |
| q4 | 0.166 | 0.110 | 0.08 | 1.51× | 2.08× |
| q5 | 0.225 | 0.170 | 0.19 | 1.32× | 1.18× |
| q6 | 0.107 | 0.110 | 0.10 | 0.97× | 1.07× |
| q7 | 0.185 | 0.170 | 0.25 | 1.09× | 0.74× |
| q8 | 0.172 | 0.180 | 0.21 | 0.95× | 0.82× |
| q9 | 0.320 | 0.340 | 0.32 | 0.94× | 1.00× |
| q10 | 0.311 | 0.360 | 0.30 | 0.86× | 1.04× |
| q11 | 0.033 | 0.050 | 0.04 | 0.67× | 0.83× |
| q12 | 0.155 | 0.130 | 0.10 | 1.20× | 1.55× |
| q13 | 0.283 | 0.250 | 0.14 | 1.13× | 2.02× |
| q14 | 0.098 | 0.160 | 0.10 | 0.61× | 0.98× |
| q15 | 0.089 | 0.110 | 0.09 | 0.81× | 0.99× |
| q16 | 0.117 | 0.090 | 0.07 | 1.30× | 1.67× |
| q17 | 0.158 | 0.150 | 0.29 | 1.05× | 0.54× |
| q18 | 0.245 | 0.220 | 0.66 | 1.11× | 0.37× |
| q19 | 0.211 | 0.220 | 0.15 | 0.96× | 1.41× |
| q20 | 0.129 | 0.170 | 0.14 | 0.76× | 0.92× |
| q21 | 0.355 | 0.410 | 0.43 | 0.87× | 0.83× |
| q22 | 0.069 | 0.140 | 0.10 | 0.49× | 0.69× |
| **total** | **3.864** | **4.03** | **4.180** | **0.96×** | **0.92×** |

## Scale 100

qdb re-run 2026-10-25 in SQL mode at scale 100, one run per query on the
Apple M5 Pro.

All 22 queries completed with status `OK`; no failures. Runner-reported totals:
processing **36.645 s**, CPU **580.401 s**. Compilation is excluded from processing:
planning **0.731 s**, kernel build **1.140 s**, LLVM **7.603 s**.
The runner rounds cumulative totals to milliseconds after each query.
The DuckDB column is still the 2026-08-22 run.
DataFusion complete-run totals: 65.210 and 62.680 s.

| Query | qdb (s) | DuckDB (s) | DataFusion (s) | qdb ÷ DuckDB | qdb ÷ DataFusion |
| --- | --- | --- | --- | --- | --- |
| q1 | 1.542 | 2.200 | 1.65 | 0.70× | 0.93× |
| q2 | 0.567 | 0.420 | 0.56 | 1.35× | 1.01× |
| q3 | 1.682 | 1.390 | 1.19 | 1.21× | 1.41× |
| q4 | 1.993 | 0.930 | 0.59 | 2.14× | 3.38× |
| q5 | 2.721 | 1.450 | 1.87 | 1.88× | 1.46× |
| q6 | 0.863 | 0.870 | 0.65 | 0.99× | 1.33× |
| q7 | 1.823 | 1.350 | 3.08 | 1.35× | 0.59× |
| q8 | 1.325 | 1.570 | 1.68 | 0.84× | 0.79× |
| q9 | 2.973 | 3.090 | 3.23 | 0.96× | 0.92× |
| q10 | 2.158 | 1.770 | 1.74 | 1.22× | 1.24× |
| q11 | 0.284 | 0.260 | 0.42 | 1.09× | 0.68× |
| q12 | 1.462 | 1.090 | 0.92 | 1.34× | 1.59× |
| q13 | 3.263 | 2.190 | 1.75 | 1.49× | 1.86× |
| q14 | 0.789 | 1.130 | 0.78 | 0.70× | 1.01× |
| q15 | 0.932 | 1.080 | 0.71 | 0.86× | 1.31× |
| q16 | 0.567 | 0.460 | 0.41 | 1.23× | 1.38× |
| q17 | 1.073 | 1.280 | 3.76 | 0.84× | 0.29× |
| q18 | 3.075 | 2.300 | 29.72 | 1.34× | 0.10× |
| q19 | 2.143 | 1.610 | 1.00 | 1.33× | 2.14× |
| q20 | 0.898 | 1.160 | 1.15 | 0.77× | 0.78× |
| q21 | 3.845 | 3.890 | 5.46 | 0.99× | 0.70× |
| q22 | 0.667 | 0.610 | 0.36 | 1.09× | 1.85× |
| **total** | **36.645** | **32.10** | **62.680** | **1.14×** | **0.58×** |
