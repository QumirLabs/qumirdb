// `--cache` reuses compiled kernel dependencies across exporter processes
// without changing what the bundle computes.

import assert from 'node:assert/strict';
import { mkdtempSync, readdirSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import path from 'node:path';

import { checkCase, exportCase, sharedCase } from '../regtest_lib.mjs';

function countObjectFiles(dir) {
  let count = 0;
  for (const entry of readdirSync(dir, { withFileTypes: true })) {
    if (entry.isDirectory()) {
      count += countObjectFiles(path.join(dir, entry.name));
    } else if (entry.name.endsWith('.o')) {
      count += 1;
    }
  }
  return count;
}

async function reuse(context) {
  const test = sharedCase(context.regtest, 'operators/aggregate');
  const cacheDir = mkdtempSync(path.join(tmpdir(), 'qdb-wasm-cache-'));
  try {
    const args = ['--cache', cacheDir];
    const first = await exportCase(test, context.qdb, { args });
    const objects = countObjectFiles(cacheDir);
    assert.ok(objects > 0, 'wasm cache did not persist any objects');
    const second = await exportCase(test, context.qdb, { args });
    assert.equal(countObjectFiles(cacheDir), objects,
      'second wasm compilation added objects instead of reusing the cache');
    await checkCase(test, first, context.runtime);
    await checkCase(test, second, context.runtime);
  } finally {
    rmSync(cacheDir, { recursive: true, force: true });
  }
}

export function tests(context) {
  return [{ name: 'wasm-cache/reuse', run: () => reuse(context) }];
}
