import { WasmSourceBatchBuilder } from './browser_runtime.js';
import { Type, printType } from './oz_type.js';

const HYPARQUET_URL = 'https://cdn.jsdelivr.net/npm/hyparquet@latest/+esm';
const HYPARQUET_COMPRESSORS_URL =
  'https://cdn.jsdelivr.net/npm/hyparquet-compressors@latest/+esm';

let hyparquetPromise = null;
let compressorsPromise = null;

async function loadHyparquet() {
  if (!hyparquetPromise) {
    hyparquetPromise = import(HYPARQUET_URL);
  }
  return hyparquetPromise;
}

async function loadCompressors() {
  if (!compressorsPromise) {
    compressorsPromise = import(HYPARQUET_COMPRESSORS_URL)
      .then(module => module.compressors)
      .catch(() => undefined);
  }
  return compressorsPromise;
}

function asyncBufferFromBlob(file) {
  return {
    byteLength: file.size,
    async slice(start, end) {
      return file.slice(start, end).arrayBuffer();
    }
  };
}

async function readMetadata(file) {
  const parquet = await loadHyparquet();
  if (parquet.parquetMetadataAsync) {
    const asyncBuffer = parquet.asyncBufferFromFile
      ? await parquet.asyncBufferFromFile(file)
      : asyncBufferFromBlob(file);
    return parquet.parquetMetadataAsync(asyncBuffer);
  }
  if (parquet.parquetMetadata) {
    return parquet.parquetMetadata(await file.arrayBuffer());
  }
  throw new Error('hyparquet metadata API is not available');
}

function tableNameFromFile(fileName) {
  return fileName
    .replace(/[.]parquet$/i, '')
    .replace(/[^A-Za-z0-9_]+/g, '_')
    .replace(/^_+|_+$/g, '') || 'table';
}

function parquetType(field) {
  const physical = String(parquetPhysicalType(field.type ?? field.physicalType)).toUpperCase();
  const converted = String(parquetConvertedType(
    field.converted_type ?? field.convertedType)).toUpperCase();
  const logical = JSON.stringify(field.logicalType || field.logical_type || {}).toUpperCase();

  if (converted.includes('DECIMAL') || logical.includes('DECIMAL')) {
    const precision = Number(field.precision ?? field.type_precision ?? field.typePrecision ?? 18);
    const scale = Number(field.scale ?? field.type_scale ?? field.typeScale ?? 0);
    return Type.named('Decimal', [precision || 18, scale || 0]);
  }
  if (converted.includes('UTF8') || logical.includes('STRING')) {
    return Type.scalar('string');
  }
  if (logical.includes('DATE')) {
    return Type.scalar('i32');
  }
  if (logical.includes('TIMESTAMP')) {
    return Type.scalar('i64');
  }
  if (physical === 'BOOLEAN') {
    return Type.scalar('bool');
  }
  if (physical === 'INT32') {
    return Type.scalar('i32');
  }
  if (physical === 'INT64') {
    return Type.scalar('i64');
  }
  if (physical === 'FLOAT') {
    return Type.scalar('f32');
  }
  if (physical === 'DOUBLE') {
    return Type.scalar('f64');
  }
  if (physical === 'BYTE_ARRAY' || physical === 'FIXED_LEN_BYTE_ARRAY') {
    return Type.scalar('string');
  }
  return Type.scalar('string');
}

function parquetNullable(field) {
  const repetition = field.repetition_type ?? field.repetitionType;
  if (typeof repetition === 'number') {
    return repetition === 1;
  }
  return String(repetition || '').toUpperCase() === 'OPTIONAL';
}

function parquetPhysicalType(type) {
  if (typeof type !== 'number') {
    return type || '';
  }
  return [
    'BOOLEAN',
    'INT32',
    'INT64',
    'INT96',
    'FLOAT',
    'DOUBLE',
    'BYTE_ARRAY',
    'FIXED_LEN_BYTE_ARRAY'
  ][type] || '';
}

function parquetConvertedType(type) {
  if (typeof type !== 'number') {
    return type || '';
  }
  return [
    'UTF8',
    'MAP',
    'MAP_KEY_VALUE',
    'LIST',
    'ENUM',
    'DECIMAL',
    'DATE',
    'TIME_MILLIS',
    'TIME_MICROS',
    'TIMESTAMP_MILLIS',
    'TIMESTAMP_MICROS',
    'UINT_8',
    'UINT_16',
    'UINT_32',
    'UINT_64',
    'INT_8',
    'INT_16',
    'INT_32',
    'INT_64',
    'JSON',
    'BSON',
    'INTERVAL'
  ][type] || '';
}

