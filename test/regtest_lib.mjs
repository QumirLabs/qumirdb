// Shared regression cases for the browser runtime: suite manifests, JSON table
// fixtures and typed .result.json goldens from test/regtest, executed through
// `qdb --plan-export` and the WASM pipeline in service/static. The native
// test_reg owns manifest validation and canonization; this module only reads.

import { spawn } from 'node:child_process';
import { existsSync, readFileSync, readdirSync } from 'node:fs';
import path from 'node:path';

import { TestFailure } from './js_harness.mjs';

const JS_VARIANT = 'js';
const DEFAULT_ROW_GROUP_ROWS = 128;
const DEFAULT_TOLERANCE = Object.freeze({ abs: 1e-12, rel: 1e-12 });
const INTEGER_TYPES = new Set(['i8', 'i16', 'i32', 'u8', 'u16', 'u32']);
const WIDE_INTEGER_TYPES = new Set(['i64', 'u64']);
const SCALAR_TYPES = new Set([...INTEGER_TYPES, ...WIDE_INTEGER_TYPES, 'f64', 'bool', 'string']);

function readJson(file) {
  try {
    return JSON.parse(readFileSync(file, 'utf8'));
  } catch (error) {
    throw new Error(`${file}: ${error.message}`);
  }
}

function findManifests(dir) {
  const manifests = [];
  for (const entry of readdirSync(dir, { withFileTypes: true })) {
    const entryPath = path.join(dir, entry.name);
    if (entry.isDirectory()) {
      manifests.push(...findManifests(entryPath));
    } else if (entry.name === 'suite.json') {
      manifests.push(entryPath);
    }
  }
  return manifests.sort();
}

// The identifier rule of the Qumir core printer.
function printIdentifier(name) {
  return name.includes(' ') || name.includes('.') ? `|${name}|` : name;
}

function printColumnType({ type, nullable }) {
  return nullable ? `<named Nullable [${type}]>` : type;
}

function printSchema(columns) {
  const fields = columns.map(column =>
    ` (${printIdentifier(column.name)} ${printColumnType(column)})`);
  return `<struct${fields.join('')}>`;
}

function scalarColumn(name, typeText) {
  const nullable = typeText.match(/^<named Nullable \[(.*)\]>$/);
  const type = nullable ? nullable[1].trim() : typeText;
  if (!SCALAR_TYPES.has(type)) {
    throw new Error(`column ${name}: unsupported type ${typeText}`);
  }
  return { name, type, nullable: Boolean(nullable) };
}

// Fields of a `<struct (name type) ...>` schema. Fixture and result types are
// scalars, optionally wrapped in Nullable, as in the native fixture reader.
function parseSchema(text) {
  const source = String(text).trim();
  if (!source.startsWith('<struct') || !source.endsWith('>')) {
    throw new Error(`schema must be a Qumir struct type: ${source}`);
  }
  const body = source.slice('<struct'.length, -1);
  const columns = [];
  let pos = 0;
  const skipSpaces = () => {
    while (pos < body.length && /\s/.test(body[pos])) ++pos;
  };
  for (skipSpaces(); pos < body.length; skipSpaces()) {
    if (body[pos] !== '(') {
      throw new Error(`invalid schema field at ${pos}: ${source}`);
    }
    ++pos;
    skipSpaces();
    let name;
    if (body[pos] === '|') {
      const end = body.indexOf('|', pos + 1);
      if (end < 0) throw new Error(`unterminated field name: ${source}`);
      name = body.slice(pos + 1, end);
      pos = end + 1;
    } else {
      const start = pos;
      while (pos < body.length && !/[\s()]/.test(body[pos])) ++pos;
      name = body.slice(start, pos);
    }
    skipSpaces();
    const start = pos;
    let depth = 0;
    while (pos < body.length && (depth > 0 || body[pos] !== ')')) {
      if ('<[('.includes(body[pos])) ++depth;
      if ('>])'.includes(body[pos])) --depth;
      ++pos;
    }
    if (!name || pos >= body.length) {
      throw new Error(`invalid schema field: ${source}`);
    }
    columns.push(scalarColumn(name, body.slice(start, pos).trim()));
    ++pos;
  }
  return columns;
}

