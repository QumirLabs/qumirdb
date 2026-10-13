# SQL query regression tests

`test_reg` uses one `TQueryCase` and one `RegQuery` check for SQL cases. A case
without table bindings checks its raw logical plan. A case with table bindings
checks the optimized logical plan and the query result, running through the
native Parquet source and scheduler. Existing AST goldens remain separate parser
checks for SQL files in directories without `suite.json`.

The test-only CMake target `regtest_lib` owns case discovery, JSON parsing,
fixture materialization, schema/result printing, and comparison. `test_reg.cpp`
contains GoogleTest checks and runner setup. `test_regtest_lib.cpp` tests the
infrastructure contracts, including a dedicated CBO ordering test.

The first data suite is [order_by](../../test/regtest/cases/order_by/suite.json).
Its seven queries cover NULL ordering, multiple sort keys, expressions,
LIMIT/OFFSET, duplicate rows, self joins, and an empty-table override. They run
with fixed fixture statistics in the single-threaded and threaded schedulers.
There is no join-order matrix applied to the whole suite.

## Files and table mapping

Keep input JSON, SQL, and result/plan goldens in ordinary Git. A directory with
`suite.json` uses its declared fixture bindings; a directory without a manifest
produces plan-only query cases. Manifests apply to their own directory and do not
implicitly change child directories.

```text
test/regtest/
  cases/order_by/
    suite.json
    tables/numbers.json
    tables/empty.json
    nulls_first.sql
    empty.sql
  goldens/order_by/
    nulls_first.plan
    nulls_first.result.json
    empty.plan
    empty.result.json
```

```json
{
  "version": 1,
  "fixtures": {
    "numbers": {"json": "tables/numbers.json", "parquet": {"row_group_rows": 2}},
    "empty": {"json": "tables/empty.json"}
  },
  "tables": {"t": "numbers"},
  "variants": ["single", "threaded"],
  "cases": {
    "nulls_first.sql": {"order": "ordered"},
    "empty.sql": {"tables": {"t": "empty"}, "order": "ordered"}
  }
}
```

SQL and fixture paths are relative to the manifest. Named fixtures are local to
its suite. Default SQL table bindings are merged with case overrides; multiple
table names can refer to one fixture. Aliases and CTEs follow SQL scope rules.
A case with no bindings runs once in the `plan` variant and does not execute SQL.

The runner derives golden paths from SQL paths: `cases/<path>.sql` maps to
`goldens/<path>.plan` and, for cases with tables, `goldens/<path>.result.json`.
The manifest cannot override these paths. All execution variants share both
files. SQL cases must be directly in the manifest directory and listed in
`cases`. Missing files, unknown fields, invalid versions, unknown fixtures or
variants, and unmapped tables in an execution case fail explicitly.

## Schema and row values

Input data and result goldens use a Qumir struct type string and row arrays.
Schema strings are parsed with `NCore::ParseType` and canonically printed with
`NCore::PrintType`. Nullable fields use `<named Nullable [T]>`, which maps to
Arrow field nullability on input and QDB's `TNullable` on output.

Input fixtures include `"version": 1`. Result goldens contain only `schema` and
`rows`; they have no version. Regenerate them with `--canonize` when the result
format changes. For example:

```json
{
  "schema": "<struct (id i64) (label <named Nullable [string]>)>",
  "rows": [
    ["1", "one"],
    ["2", null]
  ]
}
```

Input fixtures, result goldens, and diagnostics use one complete row per line,
with spaces between cells. JSON escaping keeps quotes and newlines inside string
values on that same line. Empty tables/results use `"rows": []`.

Supported JSON fixture and result types are signed/unsigned integers through
64 bits, `f64`, `bool`, and `string`, with optional nullability. All 64-bit
integers use decimal strings, parsed without conversion through double. Smaller
integers may use JSON numbers. Overflow, invalid row widths, duplicate schema
fields, and NULL in a non-nullable input column are errors. Empty and all-NULL
tables retain their schema.

Floats use JSON numbers or tags such as `{"$float":"nan"}`, `{"$float":"+inf"}`,
`{"$float":"-inf"}`, and `{"$float":"-0"}`. Results compare exactly, including
the tags. Decimal/date JSON fixtures and float tolerances are future extensions.

