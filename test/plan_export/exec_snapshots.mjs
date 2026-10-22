// Pins the exec plan that the exporter hands to the browser runtime, per
// scheduler preset, for one shared case of each browser stage kind. Every
// exported plan also runs in the browser against the case's result golden.

import assert from 'node:assert/strict';
import { mkdirSync, readFileSync, renameSync, rmSync, writeFileSync } from 'node:fs';
import path from 'node:path';
import { isDeepStrictEqual } from 'node:util';

import {
  SCHEDULER_PRESETS,
  browserExec,
  checkCase,
  exportCase,
  sharedCase,
} from '../regtest_lib.mjs';

const CASES = [
  'filter_project',
  'aggregate',
  'join',
  'cross_join',
  'left_outer_join',
  'left_outer_residual',
  'right_outer_join',
  'right_outer_residual',
  'full_outer_residual',
  'semi_join',
  'nested_limit',
  'nested_limit_zero',
  'source_limit',
  'top_sort',
];

// The exporter refuses a sort stage without its wasm kernel, so these
// snapshots are taken from the wasm bundle.
const NEEDS_WASM = new Set(['top_sort']);

// A LEFT or RIGHT join builds its preserved side when that side is estimated
// no larger than the other input, and always with a residual predicate.
const BUILD_SIDES = {
  left_outer_join: 'left',
  left_outer_residual: 'left',
  right_outer_join: 'right',
  right_outer_residual: 'right',
};

function snapshotOf(exec) {
  const snapshot = {
    supported: exec.supported,
    embedWasm: exec.embedWasm,
    nodes: exec.nodes,
    edges: exec.edges,
    root: exec.root,
    output: exec.output,
  };
  if (exec.limit !== undefined) {
    snapshot.limit = exec.limit;
  }
  return snapshot;
}

function assertStablePhysicalGroups(bundle) {
  const physical = bundle.graphs?.physical;
  assert.ok(physical, 'physical graph is missing');
  for (const node of physical.nodes) {
    assert.match(node.taskGroup, /^(exec|task):[1-9][0-9]*$/,
      `unstable physical task group ${node.taskGroup}`);
  }
  assert.deepEqual(bundle.graph, physical, 'legacy physical graph mirror drifted');
}

// The temporary-file rename keeps a golden replacement atomic if the run is
// interrupted.
function writeGolden(file, snapshot) {
  mkdirSync(path.dirname(file), { recursive: true });
  const temporary = `${file}.tmp-${process.pid}`;
  try {
    writeFileSync(temporary, `${JSON.stringify(snapshot, null, 2)}\n`, 'utf8');
    renameSync(temporary, file);
  } finally {
    rmSync(temporary, { force: true });
  }
}

async function checkSnapshot(context, name, mode) {
  const test = sharedCase(context.regtest, `operators/${name}`);
  const options = { ...SCHEDULER_PRESETS[mode], embedWasm: NEEDS_WASM.has(name) };
  const exported = await exportCase(test, context.qdb, { options });
  const exec = browserExec(exported.bundle);
  assertStablePhysicalGroups(exported.bundle);
  if (BUILD_SIDES[name]) {
    const joins = exec.nodes.filter(node => node.kind === 'join');
    assert.ok(joins.length > 0, 'join stage is missing');
    for (const join of joins) {
      assert.equal(join.buildSide, BUILD_SIDES[name], 'preserved build side changed');
    }
  }
  const snapshot = snapshotOf(exec);
  const golden = path.join(context.goldens, `${name}.${mode}.json`);
  if (context.printGoldens) {
    console.log(`=== ${path.basename(golden)} ===\n${JSON.stringify(snapshot, null, 2)}`);
    return;
  }
  if (!context.canonize) {
    assert.deepEqual(snapshot, JSON.parse(readFileSync(golden, 'utf8')),
      `exported exec JSON differs from ${golden}`);
  }
  if (name === 'join' && mode === 'threaded') {
    const repeated = await exportCase(test, context.qdb, { options });
    assert.deepEqual(exported.bundle.graphs.physical, repeated.bundle.graphs.physical,
      'physical graph changed across exporter processes');
  }
  await checkCase(test, await exportCase(test, context.qdb, { options: SCHEDULER_PRESETS[mode] }), context.runtime);
  // Canonize only a snapshot that passed the same checks as the normal run.
  if (context.canonize) {
    let current = null;
    try {
      current = JSON.parse(readFileSync(golden, 'utf8'));
    } catch (error) {
      if (error?.code !== 'ENOENT') throw error;
    }
    if (!isDeepStrictEqual(snapshot, current)) {
      writeGolden(golden, snapshot);
      console.log(`updated ${golden}`);
    }
  }
}

export function tests(context) {
  return CASES.flatMap(name => Object.keys(SCHEDULER_PRESETS).map(mode => ({
    name: `exec-snapshot/${name}.${mode}`,
    run: () => checkSnapshot(context, name, mode),
  })));
}
