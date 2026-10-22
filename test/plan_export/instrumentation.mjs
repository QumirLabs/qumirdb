// Browser operator instrumentation follows the native exporter contract:
// per-stage rows, logical payload bytes and memory, without changing results.

import assert from 'node:assert/strict';

import {
  SCHEDULER_PRESETS,
  browserExec,
  checkCase,
  exportCase,
  sharedCase,
} from '../regtest_lib.mjs';

const INSTRUMENTED = { operatorInstrumentation: true };

async function run(context, base, preset = SCHEDULER_PRESETS.single) {
  const test = sharedCase(context.regtest, base);
  const exported = await exportCase(test, context.qdb, { options: { ...preset, ...INSTRUMENTED } });
  const result = await checkCase(test, exported, context.runtime, INSTRUMENTED);
  return {
    test,
    rows: result.rows.length,
    stages: result.operator_instrumentation.stages,
    instrumentation: result.operator_instrumentation,
  };
}

const stageOf = (stages, kind) => stages.find(stage => stage.kind === kind);

// NULL and empty strings carry no payload; a two-byte UTF-8 letter counts twice.
async function stringPayload(context) {
  const { stages } = await run(context, 'operators/filter_strings');
  assert.equal(stageOf(stages, 'source').rows_out, 4);
  assert.equal(stageOf(stages, 'source').bytes_out, 37);
  assert.equal(stageOf(stages, 'filter').rows_out, 3);
  assert.equal(stageOf(stages, 'filter').bytes_out, 28);
}

async function residualSemiJoin(context) {
  const { stages } = await run(context, 'operators/semi_join_residual');
  const join = stageOf(stages, 'join');
  assert.equal(join.rows_out, 2);
  assert.ok(join.hash_memory_peak_bytes > 0);
  assert.equal(join.retained_input_bytes, 0);
  assert.ok(join.retained_input_peak_bytes > 0);
}

// Unmatched preserved rows are counted as join output; retained inputs are
// released once the join has drained.
async function outerJoin(context, name, mode) {
  const { rows, stages } = await run(context, `operators/${name}`, SCHEDULER_PRESETS[mode]);
  const joins = stages.filter(stage => stage.kind === 'join');
  assert.ok(joins.length > 0);
  assert.equal(joins.reduce((sum, stage) => sum + stage.rows_out, 0), rows);
  assert.ok(joins.some(stage => stage.hash_memory_peak_bytes > 0));
  assert.ok(joins.some(stage => stage.retained_input_peak_bytes > 0));
  assert.ok(joins.every(stage => stage.retained_input_bytes === 0));
}

const OUTER_JOINS = ['left_outer_join', 'left_outer_residual', 'right_outer_join', 'right_outer_residual', 'full_outer_residual'];

const OPERATORS = {
  filter_project(stages) {
    const filter = stageOf(stages, 'filter');
    assert.equal(filter.rows_in, 4);
    assert.equal(filter.rows_out, 3);
    assert.equal(filter.bytes_in, 64);
    assert.equal(filter.bytes_out, 48);
    return 4;
  },
  aggregate(stages) {
    const aggregate = stageOf(stages, 'aggregate');
    assert.equal(aggregate.rows_in, 5);
    assert.equal(aggregate.rows_out, 4);
    assert.equal(aggregate.groups, 4);
    assert.equal(aggregate.rehashes, 0);
    assert.ok(aggregate.hash_memory_peak_bytes > 0);
    assert.equal(aggregate.build_memory_peak_bytes, aggregate.hash_memory_peak_bytes);
    assert.equal(aggregate.materialized_bytes, 64);
    return 5;
  },
  join(stages) {
    const join = stageOf(stages, 'join');
    assert.equal(join.rows_in, 7);
    assert.equal(join.rows_out, 2);
    assert.equal(join.retained_input_bytes, 0);
    assert.ok(join.retained_input_peak_bytes > 0);
    assert.ok(join.build_memory_peak_bytes >= join.hash_memory_peak_bytes);
    return 7;
  },
};

async function operator(context, name) {
  const { test, stages, instrumentation } = await run(context, `operators/${name}`);
  assert.equal(instrumentation.schema_version, 1);
  assert.equal(instrumentation.bytes, 'logical_nonnull_payload');
  for (const stage of stages) {
    assert.equal(stage.execution_cpu_seconds, null);
    assert.ok(stage.task_elapsed_seconds >= 0);
    assert.ok(stage.bytes_out >= 0);
  }
  const sourceRows = OPERATORS[name](stages);
  assert.equal(stages.filter(stage => stage.kind === 'source')
    .reduce((sum, stage) => sum + stage.rows_out, 0), sourceRows);
  // Without the option the plan carries no instrumentation fields.
  const ordinary = browserExec((await exportCase(test, context.qdb, { options: { embedWasm: false } })).bundle);
  assert.equal(ordinary.operatorInstrumentation, undefined);
  assert.ok(ordinary.nodes.every(stage => stage.instrumentationWidths === undefined));
}

export function tests(context) {
  return [
    { name: 'instrumentation/string-payload', run: () => stringPayload(context) },
    { name: 'instrumentation/residual-semi-join', run: () => residualSemiJoin(context) },
    ...OUTER_JOINS.flatMap(name => Object.keys(SCHEDULER_PRESETS).map(mode => ({
      name: `instrumentation/${name}.${mode}`,
      run: () => outerJoin(context, name, mode),
    }))),
    ...Object.keys(OPERATORS).map(name => ({
      name: `instrumentation/${name}`,
      run: () => operator(context, name),
    })),
  ];
}
