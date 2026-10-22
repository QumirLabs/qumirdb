import assert from 'node:assert/strict';
import {
  mkdirSync,
  mkdtempSync,
  readFileSync,
  readdirSync,
  renameSync,
  rmSync,
  writeFileSync,
} from 'node:fs';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { pathToFileURL } from 'node:url';
import { spawnSync } from 'node:child_process';
import { isDeepStrictEqual } from 'node:util';

const args = process.argv.slice(2);
const printGoldens = process.env.QDB_PRINT_GOLDENS === '1';
const canonizeIndex = args.indexOf('--canonize');
const canonizeGoldens = canonizeIndex >= 0;
if (canonizeGoldens) {
  args.splice(canonizeIndex, 1);
}
let xmlOutputPath = null;
const xmlIndex = args.indexOf('--xml');
if (xmlIndex >= 0) {
  xmlOutputPath = args[xmlIndex + 1] ?? null;
  args.splice(xmlIndex, 2);
}
const [exporter, nativeRunner, runtimePath, goldensDir] = args;
// The exporter lives inside the qdb binary, behind --plan-export.
const exporterBaseArgs = ['--plan-export', '--stdin-json', '--stdout-json'];
const usageError = !exporter || !nativeRunner || !runtimePath || !goldensDir ||
  args.length !== 4 || (xmlIndex >= 0 && !xmlOutputPath) ||
  (printGoldens && canonizeGoldens)
  ? new Error(
    'usage: run.mjs <qdb> <native_runner> '
      + '<browser_runtime> <goldens> [--canonize] [--xml <junit.xml>]')
  : null;

let executeBrowserPipelineScheduled;
let Type;
let printType;
let prettifyType;
let canonizedGoldenCount = 0;

function xmlEscape(value) {
  return String(value ?? '')
    .replace(/[\u0000-\u0008\u000b\u000c\u000e-\u001f]/g, '\ufffd')
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&apos;');
}

function errorText(error) {
  return error instanceof Error
    ? error.stack || error.message
    : String(error);
}

function writeJUnit(results, elapsedMs) {
  if (!xmlOutputPath) return;
  const failures = results.filter(result => result.error).length;
  let xml = '<?xml version="1.0" encoding="UTF-8"?>\n';
  xml += '<testsuite name="plan-export-integration" '
    + `tests="${results.length}" failures="${failures}" `
    + `time="${(elapsedMs / 1000).toFixed(3)}">\n`;
  for (const result of results) {
    xml += `  <testcase name="${xmlEscape(result.name)}" `
      + 'classname="plan-export-integration" '
      + `time="${(result.elapsedMs / 1000).toFixed(3)}">\n`;
    if (result.error) {
      const detail = errorText(result.error);
      const message = detail.split('\n', 1)[0];
      xml += `    <failure message="${xmlEscape(message)}">`
        + `${xmlEscape(detail)}</failure>\n`;
    }
    xml += '  </testcase>\n';
  }
  xml += '</testsuite>\n';
  mkdirSync(path.dirname(xmlOutputPath), { recursive: true });
  writeFileSync(xmlOutputPath, xml, 'utf8');
}

