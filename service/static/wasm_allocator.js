// Segregated free-list ("bucketed") allocator over a growable
// WebAssembly.Memory. Blocks are rounded up to a size class; each class keeps a
// LIFO free list, so malloc/free are O(1) (no scan, no array splice — the
// failure mode of a naive best-fit list under realloc-doubling). Freed blocks
// are reused exactly by same-class requests; the frontier only advances by the
// live high-water mark and grows the memory (with headroom) on demand.
//
// Size classes: fine-grained below 1 KiB (16-byte steps), then geometric
// (~1.18x) up to a threshold, then page-granular (4 KiB) so large buffers waste
// little. Class sizes are memoized and shared per allocator.
//
// Metadata lives in JS (a size-per-pointer map + per-class free stacks), not in
// the linear memory, so nothing here is disturbed by WebAssembly.Memory.grow —
// we only ever re-derive typed-array views from the (possibly new) buffer.

const PAGE = 65536;

function buildSizeClasses() {
  const classes = [];
  for (let s = 16; s < 1024; s += 16) classes.push(s);
  let s = 1024;
  const PAGE_THRESHOLD = 1 << 20; // 1 MiB: switch to page granularity
  while (s < PAGE_THRESHOLD) {
    classes.push(s);
    s = Math.max(s + 16, Math.ceil((s * 1.18) / 16) * 16);
  }
  for (; s <= (1 << 30); s += 4096) classes.push(s);
  return classes;
}

export class BucketAllocator {
  // `memory`  — WebAssembly.Memory (growable)
  // `base`    — first usable byte (heap base above the wasm modules' data/slots)
  constructor(memory, base) {
    this.memory = memory;
    this.classes = buildSizeClasses();
    this.freeLists = this.classes.map(() => []); // classIdx -> [ptr, ...]
    this.blockClass = new Map();                  // ptr -> classIdx
    this.base = align(base, 16);
    this.frontier = this.base;
    this.liveBytes = 0;
    this.peakLiveBytes = 0;
  }

  stats() {
    return {
      reservedMB: ((this.frontier - this.base) / 1048576).toFixed(1),
      peakLiveMB: (this.peakLiveBytes / 1048576).toFixed(1),
      liveMB: (this.liveBytes / 1048576).toFixed(1),
    };
  }

  // Smallest class index whose size >= n (binary search over the sorted list).
  classIndexFor(n) {
    const c = this.classes;
    let lo = 0, hi = c.length;
    while (lo < hi) {
      const mid = (lo + hi) >>> 1;
      if (c[mid] < n) lo = mid + 1; else hi = mid;
    }
    if (lo >= c.length) {
      throw new Error(`wasm allocator: request too large (${n} bytes)`);
    }
    return lo;
  }

  malloc(size) {
    const n = Math.max(Number(size), 1);
    const idx = this.classIndexFor(n);
    this.liveBytes += this.classes[idx];
    if (this.liveBytes > this.peakLiveBytes) this.peakLiveBytes = this.liveBytes;
    const list = this.freeLists[idx];
    if (list.length > 0) {
      const ptr = list.pop();
      this.blockClass.set(ptr, idx);
      return ptr;
    }
    // Carve a fresh class-sized block off the frontier, growing memory (with
    // headroom to amortize grow calls) when it doesn't fit.
    const blockSize = this.classes[idx];
    const ptr = this.frontier;
    const end = ptr + blockSize;
    if (end > this.memory.buffer.byteLength) {
      const deficit = end - this.memory.buffer.byteLength;
      // Grow by the deficit plus 25% headroom, rounded to whole pages.
      const pages = Math.ceil((deficit + (deficit >> 2)) / PAGE);
      // memory64 engines require a BigInt page delta; memory32 wants a Number.
      try {
        this.memory.grow(pages);
      } catch {
        this.memory.grow(BigInt(pages));
      }
    }
    this.frontier = end;
    this.blockClass.set(ptr, idx);
    return ptr;
  }

  // Zeroed allocation (kernels rely on qdb_alloc returning fresh-zero memory,
  // matching memory.grow semantics; reused blocks may hold stale bytes).
  calloc(size) {
    const ptr = this.malloc(size);
    const n = Math.max(Number(size), 1);
    new Uint8Array(this.memory.buffer).fill(0, ptr, ptr + n);
    return ptr;
  }

  // The memory instance belongs to one query. Reclaim abandoned state after
  // task/connection cleanup, including allocations left by failed kernels.
  freeAll() {
    for (const ptr of this.blockClass.keys()) this.free(ptr);
  }

  free(ptr) {
    const p = Number(ptr);
    if (!p) return;
    const idx = this.blockClass.get(p);
    if (idx === undefined) return; // not ours / double free — ignore
    this.blockClass.delete(p);
    this.liveBytes -= this.classes[idx];
    this.freeLists[idx].push(p);
  }
}

export function align(value, to) {
  return Math.ceil(value / to) * to;
}
