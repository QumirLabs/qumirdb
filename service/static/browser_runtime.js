// Browser execution runtime for simple one-to-one pipelines.
//
// Drives the standalone WASM kernels emitted by `qdb --plan-export`
// (bundle.exec): marshals a columnar batch into TColumn[]/TRowSet using
// bundle.exec.layout, runs the kernel, and reads results back. layout.
// pointerSize is 8 for wasm64/Memory64, or 4 for the wasm32 fallback.

import { BucketAllocator } from './wasm_allocator.js';

const PAGE = 65536;
const EMPTY_BYTES = new Uint8Array(0);

function nowMs() {
  return globalThis.performance ? globalThis.performance.now() : Date.now();
}

function alignUp(value, align) {
  return (value + (align - 1)) & ~(align - 1);
}

function base64ToBytes(base64) {
  const binary = atob(base64);
  const bytes = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; ++i) {
    bytes[i] = binary.charCodeAt(i);
  }
  return bytes;
}

// Byte width of a fixed-width core type in a kernel data buffer.
function logicalTypeName(typeOrColumn) {
  const type = typeof typeOrColumn === 'object' && typeOrColumn !== null
    ? typeOrColumn.type
    : typeOrColumn;
  return String(type || '').toLowerCase();
}

function decimalScale(typeOrColumn) {
  if (typeof typeOrColumn === 'object' && typeOrColumn !== null &&
      typeOrColumn.scale !== undefined) {
    return Number(typeOrColumn.scale) || 0;
  }
  const match = String(typeOrColumn || '').match(/^decimal\s*\(\s*\d+\s*,\s*(\d+)\s*\)$/i);
  return match ? Number(match[1]) : 0;
}

function storageTypeName(typeOrColumn) {
  if (typeof typeOrColumn === 'object' && typeOrColumn !== null &&
      typeOrColumn.storageType) {
    return String(typeOrColumn.storageType).toLowerCase();
  }
  let type = logicalTypeName(typeOrColumn);
  const nullable = type.match(/^nullable<(.+)>$/i);
  if (nullable) {
    type = nullable[1].trim().toLowerCase();
  }
  if (type === 'decimal' || type === 'binint' || type.startsWith('decimal(')) {
    return 'binint';
  }
  return type;
}

function isStringColumn(column) {
  return storageTypeName(column) === 'string';
}

function isDecimalColumn(column) {
  const type = logicalTypeName(column);
  return type === 'decimal' || type.startsWith('decimal(');
}

export function coreTypeWidth(type) {
  switch (storageTypeName(type)) {
    case 'i8': case 'u8': case 'bool': return 1;
    case 'i16': case 'u16': return 2;
    case 'i32': case 'u32': return 4;
    case 'i64': case 'u64': case 'f64': return 8;
    case 'string': return 16; // StringView (ptr + size)
    case 'binint': return 16; // qdb_bin_int (Lo + Hi)
    default: return 0;
  }
}

function isBigType(type) {
  const storage = storageTypeName(type);
  return storage === 'i64' || storage === 'u64';
}

function numericArrayConstructor(type) {
  switch (storageTypeName(type)) {
    case 'i8': return Int8Array;
    case 'u8': case 'bool': return Uint8Array;
    case 'i16': return Int16Array;
    case 'u16': return Uint16Array;
    case 'i32': return Int32Array;
    case 'u32': return Uint32Array;
    case 'i64': return BigInt64Array;
    case 'u64': return BigUint64Array;
    case 'f64': return Float64Array;
    default: return null;
  }
}

function typedColumnBytes(values, type, rowCount) {
  // JS boolean arrays hold one value per element; kernel columns hold bits.
  if (storageTypeName(type) === 'bool') return null;
  const Ctor = numericArrayConstructor(type);
  if (!Ctor || !(values instanceof Ctor) || values.length < rowCount) {
    return null;
  }
  const width = coreTypeWidth(type);
  return new Uint8Array(values.buffer, values.byteOffset, rowCount * width);
}

// Validity bitmap (LSB-first, 1 = valid) for a column whose values contain
// null; null when the column has no nulls (kernels treat a null mask pointer
// as all-valid, mirroring the native arrow source). Typed arrays cannot hold
// nulls.
function columnValidityMask(values, rowCount) {
  if (!values || ArrayBuffer.isView(values)) {
    return null;
  }
  let hasNull = false;
  for (let i = 0; i < rowCount; ++i) {
    const value = values[i];
    if (value === null || value === undefined) {
      hasNull = true;
      break;
    }
  }
  if (!hasNull) {
    return null;
  }
  const mask = new Uint8Array((rowCount + 7) >> 3);
  for (let i = 0; i < rowCount; ++i) {
    const value = values[i];
    if (value !== null && value !== undefined) {
      mask[i >> 3] |= 1 << (i & 7);
    }
  }
  return mask;
}

function makeRowId(batchIdx, rowIdx) {
  return (BigInt(batchIdx) << 32n) | BigInt(rowIdx >>> 0);
}

function encodeStringColumn(values, rowCount, encoder) {
  const parts = new Array(rowCount);
  const offsets = new Int32Array(rowCount + 1);
  const cache = new Map();
  let total = 0;
  for (let i = 0; i < rowCount; ++i) {
    const value = values[i];
    let b = EMPTY_BYTES;
    if (value !== null && value !== undefined) {
      const text = String(value);
      b = cache.get(text);
      if (!b) {
        b = encoder.encode(text);
        cache.set(text, b);
      }
    }
    parts[i] = b;
    offsets[i] = total;
    total += b.length;
  }
  offsets[rowCount] = total;
  return { parts, offsets, total };
}

class RowSetWriter {
  constructor(arena, layout) {
    this.arena = arena;
    this.layout = layout;
    this.columnsBase = 0;
    this.rowsetPtr = 0;
    this.dataPtrs = [];
    this.dataCaps = [];
    this.offsetsPtrs = [];
    this.offsetsCaps = [];
    this.selectionPtr = 0;
    this.selectionCap = 0;
  }

  write(columns, rowCount, wantSelection, selection = null) {
    this.ensureStructs(columns.length);

    const encoder = new TextEncoder();
    const encoded = columns.map(column => {
      if (!isStringColumn(column)) return null;
      return encodeStringColumn(column.values, rowCount, encoder);
    });
    const masks = columns.map(column => columnValidityMask(column.values, rowCount));

    for (let c = 0; c < columns.length; ++c) {
      const column = columns[c];
      if (isStringColumn(column)) {
        this.ensureData(c, Math.max(encoded[c].total, 1), 1);
        this.ensureOffsets(c, (rowCount + 1) * 4);
      } else {
        const width = coreTypeWidth(column);
        this.ensureData(c, Math.max(rowCount, 1) * width, 8);
      }
      if (masks[c]) {
        this.ensureMask(c, masks[c].length);
      }
    }
    if (wantSelection || selection) {
      this.ensureSelection(Math.max(rowCount, 1));
    }

    const colLayout = this.layout.column;
    const rsLayout = this.layout.rowset;
    const dv = this.arena.view();
    const memBytes = this.arena.bytes();

    for (let c = 0; c < columns.length; ++c) {
      const column = columns[c];
      const colPtr = this.columnsBase + c * colLayout.size;
      new Uint8Array(this.arena.memory.buffer, colPtr, colLayout.size).fill(0);
      writePointer(dv, colPtr + colLayout.data, this.dataPtrs[c]);
      if (masks[c]) {
        memBytes.set(masks[c], this.maskPtrs[c]);
        writePointer(dv, colPtr + colLayout.mask, this.maskPtrs[c]);
      }
      if (isStringColumn(column)) {
        const enc = encoded[c];
        let p = this.dataPtrs[c];
        for (let i = 0; i < rowCount; ++i) {
          memBytes.set(enc.parts[i], p);
          p += enc.parts[i].length;
        }
        for (let i = 0; i <= rowCount; ++i) {
          dv.setInt32(this.offsetsPtrs[c] + i * 4, enc.offsets[i], true);
        }
        writePointer(dv, colPtr + colLayout.offsets, this.offsetsPtrs[c]);
        dv.setUint8(colPtr + colLayout.offsetWidth, 4);
      } else {
        const width = coreTypeWidth(column);
        const values = column.values;
        const bytes = typedColumnBytes(values, column, rowCount);
        if (bytes) {
          memBytes.set(bytes, this.dataPtrs[c]);
        } else {
          for (let i = 0; i < rowCount; ++i) {
            writeColumnValue(dv, this.dataPtrs[c], i, column, values[i]);
          }
        }
      }
    }

    new Uint8Array(this.arena.memory.buffer, this.rowsetPtr, rsLayout.size).fill(0);
    writePointer(dv, this.rowsetPtr + rsLayout.columns, this.columnsBase);
    dv.setBigInt64(this.rowsetPtr + rsLayout.columnCount, BigInt(columns.length), true);
    dv.setBigInt64(this.rowsetPtr + rsLayout.rowCount, BigInt(rowCount), true);
    if (selection) {
      copySelectionToMemory(this.arena, this.selectionPtr, selection, rowCount);
      writePointer(dv, this.rowsetPtr + rsLayout.selection, this.selectionPtr);
    } else if (wantSelection) {
      writePointer(dv, this.rowsetPtr + rsLayout.selection, this.selectionPtr);
    }

    return { rowsetPtr: this.rowsetPtr, selectionPtr: this.selectionPtr };
  }

  ensureStructs(columnCount) {
    if (this.rowsetPtr && this.dataPtrs.length === columnCount) {
      return;
    }
    const colLayout = this.layout.column;
    const rsLayout = this.layout.rowset;
    this.columnsBase = this.arena.alloc(columnCount * colLayout.size, 8);
    this.rowsetPtr = this.arena.alloc(rsLayout.size, 8);
    this.dataPtrs = new Array(columnCount).fill(0);
    this.dataCaps = new Array(columnCount).fill(0);
    this.offsetsPtrs = new Array(columnCount).fill(0);
    this.offsetsCaps = new Array(columnCount).fill(0);
    this.maskPtrs = new Array(columnCount).fill(0);
    this.maskCaps = new Array(columnCount).fill(0);
  }

  ensureMask(index, bytes) {
    if (this.maskCaps[index] >= bytes) {
      return;
    }
    this.maskPtrs[index] = this.arena.alloc(bytes, 8);
    this.maskCaps[index] = bytes;
  }

  ensureData(index, bytes, align) {
    if (this.dataCaps[index] >= bytes) {
      return;
    }
    this.dataPtrs[index] = this.arena.alloc(bytes, align);
    this.dataCaps[index] = bytes;
  }

  ensureOffsets(index, bytes) {
    if (this.offsetsCaps[index] >= bytes) {
      return;
    }
    this.offsetsPtrs[index] = this.arena.alloc(bytes, 4);
    this.offsetsCaps[index] = bytes;
  }

  ensureSelection(bytes) {
    if (this.selectionCap >= bytes) {
      return;
    }
    this.selectionPtr = this.arena.alloc(bytes, 8);
    this.selectionCap = bytes;
  }
}

function sourceChunkLength(values) {
  if (!values || typeof values.length !== 'number') {
    return 0;
  }
  return values.length;
}

function isNullish(value) {
  return value === null || value === undefined;
}

function sourceNumericValue(column, value) {
  if (column.type === 'i32' && value instanceof Date) {
    return Math.floor(value.getTime() / 86400000);
  }
  return value;
}

function clearValidityBit(memBytes, maskPtr, row) {
  memBytes[maskPtr + (row >> 3)] &= ~(1 << (row & 7));
}

export class WasmSourceBatchBuilder {
  constructor(arena, layout, columns, rowCount) {
    this.arena = arena;
    this.layout = layout;
    this.columns = shapeColumns(columns);
    this.rowCount = Number(rowCount) || 0;
    this.encoder = new TextEncoder();
    this.columnsBase = this.columns.length > 0
      ? arena.alloc(this.columns.length * layout.column.size, 8)
      : 0;
    this.rowsetPtr = arena.alloc(layout.rowset.size, 8);
    this.nameToIndex = new Map(this.columns.map((column, index) => [column.name, index]));
    this.dataPtrs = new Array(this.columns.length).fill(0);
    this.dataCaps = new Array(this.columns.length).fill(0);
    this.offsetsPtrs = new Array(this.columns.length).fill(0);
    this.maskPtrs = new Array(this.columns.length).fill(0);
    this.nextRows = new Array(this.columns.length).fill(0);
    this.stringByteSizes = new Array(this.columns.length).fill(0);

    for (let c = 0; c < this.columns.length; ++c) {
      const column = this.columns[c];
      if (isStringColumn(column)) {
        this.offsetsPtrs[c] = arena.alloc((this.rowCount + 1) * 4, 4);
        arena.bytes().fill(0, this.offsetsPtrs[c], this.offsetsPtrs[c] + (this.rowCount + 1) * 4);
      } else {
        const width = coreTypeWidth(column);
        if (width <= 0) {
          throw new Error(`unsupported source column type: ${column.type}`);
        }
        const bytes = Math.max(this.rowCount, 1) * width;
        this.dataPtrs[c] = arena.alloc(bytes, 8);
        this.dataCaps[c] = bytes;
        arena.bytes().fill(0, this.dataPtrs[c], this.dataPtrs[c] + bytes);
      }
    }
  }

  writeChunk(columnName, columnData, start, end) {
    const index = this.nameToIndex.get(String(columnName));
    if (index === undefined) {
      return;
    }
    const from = Number(start ?? 0);
    const to = end == null ? from + sourceChunkLength(columnData) : Number(end);
    if (from < 0 || to < from || to > this.rowCount) {
      throw new Error(
        `source chunk ${columnName} has invalid row range ${from}..${to} of ${this.rowCount}`);
    }
    const column = this.columns[index];
    if (isStringColumn(column)) {
      this.writeStringChunk(index, columnData, from, to);
    } else {
      this.writeNumericChunk(index, columnData, from, to);
    }
  }

  writeNumericChunk(index, values, start, end) {
    const column = this.columns[index];
    if (start !== this.nextRows[index]) {
      throw new Error(
        `source chunks for ${column.name} are not contiguous at row ${this.nextRows[index]}`);
    }
    const count = end - start;
    if (count <= 0) {
      return;
    }
    const width = coreTypeWidth(column);
    const bytes = typedColumnBytes(values, column, count);
    if (bytes) {
      this.arena.bytes().set(bytes, this.dataPtrs[index] + start * width);
      this.nextRows[index] = end;
      return;
    }

    let hasNull = false;
    for (let i = 0; i < count; ++i) {
      if (isNullish(values?.[i])) {
        hasNull = true;
        break;
      }
    }
    if (hasNull) {
      this.ensureMask(index);
    }

    const dv = this.arena.view();
    const memBytes = this.arena.bytes();
    for (let i = 0; i < count; ++i) {
      const value = values?.[i];
      const row = start + i;
      if (isNullish(value)) {
        clearValidityBit(memBytes, this.maskPtrs[index], row);
        writeColumnValue(dv, this.dataPtrs[index], row, column, 0);
      } else {
        writeColumnValue(
          dv,
          this.dataPtrs[index],
          row,
          column,
          sourceNumericValue(column, value));
      }
    }
    this.nextRows[index] = end;
  }

  writeStringChunk(index, values, start, end) {
    if (start !== this.nextRows[index]) {
      throw new Error(
        `source chunks for ${this.columns[index].name} are not contiguous at row ${this.nextRows[index]}`);
    }
    let hasNull = false;
    for (let i = 0; i < end - start; ++i) {
      if (isNullish(values?.[i])) {
        hasNull = true;
        break;
      }
    }
    if (hasNull) {
      this.ensureMask(index);
    }

    let dv = this.arena.view();
    const offsetsPtr = this.offsetsPtrs[index];
    let size = this.stringByteSizes[index];

    for (let i = 0; i < end - start; ++i) {
      const row = start + i;
      const value = values?.[i];
      dv.setInt32(offsetsPtr + row * 4, size, true);
      if (isNullish(value)) {
        clearValidityBit(this.arena.bytes(), this.maskPtrs[index], row);
      } else {
        size = this.writeStringValue(index, String(value), size);
        dv = this.arena.view();
      }
      dv.setInt32(offsetsPtr + (row + 1) * 4, size, true);
    }
    this.stringByteSizes[index] = size;
    this.nextRows[index] = end;
  }

  writeStringValue(index, text, size) {
    let rest = text;
    while (rest.length > 0) {
      const needed = size + Math.max(rest.length * 3, 1);
      this.ensureStringCapacity(index, needed);
      const memBytes = this.arena.bytes();
      const start = this.dataPtrs[index] + size;
      const end = this.dataPtrs[index] + this.dataCaps[index];
      const written = this.encoder.encodeInto(rest, memBytes.subarray(start, end));
      if (written.read <= 0 && rest.length > 0) {
        throw new Error('TextEncoder.encodeInto made no progress');
      }
      size += written.written;
      rest = rest.slice(written.read);
    }
    return size;
  }

  ensureStringCapacity(index, bytes) {
    if (this.dataCaps[index] >= bytes) {
      return;
    }
    const next = Math.max(bytes, this.dataCaps[index] ? this.dataCaps[index] * 2 : 64);
    const oldPtr = this.dataPtrs[index];
    const newPtr = this.arena.alloc(next, 1);
    if (oldPtr) {
      // alloc may have grown (and detached) the buffer, so take the view after.
      this.arena.bytes().copyWithin(newPtr, oldPtr, oldPtr + this.dataCaps[index]);
      this.arena.free(oldPtr);
    }
    this.dataPtrs[index] = newPtr;
    this.dataCaps[index] = next;
  }

  ensureMask(index) {
    if (this.maskPtrs[index]) {
      return;
    }
    const bytes = Math.max((this.rowCount + 7) >> 3, 1);
    this.maskPtrs[index] = this.arena.alloc(bytes, 8);
    this.arena.bytes().fill(0xff, this.maskPtrs[index], this.maskPtrs[index] + bytes);
  }

  finish() {
    for (let c = 0; c < this.columns.length; ++c) {
      if (this.nextRows[c] !== this.rowCount) {
        throw new Error(
          `source column ${this.columns[c].name} ended at row ${this.nextRows[c]} of ${this.rowCount}`);
      }
      if (!isStringColumn(this.columns[c])) {
        continue;
      }
      const offsetsPtr = this.offsetsPtrs[c];
      const size = this.stringByteSizes[c];
      if (!this.dataPtrs[c]) {
        this.dataPtrs[c] = this.arena.alloc(1, 1);
        this.dataCaps[c] = 1;
      }
      const dv = this.arena.view();
      for (let row = this.nextRows[c]; row <= this.rowCount; ++row) {
        dv.setInt32(offsetsPtr + row * 4, size, true);
      }
    }

    const colLayout = this.layout.column;
    const rsLayout = this.layout.rowset;
    const dv = this.arena.view();
    for (let c = 0; c < this.columns.length; ++c) {
      const column = this.columns[c];
      const colPtr = this.columnsBase + c * colLayout.size;
      new Uint8Array(this.arena.memory.buffer, colPtr, colLayout.size).fill(0);
      writePointer(dv, colPtr + colLayout.data, this.dataPtrs[c]);
      if (this.maskPtrs[c]) {
        writePointer(dv, colPtr + colLayout.mask, this.maskPtrs[c]);
      }
      if (isStringColumn(column)) {
        writePointer(dv, colPtr + colLayout.offsets, this.offsetsPtrs[c]);
        dv.setUint8(colPtr + colLayout.offsetWidth, 4);
      }
    }

    new Uint8Array(this.arena.memory.buffer, this.rowsetPtr, rsLayout.size).fill(0);
    writePointer(dv, this.rowsetPtr + rsLayout.columns, this.columnsBase);
    dv.setBigInt64(this.rowsetPtr + rsLayout.columnCount, BigInt(this.columns.length), true);
    dv.setBigInt64(this.rowsetPtr + rsLayout.rowCount, BigInt(this.rowCount), true);

    const batch = {
      rowCount: this.rowCount,
      columns: shapeColumns(this.columns),
    };
    attachBatchWasm(batch, this.rowsetPtr, {
      pinned: true,
      destroy: () => freeMarshalledRowSet(this.arena, this.layout, this.rowsetPtr),
    });
    return batch;
  }
}

class WasmRowStore {
  constructor(arena, layout) {
    this.arena = arena;
    this.layout = layout;
    this.batches = [];
    this.ownedPtrs = []; // per batch: our marshalled rowset ptr to free, or 0 (pinned)
    this.wasmHandles = [];
    this.storePtr = 0;
    this.capacity = 0;
  }

  // `ownedPtr` is the marshalRowSet rowset this store may free at teardown.
  // When `ownedPtr` is 0, `wasmHandle` is a moved pinned handle owned by this
  // store until freeMarshalled().
  push(batch, rowsetPtr, ownedPtr = 0, wasmHandle = null) {
    if (!ownedPtr && !wasmHandle) {
      throw new Error('wasm row store needs either an owned rowset or a moved handle');
    }
    this.ensureCapacity(this.batches.length + 1);
    const idx = this.batches.length;
    this.batches.push(batch);
    this.ownedPtrs.push(ownedPtr);
    this.wasmHandles.push(wasmHandle ? moveWasmRowSetHandle(wasmHandle) : null);
    const size = this.layout.rowset.size;
    const memBytes = this.arena.bytes();
    memBytes.copyWithin(
      this.storePtr + idx * size,
      rowsetPtr,
      rowsetPtr + size);
    return idx;
  }

  // Frees the column buffers of every batch this store marshalled itself, plus
  // the store array. Build sides linger for the whole query otherwise — the
  // dominant cross-join accumulation in multi-join plans (TPC-H Q18/Q21).
  // Pinned batches (ownedPtr 0) carry a moved wasm handle owned by this store.
  freeMarshalled() {
    for (let i = 0; i < this.ownedPtrs.length; ++i) {
      const owned = this.ownedPtrs[i];
      if (!owned) {
        releaseWasmRowSetHandle(this.wasmHandles[i]);
        continue;
      }
      freeMarshalledRowSet(this.arena, this.layout, owned);
    }
    if (this.storePtr) this.arena.free(this.storePtr);
    this.storePtr = 0;
    this.capacity = 0;
    this.batches = [];
    this.ownedPtrs = [];
    this.wasmHandles = [];
  }

