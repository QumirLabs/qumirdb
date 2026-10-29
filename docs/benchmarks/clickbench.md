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

## Partial aggregate decided at run time (2026-10-29)

A/B of the branch against its base, origin/master `0725f69e`, both built
RelWithDebInfo in separate worktrees with warm JIT caches, default settings
(18 workers), `--format null --timing`. The five queries of issue #376 and the
other grouped queries whose plan or time the change could move ran five times
each, the two binaries interleaved; the table gives the median and the
individual runs of "Returned in".

| Query | master (s) | branch (s) | change | master runs | branch runs | CPU master → branch (s) |
|---|---:|---:|---:|---|---|---|
| q05 | 0.279 | 0.234 | −15.9% | 0.273 0.277 0.283 0.279 0.281 | 0.229 0.234 0.233 0.239 0.236 | 3.4 → 3.6 |
| q12 | 0.307 | 0.255 | −17.0% | 0.299 0.307 0.311 0.309 0.306 | 0.255 0.251 0.255 0.254 0.257 | 3.8 → 3.8 |
| q14 | 0.278 | 0.274 | −1.4% | 0.268 0.273 0.278 0.278 0.279 | 0.271 0.274 0.272 0.275 0.275 | 4.2 → 4.2 |
| q17 | 0.526 | 0.523 | −0.6% | 0.506 0.544 0.557 0.515 0.526 | 0.523 0.520 0.562 0.526 0.518 | 7.6 → 7.6 |
| q18 | 1.271 | 1.278 | +0.5% | 1.271 1.271 1.267 1.275 1.310 | 1.319 1.274 1.272 1.353 1.278 | 15.4 → 15.3 |
| q28 | 1.124 | 0.943 | −16.1% | 1.100 1.117 1.132 1.124 1.133 | 0.928 0.942 0.954 0.962 0.943 | 17.6 → 14.9 |
| q30 | 0.251 | 0.249 | −0.7% | 0.249 0.251 0.254 0.249 0.252 | 0.245 0.249 0.248 0.254 0.253 | 3.7 → 3.7 |
| q33 | 1.168 | 1.177 | +0.8% | 1.151 1.185 1.168 1.169 1.160 | 1.166 1.227 1.181 1.173 1.177 | 17.9 → 18.0 |
| q34 | 1.170 | 1.196 | +2.2% | 1.170 1.184 1.175 1.166 1.170 | 1.167 1.199 1.196 1.204 1.171 | 18.0 → 18.0 |

q28 groups by a `regexp_replace` of `Referer`, a key without statistics; the
branch plans the partial phase for it, and its 33 lanes reduce 81.0M rows to
3.7M (21.8x) before the exchange instead of shuffling every `Referer`. q05 and
q12 group by `SearchPhrase`, whose 6.0M values estimate a 16x reduction, so
both binaries plan the partial phase; per lane the reduction is only 1.8x, and
the lanes that reach the sample now pass their rows through instead of hashing
them. The other six keep the shuffle plan: `UserID` alone (17.6M values) rules
the partial out for q17 and q18, `URL` (18.3M values) estimates 5.5x for q33
and q34, and the key NDVs of q14 and q30 multiply beyond the row count. Their
differences are noise.

The full suite, three runs per binary interleaved, sum of per-query medians:
master 13.970 s, branch 13.630 s (−2.4%); no other query moved by more than
5%. TPC-H SF10 (SQL, three runs each): 3.846 s to 3.802 s (−1.1%), no query
beyond 5%. TPC-DS SF10 (three runs each): 23.608 s to 23.585 s (−0.1%); q74
+7% with overlapping runs (0.298 0.332 0.302 vs 0.305 0.331 0.323).
