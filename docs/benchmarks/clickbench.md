# ClickBench benchmark results

See the [benchmark index](README.md) for the test machine, engine builds,
measurement definitions, coverage, and summary across benchmarks.

## Results

qdb re-run 2026-10-29 in SQL mode, one run per query on the Apple M5 Pro.

All 43 queries completed with status `OK`; no failures or skipped queries.
Runner-reported totals: processing **12.462 s**, CPU **184.323 s**.
Compilation is excluded from processing: planning **1.504 s**, kernel build
**0.807 s**, LLVM **6.532 s**. The runner rounds cumulative totals to
milliseconds after each query.
The DuckDB column is still the 2026-08-22 run, reporting the best of several
measurements. The ClickHouse column is still the 2026-08-30 run, with one
measurement per query.

| Query | qdb (s) | DuckDB (s) | ClickHouse (s) | qdb ÷ DuckDB | qdb ÷ ClickHouse |
|---|---|---|---|---|---|
| q0 | 0.013 | 0.040 | 0.130 | 0.33× | 0.10× |
| q1 | 0.016 | 0.050 | 0.120 | 0.31× | 0.13× |
| q2 | 0.046 | 0.070 | 0.130 | 0.66× | 0.36× |
| q3 | 0.040 | 0.070 | 0.130 | 0.57× | 0.30× |
| q4 | 0.175 | 0.160 | 0.200 | 1.09× | 0.87× |
| q5 | 0.245 | 0.230 | 0.250 | 1.07× | 0.98× |
| q6 | 0.023 | 0.090 | 0.120 | 0.26× | 0.19× |
| q7 | 0.011 | 0.050 | 0.120 | 0.23× | 0.10× |
| q8 | 0.253 | 0.200 | 0.280 | 1.27× | 0.90× |
| q9 | 0.323 | 0.270 | 0.330 | 1.20× | 0.98× |
| q10 | 0.099 | 0.100 | 0.200 | 0.99× | 0.50× |
| q11 | 0.125 | 0.110 | 0.210 | 1.14× | 0.60× |
| q12 | 0.283 | 0.230 | 0.270 | 1.23× | 1.05× |
| q13 | 0.489 | 0.360 | 0.370 | 1.36× | 1.32× |
| q14 | 0.253 | 0.260 | 0.340 | 0.97× | 0.74× |
| q15 | 0.203 | 0.190 | 0.200 | 1.07× | 1.02× |
| q16 | 0.524 | 0.430 | 0.500 | 1.22× | 1.05× |
| q17 | 0.490 | 0.350 | 0.290 | 1.40× | 1.69× |
| q18 | 1.183 | 0.680 | 0.850 | 1.74× | 1.39× |
| q19 | 0.080 | 0.050 | 0.120 | 1.59× | 0.66× |
| q20 | 0.381 | 0.410 | 0.580 | 0.93× | 0.66× |
| q21 | 0.429 | 0.350 | 0.650 | 1.23× | 0.66× |
| q22 | 0.752 | 0.620 | 0.960 | 1.21× | 0.78× |
| q23 | 0.482 | 0.530 | 0.680 | 0.91× | 0.71× |
| q24 | 0.145 | 0.200 | 0.320 | 0.72× | 0.45× |
| q25 | 0.123 | 0.170 | 0.230 | 0.73× | 0.54× |
| q26 | 0.146 | 0.210 | 0.330 | 0.70× | 0.44× |
| q27 | 0.356 | 0.340 | 0.630 | 1.05× | 0.57× |
| q28 | 1.010 | 3.560 | 0.680 | 0.28× | 1.49× |
| q29 | 0.032 | 0.060 | 0.130 | 0.54× | 0.25× |
| q30 | 0.228 | 0.280 | 0.430 | 0.82× | 0.53× |
| q31 | 0.249 | 0.310 | 0.470 | 0.80× | 0.53× |
| q32 | 0.674 | 0.780 | 0.870 | 0.86× | 0.77× |
| q33 | 1.109 | 0.880 | 0.900 | 1.26× | 1.23× |
| q34 | 1.102 | 0.920 | 0.900 | 1.20× | 1.22× |
| q35 | 0.184 | 0.250 | 0.180 | 0.74× | 1.02× |
| q36 | 0.034 | 0.100 | 0.130 | 0.34× | 0.26× |
| q37 | 0.026 | 0.090 | 0.120 | 0.29× | 0.22× |
| q38 | 0.027 | 0.070 | 0.110 | 0.39× | 0.25× |
| q39 | 0.071 | 0.140 | 0.150 | 0.51× | 0.48× |
| q40 | 0.009 | 0.060 | 0.120 | 0.15× | 0.07× |
| q41 | 0.011 | 0.060 | 0.120 | 0.19× | 0.09× |
| q42 | 0.008 | 0.060 | 0.100 | 0.13× | 0.08× |
| **total** | **12.462** | **14.44** | **14.95** | **0.86×** | **0.83×** |