  dataPtr() {
    return this.storePtr;
  }

  batch(index) {
    return this.batches[index];
  }

  ensureCapacity(needed) {
    if (this.capacity >= needed) {
      return;
    }
    const size = this.layout.rowset.size;
    const next = Math.max(needed, this.capacity ? this.capacity * 2 : 4);
    const oldPtr = this.storePtr;
    const oldBytes = this.batches.length * size;
    this.storePtr = this.arena.alloc(next * size, 8);
    this.capacity = next;
    if (oldPtr && oldBytes > 0) {
      const memBytes = this.arena.bytes();
      memBytes.copyWithin(this.storePtr, oldPtr, oldPtr + oldBytes);
    }
  }
}

// One query-wide allocator over the shared linear memory. Backed by a
// segregated free-list (BucketAllocator) so qdb_free actually
// reclaims — bounding memory for join-heavy queries whose per-drain output and
// per-batch input buffers would otherwise accumulate forever in a bump arena.
// Blocks are >= 16-byte aligned, satisfying every caller's alignment request.
// As before, callers must finish all alloc() calls before taking a view()/
// bytes() (a grow inside malloc detaches the underlying ArrayBuffer).
class Arena {
  constructor(memory, base) {
    this.memory = memory;
    this.allocator = new BucketAllocator(memory, Number(base));
  }

  alloc(bytes, _align = 8) {
    return this.allocator.malloc(bytes);
  }

  free(ptr) {
    this.allocator.free(ptr);
  }

  view() {
    return new DataView(this.memory.buffer);
  }

  bytes() {
    return new Uint8Array(this.memory.buffer);
  }

  stats() {
    return this.allocator.stats();
  }
}

function createMemory64(initialPages) {
  const pages = BigInt(initialPages);
  const errors = [];
  for (const key of ['address', 'index']) {
    try {
      return new WebAssembly.Memory({ initial: pages, [key]: 'i64' });
    } catch (error) {
      errors.push(`${key}: ${error.message || String(error)}`);
    }
  }
  throw new Error(`memory64 is not available (${errors.join('; ')})`);
}

function createMemory32(initialPages) {
  return new WebAssembly.Memory({ initial: Number(initialPages) });
}

function createSharedMemory(layout, wasm) {
  const spec = layout.sharedMemory;
  if (!spec) {
    throw new Error('exec layout is missing sharedMemory');
  }
  const memory = layout.pointerSize === 4
    ? createMemory32(Number(spec.initialPages))
    : createMemory64(Number(spec.initialPages));
  return {
    memory,
    arena: new Arena(memory, Number(spec.heapBase)),
    wasm,
    kernelInstance: null,
    kernelInstancePromise: null,
    cteSpools: new Map(),
  };
}

// batch.wasm is a pointer into the current query's shared memory. Streaming
// pointers are valid until the producing RowSetWriter writes again; pinned
// pointers are fresh arena allocations and can be retained by join state.
function selectionPtrOf(selection) {
  return selection?.kind === 'wasm-selection' ? selection.ptr : 0;
}

function makeWasmRowSetHandle(rowsetPtr, { pinned, selectionPtr = 0, destroy = null }) {
  return {
    rowsetPtr,
    pinned: pinned === true,
    hasSelection: selectionPtr !== 0,
    selectionPtr,
    destroy,
    refCount: typeof destroy === 'function' ? 1 : 0,
    released: false,
  };
}

function assertLiveWasmRowSetHandle(wasm) {
  if (wasm?.released) {
    throw new Error('wasm rowset handle was already released');
  }
}

function attachBatchWasm(
    batch, rowsetPtr, { pinned, selectionPtr = 0, destroy = null }) {
  const current = batch.wasm;
  const sameOwner = current &&
    !current.released &&
    current.rowsetPtr === rowsetPtr &&
    current.destroy === destroy;
  const wasm = sameOwner
    ? current
    : makeWasmRowSetHandle(rowsetPtr, { pinned, selectionPtr, destroy });
  wasm.pinned = pinned === true;
  wasm.hasSelection = selectionPtr !== 0;
  wasm.selectionPtr = selectionPtr;
  batch.wasm = wasm;
  return wasm;
}

function moveWasmRowSetHandle(wasm) {
  if (!wasm) {
    return null;
  }
  assertLiveWasmRowSetHandle(wasm);
  return wasm;
}

function retainWasmRowSetHandle(wasm) {
  if (!wasm) {
    return null;
  }
  assertLiveWasmRowSetHandle(wasm);
  if (typeof wasm.destroy !== 'function') {
    throw new Error('cannot retain a non-owning wasm rowset handle');
  }
  ++wasm.refCount;
  return wasm;
}

function releaseWasmRowSetHandle(wasm) {
  if (!wasm || typeof wasm.destroy !== 'function') {
    return;
  }
  assertLiveWasmRowSetHandle(wasm);
  if (wasm.refCount <= 0) {
    throw new Error('wasm rowset handle refcount underflow');
  }
  --wasm.refCount;
  if (wasm.refCount !== 0) {
    return;
  }
  const destroy = wasm.destroy;
  wasm.destroy = null;
  wasm.released = true;
  destroy();
}

function wasmRowSetForSelection(batch, selection) {
  const wasm = batch?.wasm;
  if (!wasm) {
    return null;
  }
  assertLiveWasmRowSetHandle(wasm);
  if (selection == null) {
    return wasm.hasSelection ? null : wasm;
  }
  const selectionPtr = selectionPtrOf(selection);
  return selectionPtr !== 0 && wasm.hasSelection && wasm.selectionPtr === selectionPtr
    ? wasm
    : null;
}

function makeWasmSelection(arena, rowCount, ptr) {
  return ptr
    ? { kind: 'wasm-selection', arena, rowCount, ptr }
    : null;
}

function rowSelected(selection, row) {
  if (!selection) {
    return true;
  }
  if (selection.kind === 'wasm-selection') {
    return selection.arena.bytes()[selection.ptr + row] !== 0;
  }
  return selection[row] !== 0;
}

function copySelectionToMemory(arena, destPtr, selection, rowCount) {
  const out = arena.bytes();
  if (selection?.kind === 'wasm-selection') {
    out.set(
      selection.arena.bytes().subarray(selection.ptr, selection.ptr + rowCount),
      destPtr);
  } else {
    out.set(selection.subarray(0, rowCount), destPtr);
  }
}

function ensureRowSetSelection(arena, layout, rowsetPtr, rowCount) {
  const dv = arena.view();
  const selectionOffset = rowsetPtr + layout.rowset.selection;
  let selectionPtr = readPointer(dv, selectionOffset);
  if (!selectionPtr) {
    selectionPtr = arena.alloc(Math.max(rowCount, 1), 8);
    writePointer(arena.view(), selectionOffset, selectionPtr);
  }
  return selectionPtr;
}

function aliasWasmRowSetDescriptor(arena, layout, rowSet) {
  const wasm = rowSet?.batch?.wasm;
  if (!wasm) {
    return null;
  }
  assertLiveWasmRowSetHandle(wasm);
  const dv = arena.view();
  const rs = layout.rowset;
  const sourcePtr = wasm.rowsetPtr;
  const sourceColumns = readPointer(dv, sourcePtr + rs.columns);
  const columnCount = Number(dv.getBigInt64(sourcePtr + rs.columnCount, true));
  const rowCount = Number(dv.getBigInt64(sourcePtr + rs.rowCount, true));
  const columnsBase = arena.alloc(Math.max(columnCount, 1) * layout.column.size, 8);
  arena.bytes().set(
    arena.bytes().subarray(
      sourceColumns,
      sourceColumns + columnCount * layout.column.size),
    columnsBase);

  const rowsetPtr = arena.alloc(rs.size, 8);
  arena.bytes().fill(0, rowsetPtr, rowsetPtr + rs.size);
  writePointer(arena.view(), rowsetPtr + rs.columns, columnsBase);
  arena.view().setBigInt64(rowsetPtr + rs.columnCount, BigInt(columnCount), true);
  arena.view().setBigInt64(rowsetPtr + rs.rowCount, BigInt(rowCount), true);
  let selectionPtr = 0;
  if (rowSet.selection) {
    selectionPtr = arena.alloc(Math.max(rowCount, 1), 8);
    copySelectionToMemory(arena, selectionPtr, rowSet.selection, rowCount);
    writePointer(arena.view(), rowsetPtr + rs.selection, selectionPtr);
  }

  return {
    rowsetPtr,
    selectionPtr,
    destroy: () => {
      if (selectionPtr) {
        arena.free(selectionPtr);
      }
      arena.free(columnsBase);
      arena.free(rowsetPtr);
    },
  };
}

// Like aliasWasmRowSetDescriptor, but keeps only `kept` columns (in that order)
// of the batch's wasm rowset — the wasm equivalent of MakeFilterSelectProcess for
// filter column pruning. Column data stays in wasm (no JS .values needed); the
// filter's output `selection` is copied verbatim.
function narrowWasmRowSetDescriptor(arena, layout, batch, kept, selection) {
  const wasm = batch?.wasm;
  if (!wasm) {
    return null;
  }
  assertLiveWasmRowSetHandle(wasm);
  const rs = layout.rowset;
  const col = layout.column;
  const sourcePtr = wasm.rowsetPtr;
  const sourceColumns = readPointer(arena.view(), sourcePtr + rs.columns);
  const rowCount = Number(arena.view().getBigInt64(sourcePtr + rs.rowCount, true));
  const columnsBase = arena.alloc(Math.max(kept.length, 1) * col.size, 8);
  const rowsetPtr = arena.alloc(rs.size, 8);
  const selectionPtr = selection ? arena.alloc(Math.max(rowCount, 1), 8) : 0;

  // All allocations done; safe to take views.
  const bytes = arena.bytes();
  for (let o = 0; o < kept.length; ++o) {
    const src = sourceColumns + kept[o] * col.size;
    bytes.set(bytes.subarray(src, src + col.size), columnsBase + o * col.size);
  }
  const dv = arena.view();
  new Uint8Array(arena.memory.buffer, rowsetPtr, rs.size).fill(0);
  writePointer(dv, rowsetPtr + rs.columns, columnsBase);
  dv.setBigInt64(rowsetPtr + rs.columnCount, BigInt(kept.length), true);
  dv.setBigInt64(rowsetPtr + rs.rowCount, BigInt(rowCount), true);
  if (selectionPtr) {
    copySelectionToMemory(arena, selectionPtr, selection, rowCount);
    writePointer(dv, rowsetPtr + rs.selection, selectionPtr);
  }
  return {
    rowsetPtr,
    selectionPtr,
    rowCount,
    destroy: () => {
      if (selectionPtr) {
        arena.free(selectionPtr);
      }
      arena.free(columnsBase);
      arena.free(rowsetPtr);
    },
  };
}

function shapeColumns(output) {
  return (output || []).map(column => ({
    name: column.name,
    type: column.type,
    storageType: column.storageType,
    precision: column.precision,
    scale: column.scale,
    nullable: column.nullable,
  }));
}

function makeWasmOwnedBatch(state, rowsetPtr, output) {
  const rowCount = Number(
    state.arena.view().getBigInt64(rowsetPtr + state.layout.rowset.rowCount, true));
  const batch = {
    rowCount,
    columns: shapeColumns(output),
  };
  attachBatchWasm(batch, rowsetPtr, {
    pinned: true,
    destroy: () => destroyKernelOwnedRowSet(state, rowsetPtr),
  });
  return batch;
}

function pinSourceBatch(batch, arena, layout) {
  if (batch.wasm) {
    return batch;
  }
  const marshalled = marshalRowSet(
    arena, layout, batch.columns, batch.rowCount, false, null);
  attachBatchWasm(batch, marshalled.rowsetPtr, {
    pinned: true,
    destroy: () => freeMarshalledRowSet(arena, layout, marshalled.rowsetPtr),
  });
  return batch;
}

function takeRowSetRelease(rowSet) {
  const releases = [];
  if (typeof rowSet?.destroy === 'function') {
    releases.push(rowSet.destroy);
    rowSet.destroy = null;
  }
  const wasm = rowSet?.batch?.wasm;
  if (typeof wasm?.destroy === 'function') {
    assertLiveWasmRowSetHandle(wasm);
    rowSet.wasmReleaseMoved = true;
    releases.push(() => releaseWasmRowSetHandle(wasm));
  }
  if (releases.length === 0) {
    return null;
  }
  return () => {
    for (const release of releases) {
      release();
    }
  };
}

function releaseWasmRowSet(rowSet) {
  if (typeof rowSet?.destroy === 'function') {
    const destroy = rowSet.destroy;
    rowSet.destroy = null;
    destroy();
  }
  if (!rowSet?.wasmReleaseMoved) {
    releaseWasmRowSetHandle(rowSet?.batch?.wasm);
  }
}

function cloneRetainedRowSetView(rowSet) {
  const batch = rowSet?.batch;
  if (!batch) {
    throw new Error('cannot retain an empty rowset');
  }
  if (!batch.wasm) {
    throw new Error('CTE spool requires a WASM-backed rowset');
  }
  const wasm = retainWasmRowSetHandle(batch.wasm);
  return {
    batch: {
      ...batch,
      columns: batch.columns,
      wasm,
    },
    selection: rowSet.selection || null,
    sharedView: true,
  };
}

function releaseWasmBatch(batch) {
  if (batch) {
    releaseWasmRowSet({ batch, selection: null });
  }
}

function freeMarshalledRowSet(arena, layout, rowsetPtr) {
  if (!rowsetPtr) {
    return;
  }
  const dv = arena.view();
  const col = layout.column;
  const rs = layout.rowset;
  const colsBase = readPointer(dv, rowsetPtr + rs.columns);
  const colCount = Number(dv.getBigInt64(rowsetPtr + rs.columnCount, true));
  if (colsBase) {
    for (let c = 0; c < colCount; ++c) {
      const colPtr = colsBase + c * col.size;
      arena.free(readPointer(dv, colPtr + col.data));
      arena.free(readPointer(dv, colPtr + col.mask));
      arena.free(readPointer(dv, colPtr + col.offsets));
    }
    arena.free(colsBase);
  }
  arena.free(readPointer(dv, rowsetPtr + rs.selection));
  arena.free(rowsetPtr);
}

// Pointer width for the current query (4 or 8), set once from
// exec.layout.pointerSize. Safe as module state because each query runs
// in its own worker.
let PointerSize = 8;

function writePointer(dv, offset, address) {
  if (PointerSize === 4) {
    dv.setUint32(offset, Number(address), true);
  } else {
    dv.setBigUint64(offset, BigInt(Number(address)), true);
  }
}

function readPointer(dv, offset) {
  if (PointerSize === 4) {
    return dv.getUint32(offset, true);
  }
  return Number(dv.getBigUint64(offset, true));
}

// Marshals a pointer value for a wasm call: BigInt under wasm64 (every
// pointer is an i64 there), plain Number under wasm32. Do not use this for
// values the kernel always declares as i64, like counts or opcodes — pass
// BigInt(...) directly for those.
function toWasmPtr(value) {
  return PointerSize === 4 ? Number(value) : BigInt(value);
}

function fromWasmPtr(value) {
  return Number(value);
}

function decimalToScaledBigInt(value, scale) {
  if (value === null || value === undefined) {
    return 0n;
  }
  if (typeof value === 'bigint') {
    return value;
  }
  if (value instanceof Uint8Array) {
    let raw = 0n;
    for (const byte of value) {
      raw = (raw << 8n) | BigInt(byte);
    }
    if (value.length > 0 && (value[0] & 0x80)) {
      raw -= 1n << BigInt(value.length * 8);
    }
    return raw;
  }
  if (value instanceof ArrayBuffer) {
    return decimalToScaledBigInt(new Uint8Array(value), scale);
  }
  if (typeof value === 'number') {
    return BigInt(Math.round(value * (10 ** scale)));
  }
  const text = String(value).trim();
  if (!text) {
    return 0n;
  }
  const neg = text.startsWith('-');
  const unsigned = neg || text.startsWith('+') ? text.slice(1) : text;
  const [wholeText, fracText = ''] = unsigned.split('.');
  const whole = BigInt(wholeText || '0');
  const paddedFrac = (fracText + '0'.repeat(scale)).slice(0, scale);
  const frac = BigInt(paddedFrac || '0');
  const factor = 10n ** BigInt(scale);
  const result = whole * factor + frac;
  return neg ? -result : result;
}

function writeBinIntValue(dv, address, value, scale) {
  const raw = BigInt.asUintN(128, decimalToScaledBigInt(value, scale));
  dv.setBigUint64(address, BigInt.asUintN(64, raw), true);
  dv.setBigUint64(address + 8, BigInt.asUintN(64, raw >> 64n), true);
}

function readBinIntValue(dv, address) {
  const lo = dv.getBigUint64(address, true);
  const hi = dv.getBigInt64(address + 8, true);
  return (hi << 64n) + lo;
}


function formatDecimalValue(raw, scale) {
  let value = BigInt(raw);
  const neg = value < 0n;
  if (neg) {
    value = -value;
  }
  const factor = 10n ** BigInt(scale);
  const whole = value / factor;
  const frac = value % factor;
  if (scale === 0) {
    return `${neg ? '-' : ''}${whole}`;
  }
  return `${neg ? '-' : ''}${whole}.${frac.toString().padStart(scale, '0')}`;
}

// Scalar bool ABI values occupy a byte. Only column buffers use bitmaps.
function writeColumnValue(dv, dataPtr, row, type, value) {
  if (storageTypeName(type) === 'bool') {
    const address = dataPtr + (row >> 3);
    const mask = 1 << (row & 7);
    const byte = dv.getUint8(address);
    dv.setUint8(address, Number(value) ? byte | mask : byte & ~mask);
  } else {
    writeNumericValue(dv, dataPtr + row * coreTypeWidth(type), type, value);
  }
}

function readColumnValue(dv, dataPtr, row, type, bitOffset) {
  if (storageTypeName(type) === 'bool') {
    const bit = row + bitOffset;
    return (dv.getUint8(dataPtr + (bit >> 3)) >> (bit & 7)) & 1;
  }
  return readNumericValue(dv, dataPtr + row * coreTypeWidth(type), type);
}

function writeNumericValue(dv, address, type, value) {
  const storage = storageTypeName(type);
  switch (storage) {
    case 'i8': dv.setInt8(address, Number(value) | 0); break;
    case 'u8': case 'bool': dv.setUint8(address, Number(value) & 0xff); break;
    case 'i16': dv.setInt16(address, Number(value) | 0, true); break;
    case 'u16': dv.setUint16(address, Number(value) & 0xffff, true); break;
    case 'i32': dv.setInt32(address, Number(value) | 0, true); break;
    case 'u32': dv.setUint32(address, Number(value) >>> 0, true); break;
    case 'i64': dv.setBigInt64(address, BigInt(value ?? 0), true); break;
    case 'u64': dv.setBigUint64(address, BigInt(value ?? 0), true); break;
    case 'f64': dv.setFloat64(address, Number(value ?? 0), true); break;
    case 'binint': writeBinIntValue(dv, address, value, decimalScale(type)); break;
    default: throw new Error(`unsupported numeric column type: ${type}`);
  }
}

function readNumericValue(dv, address, type) {
  switch (storageTypeName(type)) {
    case 'i8': return dv.getInt8(address);
    case 'u8': case 'bool': return dv.getUint8(address);
    case 'i16': return dv.getInt16(address, true);
    case 'u16': return dv.getUint16(address, true);
    case 'i32': return dv.getInt32(address, true);
    case 'u32': return dv.getUint32(address, true);
    case 'i64': return dv.getBigInt64(address, true);
    case 'u64': return dv.getBigUint64(address, true);
    case 'f64': return dv.getFloat64(address, true);
    case 'binint': return readBinIntValue(dv, address);
    default: throw new Error(`unsupported numeric column type: ${type}`);
  }
}

// qdb_string_hash_bytes is native-only (arrow::internal::ComputeStringHash +
// XXH3); the browser doesn't need to match it bit-for-bit, only be
// self-consistent within one execution, so this is a simpler 8-byte-block
// FNV-1a fold with a murmur-style finalizer.
function hashBytes(bytes) {
  let h = 0xcbf29ce484222325n;
  const mul = 0x100000001b3n;
  const mask64 = (1n << 64n) - 1n;
  let i = 0;
  for (; i + 8 <= bytes.length; i += 8) {
    let word = 0n;
    for (let k = 0; k < 8; ++k) word |= BigInt(bytes[i + k]) << BigInt(8 * k);
    h = ((h ^ word) * mul) & mask64;
  }
  for (; i < bytes.length; ++i) {
    h = ((h ^ BigInt(bytes[i])) * mul) & mask64;
  }
  h ^= h >> 33n;
  h = (h * 0xff51afd7ed558ccdn) & mask64;
  h ^= h >> 33n;
  return BigInt.asIntN(64, h);
}