function floatValue(value) {
  if (typeof value === 'number') return value;
  switch (value?.$float) {
    case 'nan': return NaN;
    case '+inf': return Infinity;
    case '-inf': return -Infinity;
    case '-0': return -0;
    default: throw new Error(`invalid f64 value ${JSON.stringify(value)}`);
  }
}

// Fixture JSON cells as the browser source reads them: 64-bit integers are
// strings in JSON and BigInt in the runtime.
function sourceValue(column, value) {
  if (value === null) {
    if (!column.nullable) throw new Error(`NULL in non-nullable column ${column.name}`);
    return null;
  }
  if (WIDE_INTEGER_TYPES.has(column.type)) return BigInt(value);
  if (INTEGER_TYPES.has(column.type)) return Number(value);
  if (column.type === 'f64') return floatValue(value);
  if (column.type === 'bool') {
    if (typeof value !== 'boolean') throw new Error(`invalid bool value in ${column.name}`);
    return value;
  }
  return String(value);
}

// Exact NDV per column, as the native fixture writer records by default.
// Histograms only shape plans, which this runner does not check.
function computedColumnStats(columns, values) {
  return columns.map(column => {
    const present = values[column.name].filter(value => value !== null);
    if (column.type === 'f64' && present.some(Number.isNaN)) {
      return { name: column.name };
    }
    const distinct = new Set(present.map(value =>
      Object.is(value, -0) ? 0 : value));
    return { name: column.name, ndv: distinct.size, ndv_exact: true };
  });
}

function loadFixture(file, rowGroupRows) {
  const json = readJson(file);
  try {
    if (json.version !== 1) throw new Error('expected version 1');
    const columns = parseSchema(json.schema);
    const values = Object.fromEntries(columns.map(column => [column.name, []]));
    json.rows.forEach((row, index) => {
      if (row.length !== columns.length) {
        throw new Error(`row ${index}: wrong column count`);
      }
      columns.forEach((column, i) => {
        values[column.name].push(sourceValue(column, row[i]));
      });
    });
    const stats = json.stats ?? 'compute';
    const columnStats = stats === 'compute'
      ? computedColumnStats(columns, values)
      : stats === 'footer' ? [] : stats.columns;
    return { columns, values, rowCount: json.rows.length, rowGroupRows, columnStats };
  } catch (error) {
    throw new Error(`${file}: ${error.message}`);
  }
}

// Mirrors NRegTest::TestName so that both runners name a case alike.
function testName(base) {
  return base.replace(/[^A-Za-z0-9_]/g, '_');
}

class Suite {
  constructor(manifestPath, json) {
    this.manifestPath = manifestPath;
    this.dir = path.dirname(manifestPath);
    this.fixtureDefinitions = json.fixtures;
    this.fixtures = new Map();
  }

  fixture(name) {
    if (!this.fixtures.has(name)) {
      const definition = this.fixtureDefinitions[name];
      if (!definition?.json) {
        throw new Error(`${this.manifestPath}: fixture '${name}' has no JSON table`);
      }
      this.fixtures.set(name, loadFixture(
        path.join(this.dir, definition.json),
        definition.parquet?.row_group_rows ?? DEFAULT_ROW_GROUP_ROWS));
    }
    return this.fixtures.get(name);
  }
}

// A boolean option, as the native runner reads it: other JSON values are rejected.
function booleanOption(where, object, name, fallback) {
  const value = object[name] ?? fallback;
  if (typeof value !== 'boolean') throw new Error(`${where}: "${name}" must be a boolean`);
  return value;
}

