# Mupen64Plus-Next

Mupen64Plus-Next is a N64 emulation library for the [libretro API](http://www.libretro.com/), based on Mupen64Plus (see below).

It is also the successor of the old Mupen64Plus libretro core.

> You can *always* rely on it to give you an excellent Majora's Mask experience. Seriously.

#### How is this different from any N64 libretro-core, ever?

Due to the amount of libraries that are used and are in regular need of maintenance, I have strict rules about adding dependencies.  
This allows for easy maintenance, so available time can be spent on useful improvements and lowers the burden.  
By default the experience will be very simliar to the N64 emulators you know and love with *a lot extra*.

> **Sidenote:**  
While I accept pretty much every reasonable contribution, hacks must not impact behavior by default, unless justified.  
If you need to add a dependency, please consult me first.  
Force-pushes on all branches but `develop` and `master` are fair game.
`master` has the best stability memes, if that's your *thing*.

#### Used Technologies

The following projects have been incorporated into this repository:

- [mupen64plus](https://github.com/mupen64plus/mupen64plus-core)
- [GLideN64](https://github.com/gonetz/GLideN64)
- [cxd4](https://github.com/cxd4/rsp)
- [parallel-rsp](https://github.com/Themaister/parallel-rsp)
- [angrylion-rdp-plus](https://github.com/ata4/angrylion-rdp-plus) (Currently based on it's [ParaLLel](https://github.com/libretro/parallel-n64/) variant)

### Experimental WebAssembly dynamic recompiler

This repository now includes an experimental dynamic recompiler that
targets WebAssembly.  The source can be found in
`mupen64plus-core/src/device/r4300/wasm_dynarec/`.  The backend can
translate a growing subset of arithmetic, logical, memory access, and
control-flow instructions into WebAssembly text.  Generated code now loads and
stores registers relative to the `new_dynarec_hot_state` structure so that it
can eventually be executed directly.  A simple dispatcher function recompiles
and prints blocks using this state.  Branch translation still accounts for delay
slots and "likely" semantics.  Nearly all integer opcodes are now translated,
including conditional moves like `MOVN/MOVZ`, 64-bit arithmetic and the
unaligned or atomic load/store variants.  Dynamic `JR`/`JALR` instructions
store their runtime target to `pcaddr` and invoke a host dispatcher so control
flows to the correct block.  Direct `J`/`JAL` calls jump within the compiled
block when possible; if the target lies outside the current block, the
dispatcher is invoked with that static address instead.
Recent updates added handling for less common instructions such as `LDL/LDR`,
system calls (`SYSCALL`, `BREAK`, `SYNC`), and cache management opcodes.
`CACHE` and `PREF` are currently treated as no-ops.
A table showing current opcode coverage can be found in [docs/wasm_dynarec_opcode_coverage.md](docs/wasm_dynarec_opcode_coverage.md).

 An accompanying unit test in `mupen64plus-core/test/wasm_dynarec` demonstrates
 translating a small MIPS routine to WebAssembly text. The test assembles the
 generated WAT using `wat2wasm` from the
 [WABT toolkit](https://github.com/WebAssembly/wabt) to ensure it is valid
 WebAssembly code.  The same directory also provides a small Node.js script
`run_generated_wasm.js` that instantiates this module, initializes a minimal
CPU state and executes the exported block using the browser-style WebAssembly
API.
When compiled with Emscripten the `wasm_dynarec_exec` function now calls a JavaScript helper. This helper uses `Module['wabt']` or `Binaryen` to compile the generated WAT into a binary module before running it. The runtime must provide one of these objects so execution can proceed. Memory accesses from JavaScript use helper functions exported with `EMSCRIPTEN_KEEPALIVE`.

Additional unit tests exercise signed multiply/divide edge cases to ensure the
HI and LO registers receive correctly sign-extended results even when operands
are negative or the product overflows 32 bits.
Another test builds a small loop with nested branches and a dynamic `JR`
to jump back inside the block, verifying delay slots and that `pcaddr` is
updated correctly when control flow becomes complex.

To help approach full opcode coverage a helper script named
`generate_opcode_tests.py` under the same directory parses
`mips_instructions.def` and assembles one-off test blocks for any opcode with a
known template.  Running the script will emit the machine code words used for
each block and write the temporary assembly files beneath `generated/`.

Future work for the WebAssembly backend includes:
 - hooking generated code into the CPU core for execution
 - completing opcode coverage, especially for floating point and system
   instructions
 - emitting binary WebAssembly modules instead of text only
 - optimizing register usage and memory access patterns

#### Acknowledgments

A special thanks to:

- The Mupen64Plus Team, especially Gillou68310
- gonetz and those that have worked on GLideN64, especially fzurita
- The Authors of cxd4 and angrylion-rdp-plus (ata4)
- themaister for parallel-rsp and parallel-rdp (including the Vulkan integration)
- Everyone in the libretro Team


\- m4xw
