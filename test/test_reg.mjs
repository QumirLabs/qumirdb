// Runs the shared regression suites through `qdb --plan-export` and the
// browser WASM runtime. A suite (or a case) takes part when its variants list
// "js"; results are checked against the same .result.json goldens as the
// native test_reg, which alone canonizes them.
//
// usage: node test/test_reg.mjs <qdb> <browser_runtime.js> [<regtest root>]
//            [--filter <glob>[:<glob>...]] [--xml <junit.xml>]

import { availableParallelism } from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

import { errorText, runTests, takeXmlOption } from './js_harness.mjs';
import { checkCase, collectCases, exportCase } from './regtest_lib.mjs';

function takeFilter(args) {
  const index = args.indexOf('--filter');
  if (index < 0) return null;
  const patterns = args[index + 1];
  if (!patterns) throw new Error('--filter needs a pattern');
  args.splice(index, 2);
  // gtest-style globs over test names, e.g. 'boolean_columns_*:order_by_*'.
  const regexes = patterns.split(':').map(glob => new RegExp(
    `^${glob.replace(/[.+^${}()|[\]\\]/g, '\\$&').replace(/\*/g, '.*').replace(/\?/g, '.')}$`));
  return name => regexes.some(regex => regex.test(name));
}

async function main() {
  const args = process.argv.slice(2);
  const xml = takeXmlOption(args);
  const filter = takeFilter(args) ?? (() => true);
  const defaultRoot = path.join(path.dirname(fileURLToPath(import.meta.url)), 'regtest');
  const [qdb, runtimePath, root = defaultRoot] = args;
  if (!qdb || !runtimePath || args.length > 3) {
    throw new Error('usage: test_reg.mjs <qdb> <browser_runtime.js> [<regtest root>] '
      + '[--filter <glob>] [--xml <junit.xml>]');
  }
  const runtime = await import(pathToFileURL(path.resolve(runtimePath)).href);
  const collected = collectCases(root);
  const cases = collected.cases
    .map(test => ({ test, name: `${test.name}_js` }))
    .filter(({ name }) => filter(name));
  const skipped = collected.skipped
    .map(test => ({ name: `${test.name}_js`, skipped: test.reason }))
    .filter(({ name }) => filter(name));
  if (cases.length === 0) {
    throw new Error(`no js cases selected under ${path.resolve(root, 'cases')}`);
  }

  // Exporting dominates the run time and happens in child processes: keep a
  // window of exports in flight while the browser runtime executes in order.
  const window = Math.max(1, Math.min(8, availableParallelism()));
  let started = 0;
  const prefetch = end => {
    for (; started < Math.min(end, cases.length); ++started) {
      const job = cases[started];
      job.exported = exportCase(job.test, qdb).then(
        value => ({ value }),
        error => ({ error }));
    }
  };
  const tests = cases.map((job, index) => ({
    name: job.name,
    run: async () => {
      prefetch(index + window);
      const { value, error } = await job.exported;
      if (error) throw error;
      await checkCase(job.test, value, runtime);
    },
  }));
  return runTests('test_reg_js', [...tests, ...skipped], { xml });
}

try {
  if (await main() > 0) {
    process.exitCode = 1;
  }
} catch (error) {
  console.error(`test_reg.mjs: ${errorText(error)}`);
  process.exitCode = 1;
}