// Cases whose suite (or case) variants list "js" and that execute against
// tables. Executed cases without js are returned as skipped so that the runner
// reports them; plan-only cases have no result golden to check.
export function collectCases(root) {
  const casesDir = path.resolve(root, 'cases');
  const goldensDir = path.resolve(root, 'goldens');
  const cases = [];
  const skipped = [];
  for (const manifestPath of findManifests(casesDir)) {
    const json = readJson(manifestPath);
    const suiteVariants = json.variants ?? [];
    const suite = new Suite(manifestPath, json);
    const suiteExpectError = booleanOption(manifestPath, json, 'expect_error', false);
    for (const [sqlFile, options] of Object.entries(json.cases)) {
      const expectError = booleanOption(`${manifestPath}: ${sqlFile}`, options, 'expect_error', suiteExpectError);
      const variants = options.variants ?? suiteVariants;
      const sql = path.join(suite.dir, sqlFile);
      const relative = path.relative(casesDir, sql);
      const base = relative.replace(/\.sql$/, '');
      const tables = { ...json.tables, ...(options.tables ?? {}) };
      const execute = options.execute ?? json.execute ?? true;
      if (!execute || Object.keys(tables).length === 0) {
        continue;
      }
      if (!variants.includes(JS_VARIANT)) {
        skipped.push({
          name: testName(base),
          base,
          reason: `${options.variants ? 'case' : 'suite'} variants `
            + `[${variants.join(', ')}] exclude ${JS_VARIANT}`,
        });
        continue;
      }
      cases.push({
        name: testName(base),
        base,
        sql,
        golden: path.join(goldensDir, `${base}.result.json`),
        planGolden: path.join(goldensDir, `${base}.plan`),
        errorGolden: path.join(goldensDir, `${base}.error`),
        expectError,
        suite,
        tables,
        ordered: (options.order ?? 'ordered') === 'ordered',
        tol: { ...DEFAULT_TOLERANCE, ...(json.tol ?? {}), ...(options.tol ?? {}) },
      });
    }
  }
  // A deterministic report order, independent of the locale.
  const byBase = (left, right) => (left.base < right.base ? -1 : left.base > right.base ? 1 : 0);
  return { cases: cases.sort(byBase), skipped: skipped.sort(byBase) };
}

// The JSON table fixtures of a case, keyed by SQL table name.
export function caseTables(test) {
  return Object.fromEntries(Object.entries(test.tables).map(
    ([table, fixture]) => [table, test.suite.fixture(fixture)]));
}

function datasetFor(tables) {
  return {
    tables: Object.entries(tables).map(([name, table]) => ({
      name,
      columns: table.columns.map(column => ({
        name: column.name,
        type: printColumnType(column),
      })),
      stats: {
        rows: table.rowCount,
        rowGroups: Math.max(1, Math.ceil(table.rowCount / table.rowGroupRows)),
        columns: table.columnStats,
      },
    })),
  };
}

// Exporter options of the native scheduler presets; the browser executes
// either plan on one thread.
export const SCHEDULER_PRESETS = Object.freeze({
  single: { scheduler: 'single', schedulerWorkers: 1, scanTasks: 1, shufflePartitions: 1 },
  threaded: { scheduler: 'threaded', schedulerWorkers: 4, scanTasks: 4, shufflePartitions: 2 },
});

export function exportRequest(sql, tables, options = {}) {
  return {
    sql,
    dataset: datasetFor(tables),
    options: {
      scheduler: 'single',
      scanTasks: 1,
      shufflePartitions: 1,
      format: 'runtime-bundle',
      embedWasm: true,
      ...options,
    },
  };
}

export function runExporter(qdb, request, extraArgs = []) {
  return new Promise((resolve, reject) => {
    const child = spawn(qdb, ['--plan-export', '--stdin-json', '--stdout-json', ...extraArgs]);
    const stdout = [];
    const stderr = [];
    child.stdout.on('data', chunk => stdout.push(chunk));
    child.stderr.on('data', chunk => stderr.push(chunk));
    child.on('error', reject);
    child.on('close', status => {
      if (status !== 0) {
        reject(new Error(`${qdb} --plan-export failed (${status}):\n${Buffer.concat(stderr)}`));
        return;
      }
      try {
        resolve(JSON.parse(Buffer.concat(stdout).toString('utf8')));
      } catch (error) {
        reject(error);
      }
    });
    child.stdin.end(JSON.stringify(request));
  });
}

