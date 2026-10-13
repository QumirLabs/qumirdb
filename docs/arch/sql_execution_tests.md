# SQL execution regression tests

`test_reg` supports execution suites alongside the existing AST and raw-plan
regression cases. A directory with `suite.json` runs its declared SQL cases
against fixture data. SQL files in directories without a manifest retain the
previous AST/plan golden checks. Manifests apply to their own directory; they do
not implicitly change the behavior of child directories.

The first suite is [order_by](../../test/regtest/cases/order_by/suite.json).
It covers NULL ordering, multiple sort keys, expressions, LIMIT/OFFSET, duplicate
rows, self joins, and an empty-table override. Each query runs in both the
single-threaded and threaded schedulers.

## Files and table mapping

Keep typed JSON fixtures, SQL, and result goldens in ordinary Git. The runner
loads manifest metadata once and prepares each selected fixture as Parquet once
per suite and process, before executing queries. It shares `TParquetFile` objects
across cases and variants and calls `MakeSource()` for every source occurrence.
A self join and a second run therefore get independent cursors and projections.

```text
test/regtest/cases/order_by/
  suite.json
  tables/numbers.json
  tables/empty.json
  nulls_first.sql
  empty.sql
  goldens/nulls_first.result.json
  goldens/empty.result.json
```

A minimal manifest looks like this:

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
    "nulls_first.sql": {"golden": "goldens/nulls_first.result.json", "order": "ordered"},
    "empty.sql": {
      "tables": {"t": "empty"},
      "golden": "goldens/empty.result.json",
      "order": "ordered"
    }
  }
}
```

Paths are relative to the manifest. Named fixtures are local to the suite.
Default SQL table bindings are merged with a case's overrides. Multiple names
can refer to the same fixture. Aliases and CTEs follow the existing SQL builder's
scope rules; no separate alias mapping or reverse list of queries is required.

SQL cases must be files directly in the manifest directory. All SQL files there
must appear in `cases`. Missing files, unknown fields, invalid versions, unknown
fixtures or variants, and unmapped SQL tables produce explicit failures.

## Schema and values

Both fixture data and expected results use a Qumir struct type string and row
arrays. Schema strings are parsed with `NCore::ParseType` and canonically printed
with `NCore::PrintType`, rather than `TStructType::ToString()`.

```json
{
  "version": 1,
  "schema": "<struct (id i64) (label <named Nullable [string]>)>",
  "rows": [["1", "one"], ["2", null]]
}
```

Nullable columns use a generic named `Nullable` wrapper. The writer turns the
wrapper into Arrow field nullability; output serialization adapts QDB's
`TNullable` back to the same Qumir representation. Field order and names are part
of the expected result. Whitespace in the schema string is normalized by the
core parser/printer.

Supported fixture and result values are `i8`, `i16`, `i32`, `i64`, `u8`, `u16`,
`u32`, `u64`, `f64`, `bool`, and `string`, with optional nullability. All 64-bit
integers use decimal strings and are parsed without conversion through double.
Smaller integers may use JSON numbers. Overflow, wrong row widths, duplicate
schema fields, and NULL in a non-nullable fixture column are errors. Empty and
all-NULL tables keep their declared schema.

Floats use ordinary JSON numbers or tags such as `{"$float":"nan"}`,
`{"$float":"+inf"}`, `{"$float":"-inf"}`, and `{"$float":"-0"}`. Results compare
exactly, including these tags; float tolerance is not implemented. Decimal and
date fixtures are also left for a later extension.

## Preparation and execution

Manifest collection and test registration happen in `main`. GoogleTest's global
preparation environment materializes only fixtures bound by selected execution
cases. Listing tests reads metadata without generating Parquet. Unselected
execution suites do not generate files, though their manifests are validated.

The C++ Arrow/Parquet writer creates uncompressed files with dictionaries
disabled and standard statistics enabled. Row groups default to 128 rows;
`row_group_rows` overrides this per fixture. Different layouts of one JSON file
can use different fixture names; identical input paths and row-group sizes share
a generated file within a suite. Files live in a unique temporary directory
owned by the suite and are removed when its registry is destroyed. There is no
persistent cache or Python generator.

Each test builds a fresh plan through `BuildPlan`, applies `ApplyPlanPasses`,
lowers it, runs the native scheduler, and collects typed output. The `single`
preset uses one worker and one scan task. The `threaded` preset uses four workers
and up to four row-group scan tasks. Both use row-group-range scan splitting.
Runtime filters and CBO use normal plan-pass defaults.

An `ordered` expectation compares the row sequence. An `unordered` expectation
compares a multiset, retaining duplicate counts. ORDER BY cases use ordered
comparison and explicit tie-breaking keys. All variants share one golden.

Generated Parquet uses the production source, but an output golden does not
prove that a particular optimization activated. Tiny-fixture partial aggregation
and estimator regressions still need targeted settings or internal assertions.
Row-group size also does not guarantee an exact output record-batch boundary.

## Commands and canonization

```sh
cmake --build build --target test_reg -j 4
ctest --test-dir build -R '^test_reg$' --output-on-failure
build/test/test_reg test/regtest --gtest_filter='RegExec.order_by_*' --print
build/test/test_reg test/regtest --gtest_filter='RegExec.order_by_*' --gtest_list_tests
```

`--canonize` collects execution outputs in memory and checks that selected
variants agree. Result goldens are written only after the entire selected run
passes; setup errors, execution failures, or differing variants prevent the
commit. Existing AST/plan canonization retains its previous behavior. Use a
filter when updating only execution goldens and review the expected rows.

## Later extensions

The [Roadmap](https://github.com/resetius/qumirdb/wiki/Roadmap) describes the
execution-testing goal; fixture storage remains a design choice. The current
JSON-to-Parquet path can later share the same catalog with a direct JSON/memory
source or existing Parquet fixtures. Existing files are useful for malformed
inputs, physical encodings, or external writer behavior that generated files
cannot reproduce. LFS is optional for those fixtures.

Other possible additions are decimal/date values, explicit writer settings,
custom NDV/histogram metadata, selected optimizer profiles, and a cache across
processes if preparation time warrants it. Move C++ tests whose contract is a
SQL input and result into suites; retain tests of kernel ABI, ownership,
scheduling, and internal estimates that result goldens cannot verify.
