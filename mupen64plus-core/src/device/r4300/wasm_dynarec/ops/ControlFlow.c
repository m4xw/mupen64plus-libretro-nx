#include "../wasm_dynarec.h"
#include "../new_dynarec/new_dynarec.h"

/*
 * Emit basic branch and jump instructions. This implementation is intentionally
 * simple and always exits the current WebAssembly block when a branch is taken.
 * It uses the PC stored in the hot state to compute targets at runtime.
 * Returns 1 if the instruction was handled, 0 otherwise.
 */
int emit_controlflow_instr(char **buf, size_t *size, uint32_t inst)
{
    uint32_t op = inst >> 26;
    uint32_t rs = (inst >> 21) & 0x1f;
    uint32_t rt = (inst >> 16) & 0x1f;
    uint32_t rd = (inst >> 11) & 0x1f;
    uint32_t funct = inst & 0x3f;
    int16_t imm = inst & 0xffff;

    switch (op) {
    case 0x02: /* J */
    case 0x03: /* JAL */
    {
        uint32_t target = (inst & 0x03ffffff) << 2;
        append(buf, size,
               op == 0x02 ? "    ;; j %08x\n" : "    ;; jal %08x\n",
               target);
        if (op == 0x03) {
            /* ra = pc + 8 */
            append(buf, size,
                   "    local.get $base i32.load offset=%zu\n"
                   "    i32.const 8\n"
                   "    i32.add\n"
                   "    i64.extend_i32_s\n"
                   "    local.get $base\n"
                   "    i64.store offset=%zu\n",
                   (size_t)PC_OFFSET,
                   (size_t)GPR_OFFSET(31));
        }
        /* compute absolute target using current PC */
        append(buf, size,
               "    local.get $base i32.load offset=%zu\n"
               "    i32.const 0xf0000000\n"
               "    i32.and\n"
               "    i32.const %u\n"
               "    i32.or\n"
               "    local.set $tmp\n"
               "    local.get $base\n"
               "    local.get $tmp\n"
               "    i32.store offset=%zu\n"
               "    local.get $base\n"
               "    local.get $tmp\n"
               "    call $dispatch\n"
               "    return\n",
               (size_t)PC_OFFSET,
               target,
               (size_t)PC_OFFSET);
        return 1;
    }

    case 0x00: /* SPECIAL */
        if (funct == 0x08 || funct == 0x09) {
            /* jr/jalr */
            append(buf, size,
                   funct == 0x08 ? "    ;; jr r%u\n" : "    ;; jalr r%u, r%u\n",
                   rs, rs);
            if (funct == 0x09) {
                /* link register */
                append(buf, size,
                       "    local.get $base i32.load offset=%zu\n"
                       "    i32.const 8\n"
                       "    i32.add\n"
                       "    i64.extend_i32_s\n"
                       "    local.get $base\n"
                       "    i64.store offset=%zu\n",
                       (size_t)PC_OFFSET,
                       (size_t)GPR_OFFSET(rd ? rd : 31));
            }
            append(buf, size,
                   "    local.get $base i64.load offset=%zu\n"
                   "    i32.wrap_i64\n"
                   "    local.set $tmp\n"
                   "    local.get $base\n"
                   "    local.get $tmp\n"
                   "    i32.store offset=%zu\n"
                   "    local.get $base\n"
                   "    local.get $tmp\n"
                   "    call $dispatch\n"
                   "    return\n",
                   (size_t)GPR_OFFSET(rs),
                   (size_t)PC_OFFSET);
            return 1;
        }
        break;

    case 0x04: /* BEQ */
    case 0x05: /* BNE */
    {
        const char *cond = (op == 0x04) ? "eq" : "ne";
        int32_t offset = ((int16_t)imm) << 2;
        append(buf, size,
               "    ;; branch r%u, r%u, %d\n"
               "    local.get $base i64.load offset=%zu\n"
               "    local.get $base i64.load offset=%zu\n"
               "    i64.%s\n"
               "    if\n"
               "      local.get $base i32.load offset=%zu\n"
               "      i32.const %d\n"
               "      i32.add\n"
               "      local.set $tmp\n"
               "      local.get $base\n"
               "      local.get $tmp\n"
               "      i32.store offset=%zu\n"
               "      local.get $base\n"
               "      local.get $tmp\n"
               "      call $dispatch\n"
               "      return\n"
               "    end\n",
               rs, rt, offset,
               (size_t)GPR_OFFSET(rs),
               (size_t)GPR_OFFSET(rt), cond,
               (size_t)PC_OFFSET, offset + 4,
               (size_t)PC_OFFSET);
        return 1;
    }
    }

    return 0;
}