// The executable plan of a bundle, or an error naming why the exporter cannot
// run the query in the browser.
export function browserExec(bundle) {
  if (bundle.ok !== true) {
    throw new TestFailure(`exporter rejected the query: ${bundle.error?.message ?? 'unknown error'}`);
  }
  if (bundle.exec?.supported !== true) {
    throw new TestFailure(`browser runtime does not support the plan: ${bundle.exec?.reason ?? 'unknown reason'}`);
  }
  const artifact = bundle.exec.wasm;
  if (!artifact) {
    return bundle.exec;
  }
  const wasm = bundle.artifacts?.[artifact]?.data;
  if (!wasm) throw new Error(`exec artifact is missing: ${artifact}`);
  return { ...bundle.exec, wasm };
}

// Source batches cut at Parquet row-group boundaries, as the native fixture
// file is split.
export function fixtureSource(tables) {
  return async function* (stage) {
    const table = tables[stage.table];
    if (!table) throw new Error(`browser requested unknown table ${stage.table}`);
    for (let begin = 0; begin < table.rowCount;) {
      const end = Math.min(table.rowCount, begin + table.rowGroupRows);
      yield {
        rowCount: end - begin,
        columns: stage.columns.map(column => {
          const values = table.values[column.name];
          if (!values) {
            throw new Error(`browser requested unknown column ${stage.table}.${column.name}`);
          }
          return { ...column, values: values.slice(begin, end) };
        }),
      };
      begin = end;
    }
  };
}

function floatJson(value) {
  if (Number.isNaN(value)) return { $float: 'nan' };
  if (value === Infinity) return { $float: '+inf' };
  if (value === -Infinity) return { $float: '-inf' };
  if (Object.is(value, -0)) return { $float: '-0' };
  return value;
}

function resultCell(column, value) {
  if (value === null || value === undefined) return null;
  if (WIDE_INTEGER_TYPES.has(column.type)) return String(value);
  if (INTEGER_TYPES.has(column.type)) return Number(value);
  if (column.type === 'f64') return floatJson(Number(value));
  if (column.type === 'bool') {
    if (value === true || value === 1) return true;
    if (value === false || value === 0) return false;
    throw new Error(`invalid bool result value ${String(value)} in ${column.name}`);
  }
  return String(value);
}

// The browser result in the typed .result.json form, schema taken from the
// exported plan so that an empty result is typed too.
export function typedResult(exec, result) {
  const columns = exec.output.map(column => {
    if (!SCALAR_TYPES.has(column.type)) {
      throw new Error(`result column ${column.name}: unsupported type ${column.type}`);
    }
    return { name: column.name, type: column.type, nullable: column.nullable === true };
  });
  const names = JSON.stringify(columns.map(column => column.name));
  if (result.columns.length > 0 && JSON.stringify(result.columns) !== names) {
    throw new Error(`result columns ${JSON.stringify(result.columns)} differ from `
      + `the plan output ${names}`);
  }
  return {
    schema: printSchema(columns),
    rows: result.rows.map(row => row.map((value, i) => resultCell(columns[i], value))),
  };
}

const cellKey = cell => JSON.stringify(cell);
const rowKey = row => JSON.stringify(row);

// Canonical schema text and float cells; unordered rows in a fixed order.
function normalize(result, ordered) {
  const columns = parseSchema(result.schema);
  const rows = result.rows.map((row, index) => {
    if (!Array.isArray(row) || row.length !== columns.length) {
      throw new Error(`result row ${index}: wrong width`);
    }
    return row.map((cell, i) =>
      cell !== null && columns[i].type === 'f64' ? floatJson(floatValue(cell)) : cell);
  });
  if (!ordered) {
    rows.sort((left, right) => (rowKey(left) < rowKey(right) ? -1 : rowKey(left) > rowKey(right) ? 1 : 0));
  }
  return { schema: printSchema(columns), columns, rows };
}

// NRegTest's Close(): absolute or relative tolerance without overflow.
function close(actual, expected, tol) {
  const difference = Math.abs(actual - expected);
  if (difference <= tol.abs) return true;
  const scale = Math.max(Math.abs(actual), Math.abs(expected));
  const scaledDifference = Number.isFinite(difference)
    ? difference / scale
    : Math.abs(actual / scale - expected / scale);
  return scaledDifference <= tol.abs / scale + tol.rel;
}