const fixtures = {
  filter_project: {
    sql: 'SELECT k AS k, v + 1 AS x FROM t WHERE v >= 20',
    tables: {
      t: { k: [1, 2, 3, 4], v: [10, 20, 30, 40] },
    },
  },
  aggregate: {
    sql: 'SELECT k AS k, sum(v) AS s FROM t GROUP BY k',
    tables: {
      t: { k: [1, 2, 3, 4, 4], v: [10, 20, 3, 4, 5] },
    },
    stats: {
      t: {
        columns: [{ name: 'k', ndv: 4, ndv_exact: true }],
      },
    },
  },
  join: {
    sql: 'SELECT l.k AS k, l.v + r.w AS total FROM l JOIN r ON l.k = r.rk',
    tables: {
      l: { k: [1, 2, 3, 4], v: [10, 20, 30, 40] },
      r: { rk: [2, 4, 5], w: [200, 400, 500] },
    },
  },
  // A join without equi-keys: the nested-loop kernel and its residual filter.
  cross_join: {
    sql: 'SELECT l.k AS k, r.w AS w FROM l, r WHERE l.v < r.w',
    tables: {
      l: { k: [1, 2, 3, 4], v: [10, 20, 30, 40] },
      r: { w: [15, 35] },
    },
    browserBatchSizes: { l: [2, 2] },
  },
  left_outer_join: {
    sql: 'SELECT l.k AS k, l.v AS v, r.rk AS rk, r.w AS w '
      + 'FROM l LEFT JOIN r ON l.k = r.rk',
    tables: {
      l: { k: [1, 1, 2, 3, 4], v: [10, 30, 20, 40, 50] },
      r: { rk: [1, 1, 2, 4, 5, 6], w: [5, 15, 25, 1, 100, 200] },
    },
    browserBatchSizes: { l: [2, 3], r: [2, 1, 3] },
    expectedBuildSide: 'left',
    expectedRows: [
      [1, 10, 1, 5], [1, 10, 1, 15], [1, 30, 1, 5], [1, 30, 1, 15],
      [2, 20, 2, 25], [3, 40, '', ''], [4, 50, 4, 1],
    ],
  },
  left_outer_residual: {
    sql: 'SELECT l.k AS k, l.v AS v, r.rk AS rk, r.w AS w '
      + 'FROM l LEFT JOIN r ON l.k = r.rk AND l.v < r.w',
    tables: {
      l: { k: [1, 1, 2, 3, 4], v: [10, 30, 20, 40, 50] },
      r: { rk: [1, 1, 2, 4, 5, 6], w: [5, 15, 25, 1, 100, 200] },
    },
    browserBatchSizes: { l: [2, 3], r: [2, 1, 3] },
    expectedBuildSide: 'left',
    expectedRows: [
      [1, 10, 1, 15], [1, 30, '', ''], [2, 20, 2, 25],
      [3, 40, '', ''], [4, 50, '', ''],
    ],
  },
  right_outer_join: {
    sql: 'SELECT l.k AS k, l.v AS v, r.rk AS rk, r.w AS w '
      + 'FROM l RIGHT JOIN r ON l.k = r.rk',
    tables: {
      l: { k: [1, 1, 2, 4, 5, 6], v: [5, 15, 25, 1, 100, 200] },
      r: { rk: [1, 1, 2, 3, 4], w: [10, 30, 20, 40, 50] },
    },
    browserBatchSizes: { l: [2, 1, 3], r: [2, 3] },
    expectedBuildSide: 'right',
    expectedRows: [
      [1, 5, 1, 10], [1, 15, 1, 10], [1, 5, 1, 30], [1, 15, 1, 30],
      [2, 25, 2, 20], ['', '', 3, 40], [4, 1, 4, 50],
    ],
  },
  // The preserved side is larger, so residual correctness must force its build.
  right_outer_residual: {
    sql: 'SELECT l.k AS k, l.v AS v, r.rk AS rk, r.w AS w '
      + 'FROM l RIGHT JOIN r ON l.k = r.rk AND l.v < r.w',
    tables: {
      l: { k: [1, 1, 2, 3, 4], v: [10, 30, 20, 40, 50] },
      r: { rk: [1, 1, 2, 4, 5, 6], w: [5, 15, 25, 1, 100, 200] },
    },
    browserBatchSizes: { l: [2, 3], r: [2, 1, 3] },
    expectedBuildSide: 'right',
    expectedRows: [
      ['', '', 1, 5], [1, 10, 1, 15], [2, 20, 2, 25],
      ['', '', 4, 1], ['', '', 5, 100], ['', '', 6, 200],
    ],
  },
  // Force right-build SEMI with a ratio above JoinAsymmetryRatio.
  semi_join: {
    sql: 'SELECT k AS k, v AS v FROM l WHERE k IN (SELECT rk FROM r)',
    tables: {
      l: { k: [1, 2, 3, 4, 5, 6, 1, 2, 3, 4, 5, 6, 1, 2, 3, 4, 5, 6, 1, 2, 3, 4, 5, 6, 1, 2, 3, 4, 5, 6, 1, 2, 3, 4, 5, 6, 1, 2, 3, 4], v: [10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120, 130, 140, 150, 160, 170, 180, 190, 200, 210, 220, 230, 240, 250, 260, 270, 280, 290, 300, 310, 320, 330, 340, 350, 360, 370, 380, 390, 400] },
      r: { rk: [2, 4, 5] },
    },
    browserBatchSizes: { l: [16, 16, 8] },
  },
  nested_limit: {
    sql: 'SELECT count(*) AS n FROM '
      + '(SELECT k FROM t WHERE k >= 2 LIMIT 2 OFFSET 1) u',
    tables: {
      t: { k: [1, 2, 3, 4, 5] },
    },
    browserBatchSizes: { t: [2, 1, 2] },
  },
  nested_limit_zero: {
    sql: 'SELECT count(*) AS n FROM (SELECT k FROM t LIMIT 0) u',
    tables: {
      t: { k: [1, 2, 3] },
    },
    browserBatchSizes: { t: [1, 2] },
  },
  source_limit: {
    sql: 'SELECT * FROM t LIMIT 10',
    tables: {
      t: { k: [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12] },
    },
    browserBatchSizes: { t: [4, 4, 4] },
  },
  // Several batches and a limit below the row count, so the top-sort kernel
  // really merges a running top-K instead of sorting once.
  top_sort: {
    sql: 'SELECT k AS k, v AS v FROM t ORDER BY v DESC, k ASC LIMIT 3',
    tables: {
      t: {
        k: [1, 2, 3, 4, 5, 6, 7, 8],
        v: [30, 10, 30, 40, 20, 40, 10, 20],
      },
    },
    browserBatchSizes: { t: [3, 3, 2] },
    // The exporter refuses a sort stage without an embedded wasm kernel, so the
    // snapshot has to be taken from the wasm bundle too.
    needsWasm: true,
  },
};

function datasetFor(fixture) {
  return {
    tables: Object.entries(fixture.tables).map(([name, columns]) => {
      const rowCount = Object.values(columns)[0]?.length ?? 0;
      return {
        name,
        columns: Object.keys(columns).map(column => ({
          name: column,
          type: 'i64',
        })),
        stats: {
          rows: rowCount,
          bytes: rowCount * Object.keys(columns).length * 8,
          rowGroups: 4,
          ...(fixture.stats?.[name] || {}),
        },
      };
    }),
  };
}

function run(command, args, input = undefined) {
  const result = spawnSync(command, args, {
    input,
    encoding: 'utf8',
    maxBuffer: 128 * 1024 * 1024,
  });
  if (result.error) {
    throw result.error;
  }
  assert.equal(
    result.status,
    0,
    `${command} failed (${result.status}):\n${result.stderr}`);
  return result.stdout;
}

function exportBundle(name, mode, embedWasm, cacheDir = null, instrumentation = false) {
  const fixture = fixtures[name];
  const multi = mode === 'threaded';
  const request = {
    sql: fixture.sql,
    dataset: datasetFor(fixture),
    options: {
      scheduler: mode,
      schedulerWorkers: multi ? 4 : 1,
      scanTasks: multi ? 4 : 1,
      shufflePartitions: multi ? 2 : 1,
      format: 'runtime-bundle',
      embedWasm,
      ...(instrumentation ? { operatorInstrumentation: true } : {}),
    },
  };
  const exporterArgs = [...exporterBaseArgs];
  if (cacheDir) {
    exporterArgs.push('--cache', cacheDir);
  }
  const bundle = JSON.parse(run(exporter, exporterArgs, JSON.stringify(request)));
  assert.equal(bundle.ok, true, `${name}/${mode}: exporter rejected request`);
  assert.equal(
    bundle.exec?.supported,
    true,
    `${name}/${mode}: ${bundle.exec?.reason || 'unsupported exec plan'}`);
  return bundle;
}