// compiler-rt 128-bit builtins for the wasm kernels: the i64-pair marshalling
// the wasm ABI requires, wrapped around plain BigInt arithmetic.
function bit128Builtins(getMemory) {
  const MASK64 = 0xFFFFFFFFFFFFFFFFn;
  const wide = (lo, hi) =>
    (BigInt.asIntN(64, BigInt(hi)) << 64n) | BigInt.asUintN(64, BigInt(lo));
  const wideU = (lo, hi) =>
    (BigInt.asUintN(64, BigInt(hi)) << 64n) | BigInt.asUintN(64, BigInt(lo));
  const store = (ptr, value) => {
    const bits = BigInt.asUintN(128, value);
    const dv = new DataView(getMemory().buffer);
    const at = Number(ptr);
    dv.setBigUint64(at, bits & MASK64, true);
    dv.setBigUint64(at + 8, bits >> 64n, true);
  };
  const shiftOf = (shift) => BigInt(Number(shift) & 127);

  return {
    __multi3: (r, aLo, aHi, bLo, bHi) =>
      store(r, wide(aLo, aHi) * wide(bLo, bHi)),
    // Division by zero is undefined in C; return zero rather than throwing out
    // of a wasm call, which would abort the whole query.
    __divti3: (r, aLo, aHi, bLo, bHi) => {
      const d = wide(bLo, bHi);
      store(r, d === 0n ? 0n : wide(aLo, aHi) / d);
    },
    __udivti3: (r, aLo, aHi, bLo, bHi) => {
      const d = wideU(bLo, bHi);
      store(r, d === 0n ? 0n : wideU(aLo, aHi) / d);
    },
    __modti3: (r, aLo, aHi, bLo, bHi) => {
      const d = wide(bLo, bHi);
      store(r, d === 0n ? 0n : wide(aLo, aHi) % d);
    },
    __umodti3: (r, aLo, aHi, bLo, bHi) => {
      const d = wideU(bLo, bHi);
      store(r, d === 0n ? 0n : wideU(aLo, aHi) % d);
    },
    __ashlti3: (r, lo, hi, shift) => store(r, wide(lo, hi) << shiftOf(shift)),
    __ashrti3: (r, lo, hi, shift) => store(r, wide(lo, hi) >> shiftOf(shift)),
    __lshrti3: (r, lo, hi, shift) => store(r, wideU(lo, hi) >> shiftOf(shift)),
    __floattidf: (lo, hi) => Number(wide(lo, hi)),
    __floatuntidf: (lo, hi) => Number(wideU(lo, hi)),
    __fixdfti: (r, value) =>
      store(r, Number.isFinite(value) ? BigInt(Math.trunc(value)) : 0n),
    __fixunsdfti: (r, value) =>
      store(r, Number.isFinite(value) && value > 0
        ? BigInt(Math.trunc(value))
        : 0n),
  };
}

// memcmp-style byte comparison matching qdb_filter_string_compare: -1/0/1 with
// length as tiebreak.
function compareBytes(a, b) {
  const n = Math.min(a.length, b.length);
  for (let i = 0; i < n; ++i) {
    if (a[i] !== b[i]) return a[i] < b[i] ? -1 : 1;
  }
  return a.length === b.length ? 0 : (a.length < b.length ? -1 : 1);
}

// SQL LIKE over bytes (case-sensitive, % = any run, _ = one byte, no escape),
// matching qdb_string_view_sql_like. Latin1 keeps one byte == one char.
function sqlLikeBytes(value, pattern) {
  let re = '^';
  for (const b of pattern) {
    const ch = String.fromCharCode(b);
    if (ch === '%') re += '.*';
    else if (ch === '_') re += '.';
    else re += ch.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
  }
  re += '$';
  let s = '';
  for (const b of value) s += String.fromCharCode(b);
  return new RegExp(re, 's').test(s) ? 1 : 0;
}

// Fast paths for constant LIKE patterns; mirror qdb_like_* in the native runtime.
function likeEquals(str, lit) {
  if (str.length !== lit.length) return 0;
  for (let i = 0; i < lit.length; ++i) if (str[i] !== lit[i]) return 0;
  return 1;
}

function likePrefix(str, lit) {
  if (str.length < lit.length) return 0;
  for (let i = 0; i < lit.length; ++i) if (str[i] !== lit[i]) return 0;
  return 1;
}

function likeSuffix(str, lit) {
  if (str.length < lit.length) return 0;
  const off = str.length - lit.length;
  for (let i = 0; i < lit.length; ++i) if (str[off + i] !== lit[i]) return 0;
  return 1;
}

// TypedArray.indexOf is native, so scan for the first byte and verify the rest.
function likeContains(str, lit) {
  if (lit.length === 0) return 1;
  if (str.length < lit.length) return 0;
  const last = str.length - lit.length;
  for (let i = str.indexOf(lit[0]); i >= 0 && i <= last; i = str.indexOf(lit[0], i + 1)) {
    let hit = 1;
    for (let j = 1; j < lit.length; ++j) if (str[i + j] !== lit[j]) { hit = 0; break; }
    if (hit) return 1;
  }
  return 0;
}

function divTrunc(n, d) {
  return Math.trunc(n / d);
}

function daysFromCivil(y, m, d) {
  y -= m <= 2 ? 1 : 0;
  const era = divTrunc(y >= 0 ? y : y - 399, 400);
  const yoe = y - era * 400;
  const doy = divTrunc(153 * (m + (m > 2 ? -3 : 9)) + 2, 5) + d - 1;
  const doe = yoe * 365 + divTrunc(yoe, 4) - divTrunc(yoe, 100) + doy;
  return (era * 146097 + doe - 719468) | 0;
}

function dateYear(days) {
  const z = (Number(days) | 0) + 719468;
  const era = divTrunc(z >= 0 ? z : z - 146096, 146097);
  const doe = z - era * 146097;
  const yoe = divTrunc(doe - divTrunc(doe, 1460) + divTrunc(doe, 36524) - divTrunc(doe, 146096), 365);
  let y = yoe + era * 400;
  const doy = doe - (365 * yoe + divTrunc(yoe, 4) - divTrunc(yoe, 100));
  const mp = divTrunc(5 * doy + 2, 153);
  y += mp >= 10 ? 1 : 0;
  return y | 0;
}

// The `env` imports the kernels need. Pointer + size args are i64 (BigInt
// under memory64); bool results are i32 (0/1), compare/like are i64.
// `getMemory` is read lazily since the instance doesn't exist yet at build
// time; `holder.arena` is bound by drivers whose kernels allocate
// (qdb_alloc/realloc/free — e.g. the aggregate hash table).
function createQdbEnv(getMemory, holder) {
  const textDecoder = new TextDecoder();
  const textEncoder = new TextEncoder();
  const alloc = (size) => {
    if (!holder.arena) {
      throw new Error('kernel called qdb_alloc but no allocator is bound');
    }
    return holder.arena.alloc(Math.max(Number(size), 1), 8);
  };
  const allocStringScratch = (size) => {
    const ptr = alloc(size);
    if (holder.stringScratchAllocs) {
      holder.stringScratchAllocs.push(ptr);
    }
    return ptr;
  };
  const bytesAt = (ptr, size) => {
    const mem = new Uint8Array(getMemory().buffer);
    const start = Number(ptr);
    return mem.subarray(start, start + Number(size));
  };
  const cstrAt = (ptr) => {
    const mem = new Uint8Array(getMemory().buffer);
    let start = Number(ptr);
    let end = start;
    while (mem[end] !== 0) ++end;
    return mem.subarray(start, end);
  };
  // A by-value struct argument crosses the wasm boundary as one pointer (byval),
  // and a struct result is written through the leading sret argument: qumir
  // coerces wasm external calls to the target C ABI, exactly like the native
  // build. Only structs declared as such in qumirdb.oz are affected —
  // qdb_filter_string_compare takes an explicit pointer/size pair and stays flat.
  const svBytes = (ptr) => {
    const dv = new DataView(getMemory().buffer);
    const at = Number(ptr);
    return bytesAt(readPointer(dv, at), dv.getBigInt64(at + 8, true));
  };
  // BinInt{Lo,Hi} is a 128-bit two's-complement value, little-endian.
  // Size-aware: CAST(<string column> AS DATE) passes a non-null-terminated StringView.
  const sqlDate = (str) => {
    const bytes = svBytes(str);
    const parts = [0, 0, 0];
    let idx = 0;
    for (const b of bytes) {
      if (idx >= 3) break;
      if (b === 45) {
        ++idx;
      } else if (b >= 48 && b <= 57) {
        parts[idx] = parts[idx] * 10 + (b - 48);
      }
    }
    return daysFromCivil(parts[0], parts[1], parts[2]);
  };
  // year/month are approximate (365/30). amount and unit are StringViews.
  const sqlInterval = (amount, unit) => {
    const bytes = svBytes(amount);
    let n = 0;
    let neg = false;
    for (const b of bytes) {
      if (b === 45) neg = true;
      else if (b >= 48 && b <= 57) n = n * 10 + (b - 48);
    }
    if (neg) n = -n;
    const bytesEqual = (bs, text) => {
      if (bs.length !== text.length) return false;
      for (let i = 0; i < bs.length; ++i) if (bs[i] !== text.charCodeAt(i)) return false;
      return true;
    };
    const u = svBytes(unit);
    if (bytesEqual(u, 'year') || bytesEqual(u, 'years')) return (n * 365) | 0;
    if (bytesEqual(u, 'month') || bytesEqual(u, 'months')) return (n * 30) | 0;
    return n | 0;
  };
  const substring = (out, str, start, length) => {
    const view = new DataView(getMemory().buffer);
    const data = readPointer(view, Number(str));
    const strSize = Number(view.getBigInt64(Number(str) + 8, true));
    let offset = Number(start) - 1;
    if (offset < 0) offset = 0;
    let len = Number(length);
    if (offset >= strSize) {
      offset = strSize;
      len = 0;
    } else {
      if (len < 0) len = 0;
      if (offset + len > strSize) len = strSize - offset;
    }
    const dv = new DataView(getMemory().buffer);
    writePointer(dv, Number(out), data + offset);
    dv.setBigInt64(Number(out) + 8, BigInt(len), true);
  };
  const stringConcat = (out, _arena, left, right) => {
    const args = new DataView(getMemory().buffer);
    const ld = readPointer(args, Number(left));
    const rd = readPointer(args, Number(right));
    const leftSize = Math.max(Number(args.getBigInt64(Number(left) + 8, true)), 0);
    const rightSize = Math.max(Number(args.getBigInt64(Number(right) + 8, true)), 0);
    const size = leftSize + rightSize;
    const dv = new DataView(getMemory().buffer);
    if (size <= 0) {
      writePointer(dv, Number(out), 0);
      dv.setBigInt64(Number(out) + 8, 0n, true);
      return;
    }
    const ptr = allocStringScratch(size);
    const mem = new Uint8Array(getMemory().buffer);
    let offset = ptr;
    if (leftSize > 0) {
      mem.set(mem.subarray(Number(ld), Number(ld) + leftSize), offset);
      offset += leftSize;
    }
    if (rightSize > 0) {
      mem.set(mem.subarray(Number(rd), Number(rd) + rightSize), offset);
    }
    const outView = new DataView(getMemory().buffer);
    writePointer(outView, Number(out), ptr);
    outView.setBigInt64(Number(out) + 8, BigInt(size), true);
  };
  const regexpReplace = (out, _arena, handle, str) => {
    const args = new DataView(getMemory().buffer);
    const data = readPointer(args, Number(str));
    const size = args.getBigInt64(Number(str) + 8, true);
    const program = holder.regexPrograms?.[Number(handle) - 1];
    if (!program) {
      throw new Error(`kernel used unknown regex handle ${String(handle)}`);
    }
    const input = textDecoder.decode(bytesAt(data, size));
    program.regex.lastIndex = 0;
    if (!program.regex.exec(input)) {
      const dv = new DataView(getMemory().buffer);
      writePointer(dv, Number(out), Number(data));
      dv.setBigInt64(Number(out) + 8, BigInt(size), true);
      return;
    }
    program.regex.lastIndex = 0;
    const encoded = textEncoder.encode(
      input.replace(program.regex, program.replacement));
    const ptr = encoded.length > 0 ? allocStringScratch(encoded.length) : 0;
    if (encoded.length > 0) {
      new Uint8Array(getMemory().buffer).set(encoded, ptr);
    }
    const dv = new DataView(getMemory().buffer);
    writePointer(dv, Number(out), ptr);
    dv.setBigInt64(Number(out) + 8, BigInt(encoded.length), true);
  };
  return {
    sqrt: Math.sqrt,
    sin: Math.sin,
    cos: Math.cos,
    fmod: (left, right) => left % right,

    // malloc family over the query's shared linear memory (segregated
    // free-list; free reclaims). qdb_realloc is not imported: it is implemented
    // in Oz (qumirdb.oz) on top of qdb_alloc/qdb_free.
    qdb_alloc: (size) => toWasmPtr(alloc(size)),
    qdb_free: (ptr) => holder.arena.free(Number(ptr)),

    // builtin::memcmp is declared as an external symbol rather than lowered to
    // an LLVM intrinsic, so wasm imports it. Returns i32, not i64.
    memcmp: (a, b, n) => {
      const len = Number(n);
      const x = bytesAt(a, len);
      const y = bytesAt(b, len);
      for (let i = 0; i < len; ++i) {
        if (x[i] !== y[i]) return x[i] < y[i] ? -1 : 1;
      }
      return 0;
    },

    // Decimal is an i128 and wasm has no 128-bit value type, so LLVM lowers
    // 128-bit arithmetic to compiler-rt calls: the value travels as an i64
    // pair, and a 128-bit result comes back through an sret pointer. The
    // signatures were read off the emitted wasm, not guessed. The whole family
    // is provided so a query shape reaching a different one cannot fail at
    // load time.
    ...bit128Builtins(getMemory),

    qdb_filter_string_compare: (ld, ls, rd, rs) =>
      BigInt(compareBytes(bytesAt(ld, ls), bytesAt(rd, rs))),
    qdb_string_hash_bytes: (ptr, size) => hashBytes(bytesAt(ptr, size)),
    // Pattern is a StringView (string literals are emitted as StringView).
    qdb_string_view_sql_like: (str, pattern) =>
      BigInt(sqlLikeBytes(svBytes(str), svBytes(pattern))),
    qdb_like_equals: (str, lit) =>
      BigInt(likeEquals(svBytes(str), svBytes(lit))),
    qdb_like_prefix: (str, lit) =>
      BigInt(likePrefix(svBytes(str), svBytes(lit))),
    qdb_like_suffix: (str, lit) =>
      BigInt(likeSuffix(svBytes(str), svBytes(lit))),
    qdb_like_contains: (str, lit) =>
      BigInt(likeContains(svBytes(str), svBytes(lit))),
    qdb_substring: substring,
    qdb_string_concat: stringConcat,
    qdb_regexp_replace: regexpReplace,
    qdb_date_year: dateYear,
    qdb_sql_date: sqlDate,
    qdb_sql_interval: sqlInterval,
    // SQL ROUND: round half away from zero (Math.round is half-up, so split the sign).
    qdb_round: (value, digits) => {
      const factor = Math.pow(10, digits);
      const x = value * factor;
      return (Math.sign(x) * Math.round(Math.abs(x))) / factor;
    },
    qdb_abs_i32: (value) => {
      value = Number(value) | 0;
      if (value === -2147483648) {
        throw new Error('abs overflow for i32');
      }
      return Math.abs(value) | 0;
    },
    qdb_abs_i64: (value) => {
      value = BigInt.asIntN(64, BigInt(value));
      if (value === -(1n << 63n)) {
        throw new Error('abs overflow for i64');
      }
      return value < 0n ? -value : value;
    },
    qdb_fabs: Math.abs,
    // Cast a string-literal char* to a StringView {Data = lit, Size = strlen}
    // (sret out pointer). Matches qdb_lit_to_sv in qumirdb_runtime.cpp.
    qdb_lit_to_sv: (out, lit) => {
      const bytes = cstrAt(lit);
      const dv = new DataView(getMemory().buffer);
      writePointer(dv, Number(out), lit);
      dv.setBigInt64(Number(out) + 8, BigInt(bytes.length), true);
    },
  };
}

// Compile + instantiate the query-level kernel module once, wiring the `env`
// runtime. Any import we do not implement is reported immediately.
async function instantiateQueryKernel(shared) {
  if (shared.kernelInstance) {
    return shared.kernelInstance;
  }
  if (!shared.wasm) {
    throw new Error('exec plan is missing query wasm module');
  }
  if (shared.kernelInstancePromise) {
    return shared.kernelInstancePromise;
  }
  const holder = { memory: shared.memory, arena: shared.arena };
  shared.kernelInstancePromise = (async () => {
    const module = await WebAssembly.compile(base64ToBytes(shared.wasm));
    const env = createQdbEnv(() => holder.memory, holder);
    env.memory = shared.memory;
    for (const imp of WebAssembly.Module.imports(module)) {
      if (imp.module !== 'env') {
        throw new Error(`kernel needs unsupported import: ${imp.module}.${imp.name}`);
      }
      if (imp.name === 'memory' && imp.kind === 'memory') {
        continue;
      }
      if (typeof env[imp.name] !== 'function') {
        throw new Error(`kernel needs unsupported import: ${imp.module}.${imp.name}`);
      }
    }
    const instance = await WebAssembly.instantiate(module, { env });
    if (instance.exports.__wasm_call_ctors) {
      instance.exports.__wasm_call_ctors();
    }
    shared.kernelInstance = { instance, holder };
    return shared.kernelInstance;
  })();
  return shared.kernelInstancePromise;
}

// Return one entry from the shared query-level wasm instance.
export async function instantiateKernel(entryName, shared) {
  const kernel = await instantiateQueryKernel(shared);
  const fn = kernel.instance.exports[entryName];
  if (typeof fn !== 'function') {
    throw new Error(`kernel is missing entry ${entryName}`);
  }
  return { ...kernel, fn };
}

function beginKernelStringScratch(kernel) {
  const holder = kernel?.holder;
  if (!holder?.arena) {
    throw new Error('kernel string scratch needs the query arena');
  }
  if (holder.stringScratchAllocs) {
    throw new Error('nested kernel string scratch invocation');
  }
  const allocations = [];
  holder.stringScratchAllocs = allocations;
  let released = false;
  return () => {
    if (released) return;
    released = true;
    holder.stringScratchAllocs = null;
    for (const ptr of allocations) {
      holder.arena.free(ptr);
    }
  };
}

function regexReplacement(replacement) {
  let result = '';
  for (let i = 0; i < replacement.length; ++i) {
    const ch = replacement[i];
    if (ch === '$') {
      result += '$$';
      continue;
    }
    if (ch !== '\\' || i + 1 === replacement.length) {
      result += ch;
      continue;
    }
    const escaped = replacement[++i];
    if (escaped >= '0' && escaped <= '9') {
      result += '$' + escaped;
      while (i + 1 < replacement.length &&
             replacement[i + 1] >= '0' && replacement[i + 1] <= '9') {
        result += replacement[++i];
      }
    } else if (escaped === '&') {
      result += '$&';
    } else {
      result += escaped;
    }
  }
  return result;
}

function bindKernelRegexes(kernel, stage) {
  const specs = Array.isArray(stage?.regexes) ? stage.regexes : [];
  if (specs.length === 0) {
    return { ...kernel, regexHandles: 0 };
  }
  const holder = kernel.holder;
  holder.regexPrograms ||= [];
  holder.regexProgramIds ||= new Map();
  holder.regexHandleTables ||= new Map();
  const handles = specs.map(spec => {
    const pattern = String(spec.pattern ?? '');
    const replacement = String(spec.replacement ?? '');
    const key = JSON.stringify([pattern, replacement]);
    let handle = holder.regexProgramIds.get(key);
    if (handle === undefined) {
      handle = holder.regexPrograms.length + 1;
      holder.regexPrograms.push({
        regex: new RegExp(pattern),
        replacement: regexReplacement(replacement),
      });
      holder.regexProgramIds.set(key, handle);
    }
    return handle;
  });
  const tableKey = handles.join(',');
  let table = holder.regexHandleTables.get(tableKey);
  if (table === undefined) {
    table = holder.arena.alloc(handles.length * 8, 8);
    const dv = holder.arena.view();
    for (let i = 0; i < handles.length; ++i) {
      dv.setBigUint64(table + i * 8, BigInt(handles[i]), true);
    }
    holder.regexHandleTables.set(tableKey, table);
  }
  return { ...kernel, regexHandles: table };
}

function stageEntrypoint(stage, name) {
  const entry = stage?.entrypoints?.[name];
  if (typeof entry !== 'string' || entry.length === 0) {
    throw new Error(`${stage?.kind || 'stage'} stage is missing entrypoint ${name}`);
  }
  return entry;
}

function kernelExport(kernel, stage, name) {
  const entry = stageEntrypoint(stage, name);
  const fn = kernel.instance.exports[entry];
  if (typeof fn !== 'function') {
    throw new Error(`${stage?.kind || 'stage'} kernel is missing entry ${entry}`);
  }
  return fn;
}

