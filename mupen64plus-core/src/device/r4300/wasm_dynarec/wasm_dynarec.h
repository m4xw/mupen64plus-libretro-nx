#ifndef M64P_DEVICE_R4300_WASM_DYNAREC_H
#define M64P_DEVICE_R4300_WASM_DYNAREC_H

#include <stdint.h>
#include <stddef.h>

struct r4300_core;

void wasm_dynarec_init(struct r4300_core *r4300);
void wasm_dynarec_cleanup(void);
void wasm_dynarec_recompile_block(struct r4300_core *r4300, const uint32_t *iw, size_t count, uint32_t address);
void wasm_dynarec_exec(struct r4300_core *r4300, uint32_t address);

/* Debug helper to dump generated WebAssembly text for a block */
void wasm_dynarec_dump(uint32_t address);

/* Get the generated WebAssembly text for the block at ADDRESS */
const char *wasm_dynarec_get_wat(uint32_t address);

/* Dispatcher using new_dynarec_hot_state */
void wasm_dynarec_dispatch(struct r4300_core *r4300, uint32_t address);
void wasm_dynarec_entry(struct r4300_core *r4300);

#endif /* M64P_DEVICE_R4300_WASM_DYNAREC_H */