async function runRuntimeFilterExportContract() {
  const fixture = fixtures.join;
  const request = {
    sql: fixture.sql,
    dataset: datasetFor({
      ...fixture,
      stats: {
        l: {
          rows: 1_000_000,
          columns: [{ name: 'k', ndv: 1_000_000, ndv_exact: true }],
        },
        r: {
          rows: 100_000,
          columns: [{ name: 'rk', ndv: 10_000, ndv_exact: true }],
        },
      },
    }),
    options: {
      scheduler: 'single',
      scanTasks: 1,
      shufflePartitions: 1,
      format: 'runtime-bundle',
      embedWasm: true,
    },
  };
  const bundle = JSON.parse(run(
    exporter, exporterBaseArgs, JSON.stringify(request)));
  assert.equal(bundle.ok, true, bundle.error?.message);
  assert.equal(bundle.exec?.supported, true, bundle.exec?.reason);
  assert.ok(bundle.graphs?.physical?.nodes?.length > 0);
  assert.ok(!bundle.exec.nodes.some(node => node.kind === 'runtime-filter'));
  assert.deepEqual(
    normalizeResult(await browserResult('join', resolveExecArtifacts(bundle))),
    normalizeResult(nativeResult('join', 'single')));
}

function schemaTypeRequest(type) {
  return {
    sql: 'SELECT amount FROM t',
    dataset: {
      tables: [{
        name: 't',
        columns: [{ name: 'amount', type }],
        stats: { rows: 1, bytes: 16, rowGroups: 1 },
      }],
    },
    options: {
      scheduler: 'single',
      scanTasks: 1,
      shufflePartitions: 1,
      format: 'runtime-bundle',
      embedWasm: false,
    },
  };
}

function runSchemaTypeContract() {
  const decimal = Type.named('Decimal', [18, 2]);
  const nullableDecimal = Type.named('Nullable', [decimal]);
  const printed = printType(nullableDecimal);
  assert.equal(printed, '<named Nullable [<named Decimal [18 2]>]>');
  assert.equal(prettifyType(printed), 'Nullable[Decimal[18 2]]');
  assert.equal(prettifyType('i32'), 'i32');

  const bundle = JSON.parse(run(
    exporter,
    exporterBaseArgs,
    JSON.stringify(schemaTypeRequest(printed))));
  assert.equal(bundle.ok, true, bundle.error?.message);
  const source = bundle.exec?.nodes?.find(node => node.kind === 'source');
  assert.ok(source, 'schema type contract did not produce a source node');
  assert.deepEqual(source.columns[0], {
    name: 'amount',
    nullable: true,
    precision: 18,
    scale: 2,
    storageType: 'binint',
    type: 'decimal',
    width: 16,
  });

  const legacy = JSON.parse(run(
    exporter,
    exporterBaseArgs,
    JSON.stringify(schemaTypeRequest('Nullable<DECIMAL(18,2)>'))));
  assert.equal(legacy.ok, false, 'legacy schema type syntax is still accepted');
  assert.match(legacy.error?.message || '', /unexpected trailing input/);
}

function runTypeErrorDiagnosticContract() {
  const request = {
    sql: "SELECT amount + 'x' AS bad FROM t",
    dataset: {
      tables: [{
        name: 't',
        columns: [{ name: 'amount', type: 'f64' }],
        stats: { rows: 1, bytes: 8, rowGroups: 1 },
      }],
    },
    options: {
      scheduler: 'single',
      scanTasks: 1,
      format: 'runtime-bundle',
      embedWasm: true,
    },
  };
  const bundle = JSON.parse(run(
    exporter,
    exporterBaseArgs,
    JSON.stringify(request)));
  assert.equal(bundle.ok, true, 'type-error bundle was not formed');
  assert.equal(bundle.exec?.supported, false, 'invalid expression compiled');
  const diagnostic = bundle.diagnostics?.find(
    item => item.stage === 'wasm-fusion' && item.message);
  assert.ok(diagnostic, 'type error is missing from bundle diagnostics');
  assert.match(diagnostic.message, /оператор \+/);
  assert.equal(bundle.exec.reason, diagnostic.message);
  assert.deepEqual(bundle.exec.error, diagnostic);
  assert.doesNotMatch(bundle.exec.reason, /kernel failed to compile to wasm/);
}

async function runExternalModuleBatchContract() {
  const request = {
    sql: `
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
SELECT orbit_result(a) FROM t;
`,
    dataset: {
      tables: [{
        name: 't',
        columns: [{ name: 'a', type: 'f64' }],
      stats: { rows: 2, bytes: 16, rowGroups: 1 },
      }],
    },
    options: {
      scheduler: 'single',
      scanTasks: 1,
      shufflePartitions: 1,
      format: 'runtime-bundle',
      embedWasm: true,
    },
  };
  const bundle = JSON.parse(run(
    exporter,
    exporterBaseArgs,
    JSON.stringify(request)));
  assert.equal(bundle.ok, true, bundle.error?.message);
  assert.equal(
    bundle.exec?.supported,
    true,
    JSON.stringify({ reason: bundle.exec?.reason, diagnostics: bundle.diagnostics }));
  const exec = resolveExecArtifacts(bundle);
  const result = await executeBrowserPipelineScheduled(
    exec,
    async function* (stage) {
      assert.equal(stage.table, 't');
      yield {
        rowCount: 2,
        columns: stage.columns.map(column => ({
          ...column,
          values: [4.0, 9.0],
        })),
      };
    });
  assert.deepEqual(normalizeResult(result), {
    columns: ['col0', 'col1', 'col2'],
    rows: [['2', '4', '1'], ['3', '9', '1']],
  });
}