// Marshal `columns` (source column order) into a fresh, pinned TRowSet inside `arena`.
// Numeric columns get a data buffer; string columns get a concatenated UTF-8
// bytes buffer plus an i32 offsets array (OffsetWidth = 4), matching the layout
// the kernel reads (Data + Offsets[row]).
function marshalRowSet(arena, layout, columns, rowCount, wantSelection, selection = null) {
  const colLayout = layout.column;
  const rsLayout = layout.rowset;
  const encoder = new TextEncoder();

  // Pre-encode string columns before any allocation (a mid-write grow would
  // detach views): parts hold each row's bytes, offsets are cumulative.
  const encoded = columns.map(column => {
    if (!isStringColumn(column)) return null;
    return encodeStringColumn(column.values, rowCount, encoder);
  });

  const columnsBase = arena.alloc(columns.length * colLayout.size, 8);
  const dataPtrs = new Array(columns.length).fill(0);
  const offsetsPtrs = new Array(columns.length).fill(0);
  const masks = columns.map(column => columnValidityMask(column.values, rowCount));
  const maskPtrs = new Array(columns.length).fill(0);

  for (let c = 0; c < columns.length; ++c) {
    const column = columns[c];
    if (isStringColumn(column)) {
      dataPtrs[c] = arena.alloc(Math.max(encoded[c].total, 1), 1);
      offsetsPtrs[c] = arena.alloc((rowCount + 1) * 4, 4);
    } else {
      const width = coreTypeWidth(column);
      dataPtrs[c] = arena.alloc(Math.max(rowCount, 1) * width, 8);
    }
    if (masks[c]) {
      maskPtrs[c] = arena.alloc(masks[c].length, 8);
    }
  }

  const selectionPtr = (wantSelection || selection)
    ? arena.alloc(Math.max(rowCount, 1), 8)
    : 0;
  const rowsetPtr = arena.alloc(rsLayout.size, 8);

  // All allocations done; safe to take views and write.
  const dv = arena.view();
  const memBytes = arena.bytes();

  for (let c = 0; c < columns.length; ++c) {
    const column = columns[c];
    const colPtr = columnsBase + c * colLayout.size;
    new Uint8Array(arena.memory.buffer, colPtr, colLayout.size).fill(0);
    writePointer(dv, colPtr + colLayout.data, dataPtrs[c]);
    if (masks[c]) {
      memBytes.set(masks[c], maskPtrs[c]);
      writePointer(dv, colPtr + colLayout.mask, maskPtrs[c]);
    }
    if (isStringColumn(column)) {
      const enc = encoded[c];
      let p = dataPtrs[c];
      for (let i = 0; i < rowCount; ++i) {
        memBytes.set(enc.parts[i], p);
        p += enc.parts[i].length;
      }
      for (let i = 0; i <= rowCount; ++i) {
        dv.setInt32(offsetsPtrs[c] + i * 4, enc.offsets[i], true);
      }
      writePointer(dv, colPtr + colLayout.offsets, offsetsPtrs[c]);
      dv.setUint8(colPtr + colLayout.offsetWidth, 4);
    } else {
      const width = coreTypeWidth(column);
      const values = column.values;
      const bytes = typedColumnBytes(values, column, rowCount);
      if (bytes) {
        memBytes.set(bytes, dataPtrs[c]);
      } else {
        for (let i = 0; i < rowCount; ++i) {
          writeColumnValue(dv, dataPtrs[c], i, column, values[i]);
        }
      }
    }
  }

  new Uint8Array(arena.memory.buffer, rowsetPtr, rsLayout.size).fill(0);
  writePointer(dv, rowsetPtr + rsLayout.columns, columnsBase);
  dv.setBigInt64(rowsetPtr + rsLayout.columnCount, BigInt(columns.length), true);
  dv.setBigInt64(rowsetPtr + rsLayout.rowCount, BigInt(rowCount), true);
  if (selection) {
    copySelectionToMemory(arena, selectionPtr, selection, rowCount);
    writePointer(dv, rowsetPtr + rsLayout.selection, selectionPtr);
  } else if (wantSelection) {
    writePointer(dv, rowsetPtr + rsLayout.selection, selectionPtr);
  }

  return { rowsetPtr, selectionPtr, columnsBase };
}

function reusableFilterWasm(rowSet, pinned) {
  if (rowSet?.sharedView) {
    return null;
  }
  const wasm = wasmRowSetForSelection(rowSet.batch, rowSet.selection);
  return wasm && (wasm.pinned || !pinned) ? wasm : null;
}

// Run a filter kernel over `rowSet` (columns in source order). The kernel writes
// the result selection into the rowset's Selection buffer; JS carries only the
// pointer descriptor.
export function runFilter(kernel, layout, rowSet, writer, options = {}) {
  const pinned = options.pinned === true;
  const { batch, selection } = rowSet;
  const arena = kernel.holder.arena;
  const wasm = reusableFilterWasm(rowSet, pinned);
  let rowsetPtr;
  let selectionPtr;
  let destroy = null;
  if (wasm) {
    rowsetPtr = wasm.rowsetPtr;
    selectionPtr = ensureRowSetSelection(arena, layout, rowsetPtr, batch.rowCount);
    destroy = wasm.destroy || null;
  } else if (rowSet.sharedView && batch.wasm) {
    const alias = aliasWasmRowSetDescriptor(arena, layout, rowSet);
    if (!alias) {
      throw new Error('shared rowset view has no wasm descriptor');
    }
    rowsetPtr = alias.rowsetPtr;
    selectionPtr = ensureRowSetSelection(arena, layout, rowsetPtr, batch.rowCount);
    destroy = alias.destroy;
  } else if (pinned) {
    const marshalled = marshalRowSet(
      arena, layout, batch.columns, batch.rowCount, true, selection);
    rowsetPtr = marshalled.rowsetPtr;
    selectionPtr = marshalled.selectionPtr;
    destroy = () => freeMarshalledRowSet(arena, layout, rowsetPtr);
  } else {
    const written = writer.write(
      batch.columns, batch.rowCount, true, selection);
    rowsetPtr = written.rowsetPtr;
    selectionPtr = written.selectionPtr;
  }
  const releaseStringScratch = beginKernelStringScratch(kernel);
  try {
    // The last argument is the input mask; null makes the kernel evaluate
    // every row, as this runtime always did.
    kernel.fn(
      toWasmPtr(rowsetPtr), toWasmPtr(0), toWasmPtr(kernel.regexHandles || 0),
      toWasmPtr(0));
  } finally {
    releaseStringScratch();
  }
  attachBatchWasm(batch, rowsetPtr, {
    pinned: wasm ? wasm.pinned : pinned,
    selectionPtr,
    destroy,
  });
  return makeWasmSelection(arena, batch.rowCount, selectionPtr);
}

function stableProjectInput(rowSet, arena, layout) {
  const wasm = wasmRowSetForSelection(rowSet.batch, rowSet.selection);
  if (wasm?.pinned) {
    return {
      rowsetPtr: wasm.rowsetPtr,
      release: takeRowSetRelease(rowSet),
    };
  }
  if (wasm) {
    const alias = aliasWasmRowSetDescriptor(arena, layout, rowSet);
    if (!alias) {
      throw new Error('wasm project input has no rowset descriptor');
    }
    const releaseInput = takeRowSetRelease(rowSet);
    return {
      rowsetPtr: alias.rowsetPtr,
      release: () => {
        alias.destroy();
        if (releaseInput) {
          releaseInput();
        }
      },
    };
  }
  const marshalled = marshalRowSet(
    arena,
    layout,
    rowSet.batch.columns,
    rowSet.batch.rowCount,
    false,
    rowSet.selection);
  const releaseInput = takeRowSetRelease(rowSet);
  return {
    rowsetPtr: marshalled.rowsetPtr,
    release: () => {
      freeMarshalledRowSet(arena, layout, marshalled.rowsetPtr);
      if (releaseInput) {
        releaseInput();
      }
    },
  };
}

function writeEmptyColumn(dv, colPtr, layout) {
  new Uint8Array(dv.buffer, colPtr, layout.column.size).fill(0);
}

function buildProjectStringColumn(arena, layout, stringViewPtr, rowCount) {
  const sv = layout.stringView;
  let dv = arena.view();
  const views = new Array(rowCount);
  let total = 0;
  for (let r = 0; r < rowCount; ++r) {
    const base = stringViewPtr + r * sv.size;
    const dataPtr = readPointer(dv, base + sv.data);
    const size = Number(dv.getBigInt64(base + sv.length, true));
    views[r] = { dataPtr, size, offset: total };
    total += size;
  }

  const dataPtr = arena.alloc(Math.max(total, 1), 1);
  const offsetsPtr = arena.alloc((rowCount + 1) * 8, 8);
  dv = arena.view();
  const memBytes = arena.bytes();
  let writeAt = dataPtr;
  for (let r = 0; r < rowCount; ++r) {
    const view = views[r];
    dv.setBigInt64(offsetsPtr + r * 8, BigInt(view.offset), true);
    if (view.size > 0 && view.dataPtr) {
      memBytes.set(
        memBytes.subarray(view.dataPtr, view.dataPtr + view.size),
        writeAt);
      writeAt += view.size;
    }
  }
  dv.setBigInt64(offsetsPtr + rowCount * 8, BigInt(total), true);
  return { dataPtr, offsetsPtr, offsetWidth: 8 };
}

// Run a project kernel over `rowSet` and build an output TRowSet in wasm memory.
// Passthrough columns copy only TColumn descriptors; computed columns own their
// result buffers. The output destroy hook releases the retained input rowset.
export function runProject(kernel, layout, rowSet, output, arena) {
  arena = arena || kernel?.holder?.arena || rowSet.selection?.arena;
  if (!arena) {
    throw new Error('project needs shared wasm memory');
  }
  const rowCount = rowSet.batch.rowCount;
  const computed = output.filter(col => col.source === 'computed');
  if (computed.length > 0 && !kernel) {
    throw new Error('project stage is missing wasm kernel');
  }

  const input = stableProjectInput(rowSet, arena, layout);
  const ownedPtrs = [];
  let inputRelease = input.release;
  let rowsetPtr = 0;
  let columnsBase = 0;
  let outArrayPtr = 0;
  const computedBuffers = [];
  const computedMaskBuffers = [];
  let releaseStringScratch = null;

  try {
    if (computed.length > 0) {
      // `out` on the kernel side is a `ptr ptr u8` array. Each slot's
      // stride must be the real pointer width, not a fixed 8, or every
      // column after the first one lands at the wrong offset under wasm32.
      outArrayPtr = arena.alloc(computed.length * 2 * PointerSize, PointerSize);
      for (let k = 0; k < computed.length; ++k) {
        const width = Number(computed[k].width || 0);
        if (width <= 0) {
          throw new Error(`unsupported project column width: ${computed[k].name}`);
        }
        const ptr = arena.alloc(Math.max(rowCount, 1) * width, 8);
        computedBuffers.push(ptr);
        if (computed[k].nullable) {
          const maskPtr = arena.alloc(Math.max((rowCount + 7) >> 3, 1), 8);
          arena.bytes().fill(0, maskPtr, maskPtr + Math.max((rowCount + 7) >> 3, 1));
          computedMaskBuffers.push(maskPtr);
        } else {
          computedMaskBuffers.push(0);
        }
      }
      let dv = arena.view();
      for (let k = 0; k < computedBuffers.length; ++k) {
        writePointer(dv, outArrayPtr + k * PointerSize, computedBuffers[k]);
        if (computedMaskBuffers[k]) {
          writePointer(dv, outArrayPtr + (computed.length + k) * PointerSize, computedMaskBuffers[k]);
        }
      }
      if (input.rowsetPtr === undefined || outArrayPtr === undefined) {
        throw new Error('project kernel rowset/output pointer is missing');
      }
      releaseStringScratch = beginKernelStringScratch(kernel);
      kernel.fn(
        toWasmPtr(input.rowsetPtr), toWasmPtr(outArrayPtr), toWasmPtr(0),
        toWasmPtr(kernel.regexHandles || 0));
      arena.free(outArrayPtr);
      outArrayPtr = 0;
    }

    const computedColumns = computedBuffers.map((ptr, k) => {
      const spec = computed[k];
      if (!spec.isString) {
        ownedPtrs.push(ptr);
        if (computedMaskBuffers[k]) {
          ownedPtrs.push(computedMaskBuffers[k]);
        }
        return { dataPtr: ptr, offsetsPtr: 0, offsetWidth: 0, maskPtr: computedMaskBuffers[k] };
      }
      const stringColumn = buildProjectStringColumn(arena, layout, ptr, rowCount);
      arena.free(ptr);
      ownedPtrs.push(stringColumn.dataPtr, stringColumn.offsetsPtr);
      if (computedMaskBuffers[k]) {
        ownedPtrs.push(computedMaskBuffers[k]);
      }
      return { ...stringColumn, maskPtr: computedMaskBuffers[k] };
    });
    if (releaseStringScratch) {
      releaseStringScratch();
      releaseStringScratch = null;
    }

    rowsetPtr = arena.alloc(layout.rowset.size, 8);
    columnsBase = arena.alloc(output.length * layout.column.size, 8);
    ownedPtrs.push(columnsBase, rowsetPtr);

    let dv = arena.view();
    const memBytes = arena.bytes();
    const inputColumns = readPointer(dv, input.rowsetPtr + layout.rowset.columns);
    const selectionPtr = readPointer(dv, input.rowsetPtr + layout.rowset.selection);
    let computedCursor = 0;
    for (let i = 0; i < output.length; ++i) {
      const spec = output[i];
      const outCol = columnsBase + i * layout.column.size;
      if (spec.source === 'passthrough') {
        const inCol = inputColumns + Number(spec.inputIndex) * layout.column.size;
        memBytes.copyWithin(outCol, inCol, inCol + layout.column.size);
        continue;
      }
      writeEmptyColumn(dv, outCol, layout);
      const computedColumn = computedColumns[computedCursor++];
      writePointer(dv, outCol + layout.column.data, computedColumn.dataPtr);
      if (computedColumn.maskPtr) {
        writePointer(dv, outCol + layout.column.mask, computedColumn.maskPtr);
      }
      if (computedColumn.offsetsPtr) {
        writePointer(dv, outCol + layout.column.offsets, computedColumn.offsetsPtr);
        dv.setUint8(outCol + layout.column.offsetWidth, computedColumn.offsetWidth);
      }
    }

    new Uint8Array(arena.memory.buffer, rowsetPtr, layout.rowset.size).fill(0);
    dv = arena.view();
    writePointer(dv, rowsetPtr + layout.rowset.columns, columnsBase);
    dv.setBigInt64(rowsetPtr + layout.rowset.columnCount, BigInt(output.length), true);
    dv.setBigInt64(rowsetPtr + layout.rowset.rowCount, BigInt(rowCount), true);
    if (selectionPtr) {
      writePointer(dv, rowsetPtr + layout.rowset.selection, selectionPtr);
    }

    const batch = {
      rowCount,
      columns: shapeColumns(output),
    };
    const destroy = () => {
      for (const ptr of ownedPtrs) {
        arena.free(ptr);
      }
      if (inputRelease) {
        inputRelease();
        inputRelease = null;
      }
    };
    attachBatchWasm(batch, rowsetPtr, {
      pinned: true,
      selectionPtr,
      destroy,
    });
    return {
      batch,
      selection: makeWasmSelection(arena, rowCount, selectionPtr),
    };
  } catch (error) {
    if (releaseStringScratch) {
      releaseStringScratch();
    }
    if (outArrayPtr) {
      arena.free(outArrayPtr);
    }
    for (const ptr of computedBuffers) {
      arena.free(ptr);
    }
    for (const ptr of computedMaskBuffers) {
      arena.free(ptr);
    }
    for (const ptr of ownedPtrs) {
      arena.free(ptr);
    }
    if (inputRelease) {
      inputRelease();
    }
    throw error;
  }
}

// Read `count` validity bits (LSB-first per byte, qdb_bitmap_set_valid order).
function readValidityBits(memBytes, maskPtr, count, bitOffset = 0) {
  const valid = new Array(count);
  for (let i = 0; i < count; ++i) {
    const bit = i + bitOffset;
    valid[i] = (memBytes[maskPtr + (bit >> 3)] & (1 << (bit & 7))) !== 0;
  }
  return valid;
}

function materializeWasmRowSet(arena, layout, output, rowsetPtr) {
  const dv = arena.view();
  const memBytes = arena.bytes();
  const rowCount = Number(dv.getBigInt64(rowsetPtr + layout.rowset.rowCount, true));
  const columnsPtr = readPointer(dv, rowsetPtr + layout.rowset.columns);
  const decoder = new TextDecoder();
  const columns = [];

  for (let i = 0; i < output.length; ++i) {
    const spec = output[i];
    const colPtr = columnsPtr + i * layout.column.size;
    const dataPtr = readPointer(dv, colPtr + layout.column.data);
    const maskPtr = readPointer(dv, colPtr + layout.column.mask);
    const offsetsPtr = readPointer(dv, colPtr + layout.column.offsets);
    const offsetWidth = dv.getUint8(colPtr + layout.column.offsetWidth);
    const dataBitOffset = layout.column.dataBitOffset === undefined
      ? 0 : dv.getInt32(colPtr + layout.column.dataBitOffset, true);
    const maskBitOffset = layout.column.maskBitOffset === undefined
      ? 0 : dv.getInt32(colPtr + layout.column.maskBitOffset, true);
    const valid = maskPtr ? readValidityBits(memBytes, maskPtr, rowCount, maskBitOffset) : null;
    const values = new Array(rowCount);

    if (isStringColumn(spec)) {
      for (let r = 0; r < rowCount; ++r) {
        if (valid && !valid[r]) {
          values[r] = null;
          continue;
        }
        const begin = offsetWidth === 4
          ? dv.getInt32(offsetsPtr + r * 4, true)
          : Number(dv.getBigInt64(offsetsPtr + r * 8, true));
        const end = offsetWidth === 4
          ? dv.getInt32(offsetsPtr + (r + 1) * 4, true)
          : Number(dv.getBigInt64(offsetsPtr + (r + 1) * 8, true));
        values[r] = decoder.decode(memBytes.subarray(dataPtr + begin, dataPtr + end));
      }
    } else {
      for (let r = 0; r < rowCount; ++r) {
        if (valid && !valid[r]) {
          values[r] = null;
          continue;
        }
        const raw = readColumnValue(dv, dataPtr, r, spec, dataBitOffset);
        values[r] = isDecimalColumn(spec)
          ? formatDecimalValue(raw, decimalScale(spec))
          : raw;
      }
    }
    columns.push({ name: spec.name, type: spec.type, values });
  }

  const batch = { rowCount, columns };
  attachBatchWasm(batch, rowsetPtr, { pinned: true });
  return batch;
}

// Frees a kernel-materialized rowset: walks the owners list stored in Private
// (owners[0] = count, then one qdb_alloc'd pointer per buffer), frees each
// buffer, then the owners array and the rowset shell. Values must already be
// copied to JS (materializeWasmRowSet) — the wasm memory is dead afterward.
function destroyKernelOwnedRowSet(state, rowsetPtr) {
  const free = (p) => state.arena.free(p);
  const dv = state.arena.view();
  const ownersPtr = readPointer(dv, rowsetPtr + state.layout.rowset.private);
  if (ownersPtr) {
    const count = Number(dv.getBigInt64(ownersPtr, true));
    for (let i = 0; i < count; ++i) {
      free(readPointer(dv, ownersPtr + (i + 1) * 8));
    }
    free(ownersPtr);
  }
  free(rowsetPtr);
}

// Materializes one output batch from a join/cross pair buffer. When downstream
// can consume wasm rowsets, keep the kernel-owned TRowSet alive and return only
// a handle. Compatibility callers still get JS arrays and immediately free the
// wasm buffers.
function drainMaterializedBatch(
    state, maxRows, streamLeftPtr, streamRightPtr, asWasm = false) {
  const { layout, arena } = state;
  const rowsetPtr = arena.alloc(layout.rowset.size, 8);
  const leftPtr = toWasmPtr(state.leftStore.dataPtr());
  const rightPtr = toWasmPtr(state.rightStore.dataPtr());
  // For streamed pairs (batch index -1 on a side) jt_materialize reads that side
  // from the stream_* rowset rather than the store; buffered pairs ignore them.
  const streamLeft = streamLeftPtr !== undefined ? toWasmPtr(streamLeftPtr) : leftPtr;
  const streamRight = streamRightPtr !== undefined ? toWasmPtr(streamRightPtr) : rightPtr;
  const produced = Number(state.materialize(
    toWasmPtr(state.pairBuffer),
    leftPtr, rightPtr, streamLeft, streamRight,
    BigInt(state.materializeCursor),
    BigInt(maxRows),
    toWasmPtr(rowsetPtr)));
  if (produced <= 0) {
    arena.free(rowsetPtr);
    throw new Error('join materialize failed');
  }
  state.materializeCursor += produced;
  if (asWasm) {
    return makeWasmOwnedBatch(state, rowsetPtr, state.stage.output || []);
  }
  const batch = materializeWasmRowSet(
    arena, layout, state.stage.output || [], rowsetPtr);
  destroyKernelOwnedRowSet(state, rowsetPtr);
  delete batch.wasm; // buffers are freed; force a re-marshal downstream
  return batch;
}

// Drive the fused aggregate kernel. agg_dispatch updates the hash table;
// agg_finish_rowset owns measure/finalize/output buffer allocation and writes a
// complete TRowSet in linear memory.
export function runAggregate(kernel, layout, batch, selection, stage) {
  const state = createAggregateState(kernel, layout, stage);
  updateAggregateState(state, { batch, selection });
  return finishAggregateState(state);
}

function createAggregateState(kernel, layout, stage, instrumentation = null) {
  const dispatch = kernelExport(kernel, stage, 'agg_dispatch');
  const finishRowSet = kernelExport(kernel, stage, 'agg_finish_rowset');

  const arena = kernel.holder.arena; // qdb_alloc draws from the shared bump pointer

  const output = stage.output;

  // Browser execution contracts the native physical lanes into one semantic
  // aggregate, so the exported capacity is sized for one complete hash table.
  // aht_init requires a power-of-two capacity of at least 8: SwissTable groups
  // are 8 slots wide. Mirrors TAggregateProcessor on the native side.
  const requestedCapacity = Number.isSafeInteger(stage.initialCapacity)
      && stage.initialCapacity >= 8
    ? stage.initialCapacity
    : 8;
  let initialCapacity = 8;
  while (initialCapacity < requestedCapacity) initialCapacity *= 2;

  // init(ht, capacity)
  const ht = arena.alloc(layout.hashTable.size, 8);
  new Uint8Array(arena.memory.buffer, ht, layout.hashTable.size).fill(0);
  if (dispatch(toWasmPtr(ht), toWasmPtr(0), BigInt(initialCapacity), 0n) === 0n) {
    arena.free(ht);
    throw new Error('aggregate initialization failed');
  }

  instrumentation?.hash(ht, 0, false);
  return {
    instrumentation,
    kernel,
    layout,
    stage,
    dispatch,
    finishRowSet,
    arena,
    ht,
    output,
    inputWriter: new RowSetWriter(arena, layout),
    finished: false,
  };
}

