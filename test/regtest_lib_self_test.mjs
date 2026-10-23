// Self-tests of the browser runner's `expect_error` support (test/regtest_lib.mjs)
// against synthetic exports, so that every stage that can fail is covered.
//
// usage: node test/regtest_lib_self_test.mjs [--xml <junit.xml>]

import assert from 'node:assert/strict';
import { mkdirSync, mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import path from 'node:path';

import { errorText, runTests, takeXmlOption } from './js_harness.mjs';
import { checkCase, collectCases } from './regtest_lib.mjs';

const temp = mkdtempSync(path.join(tmpdir(), 'qdb-regtest-lib-'));
let counter = 0;

// A case whose goldens hold the given texts.
function errorCase({ plan = 'Plan\n', error = null }) {
  const dir = path.join(temp, String(counter++));
  mkdirSync(dir);
  const test = {
    expectError: true,
    planGolden: path.join(dir, 'case.plan'),
    errorGolden: path.join(dir, 'case.error'),
  };
  writeFileSync(test.planGolden, plan);
  if (error !== null) writeFileSync(test.errorGolden, error);
  return test;
}

const planned = { ok: true, exec: { supported: true } };
const runtime = outcome => ({
  executeBrowserPipelineScheduled: async () => {
    if (outcome) throw new Error(outcome);
  },
});
const exported = (bundle, tables = {}) => ({ tables, bundle });

async function failsWith(pattern, promise) {
  await assert.rejects(promise, error => {
    assert.match(error.message, pattern);
    return true;
  });
}

function collect(manifest) {
  const root = path.join(temp, `suite-${counter++}`);
  mkdirSync(path.join(root, 'cases', 'g'), { recursive: true });
  writeFileSync(path.join(root, 'cases', 'g', 'suite.json'), JSON.stringify({
    version: 1,
    fixtures: {},
    tables: { t: 'x' },
    variants: ['single', 'js'],
    cases: { 'q.sql': {} },
    ...manifest,
  }));
  return collectCases(root);
}

function tests() {
  return [
    {
      name: 'expect_error must be a boolean in the suite and in a case',
      run: async () => {
        assert.throws(() => collect({ expect_error: 'true' }), /"expect_error" must be a boolean/);
        assert.throws(() => collect({ cases: { 'q.sql': { expect_error: 1 } } }),
          /q\.sql: "expect_error" must be a boolean/);
        // A plan-only case is validated too, although the browser skips it.
        assert.throws(() => collect({ execute: false, expect_error: 'yes' }), /must be a boolean/);
        assert.equal(collect({ expect_error: true }).cases[0].expectError, true);
        assert.equal(collect({ expect_error: true, cases: { 'q.sql': { expect_error: false } } })
          .cases[0].expectError, false);
      },
    },
    {
      name: 'a planning error is compared with the plan golden',
      run: async () => {
        const test = errorCase({ plan: 'Error: bad column @ Line: 1, Byte: 8, Column: 8\n' });
        const bundle = { ok: false, error: { message: 'Error: bad column @ Line: 1, Byte: 8, Column: 8' } };
        await checkCase(test, exported(bundle), runtime(null));
        const other = { ok: false, error: { message: 'Error: other @ Line: 1, Byte: 8, Column: 8' } };
        await failsWith(/error differs from .*case\.plan/, checkCase(test, exported(other), runtime(null)));
      },
    },
    {
      name: 'a planning error is not accepted for a plan golden that holds a plan',
      run: async () => {
        const test = errorCase({ plan: 'project\n', error: 'Runtime error: boom\n' });
        const bundle = { ok: false, error: { message: 'boom' } };
        await failsWith(/error differs from .*case\.plan/, checkCase(test, exported(bundle), runtime(null)));
      },
    },
    {
      name: 'the stage comes from where the failure happened, not from the golden',
      run: async () => {
        // The plan golden holds a planning error but the exporter planned the
        // query and the run fails: the .error golden is missing.
        const test = errorCase({ plan: 'Error: bad @ Line: 1, Byte: 1, Column: 1\n' });
        await failsWith(/missing golden .*case\.error/, checkCase(test, exported(planned), runtime('boom')));
      },
    },
    {
      name: 'a lowering failure is compared with the error golden',
      run: async () => {
        const test = errorCase({ error: 'Runtime error: no kernel\n' });
        const lowering = reason => ({ ok: true, exec: { supported: false, reason } });
        await checkCase(test, exported(lowering('no kernel')), runtime(null));
        await failsWith(/error differs from .*case\.error/,
          checkCase(test, exported(lowering('another reason')), runtime(null)));
      },
    },
    {
      name: 'a failed run is compared with the error golden',
      run: async () => {
        const test = errorCase({ error: 'Runtime error: abs overflow for i64\n' });
        await checkCase(test, exported(planned), runtime('project: abs overflow for i64 (wasm reserved 0.0MB, peak live 0.0MB)'));
        await failsWith(/error differs from .*case\.error/,
          checkCase(test, exported(planned), runtime('unrelated crash')));
      },
    },
    {
      name: 'a run that succeeds fails the case',
      run: async () => {
        const test = errorCase({ error: 'Runtime error: boom\n' });
        await failsWith(/but the browser run succeeded/, checkCase(test, exported(planned), runtime(null)));
      },
    },
  ];
}

try {
  const args = process.argv.slice(2);
  const xml = takeXmlOption(args);
  if (args.length > 0) throw new Error('usage: regtest_lib_self_test.mjs [--xml <junit.xml>]');
  if (await runTests('regtest-lib-self-test', tests(), { xml }) > 0) process.exitCode = 1;
} catch (error) {
  console.error(`regtest_lib_self_test.mjs: ${errorText(error)}`);
  process.exitCode = 1;
} finally {
  rmSync(temp, { recursive: true, force: true });
}