function resultsMatch(actual, expected, ordered, tol) {
  if (actual.schema !== expected.schema || actual.rows.length !== expected.rows.length) {
    return false;
  }
  const floats = actual.columns.map(column => column.type === 'f64');
  const rowMatches = (left, right) => left.every((cell, i) => {
    if (cellKey(cell) === cellKey(right[i])) return true;
    // NULL and $float tags (NaN, infinities, signed zero) stay exact.
    return floats[i] && typeof cell === 'number' && typeof right[i] === 'number'
      && close(cell, right[i], tol);
  });
  if (ordered || !floats.includes(true)) {
    return actual.rows.every((row, i) => rowMatches(row, expected.rows[i]));
  }
  // Approximate equality is not transitive, so unordered rows need a complete
  // one-to-one matching (augmenting paths), as in the native comparison.
  const count = actual.rows.length;
  const candidates = actual.rows.map(row =>
    expected.rows.flatMap((other, index) => (rowMatches(row, other) ? [index] : [])));
  if (candidates.some(list => list.length === 0)) return false;
  const matched = new Array(count).fill(-1);
  const augment = (row, visited) => {
    for (const other of candidates[row]) {
      if (visited[other]) continue;
      visited[other] = true;
      if (matched[other] < 0 || augment(matched[other], visited)) {
        matched[other] = row;
        return true;
      }
    }
    return false;
  };
  return actual.rows.every((_, row) => augment(row, new Array(count).fill(false)));
}

// The golden layout: one result row per line.
function renderResult(result) {
  const rows = result.rows.map(row => `    [${row.map(cell => JSON.stringify(cell)).join(', ')}]`);
  return `{\n  "schema": ${JSON.stringify(result.schema)},\n  "rows": [`
    + (rows.length ? `\n${rows.join(',\n')}\n  ` : '')
    + ']\n}\n';
}

// A line diff (longest common subsequence) for failure reports.
function lineDiff(expectedText, actualText) {
  const expected = expectedText.split('\n');
  const actual = actualText.split('\n');
  const lengths = Array.from({ length: expected.length + 1 }, () =>
    new Array(actual.length + 1).fill(0));
  for (let i = expected.length - 1; i >= 0; --i) {
    for (let j = actual.length - 1; j >= 0; --j) {
      lengths[i][j] = expected[i] === actual[j]
        ? lengths[i + 1][j + 1] + 1
        : Math.max(lengths[i + 1][j], lengths[i][j + 1]);
    }
  }
  const lines = ['--- expected', '+++ actual'];
  let i = 0;
  let j = 0;
  while (i < expected.length || j < actual.length) {
    if (i < expected.length && j < actual.length && expected[i] === actual[j]) {
      lines.push(`  ${expected[i++]}`);
      ++j;
    } else if (i < expected.length && (j === actual.length || lengths[i + 1][j] >= lengths[i][j + 1])) {
      lines.push(`- ${expected[i++]}`);
    } else {
      lines.push(`+ ${actual[j++]}`);
    }
  }
  return lines.join('\n');
}

// Typed results compared like the native runner: exact cells except finite
// floats within the tolerance, rows in order or as a multiset.
export function compareResults(actualResult, expectedResult, {
  ordered = true,
  tol = DEFAULT_TOLERANCE,
  expectedName = 'the expected result',
} = {}) {
  const actual = normalize(actualResult, ordered);
  const expected = normalize(expectedResult, ordered);
  if (resultsMatch(actual, expected, ordered, tol)) return;
  throw new TestFailure(`result differs from ${expectedName} `
    + `(${ordered ? 'ordered' : 'unordered'}, tol abs=${tol.abs} rel=${tol.rel})\n`
    + lineDiff(renderResult(expected), renderResult(actual)));
}

// The exported bundle of a shared case. Export runs in a child process, so a
// runner may prepare several cases while it executes another.
export async function exportCase(test, qdb, { args = [], options = {} } = {}) {
  const tables = caseTables(test);
  const sql = readFileSync(test.sql, 'utf8');
  return { tables, bundle: await runExporter(qdb, exportRequest(sql, tables, options), args) };
}