## Partial aggregate decided at run time (2026-10-29)

A/B of the branch against its base, origin/master `0725f69e`, both built
RelWithDebInfo in separate worktrees with warm JIT caches, default settings
(18 workers), `--format null --timing`. The five queries of issue #376 and the
other grouped queries whose time moved ran five times each, the two binaries
interleaved; the table gives the median and the individual runs of "Returned
in".

| Query | master (s) | branch (s) | change | master runs | branch runs | CPU master → branch (s) |
|---|---:|---:|---:|---|---|---|
| q05 | 0.280 | 0.238 | −14.9% | 0.272 0.280 0.280 0.282 0.286 | 0.224 0.235 0.239 0.238 0.243 | 3.5 → 3.6 |
| q12 | 0.301 | 0.253 | −16.1% | 0.295 0.301 0.301 0.306 0.308 | 0.247 0.257 0.253 0.253 0.262 | 3.8 → 3.9 |
| q14 | 0.273 | 0.279 | +2.2% | 0.266 0.273 0.273 0.277 0.276 | 0.268 0.272 0.279 0.280 0.279 | 4.3 → 4.3 |
| q17 | 0.512 | 0.509 | −0.5% | 0.525 0.512 0.516 0.511 0.511 | 0.501 0.504 0.512 0.509 0.515 | 7.6 → 7.6 |
| q18 | 1.286 | 1.277 | −0.7% | 1.286 1.291 1.273 1.279 1.307 | 1.276 1.274 1.277 1.382 1.338 | 15.4 → 15.4 |
| q28 | 1.138 | 0.959 | −15.7% | 1.086 1.143 1.145 1.136 1.138 | 0.915 0.944 0.960 0.961 0.959 | 17.7 → 15.1 |
| q30 | 0.252 | 0.251 | −0.4% | 0.244 0.249 0.262 0.262 0.252 | 0.251 0.250 0.251 0.253 0.256 | 3.7 → 3.7 |
| q33 | 1.183 | 1.175 | −0.6% | 1.161 1.192 1.200 1.173 1.183 | 1.157 1.175 1.177 1.173 1.186 | 17.9 → 17.9 |
| q34 | 1.174 | 1.177 | +0.3% | 1.166 1.174 1.165 1.182 1.192 | 1.175 1.187 1.172 1.183 1.177 | 17.9 → 17.9 |

q28 groups by a `regexp_replace` of `Referer`, a key without statistics; the
branch plans the partial phase for it, and its 33 lanes reduce 81.0M rows to
3.7M (21.8x) before the exchange instead of shuffling every `Referer`. q05 and
q12 group by `SearchPhrase`, whose 6.0M values estimate a 16x reduction, so
both binaries plan the partial phase; per lane the reduction is only 1.8x, and
the lanes that reach the sample now pass their rows through instead of hashing
them. q17 and q18 keep the shuffle plan because `UserID` alone has 17.6M
values, q33 and q34 because `URL` (18.3M values) estimates 5.5x, below the 8x
the plan requires; q14 and q30 keep it because the product of their key NDVs
reaches the row count. Their differences are noise.

The full suite, three runs per binary interleaved, sum of per-query medians:
master 13.942 s, branch 13.577 s (−2.6%); no other query moved by more than
5%.
