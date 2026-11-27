// Small separate file so app.js can check this without loading all of
// browser_runtime.js on the main thread.
let cached = null;

// (module (func (result i32) (i32.load (i64.const 0))) (memory i64 1))
const memory64Module = new Uint8Array([
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f,
  0x03, 0x02, 0x01, 0x00,
  0x05, 0x03, 0x01, 0x04, 0x01,
  0x0a, 0x09, 0x01, 0x07, 0x00, 0x42, 0x00, 0x28, 0x02, 0x00, 0x0b,
]);

export function detectMemory64Support() {
  if (cached !== null) {
    return cached;
  }
  // Safari on iOS 27 accepts an i64 WebAssembly.Memory but still rejects
  // wasm64 modules ("Memory64 is not enabled"), so the compiler is checked too.
  if (!WebAssembly.validate(memory64Module)) {
    cached = false;
    return cached;
  }
  const errors = [];
  for (const key of ['address', 'index']) {
    try {
      new WebAssembly.Memory({ initial: 1n, [key]: 'i64' });
      cached = true;
      return cached;
    } catch (error) {
      errors.push(`${key}: ${error.message || String(error)}`);
    }
  }
  cached = false;
  return cached;
}