// A browser result of a shared case against its golden, and the WASM memory
// it must have released.
export function checkResult(test, exec, result) {
  if (result.memory?.liveMB !== '0.0') {
    throw new TestFailure(`browser runtime leaked wasm memory: ${result.memory?.liveMB} MB live`);
  }
  compareResults(typedResult(exec, result), readJson(test.golden), {
    ordered: test.ordered,
    tol: test.tol,
    expectedName: test.golden,
  });
}

// The golden text of an error, as the native test_reg renders it: a TError
// already reads `Error: <message> [@ Line: L, Byte: B, Column: C]`; the exporter
// reports any other exception by its bare message, which the native runner
// marks as `Runtime error: <message>`.
function errorText(message) {
  const text = String(message);
  return `${text.startsWith('Error: ') ? text : `Runtime error: ${text}`}\n`;
}

function readGolden(goldenFile, actual) {
  if (!existsSync(goldenFile)) {
    throw new TestFailure(`missing golden ${goldenFile} for the error\n${actual}`);
  }
  return readFileSync(goldenFile, 'utf8');
}

function compareText(actual, goldenFile) {
  const expected = readGolden(goldenFile, actual);
  if (actual !== expected) {
    throw new TestFailure(`error differs from ${goldenFile}\n${lineDiff(expected, actual)}`);
  }
}

// A case that must fail. The stage that failed decides the golden, as in the
// native runner: a planning error of the exporter is compared with the `.plan`
// golden; an error that the exporter reports for lowering, and an error of the
// run in the browser, with the `.error` golden. A run that succeeds fails the case.
// The WASM scheduler wraps the message of a failed task as
// `<label>: <message>`, plus an optional ` (wasm reserved ...MB, peak live ...MB)`
// suffix. The golden `Runtime error: <message>` is compared exactly, as in the
// native runner, once that wrapper is removed. The label is a free-form stage
// name, so it is matched as whatever precedes the last `: <message>`.
const wasmErrorSuffix = / \(wasm reserved [\d.]+MB, peak live [\d.]+MB\)$/;
const runtimeErrorPrefix = 'Runtime error: ';

function matchesRuntimeError(message, golden) {
  if (!golden.startsWith(runtimeErrorPrefix)) return false;
  const expected = golden.slice(runtimeErrorPrefix.length).trimEnd();
  if (expected.length === 0) return false;
  return message.replace(wasmErrorSuffix, '').endsWith(`: ${expected}`);
}

async function checkExpectedError(test, { tables, bundle }, runtime, runOptions) {
  if (bundle.ok !== true) {
    compareText(errorText(bundle.error?.message), test.planGolden);
    return null;
  }
  if (bundle.exec?.supported !== true) {
    compareText(errorText(bundle.exec?.reason), test.errorGolden);
    return null;
  }
  const exec = browserExec(bundle);
  try {
    await runtime.executeBrowserPipelineScheduled(exec, fixtureSource(tables), runOptions);
  } catch (error) {
    const message = String(error?.message);
    const golden = readGolden(test.errorGolden, errorText(message));
    if (!matchesRuntimeError(message, golden)) {
      throw new TestFailure(`error differs from ${test.errorGolden}\n${lineDiff(golden, errorText(message))}`);
    }
    return null;
  }
  throw new TestFailure(`expected an error (${test.errorGolden}) but the browser run succeeded`);
}

// Executes an exported case with its fixture rows and checks the result.
export async function checkCase(test, exported, runtime, runOptions = {}) {
  if (test.expectError) {
    return checkExpectedError(test, exported, runtime, runOptions);
  }
  const { tables, bundle } = exported;
  const exec = browserExec(bundle);
  const result = await runtime.executeBrowserPipelineScheduled(
    exec, fixtureSource(tables), runOptions);
  checkResult(test, exec, result);
  return result;
}

const collected = new Map();

// A js case of the shared suites by its path below cases/, without extension.
export function sharedCase(root, base) {
  if (!collected.has(root)) {
    collected.set(root, collectCases(root).cases);
  }
  const test = collected.get(root).find(item => item.base === base);
  if (!test) throw new Error(`no js case ${base} under ${root}`);
  return test;
}
