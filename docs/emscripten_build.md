# Building with Emscripten

This guide explains how to compile the libretro core for use in a browser.

## Build steps

Ensure the Emscripten SDK is installed and `emcc` is in your `PATH`. From the
repository root run:

```
make platform=emscripten
```

The build will generate `mupen64plus_next_libretro_emscripten.bc`. Convert the
bitcode to a runnable module with `emcc`. WebAssembly dynamic recompilation uses
64‑bit integers for callbacks which requires BigInt support when calling into
the compiled module. Pass `-s WASM_BIGINT=1` so exported functions accept
`BigInt` values from the generated blocks:

```
emcc mupen64plus_next_libretro_emscripten.bc -O2 -s WASM=1 -s MODULARIZE=1 -s EXPORT_NAME="Module" \
    -s WASM_BIGINT=1 -o mupen64plus_next.js
```

Include either `wabt.js` or `binaryen.js` in your page so the runtime provides
`Module['wabt']` or a global `Binaryen` object. `wasm_dynarec_exec` uses these
helpers to compile generated WebAssembly text on the fly.

The build exports memory callbacks via `EMSCRIPTEN_KEEPALIVE` so they remain
accessible from JavaScript. These functions appear on the `Module` object as
`_wasm_dynarec_dispatch_import`, `_wasm_dynarec_read_word`,
`_wasm_dynarec_read_dword`, `_wasm_dynarec_write_word` and
`_wasm_dynarec_write_dword`.

## Example

```html
<script src="wabt.js"></script> <!-- or binaryen.js -->
<script src="mupen64plus_next.js"></script>
<script>
Module.onRuntimeInitialized = function () {
  // Load a ROM file if desired using the Emscripten FS, then start the core.
  Module.ccall('retro_init');
  // Example: Module.ccall('retro_run');
  // Memory helpers are available as Module._wasm_dynarec_read_word(), etc.
};
</script>
```
