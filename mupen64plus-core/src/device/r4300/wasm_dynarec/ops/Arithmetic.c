#include "../wasm_dynarec.h"
#include "../new_dynarec/new_dynarec.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/*
 * Emit arithmetic and logical instructions used by the WASM dynarec.
 * Returns 1 if the instruction was handled, 0 otherwise.
 */
int emit_arithmetic_instr(char **buf, size_t *size, uint32_t inst)
{
    uint32_t op = inst >> 26;
    uint32_t rs = (inst >> 21) & 0x1f;
    uint32_t rt = (inst >> 16) & 0x1f;
    uint32_t rd = (inst >> 11) & 0x1f;
    uint32_t funct = inst & 0x3f;

    if (op != 0x00)
        return 0;

    switch (funct) {
    case 0x20: /* add */
        append(buf, size,
               "    ;; add r%u, r%u, r%u\n"
               "    local.get $base i64.load offset=%zu\n"
               "    local.get $base i64.load offset=%zu\n"
               "    i64.add\n"
               "    local.set $t\n"
               "    local.get $base\n"
               "    local.get $t\n"
               "    i64.store offset=%zu\n",
               rd, rs, rt,
               (size_t)GPR_OFFSET(rs),
               (size_t)GPR_OFFSET(rt),
               (size_t)GPR_OFFSET(rd));
        break;
    case 0x21: /* addu */
        append(buf, size,
               "    ;; addu r%u, r%u, r%u\n"
               "    local.get $base i64.load offset=%zu\n"
               "    local.get $base i64.load offset=%zu\n"
               "    i64.add\n"
               "    local.set $t\n"
               "    local.get $base\n"
               "    local.get $t\n"
               "    i64.store offset=%zu\n",
               rd, rs, rt,
               (size_t)GPR_OFFSET(rs),
               (size_t)GPR_OFFSET(rt),
               (size_t)GPR_OFFSET(rd));
        break;
    case 0x2c: /* dadd */
        append(buf, size,
               "    ;; dadd r%u, r%u, r%u\n"
               "    local.get $base i64.load offset=%zu\n"
               "    local.get $base i64.load offset=%zu\n"
               "    i64.add\n"
               "    local.set $t\n"
               "    local.get $base\n"
               "    local.get $t\n"
               "    i64.store offset=%zu\n",
               rd, rs, rt,
               (size_t)GPR_OFFSET(rs),
               (size_t)GPR_OFFSET(rt),
               (size_t)GPR_OFFSET(rd));
        break;
    case 0x2d: /* daddu */
        append(buf, size,
               "    ;; daddu r%u, r%u, r%u\n"
               "    local.get $base i64.load offset=%zu\n"
               "    local.get $base i64.load offset=%zu\n"
               "    i64.add\n"
               "    local.set $t\n"
               "    local.get $base\n"
               "    local.get $t\n"
               "    i64.store offset=%zu\n",
               rd, rs, rt,
               (size_t)GPR_OFFSET(rs),
               (size_t)GPR_OFFSET(rt),
               (size_t)GPR_OFFSET(rd));
        break;
    case 0x2e: /* dsub */
        append(buf, size,
               "    ;; dsub r%u, r%u, r%u\n"
               "    local.get $base i64.load offset=%zu\n"
               "    local.get $base i64.load offset=%zu\n"
               "    i64.sub\n"
               "    local.set $t\n"
               "    local.get $base\n"
               "    local.get $t\n"
               "    i64.store offset=%zu\n",
               rd, rs, rt,
               (size_t)GPR_OFFSET(rs),
               (size_t)GPR_OFFSET(rt),
               (size_t)GPR_OFFSET(rd));
        break;
    case 0x2f: /* dsubu */
        append(buf, size,
               "    ;; dsubu r%u, r%u, r%u\n"
               "    local.get $base i64.load offset=%zu\n"
               "    local.get $base i64.load offset=%zu\n"
               "    i64.sub\n"
               "    local.set $t\n"
               "    local.get $base\n"
               "    local.get $t\n"
               "    i64.store offset=%zu\n",
               rd, rs, rt,
               (size_t)GPR_OFFSET(rs),
               (size_t)GPR_OFFSET(rt),
               (size_t)GPR_OFFSET(rd));
        break;
    case 0x22: /* sub */
        append(buf, size,
               "    ;; sub r%u, r%u, r%u\n"
               "    local.get $base i64.load offset=%zu\n"
               "    local.get $base i64.load offset=%zu\n"
               "    i64.sub\n"
               "    local.set $t\n"
               "    local.get $base\n"
               "    local.get $t\n"
               "    i64.store offset=%zu\n",
               rd, rs, rt,
               (size_t)GPR_OFFSET(rs),
               (size_t)GPR_OFFSET(rt),
               (size_t)GPR_OFFSET(rd));
        break;
    case 0x23: /* subu */
        append(buf, size,
               "    ;; subu r%u, r%u, r%u\n"
               "    local.get $base i64.load offset=%zu\n"
               "    local.get $base i64.load offset=%zu\n"
               "    i64.sub\n"
               "    local.set $t\n"
               "    local.get $base\n"
               "    local.get $t\n"
               "    i64.store offset=%zu\n",
               rd, rs, rt,
               (size_t)GPR_OFFSET(rs),
               (size_t)GPR_OFFSET(rt),
               (size_t)GPR_OFFSET(rd));
        break;
    case 0x24: /* and */
        append(buf, size,
               "    ;; and r%u, r%u, r%u\n"
               "    local.get $base i64.load offset=%zu\n"
               "    local.get $base i64.load offset=%zu\n"
               "    i64.and\n"
               "    local.set $t\n"
               "    local.get $base\n"
               "    local.get $t\n"
               "    i64.store offset=%zu\n",
               rd, rs, rt,
               (size_t)GPR_OFFSET(rs),
               (size_t)GPR_OFFSET(rt),
               (size_t)GPR_OFFSET(rd));
        break;
    case 0x25: /* or */
        append(buf, size,
               "    ;; or r%u, r%u, r%u\n"
               "    local.get $base i64.load offset=%zu\n"
               "    local.get $base i64.load offset=%zu\n"
               "    i64.or\n"
               "    local.set $t\n"
               "    local.get $base\n"
               "    local.get $t\n"
               "    i64.store offset=%zu\n",
               rd, rs, rt,
               (size_t)GPR_OFFSET(rs),
               (size_t)GPR_OFFSET(rt),
               (size_t)GPR_OFFSET(rd));
        break;
    case 0x26: /* xor */
        append(buf, size,
               "    ;; xor r%u, r%u, r%u\n"
               "    local.get $base i64.load offset=%zu\n"
               "    local.get $base i64.load offset=%zu\n"
               "    i64.xor\n"
               "    local.set $t\n"
               "    local.get $base\n"
               "    local.get $t\n"
               "    i64.store offset=%zu\n",
               rd, rs, rt,
               (size_t)GPR_OFFSET(rs),
               (size_t)GPR_OFFSET(rt),
               (size_t)GPR_OFFSET(rd));
        break;
    case 0x27: /* nor */
        append(buf, size,
               "    ;; nor r%u, r%u, r%u\n"
               "    local.get $base i64.load offset=%zu\n"
               "    local.get $base i64.load offset=%zu\n"
               "    i64.or\n"
               "    i64.const -1\n"
               "    i64.xor\n"
               "    local.set $t\n"
               "    local.get $base\n"
               "    local.get $t\n"
               "    i64.store offset=%zu\n",
               rd, rs, rt,
               (size_t)GPR_OFFSET(rs),
               (size_t)GPR_OFFSET(rt),
               (size_t)GPR_OFFSET(rd));
        break;
    case 0x2a: /* slt */
        append(buf, size,
               "    ;; slt r%u, r%u, r%u\n"
               "    local.get $base i64.load offset=%zu\n"
               "    local.get $base i64.load offset=%zu\n"
               "    i64.lt_s\n"
               "    local.set $tmp\n"
               "    local.get $base\n"
               "    local.get $tmp\n"
               "    i64.extend_i32_u\n"
               "    i64.store offset=%zu\n",
               rd, rs, rt,
               (size_t)GPR_OFFSET(rs),
               (size_t)GPR_OFFSET(rt),
               (size_t)GPR_OFFSET(rd));
        break;
    case 0x2b: /* sltu */
        append(buf, size,
               "    ;; sltu r%u, r%u, r%u\n"
               "    local.get $base i64.load offset=%zu\n"
               "    local.get $base i64.load offset=%zu\n"
               "    i64.lt_u\n"
               "    local.set $tmp\n"
               "    local.get $base\n"
               "    local.get $tmp\n"
               "    i64.extend_i32_u\n"
               "    i64.store offset=%zu\n",
               rd, rs, rt,
               (size_t)GPR_OFFSET(rs),
               (size_t)GPR_OFFSET(rt),
               (size_t)GPR_OFFSET(rd));
        break;
    default:
        return 0;
    }

    return 1;
}