function schemaFields(metadata) {
  const schema = metadata.schema || metadata.schemaElements || metadata.schema_elements || [];
  if (!Array.isArray(schema)) {
    throw new Error('parquet schema is not an array');
  }
  return schema
    .filter(field => field?.name)
    .filter(field => {
      const children = field.num_children ?? field.numChildren ?? 0;
      return children === 0;
    })
    .filter((field, index) => index > 0 || field.type || field.physicalType)
    .map(field => {
      let type = parquetType(field);
      if (parquetNullable(field)) {
        type = Type.named('Nullable', [type]);
      }
      return {
        name: field.name,
        type: printType(type)
      };
    });
}

function rowGroupStats(metadata) {
  const rowGroups = metadata.row_groups || metadata.rowGroups || [];
  const rows = Number(metadata.num_rows ?? metadata.numRows ?? 0);
  const bytes = rowGroups.reduce((sum, group) => {
    return sum + Number(group.total_byte_size ?? group.totalByteSize ?? 0);
  }, 0);
  const stats = {
    rows,
    rowGroups: rowGroups.length || 1,
    bytes
  };
  const columns = customColumnStats(metadata);
  if (columns.length) {
    stats.columns = columns;
  }
  return stats;
}

function metadataEntries(metadata) {
  const candidates = [
    metadata.key_value_metadata,
    metadata.keyValueMetadata,
    metadata.keyValueMetadata?.key_value,
    metadata.fileMetaData?.key_value_metadata,
    metadata.fileMetaData?.keyValueMetadata,
  ].filter(Boolean);

  for (const value of candidates) {
    if (Array.isArray(value)) {
      return value;
    }
    if (value instanceof Map) {
      return Array.from(value.entries()).map(([key, entryValue]) => ({ key, value: entryValue }));
    }
    if (typeof value === 'object') {
      return Object.entries(value).map(([key, entryValue]) => ({ key, value: entryValue }));
    }
  }
  return [];
}

function decodeMetadataValue(value) {
  if (typeof value === 'string') {
    return value;
  }
  if (value instanceof Uint8Array) {
    return new TextDecoder().decode(value);
  }
  if (value instanceof ArrayBuffer) {
    return new TextDecoder().decode(new Uint8Array(value));
  }
  if (ArrayBuffer.isView(value)) {
    return new TextDecoder().decode(new Uint8Array(
      value.buffer, value.byteOffset, value.byteLength));
  }
  if (value && typeof value === 'object') {
    return decodeMetadataValue(value.value ?? value.bytes ?? value.data);
  }
  return null;
}

function metadataValue(metadata, key) {
  for (const entry of metadataEntries(metadata)) {
    if (Array.isArray(entry) && entry[0] === key) {
      return decodeMetadataValue(entry[1]);
    }
    const entryKey = entry.key ?? entry.Key ?? entry.name;
    if (entryKey !== key) {
      continue;
    }
    return decodeMetadataValue(entry.value ?? entry.Value);
  }
  return null;
}

function copyJsonStatsValue(out, input, outputName, ...inputNames) {
  for (const name of inputNames) {
    if (!Object.prototype.hasOwnProperty.call(input, name)) {
      continue;
    }
    const value = input[name];
    if (typeof value === 'number' || typeof value === 'string' || typeof value === 'boolean') {
      out[outputName] = value;
      return;
    }
  }
}

function normalizeColumnStats(column) {
  if (!column || typeof column !== 'object' || typeof column.name !== 'string') {
    return null;
  }
  const out = { name: column.name };
  copyJsonStatsValue(out, column, 'ndv', 'ndv');
  copyJsonStatsValue(out, column, 'ndv_exact', 'ndv_exact', 'ndvExact');
  copyJsonStatsValue(out, column, 'null_count', 'null_count', 'nullCount');
  copyJsonStatsValue(out, column, 'min', 'min');
  copyJsonStatsValue(out, column, 'max', 'max');
  if (Array.isArray(column.histogram)) {
    out.histogram = column.histogram.filter(value =>
      typeof value === 'number' || typeof value === 'string' || typeof value === 'boolean');
  }
  return Object.keys(out).length > 1 ? out : null;
}

function customColumnStats(metadata) {
  const rawStats = metadataValue(metadata, 'stats');
  if (!rawStats) {
    return [];
  }
  try {
    const parsed = JSON.parse(rawStats);
    const columns = Array.isArray(parsed?.columns) ? parsed.columns : [];
    return columns.map(normalizeColumnStats).filter(Boolean);
  } catch {
    return [];
  }
}

