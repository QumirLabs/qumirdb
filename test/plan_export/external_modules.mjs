// CREATE MODULE functions compile into the browser bundle: a Rust function
// returning a struct and Kumir functions, also as an aggregate argument that
// the plan builder types against the module catalog. The native test_reg runs
// single SELECT statements, so these queries stay here.

import assert from 'node:assert/strict';

import { browserExec, compareResults, runExporter, typedResult } from '../regtest_lib.mjs';

const RUST_MODULE = `
CREATE MODULE orbital LANGUAGE rust AS $$
#[repr(C)]
pub struct OrbitResult { pub root: f64, pub square: f64, pub positive: bool }
#[no_mangle]
pub extern "C" fn orbit_result(a: f64) -> OrbitResult {
  OrbitResult { root: a.sqrt(), square: a, positive: a > 0.0 }
}
$$;
CREATE FUNCTION orbit_result(a DOUBLE) RETURNS (DOUBLE, DOUBLE, BOOL)
SET MODULE TO orbital SET SYMBOL TO orbit_result;
`;

const KUMIR_MODULE = `
CREATE MODULE math LANGUAGE kumir AS $$
алг вещ kumir_root(вещ x)
нач
  знач := sqrt(x)
кон
$$;
`;

const CASES = {
  'rust-struct-result': {
    sql: `${RUST_MODULE}SELECT orbit_result(a) FROM t;`,
    values: [4, 9],
    expected: {
      schema: '<struct (col0 f64) (col1 f64) (col2 bool)>',
      rows: [[2, 4, true], [3, 9, true]],
    },
  },
  'kumir-function': {
    sql: `${KUMIR_MODULE}SELECT kumir_root(a) FROM t;`,
    values: [4, 9],
    expected: { schema: '<struct (col0 f64)>', rows: [[2], [3]] },
  },
  'kumir-aggregate-argument': {
    sql: `${KUMIR_MODULE}SELECT stddev_samp(kumir_root(a)) AS d FROM t;`,
    values: [4, 9, 16],
    expected: { schema: '<struct (d <named Nullable [f64]>)>', rows: [[1]] },
  },
};

async function check({ qdb, runtime }, { sql, values, expected }) {
  const bundle = await runExporter(qdb, {
    sql,
    dataset: {
      tables: [{
        name: 't',
        columns: [{ name: 'a', type: 'f64' }],
        stats: { rows: values.length, bytes: 8 * values.length, rowGroups: 1 },
      }],
    },
    options: { scheduler: 'single', scanTasks: 1, shufflePartitions: 1, embedWasm: true },
  });
  const exec = browserExec(bundle);
  const result = await runtime.executeBrowserPipelineScheduled(exec, async function* (stage) {
    assert.equal(stage.table, 't');
    yield {
      rowCount: values.length,
      columns: stage.columns.map(column => ({ ...column, values })),
    };
  });
  compareResults(typedResult(exec, result), expected, { ordered: false });
}

export function tests(context) {
  return Object.entries(CASES).map(([name, test]) => ({
    name: `external-modules/${name}`,
    run: () => check(context, test),
  }));
}
