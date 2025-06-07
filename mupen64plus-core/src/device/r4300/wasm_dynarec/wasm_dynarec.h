#ifndef M64P_DEVICE_R4300_WASM_DYNAREC_H
#define M64P_DEVICE_R4300_WASM_DYNAREC_H

#include <stdint.h>
#include <stddef.h>

struct r4300_core;

/* Helpers to calculate offsets within new_dynarec_hot_state */
#define GPR_OFFSET(r) (offsetof(struct new_dynarec_hot_state, regs) + (r) * sizeof(int64_t))
#define HI_OFFSET offsetof(struct new_dynarec_hot_state, hi)
#define LO_OFFSET offsetof(struct new_dynarec_hot_state, lo)
#define PC_OFFSET offsetof(struct new_dynarec_hot_state, pcaddr)
#define CP0_OFFSET(r) (offsetof(struct new_dynarec_hot_state, cp0_regs) + (r) * sizeof(uint32_t))
#define CP1_SIMPLE_OFFSET(r) (offsetof(struct new_dynarec_hot_state, cp1_regs_simple) + (r) * sizeof(float *))
#define CP1_DOUBLE_OFFSET(r) (offsetof(struct new_dynarec_hot_state, cp1_regs_double) + (r) * sizeof(double *))
#define CP1_FCR31_OFFSET offsetof(struct new_dynarec_hot_state, cp1_fcr31)

void wasm_dynarec_init(struct r4300_core *r4300);
void wasm_dynarec_cleanup(void);
void invalidate_cached_code_wasm_dynarec(struct r4300_core *r4300, uint32_t address, size_t size);
void wasm_dynarec_recompile_block(struct r4300_core *r4300, const uint32_t *iw, size_t count, uint32_t address);
void wasm_dynarec_exec(struct r4300_core *r4300, uint32_t address);

/* Debug helper to dump generated WebAssembly text for a block */
void wasm_dynarec_dump(uint32_t address);

/* Get the generated WebAssembly text for the block at ADDRESS */
const char *wasm_dynarec_get_wat(uint32_t address);

/* Dispatcher using new_dynarec_hot_state */
void wasm_dynarec_dispatch(struct r4300_core *r4300, uint32_t address);
void wasm_dynarec_entry(struct r4300_core *r4300);

/* emit helper functions split into separate modules */
int emit_arithmetic_instr(char **buf, size_t *size, uint32_t inst);
int emit_loadstore_instr(char **buf, size_t *size, uint32_t inst);
int emit_fpu_instr(char **buf, size_t *size, uint32_t inst);
int emit_controlflow_instr(char **buf, size_t *size, uint32_t inst);
void append(char **buf, size_t *size, const char *fmt, ...);

#endif /* M64P_DEVICE_R4300_WASM_DYNAREC_H */