async function runRegexpReplaceBatchContract() {
  const request = {
    sql: String.raw`
SELECT REGEXP_REPLACE(
  url,
  '^https?://(?:www\.)?([^/]+)/.*$',
  '\1'
) AS domain
FROM t
WHERE REGEXP_REPLACE(
  url,
  '^https?://(?:www\.)?([^/]+)/.*$',
  '\1'
) <> 'blocked'
ORDER BY REGEXP_REPLACE(
  url,
  '^https?://(?:www\.)?([^/]+)/.*$',
  '\1'
);
`,
    dataset: {
      tables: [{
        name: 't',
        columns: [{ name: 'url', type: 'string' }],
        stats: { rows: 3, bytes: 64, rowGroups: 1 },
      }],
    },
    options: {
      scheduler: 'single',
      scanTasks: 1,
      shufflePartitions: 1,
      format: 'runtime-bundle',
      embedWasm: true,
    },
  };
  const bundle = JSON.parse(run(
    exporter,
    exporterBaseArgs,
    JSON.stringify(request)));
  assert.equal(bundle.ok, true, bundle.error?.message);
  assert.equal(
    bundle.exec?.supported,
    true,
    JSON.stringify({ reason: bundle.exec?.reason, diagnostics: bundle.diagnostics }));
  const exec = resolveExecArtifacts(bundle);
  const result = await executeBrowserPipelineScheduled(
    exec,
    async function* (stage) {
      assert.equal(stage.table, 't');
      yield {
        rowCount: 3,
        columns: stage.columns.map(column => ({
          ...column,
          values: [
            'https://www.example.com/a',
            'http://openai.com/x',
            'not-a-url',
          ],
        })),
      };
    });
  assert.deepEqual(normalizeResult(result), {
    columns: ['domain'],
    rows: [['example.com'], ['not-a-url'], ['openai.com']],
  });
}

async function runStringAggregateBatchContract() {
  const request = {
    sql: 'SELECT k, MIN(v) AS mn, MAX(v) AS mx FROM t GROUP BY k',
    dataset: {
      tables: [{
        name: 't',
        columns: [
          { name: 'k', type: 'i64' },
          { name: 'v', type: 'string' },
        ],
        stats: { rows: 8, bytes: 96, rowGroups: 2 },
      }],
    },
    options: {
      scheduler: 'single',
      scanTasks: 1,
      shufflePartitions: 1,
      format: 'runtime-bundle',
      embedWasm: true,
    },
  };
  const bundle = JSON.parse(run(
    exporter,
    exporterBaseArgs,
    JSON.stringify(request)));
  assert.equal(bundle.ok, true, bundle.error?.message);
  assert.equal(
    bundle.exec?.supported,
    true,
    JSON.stringify({ reason: bundle.exec?.reason, diagnostics: bundle.diagnostics }));
  const batches = [
    { k: [1, 1, 2, 2], v: ['m', 'alphabet', 'z', ''] },
    { k: [1, 1, 2, 2], v: ['a', 'zzzzzzzz', 'aardvark', 'yak'] },
  ];
  const result = await executeBrowserPipelineScheduled(
    resolveExecArtifacts(bundle),
    async function* (stage) {
      assert.equal(stage.table, 't');
      for (const batch of batches) {
        yield {
          rowCount: 4,
          columns: stage.columns.map(column => ({
            ...column,
            values: batch[column.name],
          })),
        };
      }
    });
  assert.equal(result.memory?.liveMB, '0.0', 'string aggregate leaked wasm memory');
  assert.deepEqual(normalizeResult(result), {
    columns: ['k', 'mn', 'mx'],
    rows: [['1', 'a', 'zzzzzzzz'], ['2', '', 'z']],
  });
}

async function runKumirModuleBatchContract(select, values, expected) {
  const request = {
    sql: `
CREATE MODULE math LANGUAGE kumir AS $$
алг вещ kumir_root(вещ x)
нач
  знач := sqrt(x)
кон
$$;
${select}
`,
    dataset: {
      tables: [{
        name: 't',
        columns: [{ name: 'a', type: 'f64' }],
        stats: { rows: values.length, bytes: 8 * values.length, rowGroups: 1 },
      }],
    },
    options: {
      scheduler: 'single',
      scanTasks: 1,
      shufflePartitions: 1,
      format: 'runtime-bundle',
      embedWasm: true,
    },
  };
  const bundle = JSON.parse(run(
    exporter,
    exporterBaseArgs,
    JSON.stringify(request)));
  assert.equal(bundle.ok, true, bundle.error?.message);
  assert.equal(
    bundle.exec?.supported,
    true,
    JSON.stringify({ reason: bundle.exec?.reason, diagnostics: bundle.diagnostics }));
  const exec = resolveExecArtifacts(bundle);
  const result = await executeBrowserPipelineScheduled(
    exec,
    async function* (stage) {
      assert.equal(stage.table, 't');
      yield {
        rowCount: values.length,
        columns: stage.columns.map(column => ({
          ...column,
          values,
        })),
      };
    });
  assert.deepEqual(normalizeResult(result), expected);
}

function normalizedExec(exec) {
  const normalized = {
    supported: exec.supported,
    embedWasm: exec.embedWasm,
    nodes: exec.nodes,
    edges: exec.edges,
    root: exec.root,
  };
  if (exec.limit !== undefined) {
    normalized.limit = exec.limit;
  }
  return normalized;
}

function assertStablePhysicalGroups(name, mode, bundle) {
  const physical = bundle.graphs?.physical;
  assert.ok(physical, `${name}/${mode}: physical graph is missing`);
  for (const node of physical.nodes) {
    assert.match(
      node.taskGroup,
      /^(exec|task):[1-9][0-9]*$/,
      `${name}/${mode}: unstable physical task group ${node.taskGroup}`);
  }
  assert.deepEqual(
    bundle.graph,
    physical,
    `${name}/${mode}: legacy physical graph mirror drifted`);
}

