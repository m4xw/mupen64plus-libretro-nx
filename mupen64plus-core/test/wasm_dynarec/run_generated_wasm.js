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

const GPR_OFFSET = 320; // offsetof(struct new_dynarec_hot_state, regs)
const HI_OFFSET = 576;  // offsetof(struct new_dynarec_hot_state, hi)
const LO_OFFSET = 584;  // offsetof(struct new_dynarec_hot_state, lo)
const PC_OFFSET = 264;  // offsetof(struct new_dynarec_hot_state, pcaddr)

(async () => {
  const env = {
    wasm_dynarec_dispatch: () => {},
  };
  const mod = await WebAssembly.instantiate(wasmBuffer, { env });
  const { memory, entry } = mod.instance.exports;
  const view = new DataView(memory.buffer);

  for (let i = 0; i < 32; i++) {
    const val = BigInt(state.initial.regs[i]);
    view.setBigUint64(GPR_OFFSET + i * 8, val, true);
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