function updateAggregateState(state, rowSet) {
  if (state.finished) {
    throw new Error('aggregate state is already finalized');
  }
  if (Array.isArray(state.stage.groupingSets) && state.stage.groupingSets.length > 0) {
    updateGroupingSetsAggregateState(state, rowSet);
    return;
  }
  const { batch, selection } = rowSet;
  const wasm = wasmRowSetForSelection(batch, selection);
  // update(ht, batch) — the kernel honors rowset.Selection.
  const rowsetPtr = wasm
    ? wasm.rowsetPtr
    : state.inputWriter.write(
        batch.columns, batch.rowCount, false, selection).rowsetPtr;
  if (state.dispatch(toWasmPtr(state.ht), toWasmPtr(rowsetPtr), 0n, 1n) < 0n) {
    throw new Error('aggregate update failed');
  }
}

function ensureGroupingSetScratch(state, rowCount) {
  const arena = state.arena;
  const idBytes = Math.max(rowCount, 1) * 4;
  if (!state.groupingSetIdPtr || state.groupingSetIdCap < idBytes) {
    if (state.groupingSetIdPtr) arena.free(state.groupingSetIdPtr);
    state.groupingSetIdPtr = arena.alloc(idBytes, 4);
    state.groupingSetIdCap = idBytes;
  }
  const maskBytes = Math.max((rowCount + 7) >> 3, 1);
  if (!state.groupingNullMaskPtr || state.groupingNullMaskCap < maskBytes) {
    if (state.groupingNullMaskPtr) arena.free(state.groupingNullMaskPtr);
    state.groupingNullMaskPtr = arena.alloc(maskBytes, 1);
    state.groupingNullMaskCap = maskBytes;
  }
  new Uint8Array(
    arena.memory.buffer,
    state.groupingNullMaskPtr,
    maskBytes).fill(0);
}

function writeGroupingSetIds(state, rowCount, setIndex) {
  const ids = new Int32Array(
    state.arena.memory.buffer,
    state.groupingSetIdPtr,
    Math.max(rowCount, 1));
  ids.fill(Number(setIndex));
}

function makeGroupingSetRowSet(state, baseRowsetPtr, set, setIndex) {
  const { arena, layout } = state;
  let dv = arena.view();
  const rowLayout = layout.rowset;
  const colLayout = layout.column;
  const baseColumns = readPointer(dv, baseRowsetPtr + rowLayout.columns);
  const baseColumnCount = Number(
    dv.getBigInt64(baseRowsetPtr + rowLayout.columnCount, true));
  const rowCount = Number(
    dv.getBigInt64(baseRowsetPtr + rowLayout.rowCount, true));
  const selectionPtr = readPointer(dv, baseRowsetPtr + rowLayout.selection);

  ensureGroupingSetScratch(state, rowCount);
  writeGroupingSetIds(state, rowCount, setIndex);

  const columnCount = baseColumnCount + 1;
  const columnsBase = arena.alloc(Math.max(columnCount, 1) * colLayout.size, 8);
  const rowsetPtr = arena.alloc(rowLayout.size, 8);
  dv = arena.view();
  const bytes = arena.bytes();
  new Uint8Array(arena.memory.buffer, columnsBase, columnCount * colLayout.size).fill(0);
  new Uint8Array(arena.memory.buffer, rowsetPtr, rowLayout.size).fill(0);

  writePointer(dv, columnsBase + colLayout.data, state.groupingSetIdPtr);
  const included = new Set((set || []).map(Number));
  const groupKeyCount = Number(state.stage.groupKeyCount || 0);
  for (let c = 0; c < baseColumnCount; ++c) {
    const src = baseColumns + c * colLayout.size;
    const dst = columnsBase + (c + 1) * colLayout.size;
    bytes.copyWithin(dst, src, src + colLayout.size);
    if (c < groupKeyCount && !included.has(c)) {
      writePointer(dv, dst + colLayout.mask, state.groupingNullMaskPtr);
    }
  }

  writePointer(dv, rowsetPtr + rowLayout.columns, columnsBase);
  dv.setBigInt64(rowsetPtr + rowLayout.columnCount, BigInt(columnCount), true);
  dv.setBigInt64(rowsetPtr + rowLayout.rowCount, BigInt(rowCount), true);
  if (selectionPtr) {
    writePointer(dv, rowsetPtr + rowLayout.selection, selectionPtr);
  }
  return { rowsetPtr, columnsBase };
}

function freeGroupingSetRowSet(state, view) {
  if (!view) return;
  state.arena.free(view.columnsBase);
  state.arena.free(view.rowsetPtr);
}

function updateGroupingSetsAggregateState(state, rowSet) {
  const { batch, selection } = rowSet;
  const wasm = wasmRowSetForSelection(batch, selection);
  let baseRowsetPtr;
  let marshalledPtr = 0;
  if (wasm) {
    baseRowsetPtr = wasm.rowsetPtr;
  } else {
    const marshalled = marshalRowSet(
      state.arena, state.layout, batch.columns, batch.rowCount, false, selection);
    baseRowsetPtr = marshalled.rowsetPtr;
    marshalledPtr = marshalled.rowsetPtr;
  }

  try {
    const sets = state.stage.groupingSets || [];
    for (let si = 0; si < sets.length; ++si) {
      const view = makeGroupingSetRowSet(state, baseRowsetPtr, sets[si], si);
      try {
        if (state.dispatch(
            toWasmPtr(state.ht), toWasmPtr(view.rowsetPtr), 0n, 1n) < 0n) {
          throw new Error('aggregate update failed');
        }
      } finally {
        freeGroupingSetRowSet(state, view);
      }
    }
  } finally {
    if (marshalledPtr) {
      freeMarshalledRowSet(state.arena, state.layout, marshalledPtr);
    }
  }
}

function finishAggregateState(state, asWasm = false) {
  if (state.finished) {
    throw new Error('aggregate state is already finalized');
  }
  state.finished = true;
  const {
    layout,
    finishRowSet,
    arena,
    ht,
    output,
  } = state;

  const expectedSize = Number(
    arena.view().getBigInt64(ht + layout.hashTable.sizeOffset, true));
  const rowsetPtr = arena.alloc(layout.rowset.size, 8);
  new Uint8Array(arena.memory.buffer, rowsetPtr, layout.rowset.size).fill(0);
  const finalized = Number(finishRowSet(toWasmPtr(ht), toWasmPtr(rowsetPtr)));
  if (finalized !== expectedSize) {
    throw new Error('aggregate finalize returned an unexpected row count');
  }
  if (asWasm) {
    return makeWasmOwnedBatch(state, rowsetPtr, output);
  }
  const batch = materializeWasmRowSet(arena, layout, output, rowsetPtr);
  destroyKernelOwnedRowSet(state, rowsetPtr);
  delete batch.wasm;
  return batch;
}

const JoinOp = Object.freeze({
  INIT: 0n,
  UPDATE_LEFT: 1n,
  UPDATE_RIGHT: 2n,
  STREAM_LEFT: 3n,
  STREAM_RIGHT: 4n,
  FINALIZE: 5n,
  DESTROY: 6n,
  FINALIZE_STREAM: 7n,
});

const JoinStreamMode = Object.freeze({
  SYMMETRIC: 'symmetric',
  STREAM_LEFT_AGAINST_RIGHT: 'stream-left-against-right',
  STREAM_RIGHT_AGAINST_LEFT: 'stream-right-against-left',
});

const BROWSER_JOIN_OUTPUT_BATCH_ROWS = 4096;

const CrossJoinOp = Object.freeze({
  EMIT: 0n,
  DESTROY: 1n,
});

function dispatchJoin(state, batch, batchIdx, leftStore, rightStore, arg, op) {
  return state.dispatch(
    toWasmPtr(state.leftTable),
    toWasmPtr(state.rightTable),
    toWasmPtr(batch),
    BigInt(batchIdx),
    toWasmPtr(state.pairBuffer),
    toWasmPtr(leftStore),
    toWasmPtr(rightStore),
    toWasmPtr(state.leftKeyColumns),
    toWasmPtr(state.rightKeyColumns),
    BigInt(arg),
    BigInt(op));
}

function createJoinState(kernel, layout, stage, instrumentation = null) {
  if (!browserRuntimeSupportsJoin(stage)) {
    throw new Error(`browser join type is not implemented: ${stage.joinType}` +
      (stage.hasResidual ? ' with residual' : ''));
  }
  const dispatch = kernelExport(kernel, stage, 'jt_dispatch');
  const materialize = kernelExport(kernel, stage, 'jt_materialize');

  const arena = kernel.holder.arena;
  const leftTable = arena.alloc(layout.hashTable.size, 8);
  const rightTable = arena.alloc(layout.hashTable.size, 8);
  const pairBuffer = arena.alloc(layout.pairBuffer.size, 8);
  const keys = stage.keys || [];
  if (keys.length === 0) {
    throw new Error('join stage has no key columns');
  }
  const leftKeyColumns = arena.alloc(keys.length * 8, 8);
  const rightKeyColumns = arena.alloc(keys.length * 8, 8);
  new Uint8Array(arena.memory.buffer, leftTable, layout.hashTable.size).fill(0);
  new Uint8Array(arena.memory.buffer, rightTable, layout.hashTable.size).fill(0);
  new Uint8Array(arena.memory.buffer, pairBuffer, layout.pairBuffer.size).fill(0);
  const dv = arena.view();
  for (let i = 0; i < keys.length; ++i) {
    dv.setBigInt64(leftKeyColumns + i * 8, BigInt(keys[i].leftIndex), true);
    dv.setBigInt64(rightKeyColumns + i * 8, BigInt(keys[i].rightIndex), true);
  }

  const state = {
    instrumentation,
    kernel,
    layout,
    stage,
    dispatch,
    materialize,
    arena,
    leftTable,
    rightTable,
    pairBuffer,
    leftKeyColumns,
    rightKeyColumns,
    leftStore: new WasmRowStore(arena, layout),
    rightStore: new WasmRowStore(arena, layout),
    // Reused marshalling buffer for the streamed probe-only side once one input
    // finishes. Mirrors TInnerJoinProcessor's StreamLeft/StreamRight modes.
    streamWriter: new RowSetWriter(arena, layout),
    materializeCursor: 0,
    semiAntiFinalized: false,
    outerFinalized: false,
    finalized: false,
  };
  if (!dispatchJoin(state, 0, 0, 0, 0, 256, JoinOp.INIT)) {
    throw new Error('join hash table initialization failed');
  }
  instrumentation?.hash(leftTable, 0, true);
  instrumentation?.hash(rightTable, 1, true);
  return state;
}

function isInnerJoin(stage) {
  return stage.joinType === 'inner';
}

// Streams one probe-only batch after the opposite side has finished. Pairs that
// reference the stream batch use batch index -1, so they must be materialized
// before the input rowset is released.
function streamJoinBatch(state, side, rowSet, asWasm = false) {
  const { arena, layout } = state;
  const { batch, selection } = rowSet;
  const wasm = wasmRowSetForSelection(batch, selection);
  const rowsetPtr = wasm
    ? wasm.rowsetPtr
    : state.streamWriter.write(
        batch.columns, batch.rowCount, false, selection).rowsetPtr;
  const isLeft = side === 0;
  try {
    const ok = dispatchJoin(
      state, rowsetPtr, -1, state.leftStore.dataPtr(), state.rightStore.dataPtr(),
      0, isLeft ? JoinOp.STREAM_LEFT : JoinOp.STREAM_RIGHT);
    if (!ok) {
      throw new Error('join stream failed');
    }
    const pairLayout = layout.pairBuffer;
    const count = Number(
      arena.view().getBigInt64(state.pairBuffer + pairLayout.count, true));
    const results = [];
    const streamLeftPtr = isLeft ? rowsetPtr : state.leftStore.dataPtr();
    const streamRightPtr = isLeft ? state.rightStore.dataPtr() : rowsetPtr;
    while (state.materializeCursor < count) {
      results.push(drainMaterializedBatch(
        state, BROWSER_JOIN_OUTPUT_BATCH_ROWS, streamLeftPtr, streamRightPtr, asWasm));
    }
    arena.view().setBigInt64(state.pairBuffer + pairLayout.count, 0n, true);
    state.materializeCursor = 0;
    return results;
  } finally {
    releaseWasmRowSet(rowSet);
  }
}

function browserRuntimeSupportsJoin(stage) {
  if (stage.joinType === 'inner') return true;
  if (stage.joinType === 'left' || stage.joinType === 'right') return true;
  if (stage.joinType === 'full' && !stage.hasResidual) return true;
  return isLeftSemiAntiJoin(stage);
}

function updateJoinState(state, side, rowSet) {
  const { arena, layout } = state;
  const { batch, selection } = rowSet;
  const wasm = wasmRowSetForSelection(batch, selection);
  const marshalled = wasm?.pinned
    ? null
    : marshalRowSet(
        arena, layout, batch.columns, batch.rowCount, false, selection);
  const rowsetPtr = wasm?.pinned ? wasm.rowsetPtr : marshalled.rowsetPtr;
  const isLeft = side === 0;
  const semiAnti = isLeftSemiAntiJoin(state.stage);
  let batchIdx = -1;
  let batchPtr = rowsetPtr;
  let stored = false;
  if (isLeft || !semiAnti) {
    const store = isLeft ? state.leftStore : state.rightStore;
    state.instrumentation?.retain(rowSet, side);
    batchIdx = store.push(
      batch,
      rowsetPtr,
      wasm?.pinned ? 0 : rowsetPtr,
      wasm?.pinned ? wasm : null);
    batchPtr = store.dataPtr() + batchIdx * layout.rowset.size;
    stored = true;
  }
  try {
    const ok = dispatchJoin(
      state, batchPtr, batchIdx, state.leftStore.dataPtr(), state.rightStore.dataPtr(),
      0, isLeft ? JoinOp.UPDATE_LEFT : JoinOp.UPDATE_RIGHT);
    if (!ok) {
      throw new Error('join kernel update failed');
    }
  } finally {
    if (marshalled) {
      releaseWasmRowSet(rowSet);
    }
    if (!stored) {
      if (marshalled) {
        freeMarshalledRowSet(arena, layout, marshalled.rowsetPtr);
      } else {
        releaseWasmRowSet(rowSet);
      }
    }
  }
}

function isLeftSemiAntiJoin(stage) {
  return stage.joinType === 'left_semi' ||
    stage.joinType === 'left_anti' ||
    stage.joinType === 'left-semi' ||
    stage.joinType === 'left-anti';
}

function isOuterJoin(stage) {
  return stage.joinType === 'left' ||
    stage.joinType === 'right' ||
    stage.joinType === 'full';
}

function streamsOuterJoin(stage) {
  return (stage.joinType === 'left' && stage.buildSide === 'left') ||
    (stage.joinType === 'right' && stage.buildSide === 'right');
}

function finalizeSemiAntiJoinState(state) {
  if (state.semiAntiFinalized) {
    return;
  }
  const ok = dispatchJoin(
    state, 0, 0, state.leftStore.dataPtr(), state.rightStore.dataPtr(),
    state.leftStore.batches.length, JoinOp.FINALIZE);
  if (!ok) {
    throw new Error('join semi/anti finalize failed');
  }
  state.semiAntiFinalized = true;
}

// Outer join: after both sides are consumed, emit own-side rows that never
// matched, with kNullRowId on the opposite side. jt_dispatch leaves every pair
// in canonical (left_id, right_id) order, including RIGHT joins.
function finalizeOuterJoinState(state) {
  if (state.outerFinalized) {
    return;
  }
  const ok = dispatchJoin(
    state, 0, 0, state.leftStore.dataPtr(), state.rightStore.dataPtr(),
    0, streamsOuterJoin(state.stage) ? JoinOp.FINALIZE_STREAM : JoinOp.FINALIZE);
  if (!ok) {
    throw new Error('join outer finalize failed');
  }
  state.outerFinalized = true;
}

function drainJoinPairs(state, maxRows = BROWSER_JOIN_OUTPUT_BATCH_ROWS, asWasm = false) {
  const pairLayout = state.layout.pairBuffer;
  const dv = state.arena.view();
  const count = Number(dv.getBigInt64(state.pairBuffer + pairLayout.count, true));
  if (state.materializeCursor >= count) {
    return [];
  }
  const batch = drainMaterializedBatch(
    state, maxRows, undefined, undefined, asWasm);
  if (state.materializeCursor >= count) {
    state.arena.view().setBigInt64(
      state.pairBuffer + pairLayout.count, 0n, true);
    state.materializeCursor = 0;
  }
  return [batch];
}

function finishJoinState(state) {
  if (state.finalized) {
    return;
  }
  state.finalized = true;
  dispatchJoin(state, 0, 0, 0, 0, 0, JoinOp.DESTROY);
  // Reclaim the build-side row store so it doesn't linger for the rest of the
  // query (upstream joins finish before the final one peaks).
  state.leftStore.freeMarshalled();
  state.rightStore.freeMarshalled();
  state.instrumentation?.releaseRetained();
  state.arena.free(state.leftKeyColumns);
  state.arena.free(state.rightKeyColumns);
}

function createCrossJoinState(kernel, layout, stage, instrumentation = null) {
  const dispatch = kernelExport(kernel, stage, 'xj_dispatch');
  const materialize = kernelExport(kernel, stage, 'jt_materialize');

  const arena = kernel.holder.arena;
  const pairBuffer = arena.alloc(layout.pairBuffer.size, 8);
  new Uint8Array(arena.memory.buffer, pairBuffer, layout.pairBuffer.size).fill(0);
  return {
    instrumentation,
    kernel,
    layout,
    stage,
    dispatch,
    materialize,
    arena,
    pairBuffer,
    leftStore: new WasmRowStore(arena, layout),
    rightStore: new WasmRowStore(arena, layout),
    materializeCursor: 0,
    rightRows: 0,
    finalized: false,
  };
}

function stableRowSetForStore(state, rowSet) {
  const { arena, layout } = state;
  const { batch, selection } = rowSet;
  const wasm = wasmRowSetForSelection(batch, selection);
  if (wasm?.pinned) {
    return {
      rowsetPtr: wasm.rowsetPtr,
      ownedPtr: 0,
      wasmHandle: wasm,
      releaseInput: false,
    };
  }
  const marshalled = marshalRowSet(
    arena, layout, batch.columns, batch.rowCount, false, selection);
  return {
    rowsetPtr: marshalled.rowsetPtr,
    ownedPtr: marshalled.rowsetPtr,
    wasmHandle: null,
    releaseInput: true,
  };
}

function updateCrossRightState(state, rowSet) {
  const { rowsetPtr, ownedPtr, wasmHandle, releaseInput } =
    stableRowSetForStore(state, rowSet);
  state.instrumentation?.retain(rowSet, 1);
  state.rightStore.push(rowSet.batch, rowsetPtr, ownedPtr, wasmHandle);
  if (releaseInput) {
    releaseWasmRowSet(rowSet);
  }
  state.rightRows += rowSet.batch.rowCount;
}

function updateCrossLeftState(state, rowSet) {
  if (state.rightRows === 0) {
    releaseWasmRowSet(rowSet);
    return;
  }
  const { rowsetPtr, ownedPtr, wasmHandle, releaseInput } =
    stableRowSetForStore(state, rowSet);
  state.instrumentation?.retain(rowSet, 0);
  const batchIdx = state.leftStore.push(rowSet.batch, rowsetPtr, ownedPtr, wasmHandle);
  try {
    // Plan export sends only inner cross joins here, which flag no right rows.
    const ok = state.dispatch(
      toWasmPtr(state.leftStore.dataPtr()),
      BigInt(batchIdx),
      toWasmPtr(state.rightStore.dataPtr()),
      BigInt(state.rightStore.batches.length),
      toWasmPtr(0),
      toWasmPtr(state.pairBuffer),
      CrossJoinOp.EMIT);
    if (!ok) {
      throw new Error('cross join kernel update failed');
    }
  } finally {
    if (releaseInput) {
      releaseWasmRowSet(rowSet);
    }
  }
}

function drainCrossJoinPairs(state, maxRows = BROWSER_JOIN_OUTPUT_BATCH_ROWS, asWasm = false) {
  const pairLayout = state.layout.pairBuffer;
  const dv = state.arena.view();
  const count = Number(dv.getBigInt64(state.pairBuffer + pairLayout.count, true));
  if (state.materializeCursor >= count) {
    return [];
  }
  const batch = drainMaterializedBatch(
    state, maxRows, undefined, undefined, asWasm);
  if (state.materializeCursor >= count) {
    state.arena.view().setBigInt64(
      state.pairBuffer + pairLayout.count, 0n, true);
    state.materializeCursor = 0;
  }
  return [batch];
}

function finishCrossJoinState(state) {
  if (state.finalized) {
    return;
  }
  state.finalized = true;
  state.dispatch(
    toWasmPtr(0),
    0n,
    toWasmPtr(0),
    0n,
    toWasmPtr(0),
    toWasmPtr(state.pairBuffer),
    CrossJoinOp.DESTROY);
  state.leftStore.freeMarshalled();
  state.rightStore.freeMarshalled();
  state.instrumentation?.releaseRetained();
}

function outputShapeFromRowSets(rowSets) {
  const first = rowSets.find(rowSet => rowSet.batch.columns.length > 0);
  return first ? first.batch.columns.map(col => ({
    name: col.name,
    type: col.type,
  })) : [];
}

function emptyBatchForRowSets(rowSets) {
  return emptyBatchForOutput(outputShapeFromRowSets(rowSets));
}

function emptyBatchForOutput(output) {
  return {
    rowCount: 0,
    columns: shapeColumns(output).map(col => ({
      name: col.name,
      type: col.type,
      storageType: col.storageType,
      precision: col.precision,
      scale: col.scale,
      nullable: col.nullable,
      values: [],
    })),
  };
}