function resolveExecArtifacts(bundle) {
  const artifactId = bundle.exec.wasm;
  if (!artifactId) {
    return bundle.exec;
  }
  const wasm = bundle.artifacts?.[artifactId]?.data;
  assert.ok(wasm, `exec artifact is missing: ${artifactId}`);
  return { ...bundle.exec, wasm };
}

function readGolden(name, mode) {
  const file = path.join(goldensDir, `${name}.${mode}.json`);
  return JSON.parse(readFileSync(file, 'utf8'));
}

function writeGolden(name, mode, snapshot) {
  try {
    if (isDeepStrictEqual(snapshot, readGolden(name, mode))) {
      return false;
    }
  } catch (error) {
    if (error?.code !== 'ENOENT') throw error;
  }
  mkdirSync(goldensDir, { recursive: true });
  const file = path.join(goldensDir, `${name}.${mode}.json`);
  const temporary = `${file}.tmp-${process.pid}`;
  try {
    writeFileSync(temporary, `${JSON.stringify(snapshot, null, 2)}\n`, 'utf8');
    renameSync(temporary, file);
  } finally {
    rmSync(temporary, { force: true });
  }
  return true;
}

function nativeResult(name, mode) {
  return JSON.parse(run(nativeRunner, [name, mode]));
}

function normalizeResult(result) {
  const rows = result.rows.map(row => row.map(value => (value === null ? '' : String(value))));
  rows.sort((left, right) =>
    JSON.stringify(left).localeCompare(JSON.stringify(right)));
  return {
    columns: result.columns.map(String),
    rows,
  };
}

async function browserResult(name, exec, options = {}) {
  const fixture = fixtures[name];
  const readSourceBatches = async function* (stage) {
    const table = fixture.tables[stage.table];
    assert.ok(table, `${name}: browser requested unknown table ${stage.table}`);
    const rowCount = Object.values(table)[0]?.length ?? 0;
    const batchSizes = fixture.browserBatchSizes?.[stage.table] ?? [rowCount];
    assert.equal(
      batchSizes.reduce((sum, size) => sum + size, 0),
      rowCount,
      `${name}: browser batch sizes do not cover table ${stage.table}`);
    let begin = 0;
    for (const batchSize of batchSizes) {
      const end = begin + batchSize;
      yield {
        rowCount: batchSize,
        columns: stage.columns.map(column => {
          const values = table[column.name];
          assert.ok(
            values,
            `${name}: browser requested unknown column ${column.name}`);
          return { ...column, values: values.slice(begin, end) };
        }),
      };
      begin = end;
    }
  };
  return executeBrowserPipelineScheduled(exec, readSourceBatches, options);
}

function countObjectFiles(dir) {
  let count = 0;
  for (const entry of readdirSync(dir, { withFileTypes: true })) {
    const entryPath = path.join(dir, entry.name);
    if (entry.isDirectory()) {
      count += countObjectFiles(entryPath);
    } else if (entry.name.endsWith('.o')) {
      count += 1;
    }
  }
  return count;
}

async function runWasmCacheContract() {
  const cacheDir = mkdtempSync(path.join(tmpdir(), 'qdb-wasm-cache-'));
  try {
    const first = exportBundle('aggregate', 'single', true, cacheDir);
    const firstObjectCount = countObjectFiles(cacheDir);
    assert.ok(firstObjectCount > 0, 'wasm cache did not persist any objects');

    const second = exportBundle('aggregate', 'single', true, cacheDir);
    assert.equal(
      countObjectFiles(cacheDir),
      firstObjectCount,
      'second wasm compilation added objects instead of reusing the cache');

    assert.deepEqual(
      normalizeResult(await browserResult('aggregate', resolveExecArtifacts(second))),
      normalizeResult(await browserResult('aggregate', resolveExecArtifacts(first))),
      'cached wasm execution changed the query result');
  } finally {
    rmSync(cacheDir, { recursive: true, force: true });
  }
}

async function runInstrumentationPayloadContract() {
  const request = {
    sql: 'SELECT k, v FROM t WHERE k >= 2',
    dataset: { tables: [{ name: 't', columns: [
      { name: 'k', type: 'i64' },
      { name: 'v', type: '<named Nullable [string]>' },
    ], stats: { rows: 4, bytes: 37, rowGroups: 1 } }] },
    options: { scheduler: 'single', scanTasks: 1, embedWasm: true, operatorInstrumentation: true },
  };
  const bundle = JSON.parse(run(exporter, exporterBaseArgs, JSON.stringify(request)));
  assert.equal(bundle.ok, true, bundle.error?.message);
  assert.equal(bundle.exec.supported, true, bundle.exec.reason);
  const values = { k: [1, 2, 3, 4], v: ['x', 'аб', null, ''] };
  const result = await executeBrowserPipelineScheduled(resolveExecArtifacts(bundle),
    async function* (stage) {
      yield { rowCount: 4, columns: stage.columns.map(column => ({ ...column, values: values[column.name] })) };
    }, { operatorInstrumentation: true });
  assert.deepEqual(normalizeResult(result), {
    columns: ['k', 'v'], rows: [['2', 'аб'], ['3', ''], ['4', '']],
  });
  const stages = result.operator_instrumentation.stages;
  const source = stages.find(stage => stage.kind === 'source');
  const filter = stages.find(stage => stage.kind === 'filter');
  assert.equal(source.rows_out, 4);
  assert.equal(source.bytes_out, 37);
  assert.equal(filter.rows_out, 3);
  assert.equal(filter.bytes_out, 28);
  assert.equal(result.memory.liveMB, '0.0');
}

