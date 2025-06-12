const fs = require('fs');
const path = require('path');

if (process.argv.length !== 4) {
  console.error('Usage: node run_generated_wasm.js <module.wasm> <state.json>');
  process.exit(1);
}

const wasmPath = process.argv[2];
const statePath = process.argv[3];

const wasmBuffer = fs.readFileSync(wasmPath);
const state = JSON.parse(fs.readFileSync(statePath, 'utf8'));

// Parse offset constants from the asm_defines header generated during the
// build.  This mirrors the gawk logic in the main Makefile that creates the
// same table for the assembly dynarec backends so the values stay in sync
// even when the structure layout changes or the build targets another
// architecture.
const asmPath = path.join(__dirname, '..', '..', 'src', 'device', 'r4300',
                          'new_dynarec', 'arm64', 'asm_defines_gas.h');
const asmText = fs.readFileSync(asmPath, 'utf8');

function parseOffset(name) {
  const regex = new RegExp(`#define\\s+${name}\\s+\\((0x[0-9a-fA-F]+)\\)`);
  const match = asmText.match(regex);
  if (!match)
    throw new Error(`Unable to find ${name} in ${asmPath}`);
  return parseInt(match[1], 16);
}

const GPR_OFFSET = parseOffset('offsetof_struct_new_dynarec_hot_state_regs');
const HI_OFFSET = parseOffset('offsetof_struct_new_dynarec_hot_state_hi');
const LO_OFFSET = parseOffset('offsetof_struct_new_dynarec_hot_state_lo');
const PC_OFFSET = parseOffset('offsetof_struct_new_dynarec_hot_state_pcaddr');
const CP0_OFFSET = parseOffset('offsetof_struct_new_dynarec_hot_state_cp0_regs');
const CP1_SIMPLE_OFFSET = parseOffset('offsetof_struct_new_dynarec_hot_state_cp1_regs_simple');
const CP1_DOUBLE_OFFSET = parseOffset('offsetof_struct_new_dynarec_hot_state_cp1_regs_double');
const CP1_REG_BASE = 0x8000;

(async () => {
  const module = await WebAssembly.compile(wasmBuffer);
  const memImport = WebAssembly.Module.imports(module).find(i => i.kind === 'memory');
  const pages = memImport ? memImport.minimum : 1;
  const memory = new WebAssembly.Memory({
    initial: pages,
    maximum: memImport && memImport.maximum ? memImport.maximum : pages,
    shared: memImport && memImport.shared
  });
  const env = {
    wasm_dynarec_dispatch: () => {},
    memory,
  };
  const { instance } = await WebAssembly.instantiate(module, { env });
  const { entry } = instance.exports;
  const view = new DataView(memory.buffer);

  for (let i = 0; i < 32; i++) {
    const val = BigInt(state.initial.regs[i]);
    view.setBigUint64(GPR_OFFSET + i * 8, val, true);
  }
  for (let i = 0; i < 32; i++) {
    view.setUint32(CP0_OFFSET + i * 4, state.initial.cp0[i] >>> 0, true);
  }
  for (let i = 0; i < 32; i++) {
    view.setBigUint64(CP1_SIMPLE_OFFSET + i * 8, BigInt(CP1_REG_BASE + i * 8), true);
    view.setBigUint64(CP1_DOUBLE_OFFSET + i * 8, BigInt(CP1_REG_BASE + i * 8), true);
    view.setBigUint64(CP1_REG_BASE + i * 8, BigInt(state.initial.cp1[i]), true);
  }
  view.setBigUint64(HI_OFFSET, BigInt(state.initial.hi), true);
  view.setBigUint64(LO_OFFSET, BigInt(state.initial.lo), true);
  view.setUint32(PC_OFFSET, state.initial.pcaddr, true);

  entry(0);

  let ok = true;
  for (let i = 0; i < 32; i++) {
    const got = Number(view.getBigUint64(GPR_OFFSET + i * 8, true));
    if (got !== state.expected.regs[i]) {
      console.error(`r${i} expected ${state.expected.regs[i]} got ${got}`);
      ok = false;
    }
  }
  for (let i = 0; i < 32; i++) {
    const got = view.getUint32(CP0_OFFSET + i * 4, true);
    if (got !== state.expected.cp0[i]) {
      console.error(`cp0_${i} expected ${state.expected.cp0[i]} got ${got}`);
      ok = false;
    }
  }
  for (let i = 0; i < 32; i++) {
    const got = Number(view.getBigUint64(CP1_REG_BASE + i * 8, true));
    if (got !== state.expected.cp1[i]) {
      console.error(`cp1_${i} expected ${state.expected.cp1[i]} got ${got}`);
      ok = false;
    }
  }
  const hi = Number(view.getBigUint64(HI_OFFSET, true));
  if (hi !== state.expected.hi) {
    console.error(`hi expected ${state.expected.hi} got ${hi}`);
    ok = false;
  }
  const lo = Number(view.getBigUint64(LO_OFFSET, true));
  if (lo !== state.expected.lo) {
    console.error(`lo expected ${state.expected.lo} got ${lo}`);
    ok = false;
  }
  const pcaddr = view.getUint32(PC_OFFSET, true);
  if (pcaddr !== state.expected.pcaddr) {
    console.error(`pcaddr expected ${state.expected.pcaddr} got ${pcaddr}`);
    ok = false;
  }

  process.exit(ok ? 0 : 1);
})();
