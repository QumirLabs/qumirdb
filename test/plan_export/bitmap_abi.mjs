// Boolean columns cross the JS/WASM boundary as bitmaps. The shared runner
// passes plain arrays; these tests feed the same fixture as typed arrays and
// through the chunked source batch builder, whose chunk ends do not fall on
// byte boundaries.

import { browserExec, caseTables, checkResult, exportCase, sharedCase } from '../regtest_lib.mjs';

const CASES = ['boolean_columns/project_values', 'boolean_columns/filter_projection'];
const CHUNK_ENDS = [3, 10, 66];

// The column arrays a Parquet reader produces: typed arrays where a column has
// no NULL.
function typedColumn(column, values) {
  if (values.includes(null)) {
    return values;
  }
  if (column.type === 'bool') {
    return Uint8Array.from(values, Number);
  }
  if (column.type === 'i64') {
    return BigInt64Array.from(values);
  }
  return values;
}

async function check(context, base, chunked) {
  const test = sharedCase(context.regtest, base);
  const exported = await exportCase(test, context.qdb);
  const exec = browserExec(exported.bundle);
  const { t: table } = caseTables(test);
  const values = Object.fromEntries(table.columns.map(column =>
    [column.name, typedColumn(column, table.values[column.name])]));
  const rowCount = table.rowCount;
  const { WasmSourceBatchBuilder } = context.runtime;
  const result = await context.runtime.executeBrowserPipelineScheduled(exec,
    async function* (stage, onProgress, source) {
      if (!chunked) {
        yield {
          rowCount,
          columns: stage.columns.map(column => ({ ...column, values: values[column.name] })),
        };
        return;
      }
      const builder = new WasmSourceBatchBuilder(
        source.shared.arena, source.layout, stage.columns, rowCount);
      for (const column of stage.columns) {
        let start = 0;
        for (const end of [...CHUNK_ENDS, rowCount]) {
          builder.writeChunk(column.name, values[column.name].slice(start, end), start, end);
          start = end;
        }
      }
      yield builder.finish();
    });
  checkResult(test, exec, result);
}

export function tests(context) {
  return CASES.flatMap(base => [
    { name: `bitmap-abi/${base}/typed-arrays`, run: () => check(context, base, false) },
    { name: `bitmap-abi/${base}/chunked-builder`, run: () => check(context, base, true) },
  ]);
}