export async function readParquetTable(file) {
  const metadata = await readMetadata(file);
  return {
    name: tableNameFromFile(file.name),
    columns: schemaFields(metadata),
    stats: rowGroupStats(metadata)
  };
}

// Reads the requested columns as raw column arrays keyed by column name.
// Numeric chunks stay as TypedArrays when hyparquet provides them.
export async function readParquetColumns(file, requestedColumns) {
  const specs = requestedColumns.map(column =>
    typeof column === 'string' ? { name: column } : column);
  const columnNames = specs.map(column => column.name);
  return readParquetColumnsRange(file, specs, columnNames, null);
}

export async function* readParquetColumnBatches(file, requestedColumns, options = {}) {
  const specs = requestedColumns.map(column =>
    typeof column === 'string' ? { name: column } : column);
  const columnNames = specs.map(column => column.name);
  const metadata = await readMetadata(file);
  const ranges = rowGroupRanges(metadata);
  const totalRows = Number(metadata.num_rows ?? metadata.numRows ?? 0);
  const totalUnits = Math.max(totalRows * Math.max(columnNames.length, 1), 1);
  let completedUnits = 0;

  for (let index = 0; index < ranges.length; ++index) {
    const range = ranges[index];
    let rangeUnits = 0;
    const batch = await readParquetColumnsRange(file, specs, columnNames, range, progress => {
      rangeUnits += progress.units;
      if (options.onProgress) {
        options.onProgress({
          phase: 'read',
          rowGroup: index,
          rowGroupCount: ranges.length,
          rows: range.end,
          totalRows,
          completedUnits: completedUnits + rangeUnits,
          totalUnits,
        });
      }
    });
    completedUnits += (range.end - range.start) * Math.max(columnNames.length, 1);
    yield batch;
  }
}

export async function* readParquetWasmColumnBatches(file, requestedColumns, options = {}) {
  if (!options.arena || !options.layout) {
    throw new Error('direct parquet source needs shared wasm memory');
  }

  const specs = requestedColumns.map(column =>
    typeof column === 'string' ? { name: column } : column);
  const columnNames = specs.map(column => column.name);
  const metadata = await readMetadata(file);
  const ranges = rowGroupRanges(metadata);
  const totalRows = Number(metadata.num_rows ?? metadata.numRows ?? 0);
  const totalUnits = Math.max(totalRows * Math.max(columnNames.length, 1), 1);
  let completedUnits = 0;

  for (let index = 0; index < ranges.length; ++index) {
    const range = ranges[index];
    let rangeUnits = 0;
    const batch = await readParquetWasmColumnsRange(
      file,
      specs,
      columnNames,
      range,
      options,
      progress => {
        rangeUnits += progress.units;
        if (options.onProgress) {
          options.onProgress({
            phase: 'read',
            rowGroup: index,
            rowGroupCount: ranges.length,
            rows: range.end,
            totalRows,
            completedUnits: completedUnits + rangeUnits,
            totalUnits,
          });
        }
      });
    completedUnits += (range.end - range.start) * Math.max(columnNames.length, 1);
    yield batch;
  }
}

async function readParquetWasmColumnsRange(
    file, specs, columnNames, range, options, onProgress = null) {
  let rowCount = range ? range.end - range.start : null;
  if (rowCount == null) {
    const metadata = await readMetadata(file);
    rowCount = Number(metadata.num_rows ?? metadata.numRows ?? 0);
  }
  const builder = new WasmSourceBatchBuilder(
    options.arena,
    options.layout,
    specs,
    rowCount);
  if (columnNames.length === 0) {
    return builder.finish();
  }

  const parquet = await loadHyparquet();
  const asyncBuffer = parquet.asyncBufferFromFile
    ? await parquet.asyncBufferFromFile(file)
    : asyncBufferFromBlob(file);
  const compressors = await loadCompressors();

  if (typeof parquet.parquetRead !== 'function') {
    throw new Error('hyparquet parquetRead API is not available');
  }

  const columnSet = new Set(columnNames.map(name => String(name)));
  const readOptions = {
    file: asyncBuffer,
    columns: columnNames,
    compressors,
    onChunk({ columnName, columnData, rowStart, rowEnd }) {
      const name = String(columnName);
      if (!columnSet.has(name)) return;

      const rawStart = Number(rowStart);
      const rawEnd = Number(rowEnd);
      const start = range && rawStart >= range.start ? rawStart - range.start : rawStart;
      const end = range && rawEnd > range.start ? rawEnd - range.start : rawEnd;
      builder.writeChunk(name, columnData, start, end);
      if (onProgress) {
        onProgress({ units: Math.max(end - start, 0) });
      }
    },
  };
  if (range) {
    readOptions.rowStart = range.start;
    readOptions.rowEnd = range.end;
  }
  await parquet.parquetRead(readOptions);
  return builder.finish();
}

