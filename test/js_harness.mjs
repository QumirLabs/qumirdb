// A minimal test loop for the Node.js test programs: runs named tests one by
// one, reports each failure with its cause and writes a JUnit file for CI.

import { mkdirSync, writeFileSync } from 'node:fs';
import path from 'node:path';

function xmlEscape(value) {
  return String(value ?? '')
    .replace(/[\u0000-\u0008\u000b\u000c\u000e-\u001f]/g, '�')
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&apos;');
}

// A failed expectation: its message says everything, a stack would not help.
export class TestFailure extends Error {}

export function errorText(error) {
  if (error instanceof TestFailure) {
    return error.message;
  }
  return error instanceof Error
    ? error.stack || error.message
    : String(error);
}

function writeJUnit(file, suiteName, results, elapsedMs) {
  const failures = results.filter(result => result.error).length;
  const skipped = results.filter(result => result.skipped).length;
  let xml = '<?xml version="1.0" encoding="UTF-8"?>\n';
  xml += `<testsuite name="${xmlEscape(suiteName)}" tests="${results.length}" `
    + `failures="${failures}" skipped="${skipped}" `
    + `time="${(elapsedMs / 1000).toFixed(3)}">\n`;
  for (const result of results) {
    xml += `  <testcase name="${xmlEscape(result.name)}" `
      + `classname="${xmlEscape(suiteName)}" `
      + `time="${(result.elapsedMs / 1000).toFixed(3)}">\n`;
    if (result.error) {
      const detail = errorText(result.error);
      const message = detail.split('\n', 1)[0];
      xml += `    <failure message="${xmlEscape(message)}">`
        + `${xmlEscape(detail)}</failure>\n`;
    } else if (result.skipped) {
      xml += `    <skipped message="${xmlEscape(result.skipped)}"/>\n`;
    }
    xml += '  </testcase>\n';
  }
  xml += '</testsuite>\n';
  mkdirSync(path.dirname(file), { recursive: true });
  writeFileSync(file, xml, 'utf8');
}

// Splits `--xml <file>` off the command line.
export function takeXmlOption(args) {
  const index = args.indexOf('--xml');
  if (index < 0) return null;
  const file = args[index + 1];
  if (!file) throw new Error('--xml needs a file name');
  args.splice(index, 2);
  return file;
}

// tests: [{ name, run?, skipped? }]. A test with `skipped` set is reported with
// that reason and not run. Returns the number of failures.
export async function runTests(suiteName, tests, { xml = null, quiet = false } = {}) {
  const started = performance.now();
  const results = [];
  for (const test of tests) {
    if (test.skipped) {
      console.log(`[SKIP] ${test.name}: ${test.skipped}`);
      results.push({ name: test.name, elapsedMs: 0, skipped: test.skipped });
      continue;
    }
    const testStarted = performance.now();
    let error = null;
    try {
      await test.run();
    } catch (caught) {
      error = caught ?? new Error('test failed without an error');
    }
    const elapsedMs = performance.now() - testStarted;
    results.push({ name: test.name, elapsedMs, error });
    if (error) {
      console.error(`[FAIL] ${test.name}\n${errorText(error)}`);
    } else if (!quiet) {
      console.log(`[ OK ] ${test.name} (${elapsedMs.toFixed(0)} ms)`);
    }
  }
  const elapsedMs = performance.now() - started;
  if (xml) {
    writeJUnit(xml, suiteName, results, elapsedMs);
  }
  const failures = results.filter(result => result.error);
  const skipped = results.filter(result => result.skipped).length;
  console.log(`${suiteName}: ${results.length - failures.length - skipped} passed, `
    + `${failures.length} failed, ${skipped} skipped (${(elapsedMs / 1000).toFixed(1)} s)`);
  for (const failure of failures) {
    console.log(`  FAILED ${failure.name}`);
  }
  return failures.length;
}