async function runInstrumentationSemiJoinContract() {
  const request = {
    sql: 'SELECT l.k FROM l LEFT SEMI JOIN r ON l.k = r.rk AND l.v < r.w',
    dataset: datasetFor(fixtures.join),
    options: { scheduler: 'single', scanTasks: 1, shufflePartitions: 1,
      embedWasm: true, operatorInstrumentation: true },
  };
  const bundle = JSON.parse(run(exporter, exporterBaseArgs, JSON.stringify(request)));
  assert.equal(bundle.ok, true, bundle.error?.message);
  assert.equal(bundle.exec.supported, true, bundle.exec.reason);
  const result = await browserResult('join', resolveExecArtifacts(bundle), { operatorInstrumentation: true });
  assert.deepEqual(normalizeResult(result), { columns: ['k'], rows: [['2'], ['4']] });
  const join = result.operator_instrumentation.stages.find(stage => stage.kind === 'join');
  assert.equal(join.rows_out, 2);
  assert.ok(join.hash_memory_peak_bytes > 0);
  assert.equal(join.retained_input_bytes, 0);
  assert.ok(join.retained_input_peak_bytes > 0);
  assert.equal(result.memory.liveMB, '0.0');
}

async function runOuterJoinInstrumentationContract() {
  for (const name of ['left_outer_join', 'left_outer_residual', 'right_outer_join', 'right_outer_residual']) {
    for (const mode of ['single', 'threaded']) {
      const bundle = exportBundle(name, mode, true, null, true);
      const result = await browserResult(name, resolveExecArtifacts(bundle), { operatorInstrumentation: true });
      assert.deepEqual(normalizeResult(result), normalizeResult({
        columns: ['k', 'v', 'rk', 'w'], rows: fixtures[name].expectedRows,
      }), `${name}/${mode}: instrumented outer result`);
      const joins = result.operator_instrumentation.stages.filter(stage => stage.kind === 'join');
      assert.ok(joins.length > 0);
      assert.equal(joins.reduce((sum, stage) => sum + stage.rows_out, 0), fixtures[name].expectedRows.length);
      assert.ok(joins.some(stage => stage.hash_memory_peak_bytes > 0));
      assert.ok(joins.some(stage => stage.retained_input_peak_bytes > 0));
      assert.ok(joins.every(stage => stage.retained_input_bytes === 0));
      assert.equal(result.memory?.liveMB, '0.0', `${name}/${mode}: instrumented outer leak`);
    }
  }
}

async function runOperatorInstrumentationContract() {
  await runOuterJoinInstrumentationContract();
  await runInstrumentationPayloadContract();
  await runInstrumentationSemiJoinContract();
  for (const name of ['filter_project', 'aggregate', 'join']) {
    const bundle = exportBundle(name, 'single', true, null, true);
    const exec = resolveExecArtifacts(bundle);
    const result = await browserResult(name, exec, { operatorInstrumentation: true });
    assert.deepEqual(normalizeResult(result), normalizeResult(nativeResult(name, 'single')));
    const instrumentation = result.operator_instrumentation;
    assert.equal(instrumentation.schema_version, 1);
    assert.equal(instrumentation.bytes, 'logical_nonnull_payload');
    const sourceRows = instrumentation.stages.filter(stage => stage.kind === 'source')
      .reduce((sum, stage) => sum + stage.rows_out, 0);
    assert.equal(sourceRows, name === 'join' ? 7 : name === 'aggregate' ? 5 : 4);
    for (const stage of instrumentation.stages) {
      assert.equal(stage.execution_cpu_seconds, null);
      assert.ok(stage.task_elapsed_seconds >= 0);
      assert.ok(stage.bytes_out >= 0);
    }
    if (name === 'filter_project') {
      const filter = instrumentation.stages.find(stage => stage.kind === 'filter');
      assert.equal(filter.rows_in, 4);
      assert.equal(filter.rows_out, 3);
      assert.equal(filter.bytes_in, 64);
      assert.equal(filter.bytes_out, 48);
    } else if (name === 'aggregate') {
      const aggregate = instrumentation.stages.find(stage => stage.kind === 'aggregate');
      assert.equal(aggregate.rows_in, 5);
      assert.equal(aggregate.rows_out, 4);
      assert.equal(aggregate.groups, 4);
      assert.equal(aggregate.rehashes, 0);
      assert.ok(aggregate.hash_memory_peak_bytes > 0);
      assert.equal(aggregate.build_memory_peak_bytes, aggregate.hash_memory_peak_bytes);
      assert.equal(aggregate.materialized_bytes, 64);
    } else {
      const join = instrumentation.stages.find(stage => stage.kind === 'join');
      assert.equal(join.rows_in, 7);
      assert.equal(join.rows_out, 2);
      assert.equal(join.retained_input_bytes, 0);
      assert.ok(join.retained_input_peak_bytes > 0);
      assert.ok(join.build_memory_peak_bytes >= join.hash_memory_peak_bytes);
    }
    assert.equal(result.memory?.liveMB, '0.0', `${name}: instrumentation leaked wasm memory`);
    const ordinary = exportBundle(name, 'single', false);
    assert.equal(ordinary.exec.operatorInstrumentation, undefined);
    assert.ok(ordinary.exec.nodes.every(stage => stage.instrumentationWidths === undefined));
  }
}