async function readParquetColumnsRange(file, specs, columnNames, range, onProgress = null) {
  const parquet = await loadHyparquet();
  const asyncBuffer = parquet.asyncBufferFromFile
    ? await parquet.asyncBufferFromFile(file)
    : asyncBufferFromBlob(file);
  const compressors = await loadCompressors();

  if (typeof parquet.parquetRead !== 'function') {
    throw new Error('hyparquet parquetRead API is not available');
  }

  if (columnNames.length === 0) {
    const metadata = await readMetadata(file);
    const rows = range
      ? range.end - range.start
      : Number(metadata.num_rows ?? metadata.numRows ?? 0);
    return {
      rowCount: rows,
      columns: {},
    };
  }

  const chunks = Object.fromEntries(columnNames.map(name => [name, []]));
  const readOptions = {
    file: asyncBuffer,
    columns: columnNames,
    compressors,
    onChunk({ columnName, columnData, rowStart, rowEnd }) {
      const name = String(columnName);
      const columnChunks = chunks[name];
      if (!columnChunks) return;

      const rawStart = Number(rowStart);
      const rawEnd = Number(rowEnd);
      const start = range && rawStart >= range.start ? rawStart - range.start : rawStart;
      const end = range && rawEnd > range.start ? rawEnd - range.start : rawEnd;
      columnChunks.push({ start, end, data: columnData });
      if (onProgress) {
        onProgress({ units: Math.max(end - start, 0) });
      }
    },
  };
  if (range) {
    readOptions.rowStart = range.start;
    readOptions.rowEnd = range.end;
  }
  await parquet.parquetRead(readOptions);

  const rowCount = range
    ? range.end - range.start
    : maxChunkRowCount(chunks);
  const columns = {};
  for (const spec of specs) {
    columns[spec.name] = coerceColumnForSpec(
      assembleColumn(chunks[spec.name], rowCount),
      spec);
  }
  return { rowCount, columns };
}

function coerceColumnForSpec(values, spec) {
  if (spec.type !== 'i32') {
    return values;
  }

  let sample = null;
  for (let i = 0; i < values.length; ++i) {
    if (values[i] !== null && values[i] !== undefined) {
      sample = values[i];
      break;
    }
  }
  if (!(sample instanceof Date)) {
    return values;
  }

  const out = new Int32Array(values.length);
  for (let i = 0; i < values.length; ++i) {
    const value = values[i];
    out[i] = value instanceof Date
      ? Math.floor(value.getTime() / 86400000)
      : Number(value || 0) | 0;
  }
  return out;
}

function rowGroupRanges(metadata) {
  const rowGroups = metadata.row_groups || metadata.rowGroups || [];
  if (!rowGroups.length) {
    return [{ start: 0, end: Number(metadata.num_rows ?? metadata.numRows ?? 0) }];
  }
  const ranges = [];
  let start = 0;
  for (const group of rowGroups) {
    const rows = Number(group.num_rows ?? group.numRows ?? 0);
    ranges.push({ start, end: start + rows });
    start += rows;
  }
  return ranges;
}

function maxChunkRowCount(chunksByName) {
  let rowCount = 0;
  for (const chunks of Object.values(chunksByName)) {
    for (const chunk of chunks) {
      rowCount = Math.max(rowCount, chunk.end);
    }
  }
  return rowCount;
}

function assembleColumn(chunks, rowCount) {
  if (!chunks || chunks.length === 0) return [];
  if (chunks.length === 1 && chunks[0].start === 0 && chunks[0].end === rowCount) {
    return chunks[0].data;
  }

  const first = chunks[0].data;
  if (ArrayBuffer.isView(first) && typeof first.BYTES_PER_ELEMENT === 'number') {
    const out = new first.constructor(rowCount);
    for (const chunk of chunks) {
      out.set(chunk.data, chunk.start);
    }
    return out;
  }

  const out = new Array(rowCount);
  for (const chunk of chunks) {
    for (let i = chunk.start; i < chunk.end; ++i) {
      out[i] = chunk.data[i - chunk.start];
    }
  }
  return out;
}
