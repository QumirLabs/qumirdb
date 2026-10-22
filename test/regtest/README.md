# SQL regression suites

`cases/` holds SQL files, `suite.json` manifests and JSON table fixtures;
`goldens/` holds the expected AST, plan and `.result.json` files. Two runners
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
  "outer_nested_loop.sql": {"variants": ["single", "threaded"]}
}
```

Every list keeps a native variant, because native runs produce the result
goldens that `js` checks. `test_reg` validates `js` and does not execute it.
List `js` only where the case passes in the browser; the browser runner fails a
`js` case whose plan the exporter cannot run in the browser, and reports every
executed case without `js` as skipped together with the list that excludes it.
Today the browser exporter cannot run non-inner joins without equi-keys
("browser keyless join type is not supported yet"); the `keyless_joins` suite
and the cases that decorrelate into such joins leave out `js`.

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