function selectedRowCount(rowSets) {
  let count = 0;
  for (const rowSet of rowSets) {
    const { batch, selection } = rowSet;
    if (!selection) {
      count += batch.rowCount;
      continue;
    }
    for (let row = 0; row < batch.rowCount; ++row) {
      if (rowSelected(selection, row)) ++count;
    }
  }
  return count;
}

function stableSortRowSetForStore(arena, layout, rowSet) {
  const { batch, selection } = rowSet;
  const wasm = wasmRowSetForSelection(batch, selection);
  if (wasm?.pinned) {
    return {
      rowsetPtr: wasm.rowsetPtr,
      ownedPtr: 0,
      wasmHandle: wasm,
      releaseInput: false,
    };
  }
  const marshalled = marshalRowSet(
    arena, layout, batch.columns, batch.rowCount, false, selection);
  return {
    rowsetPtr: marshalled.rowsetPtr,
    ownedPtr: marshalled.rowsetPtr,
    wasmHandle: null,
    releaseInput: true,
  };
}

function runSortKernelRowSets(
  kernel,
  layout,
  rowSets,
  radixKeys,
  limit,
  outputShape = null) {
  const arena = kernel.holder.arena;
  const output = outputShape || outputShapeFromRowSets(rowSets);

  const n = selectedRowCount(rowSets);
  const keyCount = radixKeys.length;
  const nullable = !!kernel.nullable;
  const workStride = radixKeys.some(key => key.isString) ? 4 : 1;
  const store = new WasmRowStore(arena, layout);
  const storeBatchIndexes = new Array(rowSets.length);
  const releaseInputs = [];

  for (let b = 0; b < rowSets.length; ++b) {
    const rowSet = rowSets[b];
    const stable = stableSortRowSetForStore(arena, layout, rowSet);
    if (stable.releaseInput) {
      releaseInputs.push(rowSet);
    }
    storeBatchIndexes[b] = store.push(
      rowSet.batch, stable.rowsetPtr, stable.ownedPtr, stable.wasmHandle);
  }

  if (n === 0) {
    for (const rowSet of releaseInputs) {
      releaseWasmRowSet(rowSet);
    }
    store.freeMarshalled();
    return emptyBatchForOutput(output);
  }
  const keep = (limit != null && limit < n)
    ? Math.max(Number(limit), 0)
    : n;
  if (keep === 0) {
    for (const rowSet of releaseInputs) {
      releaseWasmRowSet(rowSet);
    }
    store.freeMarshalled();
    return emptyBatchForOutput(output);
  }

  // Allocate everything up front (a mid-write grow would detach views).
  const rowIdsPtr = arena.alloc(Math.max(n, 1) * 8, 8);
  const workPtr = arena.alloc(Math.max(n, 1) * workStride * 8, 8);
  const countsPtr = arena.alloc((nullable ? 257 : 256) * 4, 4);
  const descsPtr = arena.alloc(Math.max(keyCount, 1), 1);
  const nullsFirstsPtr = arena.alloc(Math.max(keyCount, 1), 1);
  const outRowSetPtr = arena.alloc(layout.rowset.size, 8);
  let outRowSetTransferred = false;

  const dv = arena.view();
  for (let k = 0; k < keyCount; ++k) {
    const key = radixKeys[k];
    dv.setUint8(descsPtr + k, key.desc ? 1 : 0);
    dv.setUint8(nullsFirstsPtr + k, key.nullsFirst ? 1 : 0);
  }

  let out = 0;
  for (let b = 0; b < rowSets.length; ++b) {
    const rowSet = rowSets[b];
    const { batch, selection } = rowSet;
    for (let row = 0; row < batch.rowCount; ++row) {
      if (!rowSelected(selection, row)) continue;
      dv.setBigInt64(rowIdsPtr + out * 8, makeRowId(storeBatchIndexes[b], row), true);
      ++out;
    }
  }

  try {
    // wasm64: every pointer/count is an i64 param. wasm32 narrows pointer
    // params to i32 (doSort stays a plain i32 bool on both — see
    // TSortRadixCompositeDispatch in qdb/kernel/compiler.h).
    const out = Number(kernel.fn(
      toWasmPtr(store.dataPtr()),
      toWasmPtr(rowIdsPtr),
      toWasmPtr(workPtr),
      toWasmPtr(countsPtr),
      BigInt(n),
      toWasmPtr(descsPtr),
      toWasmPtr(nullsFirstsPtr),
      1,
      0n,
      BigInt(keep),
      toWasmPtr(outRowSetPtr)));
    if (out <= 0) {
      return emptyBatchForOutput(output);
    }
    outRowSetTransferred = true;
    return makeWasmOwnedBatch(
      { arena, layout },
      outRowSetPtr,
      output);
  } finally {
    arena.free(rowIdsPtr);
    arena.free(workPtr);
    arena.free(countsPtr);
    arena.free(descsPtr);
    arena.free(nullsFirstsPtr);
    if (!outRowSetTransferred) {
      arena.free(outRowSetPtr);
    }
    for (const rowSet of releaseInputs) {
      releaseWasmRowSet(rowSet);
    }
    store.freeMarshalled();
  }
}

function runRadixSortRowSets(kernel, layout, rowSets, radixKeys, limit) {
  try {
    return runSortKernelRowSets(kernel, layout, rowSets, radixKeys, limit);
  } finally {
    for (const rowSet of rowSets) {
      releaseWasmRowSet(rowSet);
    }
  }
}

// Drive the radix composite sort kernel. The kernel sorts a row-id permutation
// in place and reads key values directly from marshaled TRowSet storage,
// matching the native sort path.
export function runRadixSort(kernel, layout, batch, selection, radixKeys, limit) {
  return runRadixSortRowSets(
    kernel, layout, [{ batch, selection: selection || null }], radixKeys, limit);
}

function runWindowKernelRowSets(kernel, layout, rowSets, stage) {
  try {
    return runSortKernelRowSets(
      kernel,
      layout,
      rowSets,
      stage.radixKeys,
      null,
      stage.output || []);
  } finally {
    for (const rowSet of rowSets) {
      releaseWasmRowSet(rowSet);
    }
  }
}

const TaskResult = {
  OK: 'OK',
  NEED_DATA: 'NEED_DATA',
  BLOCKED_OUTPUT: 'BLOCKED_OUTPUT',
  FINISHED: 'FINISHED',
};

const FetchResult = {
  OK: 'OK',
  NO_DATA: 'NO_DATA',
  FINISHED: 'FINISHED',
};

const ConnectionKind = {
  OneToOne: 'one-to-one',
  Gather: 'gather',
  HashShuffle: 'hash-shuffle',
  Broadcast: 'broadcast',
};

class OneToOneConnection {
  constructor(capacity = 1) {
    this.kind = ConnectionKind.OneToOne;
    this.capacity = capacity;
    this.queue = [];
    this.finished = false;
    this.stats = {
      pushed: 0,
      popped: 0,
      finished: 0,
      blockedPush: 0,
      emptyFetch: 0,
      finishedFetch: 0,
    };
  }

  canPush() {
    return this.queue.length < this.capacity;
  }

  push(rowSet) {
    if (!this.canPush()) {
      ++this.stats.blockedPush;
      return false;
    }
    this.queue.push(rowSet);
    ++this.stats.pushed;
    return true;
  }

  finish() {
    if (!this.finished) {
      this.finished = true;
      ++this.stats.finished;
    }
  }

  fetch() {
    if (this.queue.length > 0) {
      ++this.stats.popped;
      return { result: FetchResult.OK, rowSet: this.queue.shift() };
    }
    if (this.finished) {
      ++this.stats.finishedFetch;
      return { result: FetchResult.FINISHED, rowSet: null };
    }
    ++this.stats.emptyFetch;
    return { result: FetchResult.NO_DATA, rowSet: null };
  }

  destroy() {
    while (this.queue.length > 0) {
      releaseWasmRowSet(this.queue.shift());
    }
    this.finished = true;
  }
}

class UnsupportedConnection {
  constructor(kind) {
    this.kind = kind;
    this.stats = {
      pushed: 0,
      popped: 0,
      finished: 0,
      blockedPush: 0,
      emptyFetch: 0,
      finishedFetch: 0,
    };
  }

  unsupported() {
    throw new Error(`browser scheduler connection is not implemented: ${this.kind}`);
  }

  canPush() { return this.unsupported(); }
  push() { return this.unsupported(); }
  finish() { return this.unsupported(); }
  fetch() { return this.unsupported(); }
  destroy() {}
}

class GatherConnection extends UnsupportedConnection {
  constructor() {
    super(ConnectionKind.Gather);
  }
}

class HashShuffleConnection extends UnsupportedConnection {
  constructor() {
    super(ConnectionKind.HashShuffle);
  }
}

class BroadcastConnection extends UnsupportedConnection {
  constructor() {
    super(ConnectionKind.Broadcast);
  }
}

function createConnection(kind = ConnectionKind.OneToOne, options = {}) {
  switch (kind) {
    case ConnectionKind.OneToOne:
      return new OneToOneConnection(options.capacity || 1);
    case ConnectionKind.Gather:
      return new GatherConnection();
    case ConnectionKind.HashShuffle:
      return new HashShuffleConnection();
    case ConnectionKind.Broadcast:
      return new BroadcastConnection();
    default:
      throw new Error(`unknown browser scheduler connection kind: ${kind}`);
  }
}

class SchedulerNode {
  constructor(task, label) {
    this.task = task;
    this.label = label;
    this.inbound = [];
    this.outbound = [];
    this.elapsedMs = 0;
    this.rows = 0;
  }
}

class SingleThreadedScheduler {
  constructor(roots) {
    this.roots = Array.isArray(roots) ? roots : [roots];
    this.ready = [];
    this.scheduled = new Set();
    this.stats = {
      scheduled: 0,
      popped: 0,
      executed: 0,
      needData: 0,
      blockedOutput: 0,
      ok: 0,
      finished: 0,
    };
  }

  async run() {
    for (const root of this.roots) {
      this.schedule(root);
    }
    while (this.ready.length > 0) {
      const node = this.ready.shift();
      this.scheduled.delete(node);
      ++this.stats.popped;

      const started = nowMs();
      let state;
      try {
        state = await node.task.execute();
      } catch (error) {
        const message = error?.message || String(error);
        const stats = node.task?.shared?.arena?.stats?.();
        const suffix = stats
          ? ` (wasm reserved ${stats.reservedMB}MB, peak live ${stats.peakLiveMB}MB)`
          : '';
        const wrapped = new Error(`${node.label}: ${message}${suffix}`);
        if (error?.stack) {
          wrapped.stack = `${wrapped.message}\n${error.stack}`;
        }
        throw wrapped;
      }
      node.elapsedMs += nowMs() - started;
      node.rows = node.task.rows || node.rows || 0;
      ++this.stats.executed;

      if (state === TaskResult.NEED_DATA) {
        ++this.stats.needData;
        this.scheduleInput(node);
      } else if (state === TaskResult.BLOCKED_OUTPUT || state === TaskResult.FINISHED) {
        if (state === TaskResult.BLOCKED_OUTPUT) {
          ++this.stats.blockedOutput;
        } else {
          ++this.stats.finished;
        }
        this.scheduleOutput(node);
      } else if (state === TaskResult.OK) {
        ++this.stats.ok;
        this.schedule(node);
        this.scheduleOutput(node);
      }
    }
  }

  schedule(node) {
    if (!node || this.scheduled.has(node)) {
      return;
    }
    this.ready.push(node);
    this.scheduled.add(node);
    ++this.stats.scheduled;
  }

  scheduleInput(node) {
    for (const edge of node.inbound) {
      this.schedule(edge.src);
    }
  }

  scheduleOutput(node) {
    for (const edge of node.outbound) {
      this.schedule(edge.dst);
    }
  }
}

function connect(src, dst, connection = new OneToOneConnection(1), options = {}) {
  const edge = { src, dst, connection, persistent: options.persistent === true };
  src.outbound.push(edge);
  dst.inbound.push(edge);
  return edge;
}

function assertStreamingWasmEdge(edge) {
  if (edge.connection.kind !== ConnectionKind.OneToOne || edge.connection.capacity !== 1) {
    throw new Error('streaming wasm rowsets require a one-slot one-to-one edge');
  }
}

function canConsumeWasmOutput(task) {
  if (!task) {
    return false;
  }
  if (task instanceof SinkTask) {
    return true;
  }
  const kind = task.stage?.kind;
  return kind === 'filter' ||
    kind === 'project' ||
    kind === 'aggregate' ||
    kind === 'limit' ||
    kind === 'cte-producer' ||
    kind === 'union-all' ||
    kind === 'window' ||
    kind === 'join' ||
    kind === 'cross-join' ||
    kind === 'sort' ||
    kind === 'top-sort';
}

function downstreamConsumesWasm(node) {
  return canConsumeWasmOutput(node?.outbound?.[0]?.dst?.task);
}

class SourceTask {
  constructor(stage, layout, shared, readSourceBatches, onProgress) {
    this.stage = stage;
    this.layout = layout;
    this.shared = shared;
    this.readSourceBatches = readSourceBatches;
    this.onProgress = onProgress;
    this.iterator = null;
    this.done = false;
    this.rows = 0;
  }

  async execute() {
    if (this.done) {
      return TaskResult.FINISHED;
    }
    const out = this.outbound.connection;
    if (!out.canPush()) {
      return TaskResult.BLOCKED_OUTPUT;
    }
    if (!this.iterator) {
      this.iterator = this.readSourceBatches(this.stage, this.onProgress, {
        layout: this.layout,
        shared: this.shared,
      })[Symbol.asyncIterator]();
    }
    const next = await this.iterator.next();
    if (next.done) {
      out.finish();
      this.done = true;
      return TaskResult.FINISHED;
    }
    const batch = pinSourceBatch(
      next.value, this.shared.arena, this.layout);
    this.rows += batch.rowCount;
    out.push({ batch, selection: null });
    return TaskResult.OK;
  }

  get outbound() {
    return this.node.outbound[0];
  }
}

class FilterTask {
  constructor(stage, layout, shared) {
    this.stage = stage;
    this.layout = layout;
    this.shared = shared;
    this.writer = new RowSetWriter(shared.arena, layout);
    this.kernel = null;
    this.done = false;
    this.rows = 0;
  }

  async execute() {
    if (this.done) return TaskResult.FINISHED;
    const input = this.node.inbound[0].connection;
    const outEdge = this.node.outbound[0];
    const output = outEdge.connection;
    if (!output.canPush()) return TaskResult.BLOCKED_OUTPUT;

    const fetched = input.fetch();
    if (fetched.result === FetchResult.NO_DATA) return TaskResult.NEED_DATA;
    if (fetched.result === FetchResult.FINISHED) {
      output.finish();
      this.done = true;
      return TaskResult.FINISHED;
    }
    if (!this.kernel) {
      this.kernel = bindKernelRegexes(
        await instantiateKernel(
          stageEntrypoint(this.stage, 'filter'), this.shared),
        this.stage);
    }
    assertStreamingWasmEdge(outEdge);
    const reusesInput = reusableFilterWasm(
      fetched.rowSet, outEdge.persistent) !== null;
    const releaseInput = reusesInput ? null : takeRowSetRelease(fetched.rowSet);
    let selection = null;
    try {
      selection = runFilter(
        this.kernel,
        this.layout,
        fetched.rowSet,
        this.writer,
        { pinned: outEdge.persistent });
    } finally {
      if (releaseInput) {
        releaseInput();
      }
    }
    this.rows += fetched.rowSet.batch.rowCount;
    // Filter pruning (keptColumns) drops predicate-only columns so the layout
    // matches what the kernels above were compiled for. Narrow the wasm rowset —
    // column data stays in wasm (the JS .values may be absent) — mirroring
    // MakeFilterSelectProcess. The narrowed descriptor owns only its own rowset/
    // columns/selection allocations; the shared column data lives as long as the
    // input batch, exactly as on the non-narrowed pass-through path.
    let batch = fetched.rowSet.batch;
    let outSelection = selection;
    const kept = this.stage.keptColumns;
    if (Array.isArray(kept)) {
      const narrowed = narrowWasmRowSetDescriptor(
        this.shared.arena, this.layout, batch, kept, selection);
      batch = { ...batch };
      if (Array.isArray(fetched.rowSet.batch.columns)) {
        batch.columns = kept.map(i => fetched.rowSet.batch.columns[i]);
      }
      if (narrowed) {
        // pinned: the narrowed rowset carries the data in wasm and has no JS
        // .values, so it must be consumed directly, never re-marshalled.
        attachBatchWasm(batch, narrowed.rowsetPtr, {
          pinned: true,
          selectionPtr: narrowed.selectionPtr,
          destroy: narrowed.destroy,
        });
        outSelection = makeWasmSelection(
          this.shared.arena, narrowed.rowCount, narrowed.selectionPtr);
      } else {
        delete batch.wasm;
      }
    }
    output.push({ batch, selection: outSelection });
    return TaskResult.OK;
  }
}

class ProjectTask {
  constructor(stage, layout, shared) {
    this.stage = stage;
    this.layout = layout;
    this.shared = shared;
    this.kernel = null;
    this.done = false;
    this.rows = 0;
  }

  async execute() {
    if (this.done) return TaskResult.FINISHED;
    const input = this.node.inbound[0].connection;
    const output = this.node.outbound[0].connection;
    if (!output.canPush()) return TaskResult.BLOCKED_OUTPUT;

    const fetched = input.fetch();
    if (fetched.result === FetchResult.NO_DATA) return TaskResult.NEED_DATA;
    if (fetched.result === FetchResult.FINISHED) {
      output.finish();
      this.done = true;
      return TaskResult.FINISHED;
    }

    const hasComputed = this.stage.output.some(col => col.source === 'computed');
    if (hasComputed) {
      if (!this.kernel) {
        this.kernel = bindKernelRegexes(
          await instantiateKernel(
            stageEntrypoint(this.stage, 'project'), this.shared),
          this.stage);
      }
    }
    if (fetched.rowSet.batch.wasm && !fetched.rowSet.batch.wasm.pinned) {
      assertStreamingWasmEdge(this.node.inbound[0]);
    }
    const outRowSet = runProject(
      this.kernel,
      this.layout,
      fetched.rowSet,
      this.stage.output,
      this.shared.arena);
    this.rows += outRowSet.batch.rowCount;
    output.push(outRowSet);
    return TaskResult.OK;
  }
}

class UnionAllTask {
  constructor(stage) {
    this.stage = stage;
    this.inputIndex = 0;
    this.done = false;
    this.rows = 0;
  }

  async execute() {
    if (this.done) return TaskResult.FINISHED;
    const output = this.node.outbound[0].connection;
    if (!output.canPush()) return TaskResult.BLOCKED_OUTPUT;

    while (this.inputIndex < this.node.inbound.length) {
      const input = this.node.inbound[this.inputIndex].connection;
      const fetched = input.fetch();
      if (fetched.result === FetchResult.NO_DATA) return TaskResult.NEED_DATA;
      if (fetched.result === FetchResult.FINISHED) {
        ++this.inputIndex;
        continue;
      }
      this.rows += fetched.rowSet.batch.rowCount;
      output.push(fetched.rowSet);
      return TaskResult.OK;
    }

    output.finish();
    this.done = true;
    return TaskResult.FINISHED;
  }
}

class CteSpoolState {
  constructor(id) {
    this.id = id;
    this.ready = false;
    this.rowSets = [];
  }

  destroy() {
    releaseWasmRowSets(this.rowSets, 0);
    this.rowSets = [];
    this.ready = false;
  }
}

function cteSpool(shared, id) {
  const key = Number(id);
  let spool = shared.cteSpools.get(key);
  if (!spool) {
    spool = new CteSpoolState(key);
    shared.cteSpools.set(key, spool);
  }
  return spool;
}

class CteProducerTask {
  constructor(stage, shared) {
    this.stage = stage;
    this.shared = shared;
    this.spool = cteSpool(shared, stage.materialization);
    this.done = false;
    this.rows = 0;
  }

  async execute() {
    if (this.done) return TaskResult.FINISHED;
    const input = this.node.inbound[0].connection;
    const fetched = input.fetch();
    if (fetched.result === FetchResult.NO_DATA) return TaskResult.NEED_DATA;
    if (fetched.result === FetchResult.OK) {
      this.instrumentation?.retain(fetched.rowSet, 0);
      this.instrumentation?.output(fetched.rowSet);
      this.spool.rowSets.push(fetched.rowSet);
      this.rows += fetched.rowSet.batch.rowCount;
      return TaskResult.OK;
    }

    this.spool.ready = true;
    for (const edge of this.node.outbound) {
      edge.connection.finish();
    }
    this.done = true;
    return TaskResult.FINISHED;
  }

  destroy() {
    if (this.shared.cteSpools.get(this.spool.id) === this.spool) {
      this.shared.cteSpools.delete(this.spool.id);
    }
    this.spool.destroy();
  }
}

class CteConsumerTask {
  constructor(stage, shared) {
    this.stage = stage;
    this.shared = shared;
    this.spool = cteSpool(shared, stage.materialization);
    this.ready = false;
    this.nextBatch = 0;
    this.done = false;
    this.rows = 0;
  }

  async execute() {
    if (this.done) return TaskResult.FINISHED;
    const output = this.node.outbound[0].connection;
    if (!this.ready) {
      const completion = this.node.inbound[0].connection;
      const fetched = completion.fetch();
      if (fetched.result === FetchResult.NO_DATA) return TaskResult.NEED_DATA;
      if (fetched.result === FetchResult.OK) {
        releaseWasmRowSet(fetched.rowSet);
        return TaskResult.OK;
      }
      if (!this.spool.ready) {
        throw new Error('CTE completion arrived before spool became ready');
      }
      this.ready = true;
    }
    if (this.nextBatch >= this.spool.rowSets.length) {
      output.finish();
      this.done = true;
      return TaskResult.FINISHED;
    }
    if (!output.canPush()) return TaskResult.BLOCKED_OUTPUT;
    const view = cloneRetainedRowSetView(this.spool.rowSets[this.nextBatch]);
    ++this.nextBatch;
    this.rows += view.batch.rowCount;
    output.push(view);
    return TaskResult.OK;
  }
}

