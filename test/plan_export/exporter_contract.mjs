// What the exporter promises the browser beyond a runnable plan: schema type
// syntax, compile diagnostics, and plans without the native-only runtime
// filters.

import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

import {
  browserExec,
  caseTables,
  checkResult,
  exportRequest,
  fixtureSource,
  runExporter,
  sharedCase,
} from '../regtest_lib.mjs';

function singleColumnRequest(sql, type) {
  return {
    sql,
    dataset: {
      tables: [{
        name: 't',
        columns: [{ name: 'amount', type }],
        stats: { rows: 1, bytes: 16, rowGroups: 1 },
      }],
    },
    options: { scheduler: 'single', scanTasks: 1, shufflePartitions: 1, format: 'runtime-bundle' },
  };
}

async function schemaTypes({ qdb, ozType }) {
  const { Type, printType, prettifyType } = ozType;
  const decimal = Type.named('Decimal', [18, 2]);
  const printed = printType(Type.named('Nullable', [decimal]));
  assert.equal(printed, '<named Nullable [<named Decimal [18 2]>]>');
  assert.equal(prettifyType(printed), 'Nullable[Decimal[18 2]]');
  assert.equal(prettifyType('i32'), 'i32');

  const bundle = await runExporter(qdb, singleColumnRequest('SELECT amount FROM t', printed));
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

  const legacy = await runExporter(
    qdb, singleColumnRequest('SELECT amount FROM t', 'Nullable<DECIMAL(18,2)>'));
  assert.equal(legacy.ok, false, 'legacy schema type syntax is still accepted');
  assert.match(legacy.error?.message || '', /unexpected trailing input/);
}

async function typeErrorDiagnostics({ qdb }) {
  const request = singleColumnRequest("SELECT amount + 'x' AS bad FROM t", 'f64');
  request.options.embedWasm = true;
  const bundle = await runExporter(qdb, request);
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

// Statistics that make the native planner add a runtime filter to the join;
// the browser bundle must run without one.
async function runtimeFiltersStayNative({ qdb, regtest, runtime }) {
  const test = sharedCase(regtest, 'operators/join');
  const tables = caseTables(test);
  const request = exportRequest(readFileSync(test.sql, 'utf8'), tables);
  const stats = {
    l: { rows: 1_000_000, columns: [{ name: 'k', ndv: 1_000_000, ndv_exact: true }] },
    r: { rows: 100_000, columns: [{ name: 'rk', ndv: 10_000, ndv_exact: true }] },
  };
  for (const table of request.dataset.tables) {
    table.stats = stats[table.name];
  }
  const bundle = await runExporter(qdb, request);
  const exec = browserExec(bundle);
  assert.ok(bundle.graphs?.physical?.nodes?.length > 0);
  assert.ok(!exec.nodes.some(node => node.kind === 'runtime-filter'));
  checkResult(test, exec, await runtime.executeBrowserPipelineScheduled(exec, fixtureSource(tables)));
}

export function tests(context) {
  return [
    { name: 'exporter/schema-types', run: () => schemaTypes(context) },
    { name: 'exporter/type-error-diagnostics', run: () => typeErrorDiagnostics(context) },
    { name: 'exporter/runtime-filters-stay-native', run: () => runtimeFiltersStayNative(context) },
  ];
}