async function runFixture(name, mode) {
  const snapshotBundle = exportBundle(
    name, mode, fixtures[name].needsWasm === true);
  assertStablePhysicalGroups(name, mode, snapshotBundle);
  const snapshot = normalizedExec(snapshotBundle.exec);
  if (fixtures[name].expectedBuildSide) {
    const joins = snapshot.nodes.filter(node => node.kind === 'join');
    assert.ok(joins.length > 0, `${name}/${mode}: join stage is missing`);
    for (const join of joins) {
      assert.equal(join.buildSide, fixtures[name].expectedBuildSide,
        `${name}/${mode}: preserved build side changed`);
    }
  }
  if (printGoldens) {
    console.log(`=== ${name}.${mode}.json ===`);
    console.log(JSON.stringify(snapshot, null, 2));
    return;
  }
  if (!canonizeGoldens) {
    assert.deepEqual(snapshot, readGolden(name, mode),
      `${name}/${mode}: exported exec JSON changed`);
  }

  if (name === 'join' && mode === 'threaded') {
    const repeated = exportBundle(name, mode, false);
    assert.deepEqual(
      snapshotBundle.graphs.physical,
      repeated.graphs.physical,
      `${name}/${mode}: physical graph changed across exporter processes`);
  }

  const runtimeBundle = exportBundle(name, mode, true);
  const browser = await browserResult(
    name, resolveExecArtifacts(runtimeBundle));
  assert.equal(
    browser.memory?.liveMB,
    '0.0',
    `${name}/${mode}: browser runtime leaked wasm memory`);
  assert.deepEqual(
    normalizeResult(browser),
    normalizeResult(nativeResult(name, mode)),
    `${name}/${mode}: browser and native results differ`);
  if (fixtures[name].expectedRows) {
    assert.deepEqual(normalizeResult(browser), normalizeResult({
      columns: ['k', 'v', 'rk', 'w'],
      rows: fixtures[name].expectedRows,
    }), `${name}/${mode}: unexpected outer match or NULL extension`);
  }

  // Canonize only a snapshot that passed the same stability and execution
  // checks as the normal test. The temporary-file rename keeps each golden
  // replacement atomic if the runner is interrupted.
  if (canonizeGoldens) {
    if (writeGolden(name, mode, snapshot)) {
      canonizedGoldenCount += 1;
    }
  }
}

async function runBooleanColumnContract() {
  const rowCount = 145;
  const values = {
    x: Array.from({ length: rowCount }, (_, row) => row),
    b: Uint8Array.from({ length: rowCount }, (_, row) => row % 2),
    n: Array.from({ length: rowCount }, (_, row) => row % 5 === 0 ? null : 1 - row % 2),
  };
  const request = {
    sql: 'SELECT x, b, n, b = FALSE AS inv, n = FALSE AS ni FROM t WHERE x % 3 <> 1 ORDER BY x',
    dataset: { tables: [{ name: 't', columns: [
      { name: 'x', type: 'i64' },
      { name: 'b', type: 'bool' },
      { name: 'n', type: '<named Nullable [bool]>' },
    ], stats: { rows: rowCount, bytes: 2000, rowGroups: 1 } }] },
    options: { scheduler: 'single', scanTasks: 1, embedWasm: true },
  };
  const bundle = JSON.parse(run(exporter, exporterBaseArgs, JSON.stringify(request)));
  assert.equal(bundle.ok, true, bundle.error?.message);
  assert.equal(bundle.exec.supported, true, bundle.exec.reason);
  const expected = {
    columns: ['x', 'b', 'n', 'inv', 'ni'],
    rows: values.x.filter(row => row % 3 !== 1).map(row => [
      String(row), String(values.b[row]),
      values.n[row] === null ? '' : String(values.n[row]),
      String(1 - values.b[row]),
      values.n[row] === null ? '' : String(1 - values.n[row]),
    ]),
  };
  assert.deepEqual(normalizeResult(nativeResult('boolean_columns', 'single')),
    normalizeResult(expected));
  const { WasmSourceBatchBuilder } = await import(pathToFileURL(runtimePath).href);
  for (const chunked of [false, true]) {
    const result = await executeBrowserPipelineScheduled(resolveExecArtifacts(bundle),
      async function* (stage, onProgress, context) {
        if (!chunked) {
          yield { rowCount, columns: stage.columns.map(column => ({
            ...column, values: values[column.name],
          })) };
          return;
        }
        const builder = new WasmSourceBatchBuilder(
          context.shared.arena, context.layout, stage.columns, rowCount);
        for (const column of stage.columns) {
          let start = 0;
          for (const end of [3, 10, 66, rowCount]) {
            builder.writeChunk(column.name, values[column.name].slice(start, end), start, end);
            start = end;
          }
        }
        yield builder.finish();
      });
    assert.deepEqual(normalizeResult(result), normalizeResult(expected), `chunked=${chunked}`);
    assert.equal(result.memory.liveMB, '0.0');
  }
  // Keep boolean keys inside the compiled pipeline; g makes the output span
  // multiple bitmap bytes and a 64-bit word before the key is projected away.
  for (const key of ['g % 2 = 1', 'CASE WHEN g % 3 = 0 THEN NULL ELSE g % 2 = 1 END']) {
    const request = {
      sql: `SELECT g, count(*) AS c FROM t GROUP BY g, ${key} ORDER BY g`,
      dataset: { tables: [{ name: 't', columns: [
        { name: 'g', type: 'i64' },
      ], stats: { rows: 140, bytes: 1120, rowGroups: 1 } }] },
      options: { scheduler: 'single', scanTasks: 1, embedWasm: true },
    };
    const bundle = JSON.parse(run(exporter, exporterBaseArgs, JSON.stringify(request)));
    assert.equal(bundle.ok, true, bundle.error?.message);
    assert.equal(bundle.exec.supported, true, bundle.exec.reason);
    const result = await executeBrowserPipelineScheduled(resolveExecArtifacts(bundle),
      async function* (stage) {
        yield { rowCount: 140, columns: stage.columns.map(column => ({
          ...column, values: Array.from({ length: 140 }, (_, row) => row % 70),
        })) };
      });
    assert.deepEqual(result.columns.map(String), ['g', 'c']);
    assert.deepEqual(result.rows.map(row => row.map(String)),
      Array.from({ length: 70 }, (_, group) => [String(group), '2']), key);
    assert.equal(result.memory.liveMB, '0.0');
  }
}