class WindowTask {
  constructor(stage, layout, shared) {
    this.stage = stage;
    this.layout = layout;
    this.shared = shared;
    this.inputs = [];
    this.pending = null;
    this.done = false;
    this.rows = 0;
  }

  async execute() {
    if (this.done) return TaskResult.FINISHED;
    const input = this.node.inbound[0].connection;
    const output = this.node.outbound[0].connection;

    if (this.pending) {
      if (!output.canPush()) return TaskResult.BLOCKED_OUTPUT;
      output.push({ batch: this.pending, selection: null });
      output.finish();
      this.pending = null;
      this.done = true;
      return TaskResult.FINISHED;
    }

    const fetched = input.fetch();
    if (fetched.result === FetchResult.NO_DATA) return TaskResult.NEED_DATA;
    if (fetched.result === FetchResult.OK) {
      this.instrumentation?.retain(fetched.rowSet, 0);
      this.inputs.push(fetched.rowSet);
      this.rows += fetched.rowSet.batch.rowCount;
      return TaskResult.OK;
    }

    if (!Array.isArray(this.stage.radixKeys)) {
      throw new Error('window stage is missing wasm kernel');
    }
    const kernel = await instantiateKernel(
      stageEntrypoint(this.stage, 'qdb_window_run'),
      this.shared);
    const inputs = this.inputs;
    this.inputs = [];
    this.pending = runWindowKernelRowSets(
      kernel, this.layout, inputs, this.stage);
    this.instrumentation?.releaseRetained();
    return this.execute();
  }

  destroy() {
    releaseWasmBatch(this.pending);
    this.pending = null;
    for (const rowSet of this.inputs) {
      releaseWasmRowSet(rowSet);
    }
    this.inputs = [];
  }
}

class AggregateTask {
  constructor(stage, layout, shared) {
    this.stage = stage;
    this.layout = layout;
    this.shared = shared;
    this.kernel = null;
    this.state = null;
    this.pending = null;
    this.done = false;
    this.rows = 0;
  }

  async execute() {
    if (this.done) return TaskResult.FINISHED;
    const input = this.node.inbound[0].connection;
    const output = this.node.outbound[0].connection;

    if (this.pending) {
      if (!output.canPush()) return TaskResult.BLOCKED_OUTPUT;
      output.push({ batch: this.pending, selection: null });
      output.finish();
      this.pending = null;
      this.done = true;
      return TaskResult.FINISHED;
    }

    const fetched = input.fetch();
    if (fetched.result === FetchResult.NO_DATA) return TaskResult.NEED_DATA;
    if (fetched.result === FetchResult.FINISHED) {
      if (!this.state) {
        if (!this.kernel) {
          this.kernel = await instantiateKernel(
            stageEntrypoint(this.stage, 'agg_dispatch'), this.shared);
        }
        this.state = createAggregateState(this.kernel, this.layout, this.stage, this.instrumentation);
      }
      this.pending = finishAggregateState(
        this.state, downstreamConsumesWasm(this.node));
      return this.execute();
    }

    if (!this.kernel) {
      this.kernel = await instantiateKernel(
        stageEntrypoint(this.stage, 'agg_dispatch'), this.shared);
      this.state = createAggregateState(this.kernel, this.layout, this.stage, this.instrumentation);
    }
    if (fetched.rowSet.batch.wasm && !fetched.rowSet.batch.wasm.pinned) {
      assertStreamingWasmEdge(this.node.inbound[0]);
    }
    try {
      updateAggregateState(this.state, fetched.rowSet);
      this.instrumentation?.hash(this.state.ht, 0, false);
      this.rows += fetched.rowSet.batch.rowCount;
    } finally {
      releaseWasmRowSet(fetched.rowSet);
    }
    return TaskResult.OK;
  }

  destroy() {
    releaseWasmBatch(this.pending);
    this.pending = null;
  }
}

class JoinTask {
  constructor(stage, layout, shared) {
    this.stage = stage;
    this.layout = layout;
    this.shared = shared;
    this.kernel = null;
    this.state = null;
    this.ready = [];
    this.done = false;
    this.leftDone = false;
    this.rightDone = false;
    this.bothDone = false;
    this.streamMode = JoinStreamMode.SYMMETRIC;
    this.storedLeftRows = 0;
    this.storedRightRows = 0;
    this.lastLeftBatchRows = 0;
    this.lastRightBatchRows = 0;
    this.rows = 0;
  }

  async execute() {
    if (this.done) return TaskResult.FINISHED;
    const output = this.node.outbound[0].connection;
    const asWasm = downstreamConsumesWasm(this.node);

    if (this.ready.length > 0) {
      if (!output.canPush()) return TaskResult.BLOCKED_OUTPUT;
      output.push({ batch: this.ready.shift(), selection: null });
      return TaskResult.OK;
    }

    if (!this.state) {
      if (!this.kernel) {
        this.kernel = await instantiateKernel(
          stageEntrypoint(this.stage, 'jt_dispatch'), this.shared);
      }
      this.state = createJoinState(this.kernel, this.layout, this.stage, this.instrumentation);
    }

    const drained = drainJoinPairs(this.state, BROWSER_JOIN_OUTPUT_BATCH_ROWS, asWasm);
    if (drained.length > 0) {
      this.ready.push(...drained);
      return this.execute();
    }

    if (this.bothDone || (this.leftDone && this.rightDone)) {
      if (isLeftSemiAntiJoin(this.stage) &&
          !this.streamsSemiAntiLeft() &&
          !this.state.semiAntiFinalized) {
        finalizeSemiAntiJoinState(this.state);
        this.ready.push(...drainJoinPairs(this.state, BROWSER_JOIN_OUTPUT_BATCH_ROWS, asWasm));
        if (this.ready.length > 0) {
          return this.execute();
        }
      }
      if (isOuterJoin(this.stage) && !this.state.outerFinalized) {
        finalizeOuterJoinState(this.state);
        this.ready.push(...drainJoinPairs(this.state, BROWSER_JOIN_OUTPUT_BATCH_ROWS, asWasm));
        if (this.ready.length > 0) {
          return this.execute();
        }
      }
      finishJoinState(this.state);
      output.finish();
      this.done = true;
      return TaskResult.FINISHED;
    }

    const progressed = this.pullOneInputBatch();
    if (this.instrumentation && this.state) {
      this.instrumentation.hash(this.state.leftTable, 0, true);
      this.instrumentation.hash(this.state.rightTable, 1, true);
    }
    if (progressed) {
      this.ready.push(...drainJoinPairs(this.state, BROWSER_JOIN_OUTPUT_BATCH_ROWS, asWasm));
      if (this.ready.length > 0) {
        return this.execute();
      }
      if ((this.leftDone || this.rightDone) &&
          !this.bothDone &&
          !(this.leftDone && this.rightDone)) {
        return TaskResult.NEED_DATA;
      }
      return TaskResult.OK;
    }
    return TaskResult.NEED_DATA;
  }

  pullOneInputBatch() {
    const streamSemiAnti = this.streamsSemiAntiLeft();
    if (isLeftSemiAntiJoin(this.stage) && !streamSemiAnti) {
      return this.pullOneSemiAntiInputBatch();
    }
    return this.pullOneSymmetricInputBatch(
      downstreamConsumesWasm(this.node),
      isInnerJoin(this.stage) || streamSemiAnti || streamsOuterJoin(this.stage));
  }

  chooseSymmetricPullSide() {
    if (this.storedLeftRows === 0 && this.storedRightRows === 0) {
      return 0;
    }
    if (this.storedLeftRows <= this.storedRightRows) {
      if (this.lastLeftBatchRows > 0 &&
          this.storedLeftRows + this.lastLeftBatchRows >= this.storedRightRows) {
        return 1;
      }
      return 0;
    }
    if (this.lastRightBatchRows > 0 &&
        this.storedRightRows + this.lastRightBatchRows >= this.storedLeftRows) {
      return 0;
    }
    return 1;
  }

  processStoredSide(side) {
    const fetched = this.node.inbound[side].connection.fetch();
    if (fetched.result === FetchResult.NO_DATA) {
      return false;
    }
    if (fetched.result === FetchResult.FINISHED) {
      if (side === 0) this.leftDone = true;
      else this.rightDone = true;
      return true;
    }
    const rows = fetched.rowSet.batch.rowCount;
    updateJoinState(this.state, side, fetched.rowSet);
    this.rows += rows;
    if (side === 0) {
      this.storedLeftRows += rows;
      this.lastLeftBatchRows = rows;
    } else {
      this.storedRightRows += rows;
      this.lastRightBatchRows = rows;
    }
    return true;
  }

  processStreamSide(side, asWasm) {
    const fetched = this.node.inbound[side].connection.fetch();
    if (fetched.result === FetchResult.NO_DATA) {
      return false;
    }
    if (fetched.result === FetchResult.FINISHED) {
      if (side === 0) this.leftDone = true;
      else this.rightDone = true;
      this.bothDone = true;
      return true;
    }
    this.ready.push(...streamJoinBatch(this.state, side, fetched.rowSet, asWasm));
    this.rows += fetched.rowSet.batch.rowCount;
    return true;
  }

  pullOneSymmetricInputBatch(asWasm, allowStreaming) {
    for (;;) {
      if (this.leftDone && this.rightDone) {
        this.bothDone = true;
        return false;
      }
      if (allowStreaming &&
          this.streamMode === JoinStreamMode.STREAM_LEFT_AGAINST_RIGHT) {
        return this.processStreamSide(0, asWasm);
      }
      if (allowStreaming &&
          this.streamMode === JoinStreamMode.STREAM_RIGHT_AGAINST_LEFT) {
        return this.processStreamSide(1, asWasm);
      }

      if (this.leftDone) {
        if (allowStreaming) {
          this.streamMode = JoinStreamMode.STREAM_RIGHT_AGAINST_LEFT;
          continue;
        }
        return this.processStoredSide(1);
      }
      if (this.rightDone) {
        if (allowStreaming) {
          this.streamMode = JoinStreamMode.STREAM_LEFT_AGAINST_RIGHT;
          continue;
        }
        return this.processStoredSide(0);
      }

      if (allowStreaming && this.stage.buildSide === 'right') {
        return this.processStoredSide(1);
      }
      if (allowStreaming && this.stage.buildSide === 'left') {
        return this.processStoredSide(0);
      }

      const first = this.chooseSymmetricPullSide();
      if (first === 0) {
        if (this.processStoredSide(0)) {
          return true;
        }
        return this.processStoredSide(1);
      }
      if (this.processStoredSide(1)) {
        return true;
      }
      return this.processStoredSide(0);
    }
  }

  streamsSemiAntiLeft() {
    return isLeftSemiAntiJoin(this.stage) && this.stage.buildSide === 'right';
  }

  pullOneSemiAntiInputBatch() {
    if (!this.leftDone) {
      const fetched = this.node.inbound[0].connection.fetch();
      if (fetched.result === FetchResult.OK) {
        updateJoinState(this.state, 0, fetched.rowSet);
        this.rows += fetched.rowSet.batch.rowCount;
        return true;
      }
      if (fetched.result === FetchResult.FINISHED) {
        this.leftDone = true;
        return true;
      }
      return false;
    }

    if (!this.rightDone) {
      const fetched = this.node.inbound[1].connection.fetch();
      if (fetched.result === FetchResult.OK) {
        updateJoinState(this.state, 1, fetched.rowSet);
        this.rows += fetched.rowSet.batch.rowCount;
        return true;
      }
      if (fetched.result === FetchResult.FINISHED) {
        this.rightDone = true;
        return true;
      }
    }

    return false;
  }

  destroy() {
    while (this.ready.length > 0) {
      releaseWasmBatch(this.ready.shift());
    }
    if (this.state) {
      finishJoinState(this.state);
      this.state = null;
    }
  }
}

// Cross product of two inputs (in practice: a scalar-subquery broadcast, the
// right side is one row). The kernel emits row-id pairs; jt_materialize gathers
// the left ++ right output columns.
class CrossJoinTask {
  constructor(stage, layout, shared) {
    this.stage = stage;
    this.layout = layout;
    this.shared = shared;
    this.kernel = null;
    this.state = null;
    this.ready = [];
    this.leftDone = false;
    this.rightDone = false;
    this.done = false;
    this.rows = 0;
  }

  async execute() {
    if (this.done) return TaskResult.FINISHED;
    const output = this.node.outbound[0].connection;
    const asWasm = downstreamConsumesWasm(this.node);

    if (this.ready.length > 0) {
      if (!output.canPush()) return TaskResult.BLOCKED_OUTPUT;
      const batch = this.ready.shift();
      output.push({ batch, selection: null });
      this.rows += batch.rowCount;
      return TaskResult.OK;
    }

    if (!this.state) {
      if (!this.kernel) {
        this.kernel = await instantiateKernel(
          stageEntrypoint(this.stage, 'xj_dispatch'), this.shared);
      }
      this.state = createCrossJoinState(this.kernel, this.layout, this.stage, this.instrumentation);
    }

    const drained = drainCrossJoinPairs(this.state, BROWSER_JOIN_OUTPUT_BATCH_ROWS, asWasm);
    if (drained.length > 0) {
      this.ready.push(...drained);
      return this.execute();
    }

    if (!this.rightDone) {
      const fetched = this.node.inbound[1].connection.fetch();
      if (fetched.result === FetchResult.NO_DATA) return TaskResult.NEED_DATA;
      if (fetched.result === FetchResult.OK) {
        updateCrossRightState(this.state, fetched.rowSet);
        return TaskResult.OK;
      }
      if (fetched.result === FetchResult.FINISHED) {
        this.rightDone = true;
        return TaskResult.OK;
      }
    }

    if (!this.leftDone) {
      const fetched = this.node.inbound[0].connection.fetch();
      if (fetched.result === FetchResult.NO_DATA) return TaskResult.NEED_DATA;
      if (fetched.result === FetchResult.OK) {
        updateCrossLeftState(this.state, fetched.rowSet);
        this.ready.push(...drainCrossJoinPairs(this.state, BROWSER_JOIN_OUTPUT_BATCH_ROWS, asWasm));
        if (this.ready.length > 0) {
          return this.execute();
        }
        return TaskResult.OK;
      }
      if (fetched.result === FetchResult.FINISHED) {
        this.leftDone = true;
      }
    }

    finishCrossJoinState(this.state);
    output.finish();
    this.done = true;
    return TaskResult.FINISHED;
  }

  destroy() {
    while (this.ready.length > 0) {
      releaseWasmBatch(this.ready.shift());
    }
    if (this.state) {
      finishCrossJoinState(this.state);
      this.state = null;
    }
  }
}

class TopSortWasmState {
  constructor(stage, layout, shared) {
    this.stage = stage;
    this.layout = layout;
    this.shared = shared;
    this.limit = Math.max(Number(stage.limit || 0), 0);
    this.kernel = null;
    this.stateBatch = null;
  }

  async ensureKernels() {
    if (!Array.isArray(this.stage.radixKeys)) {
      throw new Error('top-sort stage is missing wasm kernel');
    }
    if (!this.kernel) {
      this.kernel = await instantiateKernel(
        stageEntrypoint(this.stage, 'qdb_top_sort_update'),
        this.shared);
    }
  }

  async add(rowSet) {
    await this.ensureKernels();
    if (!this.stateBatch) {
      this.stateBatch = emptyBatchForRowSets([rowSet]);
    }
    if (this.limit <= 0) {
      releaseWasmRowSet(rowSet);
      return;
    }

    const n = selectedRowCount([rowSet]);
    if (n === 0) {
      releaseWasmRowSet(rowSet);
      return;
    }

    const arena = this.shared.arena;
    const oldStateBatch = this.stateBatch;
    const stateWasm = oldStateBatch?.wasm;
    const stateRowSet = stateWasm
      ? { rowsetPtr: stateWasm.rowsetPtr, ownedPtr: 0 }
      : marshalRowSet(
        arena,
        this.layout,
        oldStateBatch.columns,
        oldStateBatch.rowCount,
        false,
        null);
    const batchRowSet = stableSortRowSetForStore(arena, this.layout, rowSet);
    const pickCapacity = Math.min(
      this.limit, this.stateBatch.rowCount + n);
    const workStride = this.stage.radixKeys.some(key => key.isString) ? 4 : 1;
    const rowIdsPtr = arena.alloc(Math.max(n, pickCapacity, 1) * 8, 8);
    const workPtr = arena.alloc(Math.max(n, 1) * workStride * 8, 8);
    const countsPtr = arena.alloc((this.stage.radixNullable ? 257 : 256) * 4, 4);
    const pickSrcPtr = arena.alloc(Math.max(pickCapacity, 1), 1);
    const pickIdxPtr = arena.alloc(Math.max(pickCapacity, 1) * 4, 4);
    const outRowSetPtr = arena.alloc(this.layout.rowset.size, 8);
    let outRowSetTransferred = false;

    const dv = arena.view();
    let outRow = 0;
    const { batch, selection } = rowSet;
    for (let row = 0; row < batch.rowCount; ++row) {
      if (!rowSelected(selection, row)) continue;
      dv.setBigInt64(rowIdsPtr + outRow * 8, makeRowId(0, row), true);
      ++outRow;
    }

    try {
      const out = Number(this.kernel.fn(
        toWasmPtr(stateRowSet.rowsetPtr),
        toWasmPtr(batchRowSet.rowsetPtr),
        toWasmPtr(rowIdsPtr),
        toWasmPtr(workPtr),
        toWasmPtr(countsPtr),
        BigInt(n),
        // sortInput: the browser has no heap Top-K path, so the kernel always
        // radix-sorts the batch. Plain i32 bool, like doSort above.
        1,
        toWasmPtr(pickSrcPtr),
        toWasmPtr(pickIdxPtr),
        BigInt(this.limit),
        toWasmPtr(outRowSetPtr)));
      if (out <= 0) {
        this.stateBatch = emptyBatchForRowSets([rowSet]);
      } else {
        outRowSetTransferred = true;
        this.stateBatch = makeWasmOwnedBatch(
          { arena, layout: this.layout },
          outRowSetPtr,
          outputShapeFromRowSets([rowSet]));
      }
    } finally {
      if (stateRowSet.ownedPtr) {
        freeMarshalledRowSet(arena, this.layout, stateRowSet.ownedPtr);
      }
      if (oldStateBatch?.wasm) {
        releaseWasmRowSet({ batch: oldStateBatch });
      }
      if (batchRowSet.ownedPtr) {
        freeMarshalledRowSet(arena, this.layout, batchRowSet.ownedPtr);
      }
      releaseWasmRowSet(rowSet);
      if (!outRowSetTransferred) {
        arena.free(outRowSetPtr);
      }
      arena.free(rowIdsPtr);
      arena.free(workPtr);
      arena.free(countsPtr);
      arena.free(pickSrcPtr);
      arena.free(pickIdxPtr);
    }
  }

  finish() {
    return this.stateBatch || { rowCount: 0, columns: [] };
  }

  destroy() {
    releaseWasmBatch(this.stateBatch);
    this.stateBatch = null;
  }
}

class SortTask {
  constructor(stage, layout, shared) {
    this.stage = stage;
    this.layout = layout;
    this.shared = shared;
    this.topSort = stage.kind === 'top-sort'
      ? new TopSortWasmState(stage, layout, shared)
      : null;
    this.inputs = [];
    this.pending = null;
    this.done = false;
    this.rows = 0;
  }

  async execute() {
    if (this.done) return TaskResult.FINISHED;
    const input = this.node.inbound[0].connection;
    const output = this.node.outbound[0].connection;

    if (this.pending) {
      if (!output.canPush()) return TaskResult.BLOCKED_OUTPUT;
      output.push({ batch: this.pending, selection: null });
      if (this.topSort) this.instrumentation?.releaseRetained();
      output.finish();
      this.pending = null;
      this.done = true;
      return TaskResult.FINISHED;
    }

    const fetched = input.fetch();
    if (fetched.result === FetchResult.NO_DATA) return TaskResult.NEED_DATA;
    if (fetched.result === FetchResult.OK) {
      if (this.topSort) {
        await this.topSort.add(fetched.rowSet);
        if (this.instrumentation && this.topSort.stateBatch) {
          this.instrumentation.retain({ batch: this.topSort.stateBatch, selection: null }, 0, false);
        }
      } else {
        this.instrumentation?.retain(fetched.rowSet, 0);
        this.inputs.push(fetched.rowSet);
      }
      this.rows += fetched.rowSet.batch.rowCount;
      return TaskResult.OK;
    }

    if (this.topSort) {
      this.pending = this.topSort.finish();
      return this.execute();
    }

    if (!Array.isArray(this.stage.radixKeys)) {
      throw new Error(`${this.stage.kind} stage is missing wasm radix sort kernel`);
    }
    const radixNullable = !!this.stage.radixNullable;
    const kernel = await instantiateKernel(
      stageEntrypoint(this.stage, 'qdb_sort_run'),
      this.shared);
    kernel.nullable = radixNullable;
    const inputs = this.inputs;
    this.inputs = [];
    this.pending = runRadixSortRowSets(
      kernel, this.layout, inputs, this.stage.radixKeys, null);
    this.instrumentation?.releaseRetained();
    return this.execute();
  }

  destroy() {
    releaseWasmBatch(this.pending);
    this.pending = null;
    for (const rowSet of this.inputs) {
      releaseWasmRowSet(rowSet);
    }
    this.inputs = [];
    if (this.topSort) {
      this.topSort.destroy();
    }
  }
}

// A nested (non-root) LIMIT: streams input, keeping rows in [offset, offset+limit)
// among already-selected rows, then finishes (backpressure stops the producer).
// The root limit is applied by the sink instead and never reaches here.
class LimitTask {
  constructor(stage, layout, shared) {
    this.stage = stage;
    this.layout = layout;
    this.shared = shared;
    this.limit = Math.max(Number(stage.limit || 0), 0);
    this.offset = Math.max(Number(stage.offset || 0), 0);
    this.skipped = 0;
    this.emitted = 0;
    this.done = false;
    this.rows = 0;
  }

