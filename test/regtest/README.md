# SQL regression suites

`cases/` holds SQL files, `suite.json` manifests and JSON table fixtures;
`goldens/` holds the expected AST, plan, `.result.json` and `.error` files. Two runners
read them:

- `test/test_reg.cpp` (`build/test/test_reg`) checks ASTs, optimized plans and
  results through the native Parquet source and scheduler. It is the only
  runner that canonizes goldens.
- `test/test_reg.mjs` checks results through `qdb --plan-export` and the browser
  WASM runtime in `service/static/browser_runtime.js`.

A case's SQL file may start with `CREATE MODULE` and `CREATE FUNCTION`
definitions; both runners plan and execute the final query against them.

The manifest format, fixture types, statistics and canonization are described
in the [SQL regression tests](https://github.com/resetius/qumirdb/wiki/SQL-regression-tests)
wiki page.

## Variants

`"variants"` lists where the executed cases of a suite run:

- `single`: native, one worker and one scan task;
- `threaded`: native, four workers and up to four row-group scan tasks;
- `js`: the browser runtime.

A case may replace the suite list with its own `"variants"`, for example to
leave out `js` when the browser runtime does not support the query yet:

```json
"variants": ["single", "threaded", "js"],
"cases": {
  "project.sql": {},
  "native_only.sql": {"variants": ["single", "threaded"]}
}
```

Every list keeps a native variant, because native runs produce the result
goldens that `js` checks. `test_reg` validates `js` and does not execute it.
List `js` only where the case passes in the browser; the browser runner fails a
`js` case whose plan the exporter cannot run in the browser, and reports every
executed case without `js` as skipped together with the list that excludes it.

## Expected errors

An invalid query is a regular result. A case that must fail sets
`"expect_error": true` (per case, or as a suite default that a case overrides):

```json
"expect_error": true,
"cases": {
  "ungrouped_column.sql": {},
  "group_qualified_key_control.sql": {"expect_error": false, "order": "unordered"}
}
```

The stage that fails decides the golden, and `test_reg --canonize` writes it:

- Parsing, binding and optimization errors go to the `.plan` golden, as in
  plan-only suites; there is no result golden.
- Errors of lowering, compilation and execution go to the `.error` golden; the
  `.plan` golden then holds the optimized plan as usual.

Every variant renders the error and the variants must agree, like results.

The text of an error is one of:

- `Error: <message> @ Line: L, Byte: B, Column: C` for a `TError`, like Qumir's
  regression tests (the location is absent when the error has none);
- `Runtime error: <message>` for any other exception, for example a
  `std::runtime_error` thrown by lowering or `abs overflow for i64`. It has no
  location and the prefix marks it as an error of the engine, not of the query
  text.

The outcome must match the declaration. A case without `expect_error` that
fails, and a case with it that succeeds, fail with the message (the SQL file
and the error). With `"execute": false` the error can only be a planning error; a planning error
without `expect_error` fails the case, and `expect_error` makes a successful plan
fail it. The same holds for a suite case without tables. A SQL file outside
a suite has no manifest, cannot declare `expect_error` and records either plan.
`--canonize` also removes the golden of the
previous outcome (a `.error` of a case that now succeeds, a result of a case
that now fails) and reports each removed file on stderr. Failures of
the harness (a broken fixture, an unmapped table) are never goldens.

The browser runner checks the same cases and validates `expect_error` as a
boolean. The stage that failed decides the golden: a planning error of the exporter
is compared with the `.plan` golden exactly, and so is an error that the exporter
reports for lowering with the `.error` golden. The WASM runtime wraps the message
of a failed run (`project: <message> (wasm reserved ...)`), so the message of the
`.error` golden must occur in it; a case whose error reads differently in the
WASM runtime must not list `js`. A `js` run that succeeds fails the case.
`test/regtest_lib_self_test.mjs` (`test_reg_lib_js`) covers these stages.

## Browser runner

For each `js` case the runner exports the query with the case's tables as the
dataset (column types, row and row-group counts, exact NDV or the fixture's
explicit statistics), executes the bundle with the fixture rows and compares the
typed result with the case's `.result.json` using the case's order and
tolerance settings. Source batches are cut at the fixture's `row_group_rows`,
like the row groups of the Parquet file that `test_reg` generates. A case also
fails when the run leaves WASM memory allocated.

Plan and AST goldens stay native: the browser runs the exported plan, whose
shape can differ because the runner does not reproduce Parquet footer
statistics or histograms. Plan-only cases (`"execute": false` or no tables)
have no result golden and are not part of the browser run. The runner never
writes goldens.

```sh
node test/test_reg.mjs build/bin/qdb service/static/browser_runtime.js test/regtest
node test/test_reg.mjs build/bin/qdb service/static/browser_runtime.js test/regtest \
  --filter 'boolean_columns_*:order_by_*'
ctest --test-dir build -R '^test_reg_js$' --output-on-failure
```

`--filter` takes `:`-separated globs over test names (`<suite>_<case>_js`);
`--xml <file>` writes a JUnit report. CI runs it with Node.js 24.
