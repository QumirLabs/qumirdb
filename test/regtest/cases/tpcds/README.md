# TPC-DS execution regression suite

The tables are the first ten rows of each Parquet file in
`~/Projects/tpcds/pq10`, sampled on 2026-10-13. The 24 fixtures contain 240 rows
in total. Values are copied from the data, without synthesizing rows or changing
NULLs.

The schema preserves column names, widths, order, and nullability from each
Parquet Arrow schema. DATE32 columns become `i32` days since 1970-01-01, as in
`TParquetSource`'s Qumir schema. No decimal conversion is necessary: this dataset
already uses `f64` amounts. Statistics are computed from the sample when
`test_reg` generates temporary Parquet files.

All 103 statements check their optimized plan and typed result using both
`single` and `threaded` scheduler presets. Row groups contain two rows so the
threaded preset can split scans. The second statements of q14, q23, q24, and q39
are separate `_2.sql` cases, because each query regression case executes one
statement. Existing AST goldens are still checked once per original SQL file.
Plan and result goldens live under `test/regtest/goldens/tpcds`.

q10 and q35 execute with real schemas. Their former schema-less plan goldens
expected a subquery error; that error does not apply to schema-aware planning.

These small independent table samples primarily cover schema-dependent planning,
optimization, code generation, and execution, including empty join results.
They are not a performance dataset or a substitute for full-scale benchmarks.