  async execute() {
    if (this.done) return TaskResult.FINISHED;
    const output = this.node.outbound[0].connection;
    if (!output.canPush()) return TaskResult.BLOCKED_OUTPUT;
    if (this.emitted >= this.limit) {
      output.finish();
      this.done = true;
      return TaskResult.FINISHED;
    }
    const input = this.node.inbound[0].connection;
    const fetched = input.fetch();
    if (fetched.result === FetchResult.NO_DATA) return TaskResult.NEED_DATA;
    if (fetched.result === FetchResult.FINISHED) {
      output.finish();
      this.done = true;
      return TaskResult.FINISHED;
    }
    const { batch, selection } = fetched.rowSet;
    const rowCount = batch.rowCount;
    const mask = new Uint8Array(rowCount);
    let anyKept = false;
    for (let i = 0; i < rowCount; ++i) {
      if (!rowSelected(selection, i)) continue;
      if (this.skipped < this.offset) { this.skipped++; continue; }
      if (this.emitted >= this.limit) break;
      mask[i] = 1;
      this.emitted++;
      anyKept = true;
    }
    if (!anyKept) {
      releaseWasmRowSet(fetched.rowSet);
      return TaskResult.OK;
    }
    this.rows = this.emitted;
    // A wasm-backed batch (columns in wasm) can't be re-marshalled from JS, so
    // alias its columns and keep the input rowset alive until the alias dies.
    if (batch.wasm) {
      const arena = this.shared.arena;
      const alias = aliasWasmRowSetDescriptor(
        arena, this.layout, { batch, selection: mask });
      const releaseInput = takeRowSetRelease(fetched.rowSet);
      let released = false;
      const destroy = () => {
        if (released) return;
        released = true;
        alias.destroy();
        if (releaseInput) {
          releaseInput();
        }
      };
      const outSelection = makeWasmSelection(
        arena, rowCount, alias.selectionPtr);
      const outBatch = { ...batch };
      try {
        attachBatchWasm(outBatch, alias.rowsetPtr, {
          pinned: true,
          selectionPtr: alias.selectionPtr,
          destroy,
        });
        output.push({ batch: outBatch, selection: outSelection });
      } catch (error) {
        destroy();
        throw error;
      }
    } else {
      fetched.rowSet.selection = mask;
      output.push(fetched.rowSet);
    }
    return TaskResult.OK;
  }
}

class SinkTask {
  constructor(limit, layout, shared) {
    this.limit = limit;
    this.layout = layout;
    this.shared = shared;
    this.rowSets = [];
    this.done = false;
    this.rows = 0;
  }

  async execute() {
    if (this.done) return TaskResult.FINISHED;
    const input = this.node.inbound[0].connection;
    const fetched = input.fetch();
    if (fetched.result === FetchResult.NO_DATA) return TaskResult.NEED_DATA;
    if (fetched.result === FetchResult.FINISHED) {
      this.done = true;
      return TaskResult.FINISHED;
    }
    this.rowSets.push(fetched.rowSet);
    this.rows += fetched.rowSet.batch.rowCount;
    return TaskResult.OK;
  }

  result() {
    return materializeRowSets(
      this.rowSets, this.limit, this.layout, this.shared.arena);
  }

  destroy() {
    releaseWasmRowSets(this.rowSets, 0);
    this.rowSets = [];
  }
}

function makeNode(task, label) {
  const node = new SchedulerNode(task, label);
  task.node = node;
  return node;
}

function joinKeyLabel(key) {
  const left = key?.left ?? key?.Left ?? '?';
  const right = key?.right ?? key?.Right ?? '?';
  return `${left} = ${right}`;
}

function sortKeyLabel(key) {
  const column = key?.column ?? key?.Column ?? '?';
  const direction = key?.direction ?? key?.Direction ?? '';
  const nulls = key?.nulls ?? key?.Nulls ?? '';
  const parts = [column];
  if (direction && direction !== 'default') {
    parts.push(direction);
  }
  if (nulls && nulls !== 'default' && nulls !== 'nulls-default') {
    parts.push(String(nulls).startsWith('nulls-') ? nulls : `nulls ${nulls}`);
  }
  return parts.join(' ');
}

function aggregateLabel(stage) {
  const keys = Array.isArray(stage.groupKeys) && stage.groupKeys.length > 0
    ? ` keys=[${stage.groupKeys.join(', ')}]`
    : '';
  return `aggregate${keys}`;
}

function stageLabel(stage) {
  if (typeof stage.label === 'string' && stage.label.length > 0) {
    return stage.label;
  }
  if (stage.kind === 'source') {
    return `source ${stage.table || '?'}`;
  }
  if (stage.kind === 'filter') {
    return stage.predicate ? `filter ${stage.predicate}` : 'filter';
  }
  if (stage.kind === 'aggregate') {
    return aggregateLabel(stage);
  }
  if (stage.kind === 'union-all') {
    return 'union-all';
  }
  if (stage.kind === 'cte-producer') {
    return stage.label || `cte-producer #${stage.cteId ?? '?'}`;
  }
  if (stage.kind === 'cte-consumer') {
    return stage.label || `cte-consumer #${stage.cteId ?? '?'}`;
  }
  if (stage.kind === 'window') {
    return 'window';
  }
  if (stage.kind === 'join') {
    const type = stage.joinType || 'inner';
    const keys = Array.isArray(stage.keys) && stage.keys.length > 0
      ? ` [${stage.keys.map(joinKeyLabel).join(', ')}]`
      : '';
    const residual = stage.hasResidual ? ' residual' : '';
    return `join ${type}${keys}${residual}`;
  }
  if (stage.kind === 'cross-join') {
    return stage.hasResidual ? 'cross-join residual' : 'cross-join';
  }
  if (stage.kind === 'sort' || stage.kind === 'top-sort') {
    const keys = Array.isArray(stage.sortKeys) && stage.sortKeys.length > 0
      ? ` [${stage.sortKeys.map(sortKeyLabel).join(', ')}]`
      : '';
    const limit = stage.kind === 'top-sort' && stage.limit != null
      ? ` limit ${stage.limit}`
      : '';
    return `${stage.kind}${keys}${limit}`;
  }
  return stage.kind;
}

function taskNodeForStage(stage, layout, readSourceBatches, onProgress, shared) {
  if (stage.kind === 'source') {
    return makeNode(
      new SourceTask(stage, layout, shared, readSourceBatches, onProgress || null),
      stageLabel(stage));
  }
  if (stage.kind === 'filter') {
    return makeNode(new FilterTask(stage, layout, shared), stageLabel(stage));
  }
  if (stage.kind === 'project') {
    return makeNode(new ProjectTask(stage, layout, shared), stageLabel(stage));
  }
  if (stage.kind === 'aggregate') {
    return makeNode(new AggregateTask(stage, layout, shared), stageLabel(stage));
  }
  if (stage.kind === 'union-all') {
    return makeNode(new UnionAllTask(stage), stageLabel(stage));
  }
  if (stage.kind === 'cte-producer') {
    return makeNode(new CteProducerTask(stage, shared), stageLabel(stage));
  }
  if (stage.kind === 'cte-consumer') {
    return makeNode(new CteConsumerTask(stage, shared), stageLabel(stage));
  }
  if (stage.kind === 'window') {
    return makeNode(new WindowTask(stage, layout, shared), stageLabel(stage));
  }
  if (stage.kind === 'join') {
    return makeNode(new JoinTask(stage, layout, shared), stageLabel(stage));
  }
  if (stage.kind === 'cross-join') {
    return makeNode(new CrossJoinTask(stage, layout, shared), stageLabel(stage));
  }
  if (stage.kind === 'sort' || stage.kind === 'top-sort') {
    return makeNode(new SortTask(stage, layout, shared), stageLabel(stage));
  }
  if (stage.kind === 'limit') {
    return makeNode(new LimitTask(stage, layout, shared), stageLabel(stage));
  }
  throw new Error(`unsupported exec stage: ${stage.kind}`);
}

class OperatorInstrumentation {
  constructor(stage, shared, layout, kernels) {
    this.stage = stage;
    this.shared = shared;
    this.layout = layout;
    this.kernels = kernels;

    this.scratch = shared.arena.alloc(24, 8);
    this.stringBuffers = shared.arena.alloc(Math.max(stage.instrumentationStringBuffers?.length || 0, 1) * 8, 8);
    const view = shared.arena.view();
    (stage.instrumentationStringBuffers || []).forEach((index, i) =>
      view.setBigInt64(this.stringBuffers + i * 8, BigInt(index), true));
    this.inputs = [];
    this.rowsOut = 0;
    this.bytesOut = 0;
    this.groups = null;
    this.capacity = null;
    this.growths = null;
    this.memory = null;
    this.previous = [0, 0];
    this.sideGroups = [0, 0];
    this.sideBytes = [0, 0];
    this.inputWidths = [];
    this.retainedBytes = 0;
    this.retainedPeak = 0;
    this.buildMemory = 0;
    this.hasBuildMemory = false;
  }

  measure(rowSet, widths, allRows = false) {
    // Marshalling stays in the existing adapter; selection, validity, offset
    // widths and payload measurement are interpreted by the common Oz code.
    const wasm = wasmRowSetForSelection(rowSet.batch, rowSet.selection);
    const marshalled = wasm ? null : marshalRowSet(this.shared.arena, this.layout,
      rowSet.batch.columns, rowSet.batch.rowCount, false, rowSet.selection);
    let pointer = wasm ? wasm.rowsetPtr : marshalled.rowsetPtr;
    let descriptor = 0;
    if (allRows) {
      descriptor = this.shared.arena.alloc(this.layout.rowset.size, 8);
      this.shared.arena.bytes().copyWithin(descriptor, pointer, pointer + this.layout.rowset.size);
      writePointer(this.shared.arena.view(), descriptor + this.layout.rowset.selection, 0);
      pointer = descriptor;
    }
    const widthPtr = this.shared.arena.alloc(Math.max(widths.length, 1) * 8, 8);
    try {
      const view = this.shared.arena.view();
      widths.forEach((width, i) => view.setBigInt64(widthPtr + i * 8, BigInt(width), true));
      this.kernels.rows(toWasmPtr(pointer), toWasmPtr(widthPtr), toWasmPtr(this.scratch));
      const result = this.shared.arena.view();
      return [Number(result.getBigInt64(this.scratch, true)),
        Number(result.getBigInt64(this.scratch + 8, true))];
    } finally {
      this.shared.arena.free(widthPtr);
      if (descriptor) this.shared.arena.free(descriptor);
      if (marshalled) freeMarshalledRowSet(this.shared.arena, this.layout, marshalled.rowsetPtr);
    }
  }

  input(rowSet, widths, index) {
    const [rows, bytes] = this.measure(rowSet, widths);
    this.inputWidths[index] = widths;
    const input = this.inputs[index] ||= { rows: 0, bytes: 0 };
    input.rows += rows;
    input.bytes += bytes;
  }

  output(rowSet) {
    const [rows, bytes] = this.measure(rowSet, this.stage.instrumentationWidths);
    this.rowsOut += rows;
    this.bytesOut += bytes;
  }

  retain(rowSet, input, append = true) {
    const [, bytes] = this.measure(rowSet, this.inputWidths[input], true);
    this.retainedBytes = append ? this.retainedBytes + bytes : bytes;
    this.hasBuildMemory = true;
    this.retainedPeak = Math.max(this.retainedPeak, this.retainedBytes);
    this.buildMemory = Math.max(this.buildMemory, this.retainedBytes + this.sideBytes[0] + this.sideBytes[1]);
  }

  releaseRetained() {
    this.retainedBytes = 0;
  }

  hash(table, side, join) {
    this.hasBuildMemory = true;
    this.kernels.hash(toWasmPtr(table), join ? 1n : 0n,
      toWasmPtr(this.stringBuffers), BigInt(this.stage.instrumentationStringBuffers?.length || 0),
      toWasmPtr(this.scratch));
    const view = this.shared.arena.view();
    const groups = Number(view.getBigInt64(this.scratch, true));
    const capacity = Number(view.getBigInt64(this.scratch + 8, true));
    const bytes = Number(view.getBigInt64(this.scratch + 16, true));
    this.growths ??= 0;
    if (this.previous[side]) {
      for (let old = this.previous[side]; old < capacity; old *= 2) ++this.growths;
    }
    this.previous[side] = capacity;
    this.sideGroups[side] = groups;
    this.sideBytes[side] = bytes;
    this.groups = this.sideGroups[0] + this.sideGroups[1];
    this.capacity = this.previous[0] + this.previous[1];
    this.memory = Math.max(this.memory || 0, this.sideBytes[0] + this.sideBytes[1]);
    this.buildMemory = Math.max(this.buildMemory, this.retainedBytes + this.sideBytes[0] + this.sideBytes[1]);
  }

  snapshot(node) {
    const materializes = ['aggregate', 'join', 'cross-join', 'window', 'sort', 'top-sort', 'cte-producer'].includes(this.stage.kind);
    return {
      stage_id: this.stage.stageId ?? this.stage.id,
      kind: this.stage.kind,
      rows_in: this.inputs.reduce((sum, value) => sum + value.rows, 0),
      rows_out: this.rowsOut,
      bytes_in: this.inputs.reduce((sum, value) => sum + value.bytes, 0),
      bytes_out: this.bytesOut,
      inputs: this.inputs,
      groups: this.groups,
      hash_capacity: this.capacity,
      hash_growth_events: this.growths,
      rehashes: this.growths,
      hash_memory_peak_bytes: this.memory,
      retained_input_bytes: this.hasBuildMemory ? this.retainedBytes : null,
      retained_input_peak_bytes: this.hasBuildMemory ? this.retainedPeak : null,
      build_memory_peak_bytes: this.hasBuildMemory ? this.buildMemory : null,
      materialized_bytes: materializes ? this.bytesOut : null,
      execution_cpu_seconds: null,
      task_elapsed_seconds: node.elapsedMs / 1000,
      // The browser contracts physical lanes; a single task has no measured
      // partition distribution and cannot claim native partition skew.
      partition_skew: null,
    };
  }

  destroy() {
    this.shared.arena.free(this.stringBuffers);
    this.shared.arena.free(this.scratch);
  }
}

async function attachOperatorInstrumentation(exec, nodes, shared, layout) {
  if (!exec.operatorInstrumentation) {
    throw new Error('operator instrumentation requires an export with options.operatorInstrumentation=true');
  }
  const rows = await instantiateKernel(exec.operatorInstrumentation.qdb_instrumentation_rows, shared);
  const hash = await instantiateKernel(exec.operatorInstrumentation.qdb_instrumentation_hash, shared);
  const kernels = { rows: rows.fn, hash: hash.fn };
  for (const node of nodes) {
    if (node.task.stage) {
      node.instrumentation = new OperatorInstrumentation(node.task.stage, shared, layout, kernels);
      node.task.instrumentation = node.instrumentation;
    }
  }
  for (const node of nodes) {
    for (const edge of node.outbound) {
      const connection = edge.connection;
      const push = connection.push.bind(connection);
      const fetch = connection.fetch.bind(connection);
      connection.push = rowSet => {
        if (edge.src.instrumentation) edge.src.instrumentation.output(rowSet);
        return push(rowSet);
      };
      connection.fetch = () => {
        const result = fetch();
        if (result.result === FetchResult.OK && edge.dst.instrumentation) {
          edge.dst.instrumentation.input(result.rowSet, edge.src.task.stage.instrumentationWidths,
            edge.dst.inbound.indexOf(edge));
        }
        return result;
      };
    }
  }
}

function buildScheduledGraph(exec, readSourceBatches, options, layout, shared) {
  const nodesById = new Map();
  const nodes = [];
  for (const spec of exec.nodes || []) {
    const node = taskNodeForStage(
      spec, layout, readSourceBatches, options.onProgress, shared);
    nodesById.set(Number(spec.id), node);
    nodes.push(node);
  }
  for (const edge of exec.edges || []) {
    const src = nodesById.get(Number(edge.from));
    const dst = nodesById.get(Number(edge.to));
    if (!src || !dst) {
      throw new Error('exec graph edge references an unknown node');
    }
    connect(
      src,
      dst,
      createConnection(edge.kind || ConnectionKind.OneToOne),
      {
        persistent:
          dst.task?.stage?.kind === 'join' ||
          dst.task?.stage?.kind === 'cte-producer'
      });
  }
  const root = nodesById.get(Number(exec.root));
  if (!root) {
    throw new Error('exec graph root references an unknown node');
  }
  const limit = exec.limit
    ? { limit: Number(exec.limit.limit), offset: Number(exec.limit.offset || 0) }
    : null;
  const sink = makeNode(new SinkTask(limit, layout, shared), 'materialize');
  nodes.push(sink);
  connect(root, sink, createConnection(ConnectionKind.OneToOne));
  const roots = nodes.filter(node => node.inbound.length === 0);
  return { roots, sink, nodes };
}

function cleanupScheduledGraph(nodes) {
  const seenConnections = new Set();
  const cleanup = (label, fn) => {
    try {
      fn();
    } catch (error) {
      console.warn(`browser runtime cleanup failed (${label})`, error);
    }
  };

  for (const node of nodes) {
    for (const edge of node.outbound) {
      if (seenConnections.has(edge.connection)) {
        continue;
      }
      seenConnections.add(edge.connection);
      if (typeof edge.connection.destroy === 'function') {
        cleanup(`connection ${edge.src.label}->${edge.dst.label}`, () => {
          edge.connection.destroy();
        });
      }
    }
  }
  for (const node of nodes) {
    if (typeof node.task?.destroy === 'function') {
      cleanup(`task ${node.label}`, () => {
        node.task.destroy();
      });
    }
  }
}

export async function executeBrowserPipelineScheduled(exec, readSourceBatches, options = {}) {
  if (!exec || exec.supported !== true) {
    throw new Error(exec?.reason || 'pipeline is not supported for browser execution');
  }
  if (exec.embedWasm !== true) {
    throw new Error('exec plan was produced without embedded wasm kernels');
  }

  const layout = exec.layout;
  PointerSize = layout.pointerSize === 4 ? 4 : 8;
  if (!Array.isArray(exec.nodes)) {
    throw new Error('exec plan is missing graph nodes');
  }
  const shared = createSharedMemory(layout, exec.wasm);
  const { roots, sink, nodes } =
    buildScheduledGraph(exec, readSourceBatches, options, layout, shared);
  const scheduler = new SingleThreadedScheduler(roots);
  let result;
  try {
    if (options.operatorInstrumentation === true) {
      await attachOperatorInstrumentation(exec, nodes, shared, layout);
    }
    await scheduler.run();
    result = sink.task.result();
    if (options.operatorInstrumentation === true) {
      result.operator_instrumentation = {
        schema_version: 1,
        bytes: 'logical_nonnull_payload',
        memory: 'batch_boundary_hash_reservations',
        stages: nodes.filter(node => node.instrumentation).map(node => node.instrumentation.snapshot(node)),
      };
    }
  } finally {
    cleanupScheduledGraph(nodes);
    for (const node of nodes) node.instrumentation?.destroy();
  }
  result.timings = nodes.map(node => ({
    stage: node.label,
    rows: node.rows,
    elapsedMs: node.elapsedMs,
  }));
  result.scheduler = scheduler.stats;
  result.memory = shared.arena.stats();
  result.connections = nodes.flatMap(node =>
    node.outbound.map(edge => ({
      from: edge.src.label,
      to: edge.dst.label,
      kind: edge.connection.kind,
      ...edge.connection.stats,
    })));
  return result;
}

function batchHasJsValues(batch) {
  return batch.columns.every(column =>
    Object.prototype.hasOwnProperty.call(column, 'values'));
}

function finalMaterializeBatch(rowSet, layout, arena) {
  const batch = rowSet.batch;
  if (batchHasJsValues(batch)) {
    return batch;
  }
  if (!batch.wasm) {
    throw new Error('rowset has no JS values and no wasm handle');
  }
  return materializeWasmRowSet(arena, layout, batch.columns, batch.wasm.rowsetPtr);
}

function releaseWasmRowSets(rowSets, start) {
  for (let i = start; i < rowSets.length; ++i) {
    releaseWasmRowSet(rowSets[i]);
  }
}

function materializeRowSets(rowSets, limit, layout, arena) {
  const first = rowSets.find(rowSet => rowSet.batch.columns.length > 0);
  const columns = first ? first.batch.columns.map(col => col.name) : [];
  const rows = [];
  const offset = limit ? limit.offset : 0;
  const max = limit ? limit.limit : Infinity;
  let skipped = 0;

  if (max <= 0) {
    releaseWasmRowSets(rowSets, 0);
    return { columns, rows };
  }

  for (let setIndex = 0; setIndex < rowSets.length; ++setIndex) {
    const rowSet = rowSets[setIndex];
    const { selection } = rowSet;
    let reachedLimit = false;
    try {
      const batch = finalMaterializeBatch(rowSet, layout, arena);
      for (let i = 0; i < batch.rowCount; ++i) {
        if (!rowSelected(selection, i)) continue;
        if (skipped < offset) {
          ++skipped;
          continue;
        }
        if (rows.length >= max) {
          reachedLimit = true;
          break;
        }
        rows.push(batch.columns.map(col => formatValue(col.values[i])));
      }
    } finally {
      releaseWasmRowSet(rowSet);
      rowSets[setIndex] = null;
    }
    if (reachedLimit) {
      releaseWasmRowSets(rowSets, setIndex + 1);
      return { columns, rows };
    }
  }
  return { columns, rows };
}

function formatValue(value) {
  if (value === null || value === undefined) {
    return '';
  }
  if (typeof value === 'bigint') {
    return value.toString();
  }
  return value;
}