## Cost metadata

JSON fixtures default to computing custom statistics at materialization time;
`"stats": "compute"` requests this explicitly. The generator computes exact NDV
for supported non-NULL values and sampled equi-depth histogram boundaries for
numeric columns, then writes the existing `stats` key-value blob into the Parquet
footer. Strings and booleans receive NDV without histograms. NaN columns omit
NDV; non-finite float boundaries and unsigned values outside the metadata
reader's signed-integer range omit histograms.

A fixture can instead provide an explicit `stats` object:

```json
"stats": {
  "columns": [
    {"name": "id", "ndv": 6, "ndv_exact": true},
    {"name": "a", "ndv": 3, "ndv_exact": true, "histogram": [1, 1, 2, 2, 3]}
  ]
}
```

Explicit NDV/histogram values are written as supplied and are not supplemented
with computed values. This lets a test force the intended CBO join ordering by
controlling its input statistics. Set `ndv_exact` only when exactness is part of
the intended input. The dedicated `SuppliedStatsControlCboJoinOrder` test changes
NDV for one three-table query, checks that CBO chooses a different lower join,
and verifies the same result; ordinary suites do not enumerate join orders.

`"stats": "footer"` omits custom metadata. In all generated files, the Parquet
writer emits standard statistics; row count comes from the physical footer and
min/max/null count come from its column statistics. The custom blob supplies
NDV/histograms and can fill missing standard min/max/null-count values. It does
not override row count or existing standard column statistics.

A fixture can also use an existing file: `{"parquet_file":"tables/input.parquet"}`.
The runner opens it directly and uses its embedded standard/custom metadata
without regeneration or recomputation. Exactly one of `json` and `parquet_file`
is required. Existing-file fixtures do not accept generator options. This path
also accommodates particular encodings or malformed-input tests; LFS is optional.

## Preparation, plans, and results

`main` collects metadata once. A preparation environment materializes only
fixtures bound by selected query cases, once per suite/input/layout. Listing
tests does not generate files. Identical JSON paths and row-group sizes share a
file within a suite. The writer uses uncompressed data, no dictionaries, and
128 rows per row group unless `row_group_rows` overrides that size.

Each file has a shared `TParquetFile`, while every source occurrence gets a fresh
`MakeSource()` cursor. Self joins and repeated variants therefore have independent
scan state and projections. Generated files live in a suite-owned temporary
directory and are cleaned up when its registry is destroyed. There is no cache
across processes or Python generator.

For cases with tables, build a fresh plan, apply normal plan passes including
CBO, and compare `PrintPlanTreeWithCtes` output to `.plan`. This is the optimized
logical plan before scheduler lowering, with row/cost estimates and CTE trees.
Then execute and compare typed output to `.result.json`. Cases without tables
retain the existing raw s-expression plan format and schema-less mock sources.
Parser errors remain valid expected text in plan-only cases.

The `single` preset uses one worker and one scan task. The `threaded` preset uses
four workers and up to four row-group scan tasks. An `ordered` result compares the
row sequence; `unordered` compares a multiset, preserving duplicate counts.
ORDER BY tests use explicit tie-breaking keys. All variants share plan and result
goldens; a disagreement is a failure.

## Commands and canonization

```sh
cmake --build build --target test_reg -j 4
ctest --test-dir build -R '^test_reg$' --output-on-failure
build/test/test_reg test/regtest --gtest_filter='RegQuery.order_by_*_single:RegQuery.order_by_*_threaded' --print
build/test/test_reg test/regtest --gtest_filter='RegQuery.order_by_*_single:RegQuery.order_by_*_threaded' --canonize
```

For suite cases, `--canonize` collects plan and result goldens in memory and
checks agreement across selected variants. Both are written only after the
selected run passes. Missing expectations can be created in this mode. AST and
plan-only cases without manifests retain their existing immediate canonization.
Review plan changes and expected rows independently of the engine output.

The [Roadmap](https://github.com/resetius/qumirdb/wiki/Roadmap) describes the query
execution goal, with fixture storage as a design choice. SQL input/result tests
can migrate into suites; tests of ABI, ownership, scheduling, and internal
estimates can stay in C++ where result and plan goldens cannot verify the contract.
