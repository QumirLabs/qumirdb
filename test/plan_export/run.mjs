// Browser-only contracts of `qdb --plan-export` and the WASM runtime. SQL
// results are checked by test/test_reg.mjs on the shared regression suites;
// these modules cover what a result golden cannot: the exported exec plan,
// input ABIs, instrumentation, the kernel cache and exporter diagnostics.
// They reuse shared cases for their queries and data.
//
// usage: node test/plan_export/run.mjs <qdb> <browser_runtime.js> <regtest root>
//            <exec goldens> [--canonize] [--xml <junit.xml>]
// QDB_PRINT_GOLDENS=1 prints the exec snapshots instead of checking them.

import path from 'node:path';
import { pathToFileURL } from 'node:url';

import { errorText, runTests, takeXmlOption } from '../js_harness.mjs';
import * as bitmapAbi from './bitmap_abi.mjs';
import * as execSnapshots from './exec_snapshots.mjs';
import * as exporterContract from './exporter_contract.mjs';
import * as instrumentation from './instrumentation.mjs';
import * as wasmCache from './wasm_cache.mjs';

async function main() {
  const args = process.argv.slice(2);
  const xml = takeXmlOption(args);
  const canonizeIndex = args.indexOf('--canonize');
  const canonize = canonizeIndex >= 0;
  if (canonize) {
    args.splice(canonizeIndex, 1);
  }
  const printGoldens = process.env.QDB_PRINT_GOLDENS === '1';
  const [qdb, runtimePath, regtest, goldens] = args;
  if (!goldens || args.length !== 4 || (printGoldens && canonize)) {
    throw new Error('usage: run.mjs <qdb> <browser_runtime.js> <regtest root> '
      + '<exec goldens> [--canonize] [--xml <junit.xml>]');
  }
  const runtimeFile = path.resolve(runtimePath);
  const context = {
    qdb,
    regtest,
    goldens,
    canonize,
    printGoldens,
    runtime: await import(pathToFileURL(runtimeFile).href),
    ozType: await import(pathToFileURL(path.join(path.dirname(runtimeFile), 'oz_type.js')).href),
  };
  const tests = [execSnapshots, exporterContract, bitmapAbi, instrumentation, wasmCache]
    .flatMap(module => module.tests(context));
  return runTests('plan-export-integration', tests, { xml });
}

try {
  if (await main() > 0) {
    process.exitCode = 1;
  }
} catch (error) {
  console.error(`plan_export/run.mjs: ${errorText(error)}`);
  process.exitCode = 1;
}
