# TPC-H execution regression suite

The tables are the first ten rows of each Parquet file in
`~/Projects/tpch/pq100`, sampled on 2026-10-13. `region` has five rows in the
source, so the eight fixtures contain 75 rows in total. Values are copied from
the data, without synthesizing rows or changing NULLs.

The schema preserves column names, widths, order, and nullability from each
Parquet Arrow schema. DATE32 columns become `i32` days since 1970-01-01, as in
`TParquetSource`'s Qumir schema. No decimal conversion is necessary: this dataset
already uses `f64` prices and quantities. Statistics are computed from the
sample when `test_reg` generates temporary Parquet files.

All 22 existing SQL cases check their optimized plan and typed result using both
`single` and `threaded` scheduler presets. Row groups contain two rows so the
threaded preset can split scans. Existing AST goldens are still checked once per
SQL file. Plan and result goldens live under `test/regtest/goldens/tpch`.

These small independent table samples primarily cover schema-dependent planning,
optimization, code generation, and execution, including empty join results.
They are not a performance dataset or a substitute for full-scale benchmarks.