async function main() {
  const suiteStarted = performance.now();
  const results = [];
  try {
    if (usageError) throw usageError;
    ({ executeBrowserPipelineScheduled } = await import(
      pathToFileURL(runtimePath).href));
    ({ Type, printType, prettifyType } = await import(
      pathToFileURL(path.join(path.dirname(runtimePath), 'oz_type.js')).href));
    {
      const started = performance.now();
      try {
        await runRuntimeFilterExportContract();
        results.push({
          name: 'runtime-filter-export',
          elapsedMs: performance.now() - started,
          error: null,
        });
      } catch (error) {
        results.push({
          name: 'runtime-filter-export',
          elapsedMs: performance.now() - started,
          error,
        });
        console.error(`[FAIL] runtime-filter-export\n${errorText(error)}`);
      }
    }
    {
      const started = performance.now();
      try {
        runSchemaTypeContract();
        results.push({
          name: 'schema-types',
          elapsedMs: performance.now() - started,
          error: null,
        });
      } catch (error) {
        results.push({
          name: 'schema-types',
          elapsedMs: performance.now() - started,
          error,
        });
        console.error(`[FAIL] schema-types\n${errorText(error)}`);
      }
    }
    {
      const started = performance.now();
      try {
        runTypeErrorDiagnosticContract();
        results.push({
          name: 'type-error-diagnostics',
          elapsedMs: performance.now() - started,
          error: null,
        });
      } catch (error) {
        results.push({
          name: 'type-error-diagnostics',
          elapsedMs: performance.now() - started,
          error,
        });
        console.error(`[FAIL] type-error-diagnostics\n${errorText(error)}`);
      }
    }
    {
      const started = performance.now();
      try {
        await runExternalModuleBatchContract();
        results.push({
          name: 'external-module-batch',
          elapsedMs: performance.now() - started,
          error: null,
        });
      } catch (error) {
        results.push({
          name: 'external-module-batch',
          elapsedMs: performance.now() - started,
          error,
        });
        console.error(`[FAIL] external-module-batch\n${errorText(error)}`);
      }
    }
    {
      const started = performance.now();
      try {
        await runRegexpReplaceBatchContract();
        results.push({
          name: 'regexp-replace-batch',
          elapsedMs: performance.now() - started,
          error: null,
        });
      } catch (error) {
        results.push({
          name: 'regexp-replace-batch',
          elapsedMs: performance.now() - started,
          error,
        });
        console.error(`[FAIL] regexp-replace-batch\n${errorText(error)}`);
      }
    }
    {
      const started = performance.now();
      try {
        await runStringAggregateBatchContract();
        results.push({
          name: 'string-aggregate-batch',
          elapsedMs: performance.now() - started,
          error: null,
        });
      } catch (error) {
        results.push({
          name: 'string-aggregate-batch',
          elapsedMs: performance.now() - started,
          error,
        });
        console.error(`[FAIL] string-aggregate-batch\n${errorText(error)}`);
      }
    }
    {
      const started = performance.now();
      try {
        await runKumirModuleBatchContract(
          'SELECT kumir_root(a) FROM t;',
          [4.0, 9.0],
          { columns: ['col0'], rows: [['2'], ['3']] });
        results.push({
          name: 'kumir-module-batch',
          elapsedMs: performance.now() - started,
          error: null,
        });
      } catch (error) {
        results.push({
          name: 'kumir-module-batch',
          elapsedMs: performance.now() - started,
          error,
        });
        console.error(`[FAIL] kumir-module-batch\n${errorText(error)}`);
      }
    }
    {
      const started = performance.now();
      try {
        // The plan builder types the stddev_samp argument against the module catalog.
        await runKumirModuleBatchContract(
          'SELECT stddev_samp(kumir_root(a)) AS d FROM t;',
          [4.0, 9.0, 16.0],
          { columns: ['d'], rows: [['1']] });
        results.push({
          name: 'kumir-module-stddev',
          elapsedMs: performance.now() - started,
          error: null,
        });
      } catch (error) {
        results.push({
          name: 'kumir-module-stddev',
          elapsedMs: performance.now() - started,
          error,
        });
        console.error(`[FAIL] kumir-module-stddev\n${errorText(error)}`);
      }
    }
    {
      const started = performance.now();
      try {
        await runWasmCacheContract();
        results.push({
          name: 'wasm-cache',
          elapsedMs: performance.now() - started,
          error: null,
        });
      } catch (error) {
        results.push({
          name: 'wasm-cache',
          elapsedMs: performance.now() - started,
          error,
        });
        console.error(`[FAIL] wasm-cache\n${errorText(error)}`);
      }
    }
    {
      const started = performance.now();
      try {
        await runOperatorInstrumentationContract();
        results.push({ name: 'operator-instrumentation', elapsedMs: performance.now() - started, error: null });
      } catch (error) {
        results.push({ name: 'operator-instrumentation', elapsedMs: performance.now() - started, error });
        console.error(`[FAIL] operator-instrumentation\n${errorText(error)}`);
      }
    }
    {
      const started = performance.now();
      try {
        await runBooleanColumnContract();
        results.push({ name: 'boolean-columns', elapsedMs: performance.now() - started, error: null });
      } catch (error) {
        results.push({ name: 'boolean-columns', elapsedMs: performance.now() - started, error });
        console.error(`[FAIL] boolean-columns\n${errorText(error)}`);
      }
    }
    for (const name of Object.keys(fixtures)) {
      for (const mode of ['single', 'threaded']) {
        const started = performance.now();
        try {
          await runFixture(name, mode);
          results.push({
            name: `${name}.${mode}`,
            elapsedMs: performance.now() - started,
            error: null,
          });
        } catch (error) {
          results.push({
            name: `${name}.${mode}`,
            elapsedMs: performance.now() - started,
            error,
          });
          console.error(`[FAIL] ${name}.${mode}\n${errorText(error)}`);
        }
      }
    }
  } catch (error) {
    results.push({
      name: 'harness',
      elapsedMs: performance.now() - suiteStarted,
      error,
    });
    console.error(`[FAIL] harness\n${errorText(error)}`);
  }

  writeJUnit(results, performance.now() - suiteStarted);
  const failures = results.filter(result => result.error).length;
  if (failures) {
    console.error(`plan export integration failures: ${failures}`);
    process.exitCode = 1;
  } else if (canonizeGoldens) {
    console.log(
      `plan export goldens updated: ${canonizedGoldenCount} in ${goldensDir}`);
  } else if (!printGoldens) {
    console.log('plan export golden and native/browser parity checks passed');
  }
}

await main();
