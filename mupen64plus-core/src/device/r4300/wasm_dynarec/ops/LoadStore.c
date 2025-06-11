#include "../wasm_dynarec.h"
#include "../../new_dynarec/new_dynarec.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

/*
 * Emit load and store instructions used by the WASM dynarec.
 * Returns 1 if the instruction was handled, 0 otherwise.
 */
int emit_loadstore_instr(char **buf, size_t *size, uint32_t inst)
{
    uint32_t op = inst >> 26;
    uint32_t rs = (inst >> 21) & 0x1f;
    uint32_t rt = (inst >> 16) & 0x1f;
    int16_t imm = inst & 0xffff;

    switch (op) {
            case 0x20:
                append(buf, size,
                       "    ;; lb r%u, %d(r%u)\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i64.const %d\n"
                       "    i64.add\n"
                       "    local.set $t\n"
                       "    local.get $base\n"
                       "    local.get $t\n"
                       "    i32.wrap_i64\n"
                       "    local.tee $tmp\n"
                       "    call $mem_read32\n"
                       "    local.get $tmp\n"
                       "    i32.const 3\n"
                       "    i32.and\n"
                       "    i32.const 3\n"
                       "    i32.xor\n"
                       "    i32.const 3\n"
                       "    i32.shl\n"
                       "    i32.shr_u\n"
                       "    i32.const 24\n"
                       "    i32.shl\n"
                       "    i32.const 24\n"
                       "    i32.shr_s\n"
                       "    i64.extend_i32_s\n"
                       "    local.set $t\n"
                       "    local.get $base\n"
                       "    local.get $t\n"
                       "    i64.store offset=%zu\n",
                       rt, imm, rs,
                       (size_t)GPR_OFFSET(rs), imm,
                       (size_t)GPR_OFFSET(rt));
                return 1;
            case 0x21:
                append(buf, size,
                       "    ;; lh r%u, %d(r%u)\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i64.const %d\n"
                       "    i64.add\n"
                       "    local.set $t\n"
                       "    local.get $base\n"
                       "    local.get $t\n"
                       "    i32.wrap_i64\n"
                       "    local.tee $tmp\n"
                       "    call $mem_read32\n"
                       "    local.get $tmp\n"
                       "    i32.const 2\n"
                       "    i32.and\n"
                       "    i32.const 2\n"
                       "    i32.xor\n"
                       "    i32.const 3\n"
                       "    i32.shl\n"
                       "    i32.shr_u\n"
                       "    i32.const 16\n"
                       "    i32.shl\n"
                       "    i32.const 16\n"
                       "    i32.shr_s\n"
                       "    i64.extend_i32_s\n"
                       "    local.set $t\n"
                       "    local.get $base\n"
                       "    local.get $t\n"
                       "    i64.store offset=%zu\n",
                       rt, imm, rs,
                       (size_t)GPR_OFFSET(rs), imm,
                       (size_t)GPR_OFFSET(rt));
                return 1;
            case 0x22:
                append(buf, size,
                       "    ;; lwl r%u, %d(r%u) (approx)\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i64.const %d\n"
                       "    i64.add\n"
                       "    local.set $t\n"
                       "    local.get $base\n"
                       "    local.get $t\n"
                       "    i32.wrap_i64\n"
                       "    call $mem_read32\n"
                       "    i64.extend_i32_s\n"
                       "    local.set $t\n"
                       "    local.get $base\n"
                       "    local.get $t\n"
                       "    i64.store offset=%zu\n",
                       rt, imm, rs,
                       (size_t)GPR_OFFSET(rs), imm,
                       (size_t)GPR_OFFSET(rt));
                return 1;
            case 0x23:
                append(buf, size,
                       "    ;; lw r%u, %d(r%u)\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i64.const %d\n"
                       "    i64.add\n"
                       "    local.set $t\n"
                       "    local.get $base\n"
                       "    local.get $t\n"
                       "    i32.wrap_i64\n"
                       "    call $mem_read32\n"
                       "    i64.extend_i32_s\n"
                       "    local.set $t\n"
                       "    local.get $base\n"
                       "    local.get $t\n"
                       "    i64.store offset=%zu\n",
                       rt, imm, rs,
                       (size_t)GPR_OFFSET(rs), imm,
                       (size_t)GPR_OFFSET(rt));
                return 1;
            case 0x24:
                append(buf, size,
                       "    ;; lbu r%u, %d(r%u)\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i64.const %d\n"
                       "    i64.add\n"
                       "    local.set $t\n"
                       "    local.get $base\n"
                       "    local.get $t\n"
                       "    i32.wrap_i64\n"
                       "    local.tee $tmp\n"
                       "    call $mem_read32\n"
                       "    local.get $tmp\n"
                       "    i32.const 3\n"
                       "    i32.and\n"
                       "    i32.const 3\n"
                       "    i32.xor\n"
                       "    i32.const 3\n"
                       "    i32.shl\n"
                       "    i32.shr_u\n"
                       "    i32.const 0xff\n"
                       "    i32.and\n"
                       "    i64.extend_i32_u\n"
                       "    local.set $t\n"
                       "    local.get $base\n"
                       "    local.get $t\n"
                       "    i64.store offset=%zu\n",
                       rt, imm, rs,
                       (size_t)GPR_OFFSET(rs), imm,
                       (size_t)GPR_OFFSET(rt));
                return 1;
            case 0x25:
                append(buf, size,
                       "    ;; lhu r%u, %d(r%u)\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i64.const %d\n"
                       "    i64.add\n"
                       "    local.set $t\n"
                       "    local.get $base\n"
                       "    local.get $t\n"
                       "    i32.wrap_i64\n"
                       "    local.tee $tmp\n"
                       "    call $mem_read32\n"
                       "    local.get $tmp\n"
                       "    i32.const 2\n"
                       "    i32.and\n"
                       "    i32.const 2\n"
                       "    i32.xor\n"
                       "    i32.const 3\n"
                       "    i32.shl\n"
                       "    i32.shr_u\n"
                       "    i32.const 0xffff\n"
                       "    i32.and\n"
                       "    i64.extend_i32_u\n"
                       "    local.set $t\n"
                       "    local.get $base\n"
                       "    local.get $t\n"
                       "    i64.store offset=%zu\n",
                       rt, imm, rs,
                       (size_t)GPR_OFFSET(rs), imm,
                       (size_t)GPR_OFFSET(rt));
                return 1;
            case 0x26:
                append(buf, size,
                       "    ;; lwr r%u, %d(r%u) (approx)\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i64.const %d\n"
                       "    i64.add\n"
                       "    local.set $t\n"
                       "    local.get $base\n"
                       "    local.get $t\n"
                       "    i32.wrap_i64\n"
                       "    call $mem_read32\n"
                       "    i64.extend_i32_s\n"
                       "    local.set $t\n"
                       "    local.get $base\n"
                       "    local.get $t\n"
                       "    i64.store offset=%zu\n",
                       rt, imm, rs,
                       (size_t)GPR_OFFSET(rs), imm,
                       (size_t)GPR_OFFSET(rt));
                return 1;
            case 0x27:
                append(buf, size,
                       "    ;; lwu r%u, %d(r%u)\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i64.const %d\n"
                       "    i64.add\n"
                       "    local.set $t\n"
                       "    local.get $base\n"
                       "    local.get $t\n"
                       "    i32.wrap_i64\n"
                       "    call $mem_read32\n"
                       "    i64.extend_i32_u\n"
                       "    local.set $t\n"
                       "    local.get $base\n"
                       "    local.get $t\n"
                       "    i64.store offset=%zu\n",
                       rt, imm, rs,
                       (size_t)GPR_OFFSET(rs), imm,
                       (size_t)GPR_OFFSET(rt));
                return 1;
            case 0x30:
                append(buf, size,
                       "    ;; ll r%u, %d(r%u) (approx)\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i64.const %d\n"
                       "    i64.add\n"
                       "    local.set $t\n"
                       "    local.get $base\n"
                       "    local.get $t\n"
                       "    i32.wrap_i64\n"
                       "    call $mem_read32\n"
                       "    i64.extend_i32_s\n"
                       "    local.set $t\n"
                       "    local.get $base\n"
                       "    local.get $t\n"
                       "    i64.store offset=%zu\n",
                       rt, imm, rs,
                       (size_t)GPR_OFFSET(rs), imm,
                       (size_t)GPR_OFFSET(rt));
                return 1;
            case 0x31:
                append(buf, size,
                       "    ;; lwc1 f%u, %d(r%u)\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i64.const %d\n"
                       "    i64.add\n"
                       "    local.set $t\n"
                       "    local.get $base\n"
                       "    local.get $t\n"
                       "    i32.wrap_i64\n"
                       "    call $mem_read32\n"
                       "    local.set $tmp\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    local.get $tmp\n"
                       "    i32.store\n",
                       rt, imm, rs,
                       (size_t)GPR_OFFSET(rs), imm,
                       (size_t)CP1_SIMPLE_OFFSET(rt));
                return 1;
            case 0x35:
                append(buf, size,
                       "    ;; ldc1 f%u, %d(r%u)\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i64.const %d\n"
                       "    i64.add\n"
                       "    local.set $t\n"
                       "    local.get $base\n"
                       "    local.get $t\n"
                       "    i32.wrap_i64\n"
                       "    call $mem_read64\n"
                       "    local.set $t\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    local.get $t\n"
                       "    i64.store\n",
                       rt, imm, rs,
                       (size_t)GPR_OFFSET(rs), imm,
                       (size_t)CP1_DOUBLE_OFFSET(rt));
                return 1;
            case 0x28:
                append(buf, size,
                       "    ;; sb r%u, %d(r%u)\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i64.const %d\n"
                       "    i64.add\n"
                       "    local.set $t\n"
                       "    local.get $base\n"
                       "    local.get $t\n"
                       "    i32.wrap_i64\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    i32.const 0xff\n"
                       "    call $mem_write32\n",
                       rt, imm, rs,
                       (size_t)GPR_OFFSET(rs), imm,
                       (size_t)GPR_OFFSET(rt));
                return 1;
            case 0x29:
                append(buf, size,
                       "    ;; sh r%u, %d(r%u)\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i64.const %d\n"
                       "    i64.add\n"
                       "    local.set $t\n"
                       "    local.get $base\n"
                       "    local.get $t\n"
                       "    i32.wrap_i64\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    i32.const 0xffff\n"
                       "    call $mem_write32\n",
                       rt, imm, rs,
                       (size_t)GPR_OFFSET(rs), imm,
                       (size_t)GPR_OFFSET(rt));
                return 1;
            case 0x2a:
                append(buf, size,
                       "    ;; swl r%u, %d(r%u) (approx)\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i64.const %d\n"
                       "    i64.add\n"
                       "    local.set $t\n"
                       "    local.get $base\n"
                       "    local.get $t\n"
                       "    i32.wrap_i64\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    i32.const -1\n"
                       "    call $mem_write32\n",
                       rt, imm, rs,
                       (size_t)GPR_OFFSET(rs), imm,
                       (size_t)GPR_OFFSET(rt));
                return 1;
            case 0x2b:
                append(buf, size,
                       "    ;; sw r%u, %d(r%u)\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i64.const %d\n"
                       "    i64.add\n"
                       "    local.set $t\n"
                       "    local.get $base\n"
                       "    local.get $t\n"
                       "    i32.wrap_i64\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    i32.const -1\n"
                       "    call $mem_write32\n",
                       rt, imm, rs,
                       (size_t)GPR_OFFSET(rs), imm,
                       (size_t)GPR_OFFSET(rt));
                return 1;
            case 0x2c:
                append(buf, size,
                       "    ;; sdl r%u, %d(r%u) (approx)\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i64.const %d\n"
                       "    i64.add\n"
                       "    local.set $t\n"
                       "    local.get $base\n"
                       "    local.get $t\n"
                       "    i32.wrap_i64\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i64.const -1\n"
                       "    call $mem_write64\n",
                       rt, imm, rs,
                       (size_t)GPR_OFFSET(rs), imm,
                       (size_t)GPR_OFFSET(rt));
                return 1;
            case 0x2d:
                append(buf, size,
                       "    ;; sdr r%u, %d(r%u) (approx)\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i64.const %d\n"
                       "    i64.add\n"
                       "    local.set $t\n"
                       "    local.get $base\n"
                       "    local.get $t\n"
                       "    i32.wrap_i64\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i64.const -1\n"
                       "    call $mem_write64\n",
                       rt, imm, rs,
                       (size_t)GPR_OFFSET(rs), imm,
                       (size_t)GPR_OFFSET(rt));
                return 1;
            case 0x2e:
                append(buf, size,
                       "    ;; swr r%u, %d(r%u) (approx)\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i64.const %d\n"
                       "    i64.add\n"
                       "    local.set $t\n"
                       "    local.get $base\n"
                       "    local.get $t\n"
                       "    i32.wrap_i64\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    i32.const -1\n"
                       "    call $mem_write32\n",
                       rt, imm, rs,
                       (size_t)GPR_OFFSET(rs), imm,
                       (size_t)GPR_OFFSET(rt));
                return 1;
            case 0x2f:
                append(buf, size,
                       "    ;; cache (ignored)\n");
                return 1;
            case 0x33:
                append(buf, size,
                       "    ;; pref (ignored)\n");
                return 1;
            case 0x37:
                append(buf, size,
                       "    ;; ld r%u, %d(r%u)\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i64.const %d\n"
                       "    i64.add\n"
                       "    local.set $t\n"
                       "    local.get $base\n"
                       "    local.get $t\n"
                       "    i32.wrap_i64\n"
                       "    call $mem_read64\n"
                       "    local.set $t\n"
                       "    local.get $base\n"
                       "    local.get $t\n"
                       "    i64.store offset=%zu\n",
                       rt, imm, rs,
                       (size_t)GPR_OFFSET(rs), imm,
                       (size_t)GPR_OFFSET(rt));
                return 1;
            case 0x38:
                append(buf, size,
                       "    ;; sc r%u, %d(r%u) (approx)\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i64.const %d\n"
                       "    i64.add\n"
                       "    local.set $t\n"
                       "    local.get $base\n"
                       "    local.get $t\n"
                       "    i32.wrap_i64\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    i32.const -1\n"
                       "    call $mem_write32\n",
                       rt, imm, rs,
                       (size_t)GPR_OFFSET(rs), imm,
                       (size_t)GPR_OFFSET(rt));
                append(buf, size,
                       "    local.get $base\n"
                       "    i64.const 1\n"
                       "    i64.store offset=%zu\n",
                       (size_t)GPR_OFFSET(rt));
                return 1;
            case 0x39:
                append(buf, size,
                       "    ;; swc1 f%u, %d(r%u)\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i64.const %d\n"
                       "    i64.add\n"
                       "    local.set $t\n"
                       "    local.get $base\n"
                       "    local.get $t\n"
                       "    i32.wrap_i64\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    i32.load\n"
                       "    i32.const -1\n"
                       "    call $mem_write32\n",
                       rt, imm, rs,
                       (size_t)GPR_OFFSET(rs), imm,
                       (size_t)CP1_SIMPLE_OFFSET(rt));
                return 1;
            case 0x3d:
                append(buf, size,
                       "    ;; sdc1 f%u, %d(r%u)\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i64.const %d\n"
                       "    i64.add\n"
                       "    local.set $t\n"
                       "    local.get $base\n"
                       "    local.get $t\n"
                       "    i32.wrap_i64\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i64.const -1\n"
                       "    call $mem_write64\n",
                       rt, imm, rs,
                       (size_t)GPR_OFFSET(rs), imm,
                       (size_t)CP1_DOUBLE_OFFSET(rt));
                return 1;
            case 0x3f:
                append(buf, size,
                       "    ;; sd r%u, %d(r%u)\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i64.const %d\n"
                       "    i64.add\n"
                       "    local.set $t\n"
                       "    local.get $base\n"
                       "    local.get $t\n"
                       "    i32.wrap_i64\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i64.const -1\n"
                       "    call $mem_write64\n",
                       rt, imm, rs,
                       (size_t)GPR_OFFSET(rs), imm,
                       (size_t)GPR_OFFSET(rt));
                return 1;
            default:
                return 0;
            }
}
