#include "wasm_dynarec.h"

#include "api/callbacks.h"
#include "device/r4300/r4300_core.h"
#include "device/r4300/new_dynarec/new_dynarec.h"
#include "device/r4300/fpu.h"
#include "wasm3.h"
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

#define GPR_OFFSET(r) (offsetof(struct new_dynarec_hot_state, regs) + (r) * sizeof(int64_t))
#define HI_OFFSET offsetof(struct new_dynarec_hot_state, hi)
#define LO_OFFSET offsetof(struct new_dynarec_hot_state, lo)
#define PC_OFFSET offsetof(struct new_dynarec_hot_state, pcaddr)
#define CP0_OFFSET(r) (offsetof(struct new_dynarec_hot_state, cp0_regs) + (r) * sizeof(uint32_t))
#define CP1_SIMPLE_OFFSET(r) (offsetof(struct new_dynarec_hot_state, cp1_regs_simple) + (r) * sizeof(float *))
#define CP1_DOUBLE_OFFSET(r) (offsetof(struct new_dynarec_hot_state, cp1_regs_double) + (r) * sizeof(double *))
#define CP1_FCR31_OFFSET offsetof(struct new_dynarec_hot_state, cp1_fcr31)

struct wasm_dynarec_block
{
    uint32_t address;
    char *wat;
    size_t wat_size;
    uint32_t mem_pages;
};

static struct wasm_dynarec_block *g_blocks = NULL;
static size_t g_blocks_count = 0;

static struct r4300_core *g_current_cpu = NULL;

m3ApiRawFunction(wasm_dynarec_dispatch_import)
{
    m3ApiGetArg(uint32_t, base);
    m3ApiGetArg(uint32_t, addr);
    (void)base;
    if (g_current_cpu)
        wasm_dynarec_dispatch(g_current_cpu, addr);
    m3ApiSuccess();
}

m3ApiRawFunction(wasm_dynarec_read_word)
{
    m3ApiReturnType(uint32_t)
    m3ApiGetArg(uint32_t, base);
    m3ApiGetArg(uint32_t, addr);
    (void)base;
    uint32_t value = 0;
    if (g_current_cpu)
        r4300_read_aligned_word(g_current_cpu, addr, &value);
    m3ApiReturn(value);
}

m3ApiRawFunction(wasm_dynarec_read_dword)
{
    m3ApiReturnType(uint64_t)
    m3ApiGetArg(uint32_t, base);
    m3ApiGetArg(uint32_t, addr);
    (void)base;
    uint64_t value = 0;
    if (g_current_cpu)
        r4300_read_aligned_dword(g_current_cpu, addr, &value);
    m3ApiReturn(value);
}

m3ApiRawFunction(wasm_dynarec_write_word)
{
    m3ApiGetArg(uint32_t, base);
    m3ApiGetArg(uint32_t, addr);
    m3ApiGetArg(uint32_t, value);
    m3ApiGetArg(uint32_t, mask);
    (void)base;
    if (g_current_cpu)
        r4300_write_aligned_word(g_current_cpu, addr, value, mask);
    m3ApiSuccess();
}

m3ApiRawFunction(wasm_dynarec_write_dword)
{
    m3ApiGetArg(uint32_t, base);
    m3ApiGetArg(uint32_t, addr);
    m3ApiGetArg(uint64_t, value);
    m3ApiGetArg(uint64_t, mask);
    (void)base;
    if (g_current_cpu)
        r4300_write_aligned_dword(g_current_cpu, addr, value, mask);
    m3ApiSuccess();
}

static struct wasm_dynarec_block *get_block(uint32_t address)
{
    for (size_t i = 0; i < g_blocks_count; ++i)
        if (g_blocks[i].address == address)
            return &g_blocks[i];
    return NULL;
}

static void append(char **buf, size_t *size, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int needed = vsnprintf(NULL, 0, fmt, args);
    va_end(args);

    if (*buf == NULL) {
        *size = (size_t)needed + 1;
        *buf = malloc(*size);
        (*buf)[0] = '\0';
    } else {
        size_t len = strlen(*buf);
        if (len + needed + 1 > *size) {
            *size = len + needed + 1;
            *buf = realloc(*buf, *size);
        }
    }

    va_start(args, fmt);
    vsprintf(*buf + strlen(*buf), fmt, args);
    va_end(args);
}

/* emit WebAssembly text for simple (non-branch) instructions */
static void emit_simple_instr(char **buf, size_t *size, uint32_t inst)
{
    uint32_t op = inst >> 26;
    uint32_t rs = (inst >> 21) & 0x1f;
    uint32_t rt = (inst >> 16) & 0x1f;
    uint32_t rd = (inst >> 11) & 0x1f;
    uint32_t fd = (inst >> 6) & 0x1f;
    uint32_t funct = inst & 0x3f;
    int16_t imm = inst & 0xffff;

    switch (op) {
    case 0x00:
        switch (funct) {
        case 0x20:
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
        case 0x21:
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
        case 0x2c:
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
        case 0x2d:
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
        case 0x2e:
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
        case 0x2f:
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
        case 0x22:
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
        case 0x23:
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
        case 0x24:
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
        case 0x25:
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
        case 0x26:
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
        case 0x27:
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
        case 0x2a:
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
        case 0x2b:
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
        case 0x0a:
            append(buf, size,
                   "    ;; movz r%u, r%u, r%u\n"
                   "    local.get $base i64.load offset=%zu\n"
                   "    i64.eqz\n"
                   "    if (then\n"
                   "      local.get $base i64.load offset=%zu\n"
                   "      local.get $base\n"
                   "      i64.store offset=%zu\n"
                   "    end)\n",
                   rd, rs, rt,
                   (size_t)GPR_OFFSET(rt),
                   (size_t)GPR_OFFSET(rs),
                   (size_t)GPR_OFFSET(rd));
            break;
        case 0x0b:
            append(buf, size,
                   "    ;; movn r%u, r%u, r%u\n"
                   "    local.get $base i64.load offset=%zu\n"
                   "    i64.eqz\n"
                   "    i32.eqz\n"
                   "    if (then\n"
                   "      local.get $base i64.load offset=%zu\n"
                   "      local.get $base\n"
                   "      i64.store offset=%zu\n"
                   "    end)\n",
                   rd, rs, rt,
                   (size_t)GPR_OFFSET(rt),
                   (size_t)GPR_OFFSET(rs),
                   (size_t)GPR_OFFSET(rd));
            break;
        case 0x00: {
            uint32_t sa = (inst >> 6) & 0x1f;
            if (rd == 0 && rt == 0 && sa == 0) {
                append(buf, size, "    ;; nop\n");
            } else {
                append(buf, size,
                       "    ;; sll r%u, r%u, %u\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i64.const %u\n"
                       "    i64.shl\n"
                       "    local.set $t\n"
                       "    local.get $base\n"
                       "    local.get $t\n"
                       "    i64.store offset=%zu\n",
                       rd, rt, sa,
                       (size_t)GPR_OFFSET(rt), sa,
                       (size_t)GPR_OFFSET(rd));
            }
            break; }
        case 0x02: {
            uint32_t sa = (inst >> 6) & 0x1f;
            append(buf, size,
                   "    ;; srl r%u, r%u, %u\n"
                   "    local.get $base i64.load offset=%zu\n"
                   "    i64.const %u\n"
                   "    i64.shr_u\n"
                   "    local.set $t\n"
                   "    local.get $base\n"
                   "    local.get $t\n"
                   "    i64.store offset=%zu\n",
                   rd, rt, sa,
                   (size_t)GPR_OFFSET(rt), sa,
                   (size_t)GPR_OFFSET(rd));
            break; }
        case 0x03: {
            uint32_t sa = (inst >> 6) & 0x1f;
            append(buf, size,
                   "    ;; sra r%u, r%u, %u\n"
                   "    local.get $base i64.load offset=%zu\n"
                   "    i64.const %u\n"
                   "    i64.shr_s\n"
                   "    local.set $t\n"
                   "    local.get $base\n"
                   "    local.get $t\n"
                   "    i64.store offset=%zu\n",
                   rd, rt, sa,
                   (size_t)GPR_OFFSET(rt), sa,
                   (size_t)GPR_OFFSET(rd));
            break; }
        case 0x38: {
            uint32_t sa = (inst >> 6) & 0x1f;
            append(buf, size,
                   "    ;; dsll r%u, r%u, %u\n"
                   "    local.get $base i64.load offset=%zu\n"
                   "    i64.const %u\n"
                   "    i64.shl\n"
                   "    local.set $t\n"
                   "    local.get $base\n"
                   "    local.get $t\n"
                   "    i64.store offset=%zu\n",
                   rd, rt, sa,
                   (size_t)GPR_OFFSET(rt), sa,
                   (size_t)GPR_OFFSET(rd));
            break; }
        case 0x3a: {
            uint32_t sa = (inst >> 6) & 0x1f;
            append(buf, size,
                   "    ;; dsrl r%u, r%u, %u\n"
                   "    local.get $base i64.load offset=%zu\n"
                   "    i64.const %u\n"
                   "    i64.shr_u\n"
                   "    local.set $t\n"
                   "    local.get $base\n"
                   "    local.get $t\n"
                   "    i64.store offset=%zu\n",
                   rd, rt, sa,
                   (size_t)GPR_OFFSET(rt), sa,
                   (size_t)GPR_OFFSET(rd));
            break; }
        case 0x3b: {
            uint32_t sa = (inst >> 6) & 0x1f;
            append(buf, size,
                   "    ;; dsra r%u, r%u, %u\n"
                   "    local.get $base i64.load offset=%zu\n"
                   "    i64.const %u\n"
                   "    i64.shr_s\n"
                   "    local.set $t\n"
                   "    local.get $base\n"
                   "    local.get $t\n"
                   "    i64.store offset=%zu\n",
                   rd, rt, sa,
                   (size_t)GPR_OFFSET(rt), sa,
                   (size_t)GPR_OFFSET(rd));
            break; }
        case 0x3c: {
            uint32_t sa = (inst >> 6) & 0x1f;
            append(buf, size,
                   "    ;; dsll32 r%u, r%u, %u\n"
                   "    local.get $base i64.load offset=%zu\n"
                   "    i64.const %u\n"
                   "    i64.shl\n"
                   "    local.set $t\n"
                   "    local.get $base\n"
                   "    local.get $t\n"
                   "    i64.store offset=%zu\n",
                   rd, rt, 32 + sa,
                   (size_t)GPR_OFFSET(rt), 32 + sa,
                   (size_t)GPR_OFFSET(rd));
            break; }
        case 0x3e: {
            uint32_t sa = (inst >> 6) & 0x1f;
            append(buf, size,
                   "    ;; dsrl32 r%u, r%u, %u\n"
                   "    local.get $base i64.load offset=%zu\n"
                   "    i64.const %u\n"
                   "    i64.shr_u\n"
                   "    local.set $t\n"
                   "    local.get $base\n"
                   "    local.get $t\n"
                   "    i64.store offset=%zu\n",
                   rd, rt, 32 + sa,
                   (size_t)GPR_OFFSET(rt), 32 + sa,
                   (size_t)GPR_OFFSET(rd));
            break; }
        case 0x3f: {
            uint32_t sa = (inst >> 6) & 0x1f;
            append(buf, size,
                   "    ;; dsra32 r%u, r%u, %u\n"
                   "    local.get $base i64.load offset=%zu\n"
                   "    i64.const %u\n"
                   "    i64.shr_s\n"
                   "    local.set $t\n"
                   "    local.get $base\n"
                   "    local.get $t\n"
                   "    i64.store offset=%zu\n",
                   rd, rt, 32 + sa,
                   (size_t)GPR_OFFSET(rt), 32 + sa,
                   (size_t)GPR_OFFSET(rd));
            break; }
        case 0x04:
            append(buf, size,
                   "    ;; sllv r%u, r%u, r%u\n"
                   "    local.get $base i64.load offset=%zu\n"
                   "    local.get $base i64.load offset=%zu\n"
                   "    i64.shl\n"
                   "    local.set $t\n"
                   "    local.get $base\n"
                   "    local.get $t\n"
                   "    i64.store offset=%zu\n",
                   rd, rt, rs,
                   (size_t)GPR_OFFSET(rt),
                   (size_t)GPR_OFFSET(rs),
                   (size_t)GPR_OFFSET(rd));
            break;
        case 0x06:
            append(buf, size,
                   "    ;; srlv r%u, r%u, r%u\n"
                   "    local.get $base i64.load offset=%zu\n"
                   "    local.get $base i64.load offset=%zu\n"
                   "    i64.shr_u\n"
                   "    local.set $t\n"
                   "    local.get $base\n"
                   "    local.get $t\n"
                   "    i64.store offset=%zu\n",
                   rd, rt, rs,
                   (size_t)GPR_OFFSET(rt),
                   (size_t)GPR_OFFSET(rs),
                   (size_t)GPR_OFFSET(rd));
            break;
        case 0x07:
            append(buf, size,
                   "    ;; srav r%u, r%u, r%u\n"
                   "    local.get $base i64.load offset=%zu\n"
                   "    local.get $base i64.load offset=%zu\n"
                   "    i64.shr_s\n"
                   "    local.set $t\n"
                   "    local.get $base\n"
                   "    local.get $t\n"
                   "    i64.store offset=%zu\n",
                   rd, rt, rs,
                   (size_t)GPR_OFFSET(rt),
                   (size_t)GPR_OFFSET(rs),
                   (size_t)GPR_OFFSET(rd));
            break;
        case 0x14:
            append(buf, size,
                   "    ;; dsllv r%u, r%u, r%u\n"
                   "    local.get $base i64.load offset=%zu\n"
                   "    local.get $base i64.load offset=%zu\n"
                   "    i64.shl\n"
                   "    local.set $t\n"
                   "    local.get $base\n"
                   "    local.get $t\n"
                   "    i64.store offset=%zu\n",
                   rd, rt, rs,
                   (size_t)GPR_OFFSET(rt),
                   (size_t)GPR_OFFSET(rs),
                   (size_t)GPR_OFFSET(rd));
            break;
        case 0x16:
            append(buf, size,
                   "    ;; dsrlv r%u, r%u, r%u\n"
                   "    local.get $base i64.load offset=%zu\n"
                   "    local.get $base i64.load offset=%zu\n"
                   "    i64.shr_u\n"
                   "    local.set $t\n"
                   "    local.get $base\n"
                   "    local.get $t\n"
                   "    i64.store offset=%zu\n",
                   rd, rt, rs,
                   (size_t)GPR_OFFSET(rt),
                   (size_t)GPR_OFFSET(rs),
                   (size_t)GPR_OFFSET(rd));
            break;
        case 0x17:
            append(buf, size,
                   "    ;; dsrav r%u, r%u, r%u\n"
                   "    local.get $base i64.load offset=%zu\n"
                   "    local.get $base i64.load offset=%zu\n"
                   "    i64.shr_s\n"
                   "    local.set $t\n"
                   "    local.get $base\n"
                   "    local.get $t\n"
                   "    i64.store offset=%zu\n",
                   rd, rt, rs,
                   (size_t)GPR_OFFSET(rt),
                   (size_t)GPR_OFFSET(rs),
                   (size_t)GPR_OFFSET(rd));
            break;
        case 0x18:
            append(buf, size,
                   "    ;; mult r%u, r%u\n"
                   "    local.get $base i64.load offset=%zu\n"
                   "    local.get $base i64.load offset=%zu\n"
                   "    i64.mul\n"
                   "    local.set $t\n"
                   "    local.get $base\n"
                   "    local.get $t\n"
                   "    i64.const 32\n"
                   "    i64.shr_s\n"
                   "    i64.store offset=%zu\n"
                   "    local.get $base\n"
                   "    local.get $t\n"
                   "    i64.store offset=%zu\n",
                   rs, rt,
                   (size_t)GPR_OFFSET(rs),
                   (size_t)GPR_OFFSET(rt),
                   (size_t)HI_OFFSET,
                   (size_t)LO_OFFSET);
            break;
        case 0x19:
            append(buf, size,
                   "    ;; multu r%u, r%u\n"
                   "    local.get $base i64.load offset=%zu\n"
                   "    local.get $base i64.load offset=%zu\n"
                   "    i64.mul\n"
                   "    local.set $t\n"
                   "    local.get $base\n"
                   "    local.get $t\n"
                   "    i64.const 32\n"
                   "    i64.shr_u\n"
                   "    i64.store offset=%zu\n"
                   "    local.get $base\n"
                   "    local.get $t\n"
                   "    i64.store offset=%zu\n",
                   rs, rt,
                   (size_t)GPR_OFFSET(rs),
                   (size_t)GPR_OFFSET(rt),
                   (size_t)HI_OFFSET,
                   (size_t)LO_OFFSET);
            break;
        case 0x1a:
            append(buf, size,
                   "    ;; div r%u, r%u\n"
                   "    local.get $base i64.load offset=%zu\n"
                   "    local.get $base i64.load offset=%zu\n"
                   "    i64.div_s\n"
                   "    local.set $t\n"
                   "    local.get $base\n"
                   "    local.get $t\n"
                   "    i64.store offset=%zu\n"
                   "    local.get $base i64.load offset=%zu\n"
                   "    local.get $base i64.load offset=%zu\n"
                   "    i64.rem_s\n"
                   "    local.set $t\n"
                   "    local.get $base\n"
                   "    local.get $t\n"
                   "    i64.store offset=%zu\n",
                   rs, rt,
                   (size_t)GPR_OFFSET(rs),
                   (size_t)GPR_OFFSET(rt),
                   (size_t)LO_OFFSET,
                   (size_t)GPR_OFFSET(rs),
                   (size_t)GPR_OFFSET(rt),
                   (size_t)HI_OFFSET);
            break;
        case 0x1b:
            append(buf, size,
                   "    ;; divu r%u, r%u\n"
                   "    local.get $base i64.load offset=%zu\n"
                   "    local.get $base i64.load offset=%zu\n"
                   "    i64.div_u\n"
                   "    local.set $t\n"
                   "    local.get $base\n"
                   "    local.get $t\n"
                   "    i64.store offset=%zu\n"
                   "    local.get $base i64.load offset=%zu\n"
                   "    local.get $base i64.load offset=%zu\n"
                   "    i64.rem_u\n"
                   "    local.set $t\n"
                   "    local.get $base\n"
                   "    local.get $t\n"
                   "    i64.store offset=%zu\n",
                   rs, rt,
                   (size_t)GPR_OFFSET(rs),
                   (size_t)GPR_OFFSET(rt),
                   (size_t)LO_OFFSET,
                   (size_t)GPR_OFFSET(rs),
                   (size_t)GPR_OFFSET(rt),
                   (size_t)HI_OFFSET);
            break;
        case 0x1c:
            append(buf, size,
                   "    ;; dmult r%u, r%u (approx)\n"
                   "    local.get $base i64.load offset=%zu\n"
                   "    local.get $base i64.load offset=%zu\n"
                   "    i64.mul\n"
                   "    local.set $t\n"
                   "    local.get $base\n"
                   "    i64.const 0\n"
                   "    i64.store offset=%zu\n"
                   "    local.get $base\n"
                   "    local.get $t\n"
                   "    i64.store offset=%zu\n",
                   rs, rt,
                   (size_t)GPR_OFFSET(rs),
                   (size_t)GPR_OFFSET(rt),
                   (size_t)HI_OFFSET,
                   (size_t)LO_OFFSET);
            break;
        case 0x1d:
            append(buf, size,
                   "    ;; dmultu r%u, r%u (approx)\n"
                   "    local.get $base i64.load offset=%zu\n"
                   "    local.get $base i64.load offset=%zu\n"
                   "    i64.mul\n"
                   "    local.set $t\n"
                   "    local.get $base\n"
                   "    i64.const 0\n"
                   "    i64.store offset=%zu\n"
                   "    local.get $base\n"
                   "    local.get $t\n"
                   "    i64.store offset=%zu\n",
                   rs, rt,
                   (size_t)GPR_OFFSET(rs),
                   (size_t)GPR_OFFSET(rt),
                   (size_t)HI_OFFSET,
                   (size_t)LO_OFFSET);
            break;
        case 0x1e:
            append(buf, size,
                   "    ;; ddiv r%u, r%u\n"
                   "    local.get $base i64.load offset=%zu\n"
                   "    local.get $base i64.load offset=%zu\n"
                   "    i64.div_s\n"
                   "    local.set $t\n"
                   "    local.get $base\n"
                   "    local.get $t\n"
                   "    i64.store offset=%zu\n"
                   "    local.get $base i64.load offset=%zu\n"
                   "    local.get $base i64.load offset=%zu\n"
                   "    i64.rem_s\n"
                   "    local.set $t\n"
                   "    local.get $base\n"
                   "    local.get $t\n"
                   "    i64.store offset=%zu\n",
                   rs, rt,
                   (size_t)GPR_OFFSET(rs),
                   (size_t)GPR_OFFSET(rt),
                   (size_t)LO_OFFSET,
                   (size_t)GPR_OFFSET(rs),
                   (size_t)GPR_OFFSET(rt),
                   (size_t)HI_OFFSET);
            break;
        case 0x1f:
            append(buf, size,
                   "    ;; ddivu r%u, r%u\n"
                   "    local.get $base i64.load offset=%zu\n"
                   "    local.get $base i64.load offset=%zu\n"
                   "    i64.div_u\n"
                   "    local.set $t\n"
                   "    local.get $base\n"
                   "    local.get $t\n"
                   "    i64.store offset=%zu\n"
                   "    local.get $base i64.load offset=%zu\n"
                   "    local.get $base i64.load offset=%zu\n"
                   "    i64.rem_u\n"
                   "    local.set $t\n"
                   "    local.get $base\n"
                   "    local.get $t\n"
                   "    i64.store offset=%zu\n",
                   rs, rt,
                   (size_t)GPR_OFFSET(rs),
                   (size_t)GPR_OFFSET(rt),
                   (size_t)LO_OFFSET,
                   (size_t)GPR_OFFSET(rs),
                   (size_t)GPR_OFFSET(rt),
                   (size_t)HI_OFFSET);
            break;
        case 0x10:
            append(buf, size,
                   "    ;; mfhi r%u\n"
                   "    local.get $base\n"
                   "    local.get $base i64.load offset=%zu\n"
                   "    i64.store offset=%zu\n",
                   rd,
                   (size_t)HI_OFFSET,
                   (size_t)GPR_OFFSET(rd));
            break;
        case 0x11:
            append(buf, size,
                   "    ;; mthi r%u\n"
                   "    local.get $base\n"
                   "    local.get $base i64.load offset=%zu\n"
                   "    i64.store offset=%zu\n",
                   rs,
                   (size_t)GPR_OFFSET(rs),
                   (size_t)HI_OFFSET);
            break;
        case 0x12:
            append(buf, size,
                   "    ;; mflo r%u\n"
                   "    local.get $base\n"
                   "    local.get $base i64.load offset=%zu\n"
                   "    i64.store offset=%zu\n",
                   rd,
                   (size_t)LO_OFFSET,
                   (size_t)GPR_OFFSET(rd));
            break;
        case 0x13:
            append(buf, size,
                   "    ;; mtlo r%u\n"
                   "    local.get $base\n"
                   "    local.get $base i64.load offset=%zu\n"
                   "    i64.store offset=%zu\n",
                   rs,
                   (size_t)GPR_OFFSET(rs),
                   (size_t)LO_OFFSET);
            break;
        case 0x0c:
            append(buf, size,
                   "    ;; syscall %u (ignored)\n",
                   (inst >> 6) & 0xfffff);
            break;
        case 0x0d:
            append(buf, size,
                   "    ;; break %u (ignored)\n",
                   (inst >> 6) & 0xfffff);
            break;
        case 0x0f:
            append(buf, size,
                   "    ;; sync (ignored)\n");
            break;
        default:
            append(buf, size,
                   "    ;; unsupported SPECIAL funct %02x\n", funct);
            break;
        }
        break;

    case 0x08:
        append(buf, size,
               "    ;; addi r%u, r%u, %d\n"
               "    local.get $base i64.load offset=%zu\n"
               "    i64.const %d\n"
               "    i64.add\n"
               "    local.set $t\n"
               "    local.get $base\n"
               "    local.get $t\n"
               "    i64.store offset=%zu\n",
               rt, rs, imm,
               (size_t)GPR_OFFSET(rs), imm,
               (size_t)GPR_OFFSET(rt));
        break;
    case 0x09:
        append(buf, size,
               "    ;; addiu r%u, r%u, %d\n"
               "    local.get $base i64.load offset=%zu\n"
               "    i64.const %d\n"
               "    i64.add\n"
               "    local.set $t\n"
               "    local.get $base\n"
               "    local.get $t\n"
               "    i64.store offset=%zu\n",
               rt, rs, imm,
               (size_t)GPR_OFFSET(rs), imm,
               (size_t)GPR_OFFSET(rt));
        break;
    case 0x18:
        append(buf, size,
               "    ;; daddi r%u, r%u, %d\n"
               "    local.get $base i64.load offset=%zu\n"
               "    i64.const %d\n"
               "    i64.add\n"
               "    local.set $t\n"
               "    local.get $base\n"
               "    local.get $t\n"
               "    i64.store offset=%zu\n",
               rt, rs, imm,
               (size_t)GPR_OFFSET(rs), imm,
               (size_t)GPR_OFFSET(rt));
        break;
    case 0x19:
        append(buf, size,
               "    ;; daddiu r%u, r%u, %d\n"
               "    local.get $base i64.load offset=%zu\n"
               "    i64.const %d\n"
               "    i64.add\n"
               "    local.set $t\n"
               "    local.get $base\n"
               "    local.get $t\n"
               "    i64.store offset=%zu\n",
               rt, rs, imm,
               (size_t)GPR_OFFSET(rs), imm,
               (size_t)GPR_OFFSET(rt));
        break;
    case 0x1a:
        append(buf, size,
               "    ;; ldl r%u, %d(r%u) (approx)\n"
               "    local.get $base i64.load offset=%zu\n"
               "    i64.const %d\n"
               "    i64.add\n"
               "    i32.wrap_i64\n"
               "    i64.load\n"
               "    local.set $t\n"
               "    local.get $base\n"
               "    local.get $t\n"
               "    i64.store offset=%zu\n",
               rt, imm, rs,
               (size_t)GPR_OFFSET(rs), imm,
               (size_t)GPR_OFFSET(rt));
        break;
    case 0x1b:
        append(buf, size,
               "    ;; ldr r%u, %d(r%u) (approx)\n"
               "    local.get $base i64.load offset=%zu\n"
               "    i64.const %d\n"
               "    i64.add\n"
               "    i32.wrap_i64\n"
               "    i64.load\n"
               "    local.set $t\n"
               "    local.get $base\n"
               "    local.get $t\n"
               "    i64.store offset=%zu\n",
               rt, imm, rs,
               (size_t)GPR_OFFSET(rs), imm,
               (size_t)GPR_OFFSET(rt));
        break;
    case 0x0c:
        append(buf, size,
               "    ;; andi r%u, r%u, %u\n"
               "    local.get $base i64.load offset=%zu\n"
               "    i64.const %u\n"
               "    i64.and\n"
               "    local.set $t\n"
               "    local.get $base\n"
               "    local.get $t\n"
               "    i64.store offset=%zu\n",
               rt, rs, (uint16_t)imm,
               (size_t)GPR_OFFSET(rs), (uint16_t)imm,
               (size_t)GPR_OFFSET(rt));
        break;
    case 0x0d:
        append(buf, size,
               "    ;; ori r%u, r%u, %u\n"
               "    local.get $base i64.load offset=%zu\n"
               "    i64.const %u\n"
               "    i64.or\n"
               "    local.set $t\n"
               "    local.get $base\n"
               "    local.get $t\n"
               "    i64.store offset=%zu\n",
               rt, rs, (uint16_t)imm,
               (size_t)GPR_OFFSET(rs), (uint16_t)imm,
               (size_t)GPR_OFFSET(rt));
        break;
    case 0x0e:
        append(buf, size,
               "    ;; xori r%u, r%u, %u\n"
               "    local.get $base i64.load offset=%zu\n"
               "    i64.const %u\n"
               "    i64.xor\n"
               "    local.set $t\n"
               "    local.get $base\n"
               "    local.get $t\n"
               "    i64.store offset=%zu\n",
               rt, rs, (uint16_t)imm,
               (size_t)GPR_OFFSET(rs), (uint16_t)imm,
               (size_t)GPR_OFFSET(rt));
        break;
    case 0x0f:
        append(buf, size,
               "    ;; lui r%u, %u\n"
               "    local.get $base\n"
               "    i64.const %u\n"
               "    i64.store offset=%zu\n",
               rt, (uint32_t)(uint16_t)imm,
               (uint32_t)(uint16_t)imm << 16,
               (size_t)GPR_OFFSET(rt));
        break;
    case 0x0a:
        append(buf, size,
               "    ;; slti r%u, r%u, %d\n"
               "    local.get $base i64.load offset=%zu\n"
               "    i64.const %d\n"
               "    i64.lt_s\n"
               "    local.set $tmp\n"
               "    local.get $base\n"
               "    local.get $tmp\n"
               "    i64.extend_i32_u\n"
               "    i64.store offset=%zu\n",
               rt, rs, imm,
               (size_t)GPR_OFFSET(rs), imm,
               (size_t)GPR_OFFSET(rt));
        break;
    case 0x0b:
        append(buf, size,
               "    ;; sltiu r%u, r%u, %d\n"
               "    local.get $base i64.load offset=%zu\n"
               "    i64.const %d\n"
               "    i64.lt_u\n"
               "    local.set $tmp\n"
               "    local.get $base\n"
               "    local.get $tmp\n"
               "    i64.extend_i32_u\n"
               "    i64.store offset=%zu\n",
               rt, rs, imm,
               (size_t)GPR_OFFSET(rs), imm,
               (size_t)GPR_OFFSET(rt));
        break;
    case 0x10: {
        uint32_t sub = rs;
        switch (sub) {
        case 0x00:
            append(buf, size,
                   "    ;; mfc0 r%u, c%u\n"
                   "    local.get $base i32.load offset=%zu\n"
                   "    i64.extend_i32_s\n"
                   "    local.set $t\n"
                   "    local.get $base\n"
                   "    local.get $t\n"
                   "    i64.store offset=%zu\n",
                   rt, rd,
                   (size_t)CP0_OFFSET(rd),
                   (size_t)GPR_OFFSET(rt));
            break;
        case 0x01:
            append(buf, size,
                   "    ;; dmfc0 r%u, c%u\n"
                   "    local.get $base i32.load offset=%zu\n"
                   "    i64.extend_i32_s\n"
                   "    local.set $t\n"
                   "    local.get $base\n"
                   "    local.get $t\n"
                   "    i64.store offset=%zu\n",
                   rt, rd,
                   (size_t)CP0_OFFSET(rd),
                   (size_t)GPR_OFFSET(rt));
            break;
        case 0x04:
        case 0x05:
            append(buf, size,
                   "    ;; mtc0/dmtc0 r%u, c%u\n"
                   "    local.get $base\n"
                   "    local.get $base i64.load offset=%zu\n"
                   "    i32.wrap_i64\n"
                   "    i32.store offset=%zu\n",
                   rt, rd,
                   (size_t)GPR_OFFSET(rt),
                   (size_t)CP0_OFFSET(rd));
            break;
        default:
            append(buf, size,
                   "    ;; unsupported COP0 subop %u\n", sub);
            break;
        }
        break; }
    case 0x11: {
        uint32_t sub = rs;
        switch (sub) {
        case 0x00:
            append(buf, size,
                   "    ;; mfc1 r%u, f%u\n"
                   "    local.get $base i64.load offset=%zu\n"
                   "    i32.wrap_i64\n"
                   "    i32.load\n"
                   "    i64.extend_i32_s\n"
                   "    local.set $t\n"
                   "    local.get $base\n"
                   "    local.get $t\n"
                   "    i64.store offset=%zu\n",
                   rt, rd,
                   (size_t)CP1_SIMPLE_OFFSET(rd),
                   (size_t)GPR_OFFSET(rt));
            break;
        case 0x01:
            append(buf, size,
                   "    ;; dmfc1 r%u, f%u\n"
                   "    local.get $base i64.load offset=%zu\n"
                   "    i32.wrap_i64\n"
                   "    i64.load\n"
                   "    local.set $t\n"
                   "    local.get $base\n"
                   "    local.get $t\n"
                   "    i64.store offset=%zu\n",
                   rt, rd,
                   (size_t)CP1_DOUBLE_OFFSET(rd),
                   (size_t)GPR_OFFSET(rt));
            break;
        case 0x04:
            append(buf, size,
                   "    ;; mtc1 r%u, f%u\n"
                   "    local.get $base i64.load offset=%zu\n"
                   "    i32.wrap_i64\n"
                   "    local.get $base i64.load offset=%zu\n"
                   "    i32.wrap_i64\n"
                   "    i32.store\n",
                   rt, rd,
                   (size_t)CP1_SIMPLE_OFFSET(rd),
                   (size_t)GPR_OFFSET(rt));
            break;
        case 0x05:
            append(buf, size,
                   "    ;; dmtc1 r%u, f%u\n"
                   "    local.get $base i64.load offset=%zu\n"
                   "    i32.wrap_i64\n"
                   "    local.get $base i64.load offset=%zu\n"
                   "    i64.store\n",
                   rt, rd,
                   (size_t)CP1_DOUBLE_OFFSET(rd),
                   (size_t)GPR_OFFSET(rt));
            break;
        case 0x10: {
            uint32_t fs = (inst >> 11) & 0x1f;
            uint32_t ft = (inst >> 16) & 0x1f;
            switch (funct) {
            case 0x00:
                append(buf, size,
                       "    ;; add.s f%u, f%u, f%u\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    i32.load\n"
                       "    f32.reinterpret_i32\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    i32.load\n"
                       "    f32.reinterpret_i32\n"
                       "    f32.add\n"
                       "    i32.reinterpret_f32\n"
                       "    i32.store\n",
                       fd, fs, ft,
                       (size_t)CP1_SIMPLE_OFFSET(fd),
                       (size_t)CP1_SIMPLE_OFFSET(fs),
                       (size_t)CP1_SIMPLE_OFFSET(ft));
                break;
            case 0x01:
                append(buf, size,
                       "    ;; sub.s f%u, f%u, f%u\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    i32.load\n"
                       "    f32.reinterpret_i32\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    i32.load\n"
                       "    f32.reinterpret_i32\n"
                       "    f32.sub\n"
                       "    i32.reinterpret_f32\n"
                       "    i32.store\n",
                       fd, fs, ft,
                       (size_t)CP1_SIMPLE_OFFSET(fd),
                       (size_t)CP1_SIMPLE_OFFSET(fs),
                       (size_t)CP1_SIMPLE_OFFSET(ft));
                break;
            case 0x02:
                append(buf, size,
                       "    ;; mul.s f%u, f%u, f%u\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    i32.load\n"
                       "    f32.reinterpret_i32\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    i32.load\n"
                       "    f32.reinterpret_i32\n"
                       "    f32.mul\n"
                       "    i32.reinterpret_f32\n"
                       "    i32.store\n",
                       fd, fs, ft,
                       (size_t)CP1_SIMPLE_OFFSET(fd),
                       (size_t)CP1_SIMPLE_OFFSET(fs),
                       (size_t)CP1_SIMPLE_OFFSET(ft));
                break;
            case 0x03:
                append(buf, size,
                       "    ;; div.s f%u, f%u, f%u\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    i32.load\n"
                       "    f32.reinterpret_i32\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    i32.load\n"
                       "    f32.reinterpret_i32\n"
                       "    f32.div\n"
                       "    i32.reinterpret_f32\n"
                       "    i32.store\n",
                       fd, fs, ft,
                       (size_t)CP1_SIMPLE_OFFSET(fd),
                       (size_t)CP1_SIMPLE_OFFSET(fs),
                       (size_t)CP1_SIMPLE_OFFSET(ft));
                break;
            case 0x05:
                append(buf, size,
                       "    ;; abs.s f%u, f%u\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    i32.load\n"
                       "    f32.reinterpret_i32\n"
                       "    f32.abs\n"
                       "    i32.reinterpret_f32\n"
                       "    i32.store\n",
                       fd, fs,
                       (size_t)CP1_SIMPLE_OFFSET(fd),
                       (size_t)CP1_SIMPLE_OFFSET(fs));
                break;
            case 0x06:
                append(buf, size,
                       "    ;; mov.s f%u, f%u\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    i32.load\n"
                       "    i32.store\n",
                       fd, fs,
                       (size_t)CP1_SIMPLE_OFFSET(fd),
                       (size_t)CP1_SIMPLE_OFFSET(fs));
                break;
            case 0x07:
                append(buf, size,
                       "    ;; neg.s f%u, f%u\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    i32.load\n"
                       "    f32.reinterpret_i32\n"
                       "    f32.neg\n"
                       "    i32.reinterpret_f32\n"
                       "    i32.store\n",
                       fd, fs,
                       (size_t)CP1_SIMPLE_OFFSET(fd),
                       (size_t)CP1_SIMPLE_OFFSET(fs));
                break;
            case 0x0a:
                append(buf, size,
                       "    ;; ceil.l.s f%u, f%u\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    i32.load\n"
                       "    f32.reinterpret_i32\n"
                       "    f32.ceil\n"
                       "    i64.trunc_f32_s\n"
                       "    i64.store\n",
                       fd, fs,
                       (size_t)CP1_DOUBLE_OFFSET(fd),
                       (size_t)CP1_SIMPLE_OFFSET(fs));
                break;
            case 0x0b:
                append(buf, size,
                       "    ;; floor.l.s f%u, f%u\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    i32.load\n"
                       "    f32.reinterpret_i32\n"
                       "    f32.floor\n"
                       "    i64.trunc_f32_s\n"
                       "    i64.store\n",
                       fd, fs,
                       (size_t)CP1_DOUBLE_OFFSET(fd),
                       (size_t)CP1_SIMPLE_OFFSET(fs));
                break;
            case 0x0e:
                append(buf, size,
                       "    ;; ceil.w.s f%u, f%u\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    i32.load\n"
                       "    f32.reinterpret_i32\n"
                       "    f32.ceil\n"
                       "    i32.trunc_f32_s\n"
                       "    i32.store\n",
                       fd, fs,
                       (size_t)CP1_SIMPLE_OFFSET(fd),
                       (size_t)CP1_SIMPLE_OFFSET(fs));
                break;
            case 0x0f:
                append(buf, size,
                       "    ;; floor.w.s f%u, f%u\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    i32.load\n"
                       "    f32.reinterpret_i32\n"
                       "    f32.floor\n"
                       "    i32.trunc_f32_s\n"
                       "    i32.store\n",
                       fd, fs,
                       (size_t)CP1_SIMPLE_OFFSET(fd),
                       (size_t)CP1_SIMPLE_OFFSET(fs));
                break;
            case 0x21:
                append(buf, size,
                       "    ;; cvt.d.s f%u, f%u\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    i32.load\n"
                       "    f32.reinterpret_i32\n"
                       "    f64.promote_f32\n"
                       "    i64.reinterpret_f64\n"
                       "    i64.store\n",
                       fd, fs,
                       (size_t)CP1_DOUBLE_OFFSET(fd),
                       (size_t)CP1_SIMPLE_OFFSET(fs));
                break;
            case 0x24:
                append(buf, size,
                       "    ;; cvt.w.s f%u, f%u\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    i32.load\n"
                       "    f32.reinterpret_i32\n"
                       "    i32.trunc_f32_s\n"
                       "    i32.store\n",
                       fd, fs,
                       (size_t)CP1_SIMPLE_OFFSET(fd),
                       (size_t)CP1_SIMPLE_OFFSET(fs));
                break;
            case 0x25:
                append(buf, size,
                       "    ;; cvt.l.s f%u, f%u\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    i32.load\n"
                       "    f32.reinterpret_i32\n"
                       "    i64.trunc_f32_s\n"
                       "    i64.store\n",
                       fd, fs,
                       (size_t)CP1_DOUBLE_OFFSET(fd),
                       (size_t)CP1_SIMPLE_OFFSET(fs));
                break;
            default:
                append(buf, size,
                       "    ;; unsupported COP1 fmt %u funct %u\n", sub, funct);
                break;
            }
            break; }
        case 0x11: {
            uint32_t fs = (inst >> 11) & 0x1f;
            uint32_t ft = (inst >> 16) & 0x1f;
            switch (funct) {
            case 0x00:
                append(buf, size,
                       "    ;; add.d f%u, f%u, f%u\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    i64.load\n"
                       "    f64.reinterpret_i64\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    i64.load\n"
                       "    f64.reinterpret_i64\n"
                       "    f64.add\n"
                       "    i64.reinterpret_f64\n"
                       "    i64.store\n",
                       fd, fs, ft,
                       (size_t)CP1_DOUBLE_OFFSET(fd),
                       (size_t)CP1_DOUBLE_OFFSET(fs),
                       (size_t)CP1_DOUBLE_OFFSET(ft));
                break;
            case 0x01:
                append(buf, size,
                       "    ;; sub.d f%u, f%u, f%u\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    i64.load\n"
                       "    f64.reinterpret_i64\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    i64.load\n"
                       "    f64.reinterpret_i64\n"
                       "    f64.sub\n"
                       "    i64.reinterpret_f64\n"
                       "    i64.store\n",
                       fd, fs, ft,
                       (size_t)CP1_DOUBLE_OFFSET(fd),
                       (size_t)CP1_DOUBLE_OFFSET(fs),
                       (size_t)CP1_DOUBLE_OFFSET(ft));
                break;
            case 0x02:
                append(buf, size,
                       "    ;; mul.d f%u, f%u, f%u\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    i64.load\n"
                       "    f64.reinterpret_i64\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    i64.load\n"
                       "    f64.reinterpret_i64\n"
                       "    f64.mul\n"
                       "    i64.reinterpret_f64\n"
                       "    i64.store\n",
                       fd, fs, ft,
                       (size_t)CP1_DOUBLE_OFFSET(fd),
                       (size_t)CP1_DOUBLE_OFFSET(fs),
                       (size_t)CP1_DOUBLE_OFFSET(ft));
                break;
            case 0x03:
                append(buf, size,
                       "    ;; div.d f%u, f%u, f%u\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    i64.load\n"
                       "    f64.reinterpret_i64\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    i64.load\n"
                       "    f64.reinterpret_i64\n"
                       "    f64.div\n"
                       "    i64.reinterpret_f64\n"
                       "    i64.store\n",
                       fd, fs, ft,
                       (size_t)CP1_DOUBLE_OFFSET(fd),
                       (size_t)CP1_DOUBLE_OFFSET(fs),
                       (size_t)CP1_DOUBLE_OFFSET(ft));
                break;
            case 0x05:
                append(buf, size,
                       "    ;; abs.d f%u, f%u\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    i64.load\n"
                       "    f64.reinterpret_i64\n"
                       "    f64.abs\n"
                       "    i64.reinterpret_f64\n"
                       "    i64.store\n",
                       fd, fs,
                       (size_t)CP1_DOUBLE_OFFSET(fd),
                       (size_t)CP1_DOUBLE_OFFSET(fs));
                break;
            case 0x06:
                append(buf, size,
                       "    ;; mov.d f%u, f%u\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    i64.load\n"
                       "    i64.store\n",
                       fd, fs,
                       (size_t)CP1_DOUBLE_OFFSET(fd),
                       (size_t)CP1_DOUBLE_OFFSET(fs));
                break;
            case 0x07:
                append(buf, size,
                       "    ;; neg.d f%u, f%u\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    i64.load\n"
                       "    f64.reinterpret_i64\n"
                       "    f64.neg\n"
                       "    i64.reinterpret_f64\n"
                       "    i64.store\n",
                       fd, fs,
                       (size_t)CP1_DOUBLE_OFFSET(fd),
                       (size_t)CP1_DOUBLE_OFFSET(fs));
                break;
            case 0x0a:
                append(buf, size,
                       "    ;; ceil.l.d f%u, f%u\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    i64.load\n"
                       "    f64.reinterpret_i64\n"
                       "    f64.ceil\n"
                       "    i64.trunc_f64_s\n"
                       "    i64.store\n",
                       fd, fs,
                       (size_t)CP1_DOUBLE_OFFSET(fd),
                       (size_t)CP1_DOUBLE_OFFSET(fs));
                break;
            case 0x0b:
                append(buf, size,
                       "    ;; floor.l.d f%u, f%u\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    i64.load\n"
                       "    f64.reinterpret_i64\n"
                       "    f64.floor\n"
                       "    i64.trunc_f64_s\n"
                       "    i64.store\n",
                       fd, fs,
                       (size_t)CP1_DOUBLE_OFFSET(fd),
                       (size_t)CP1_DOUBLE_OFFSET(fs));
                break;
            case 0x0e:
                append(buf, size,
                       "    ;; ceil.w.d f%u, f%u\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    i64.load\n"
                       "    f64.reinterpret_i64\n"
                       "    f64.ceil\n"
                       "    i32.trunc_f64_s\n"
                       "    i32.store\n",
                       fd, fs,
                       (size_t)CP1_SIMPLE_OFFSET(fd),
                       (size_t)CP1_DOUBLE_OFFSET(fs));
                break;
            case 0x0f:
                append(buf, size,
                       "    ;; floor.w.d f%u, f%u\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    i64.load\n"
                       "    f64.reinterpret_i64\n"
                       "    f64.floor\n"
                       "    i32.trunc_f64_s\n"
                       "    i32.store\n",
                       fd, fs,
                       (size_t)CP1_SIMPLE_OFFSET(fd),
                       (size_t)CP1_DOUBLE_OFFSET(fs));
                break;
            case 0x20:
                append(buf, size,
                       "    ;; cvt.s.d f%u, f%u\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    i64.load\n"
                       "    f64.reinterpret_i64\n"
                       "    f32.demote_f64\n"
                       "    i32.reinterpret_f32\n"
                       "    i32.store\n",
                       fd, fs,
                       (size_t)CP1_SIMPLE_OFFSET(fd),
                       (size_t)CP1_DOUBLE_OFFSET(fs));
                break;
            case 0x24:
                append(buf, size,
                       "    ;; cvt.w.d f%u, f%u\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    i64.load\n"
                       "    f64.reinterpret_i64\n"
                       "    i32.trunc_f64_s\n"
                       "    i32.store\n",
                       fd, fs,
                       (size_t)CP1_SIMPLE_OFFSET(fd),
                       (size_t)CP1_DOUBLE_OFFSET(fs));
                break;
            case 0x25:
                append(buf, size,
                       "    ;; cvt.l.d f%u, f%u\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    i64.load\n"
                       "    f64.reinterpret_i64\n"
                       "    i64.trunc_f64_s\n"
                       "    i64.store\n",
                       fd, fs,
                       (size_t)CP1_DOUBLE_OFFSET(fd),
                       (size_t)CP1_DOUBLE_OFFSET(fs));
                break;
            default:
                append(buf, size,
                       "    ;; unsupported COP1 fmt %u funct %u\n", sub, funct);
                break;
            }
            break; }
        case 0x14: {
            uint32_t fs = (inst >> 11) & 0x1f;
            switch (funct) {
            case 0x20:
                append(buf, size,
                       "    ;; cvt.s.w f%u, f%u\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    i32.load\n"
                       "    f32.convert_i32_s\n"
                       "    i32.reinterpret_f32\n"
                       "    i32.store\n",
                       fd, fs,
                       (size_t)CP1_SIMPLE_OFFSET(fd),
                       (size_t)CP1_SIMPLE_OFFSET(fs));
                break;
            case 0x21:
                append(buf, size,
                       "    ;; cvt.d.w f%u, f%u\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    i32.load\n"
                       "    f64.convert_i32_s\n"
                       "    i64.reinterpret_f64\n"
                       "    i64.store\n",
                       fd, fs,
                       (size_t)CP1_DOUBLE_OFFSET(fd),
                       (size_t)CP1_SIMPLE_OFFSET(fs));
                break;
            default:
                append(buf, size,
                       "    ;; unsupported COP1 fmt %u funct %u\n", sub, funct);
                break;
            }
            break; }
        case 0x15: {
            uint32_t fs = (inst >> 11) & 0x1f;
            switch (funct) {
            case 0x20:
                append(buf, size,
                       "    ;; cvt.s.l f%u, f%u\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    i64.load\n"
                       "    f32.convert_i64_s\n"
                       "    i32.reinterpret_f32\n"
                       "    i32.store\n",
                       fd, fs,
                       (size_t)CP1_SIMPLE_OFFSET(fd),
                       (size_t)CP1_DOUBLE_OFFSET(fs));
                break;
            case 0x21:
                append(buf, size,
                       "    ;; cvt.d.l f%u, f%u\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    i64.load\n"
                       "    f64.convert_i64_s\n"
                       "    i64.reinterpret_f64\n"
                       "    i64.store\n",
                       fd, fs,
                       (size_t)CP1_DOUBLE_OFFSET(fd),
                       (size_t)CP1_DOUBLE_OFFSET(fs));
                break;
            default:
                append(buf, size,
                       "    ;; unsupported COP1 fmt %u funct %u\n", sub, funct);
                break;
            }
            break; }
        default:
            append(buf, size,
                   "    ;; unsupported COP1 subop %u\n", sub);
            break;
        }
        break; }
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
        break;
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
        break;
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
        break;
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
        break;
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
        break;
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
        break;
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
        break;
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
        break;
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
        break;
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
        break;
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
        break;
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
        break;
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
        break;
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
        break;
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
        break;
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
        break;
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
        break;
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
        break;
    case 0x2f:
        append(buf, size,
               "    ;; cache (ignored)\n");
        break;
    case 0x33:
        append(buf, size,
               "    ;; pref (ignored)\n");
        break;
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
        break;
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
        break;
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
        break;
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
        break;
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
        break;
    default:
        append(buf, size,
               "    ;; unsupported opcode %02x\n", op);
        break;
    }
}

void wasm_dynarec_init(struct r4300_core *r4300)
{
    (void)r4300;

    g_blocks = NULL;
    g_blocks_count = 0;

    DebugMessage(M64MSG_INFO, "Initializing experimental WebAssembly dynarec");
}

void wasm_dynarec_cleanup(void)
{
    for (size_t i = 0; i < g_blocks_count; ++i)
        free(g_blocks[i].wat);
    free(g_blocks);
    g_blocks = NULL;
    g_blocks_count = 0;

    DebugMessage(M64MSG_INFO, "Cleaning up WebAssembly dynarec");
}

void wasm_dynarec_recompile_block(struct r4300_core *r4300, const uint32_t *iw, size_t count, uint32_t address)
{
    (void)r4300;

    uint32_t targets[32];
    size_t target_count = 0;

    struct wasm_dynarec_block *block = get_block(address);
    if (block) {
        free(block->wat);
        block->wat = NULL;
        block->wat_size = 0;
    } else {
        g_blocks = realloc(g_blocks, sizeof(*g_blocks) * (g_blocks_count + 1));
        block = &g_blocks[g_blocks_count++];
        block->address = address;
        block->wat = NULL;
        block->wat_size = 0;
        block->mem_pages = 0;
    }

    size_t mem_bytes = sizeof(struct new_dynarec_hot_state) + 0x8000 + 0x1000;
    block->mem_pages = (mem_bytes + 65535) / 65536;

    append(&block->wat, &block->wat_size,
           "(module\n"
           "  (import \"env\" \"wasm_dynarec_dispatch\" (func $dispatch (param i32 i32)))\n"
           "  (import \"env\" \"mem_read32\" (func $mem_read32 (param i32 i32) (result i32)))\n"
           "  (import \"env\" \"mem_read64\" (func $mem_read64 (param i32 i32) (result i64)))\n"
           "  (import \"env\" \"mem_write32\" (func $mem_write32 (param i32 i32 i32 i32)))\n"
           "  (import \"env\" \"mem_write64\" (func $mem_write64 (param i32 i32 i64 i64)))\n"
           "  (memory %u)\n"
           "  (func $block_%x (param $base i32) (local $t i64) (local $tmp i32)\n",
           block->mem_pages, address);

    for (size_t i = 0; i < count; ++i) {
        uint32_t inst = iw[i];
        uint32_t op = inst >> 26;
        uint32_t rs = (inst >> 21) & 0x1f;
        uint32_t rt = (inst >> 16) & 0x1f;
        uint32_t rd = (inst >> 11) & 0x1f;
        uint32_t funct = inst & 0x3f;
        int16_t imm = inst & 0xffff;

        switch (op) {
        case 0x00: /* SPECIAL */
            if (funct == 0x08 /* JR */ || funct == 0x09 /* JALR */) {
                uint32_t delay = (i + 1 < count) ? iw[i + 1] : 0;
                uint32_t linkreg = rd ? rd : 31;
                uint32_t linkpc = address + (i + 1) * 4 + 4;

                if (funct == 0x08)
                    append(&block->wat, &block->wat_size,
                           "    ;; jr r%u\n", rs);
                else
                    append(&block->wat, &block->wat_size,
                           "    ;; jalr r%u, r%u\n", linkreg, rs);

                emit_simple_instr(&block->wat, &block->wat_size, delay);

                if (funct == 0x09) {
                    append(&block->wat, &block->wat_size,
                           "    local.get $base\n"
                           "    i32.const %u\n"
                           "    i64.extend_i32_s\n"
                           "    i64.store offset=%zu\n",
                           linkpc,
                           (size_t)GPR_OFFSET(linkreg));
                }

                append(&block->wat, &block->wat_size,
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    local.set $tmp\n"
                       "    local.get $base\n"
                       "    local.get $tmp\n"
                       "    i32.store offset=%zu\n"
                       "    local.get $base\n"
                       "    local.get $tmp\n"
                       "    call $dispatch\n",
                       (size_t)GPR_OFFSET(rs),
                       (size_t)PC_OFFSET);

                i++; /* skip delay slot */
                break;
            }
            emit_simple_instr(&block->wat, &block->wat_size, inst);
            break;

        case 0x02: /* J */
        case 0x03: /* JAL */
        {
            uint32_t pc = address + i * 4;
            uint32_t target = ((pc + 4) & 0xf0000000) | ((inst & 0x03ffffff) << 2);
            uint32_t delay = (i + 1 < count) ? iw[i + 1] : 0;
            append(&block->wat, &block->wat_size,
                   op == 0x02 ? "    ;; j %08x\n" : "    ;; jal %08x\n",
                   target);
            emit_simple_instr(&block->wat, &block->wat_size, delay);

            if (op == 0x03) {
                uint32_t linkpc = address + (i + 1) * 4 + 4;
                append(&block->wat, &block->wat_size,
                       "    local.get $base\n"
                       "    i32.const %u\n"
                       "    i64.extend_i32_s\n"
                       "    i64.store offset=%zu\n",
                       linkpc,
                       (size_t)GPR_OFFSET(31));
            }

            if (target < address || target >= address + count * 4) {
                append(&block->wat, &block->wat_size,
                       "    i32.const %u\n"
                       "    local.set $tmp\n"
                       "    local.get $base\n"
                       "    local.get $tmp\n"
                       "    i32.store offset=%zu\n"
                       "    local.get $base\n"
                       "    local.get $tmp\n"
                       "    call $dispatch\n",
                       target,
                       (size_t)PC_OFFSET);
            } else {
                append(&block->wat, &block->wat_size,
                       "    local.get $base\n"
                       "    call $block_%08x\n",
                       target);
                if (target_count < 32) {
                    int known = 0;
                    for (size_t ti = 0; ti < target_count; ++ti)
                        if (targets[ti] == target) { known = 1; break; }
                    if (!known) targets[target_count++] = target;
                }
            }
            i++;
        }
            break;

        case 0x04: /* BEQ */
        case 0x05: /* BNE */
        case 0x06: /* BLEZ */
        case 0x07: /* BGTZ */
        case 0x14: /* BEQL */
        case 0x15: /* BNEL */
        case 0x16: /* BLEZL */
        case 0x17: /* BGTZL */
        {
            uint32_t target = (address + (i + 1) * 4) + ((int16_t)imm << 2);
            uint32_t fallthrough = address + (i + 2) * 4;
            uint32_t delay = (i + 1 < count) ? iw[i + 1] : 0;

            int likely = (op >= 0x14);
            const char *condop = NULL;
            switch (op & 0x07) {
            case 0x04: condop = "eq"; break;
            case 0x05: condop = "ne"; break;
            case 0x06: condop = "le_s"; break;
            case 0x07: condop = "gt_s"; break;
            }

            append(&block->wat, &block->wat_size,
                   "    ;; branch op %02x\n", op);

            if (!likely)
                emit_simple_instr(&block->wat, &block->wat_size, delay);

            /* branch condition */
            append(&block->wat, &block->wat_size,
                   "    local.get $base i64.load offset=%zu\n",
                   (size_t)GPR_OFFSET(rs));
            if (op == 0x04 || op == 0x05 || op == 0x14 || op == 0x15) {
                append(&block->wat, &block->wat_size,
                       "    local.get $base i64.load offset=%zu\n"
                       "    i64.%s\n",
                       (size_t)GPR_OFFSET(rt), condop);
            } else {
                append(&block->wat, &block->wat_size,
                       "    i64.const 0\n"
                       "    i64.%s\n",
                       condop);
            }
            append(&block->wat, &block->wat_size, "    if\n");
            if (likely)
                emit_simple_instr(&block->wat, &block->wat_size, delay);
            append(&block->wat, &block->wat_size,
                   "      local.get $base\n      call $block_%08x\n    else\n      local.get $base\n      call $block_%08x\n    end\n",
                   target, fallthrough);
            if (target_count < 32) {
                int known = 0;
                for (size_t ti = 0; ti < target_count; ++ti)
                    if (targets[ti] == target) { known = 1; break; }
                if (!known) targets[target_count++] = target;
            }
            if (target_count < 32) {
                int known = 0;
                for (size_t ti = 0; ti < target_count; ++ti)
                    if (targets[ti] == fallthrough) { known = 1; break; }
                if (!known) targets[target_count++] = fallthrough;
            }
            i++;
        }
            break;

        case 0x01: /* REGIMM */
        {
            uint32_t rtcode = rt;
            uint32_t target = (address + (i + 1) * 4) + ((int16_t)imm << 2);
            uint32_t fallthrough = address + (i + 2) * 4;
            uint32_t delay = (i + 1 < count) ? iw[i + 1] : 0;
            int likely = (rtcode == 0x02 || rtcode == 0x03 || rtcode == 0x12 || rtcode == 0x13);

            const char *cond = NULL;
            switch (rtcode & ~0x10) {
            case 0x00: /* BLTZ/BLTZAL */
            case 0x02: /* BLTZL/BLTZALL */
                cond = "lt_s";
                break;
            case 0x01: /* BGEZ/BGEZAL */
            case 0x03: /* BGEZL/BGEZALL */
                cond = "ge_s";
                break;
            default:
                append(&block->wat, &block->wat_size,
                       "    ;; unsupported REGIMM %02x\n", rtcode);
                if (!likely)
                    emit_simple_instr(&block->wat, &block->wat_size, delay);
                i++;
                break;
            }
            if (cond) {
                append(&block->wat, &block->wat_size,
                       "    ;; regimm %02x\n", rtcode);

                if (!likely)
                    emit_simple_instr(&block->wat, &block->wat_size, delay);

                /* branch condition */
                append(&block->wat, &block->wat_size,
                       "    local.get $base i64.load offset=%zu\n"
                       "    i64.const 0\n"
                       "    i64.%s\n",
                       (size_t)GPR_OFFSET(rs), cond);
                append(&block->wat, &block->wat_size, "    if\n");
                if (likely)
                    emit_simple_instr(&block->wat, &block->wat_size, delay);
                uint32_t linkpc = address + (i + 1) * 4 + 4;
                append(&block->wat, &block->wat_size,
                       "      local.get $base\n");
                if (rtcode & 0x10) {
                    append(&block->wat, &block->wat_size,
                           "      i32.const %u\n"
                           "      i64.extend_i32_s\n"
                           "      i64.store offset=%zu\n",
                           linkpc,
                           (size_t)GPR_OFFSET(31));
                    append(&block->wat, &block->wat_size,
                           "      local.get $base\n");
                }
                append(&block->wat, &block->wat_size,
                       "      call $block_%08x\n",
                       target);
                append(&block->wat, &block->wat_size,
                       "    else\n      local.get $base\n      call $block_%08x\n    end\n",
                       fallthrough);
                if (target_count < 32) {
                    int known = 0;
                    for (size_t ti = 0; ti < target_count; ++ti)
                        if (targets[ti] == target) { known = 1; break; }
                    if (!known) targets[target_count++] = target;
                }
                if (target_count < 32) {
                    int known = 0;
                    for (size_t ti = 0; ti < target_count; ++ti)
                        if (targets[ti] == fallthrough) { known = 1; break; }
                    if (!known) targets[target_count++] = fallthrough;
                }
                i++;
            }
        }
            break;

        case 0x11: /* COP1 */
        {
            if (rs == 0x08) {
                uint32_t rtcode = rt & 0x3;
                uint32_t target = (address + (i + 1) * 4) + ((int16_t)imm << 2);
                uint32_t fallthrough = address + (i + 2) * 4;
                uint32_t delay = (i + 1 < count) ? iw[i + 1] : 0;

                int likely = (rtcode & 2) != 0;
                int cond = (rtcode & 1) != 0;

                append(&block->wat, &block->wat_size,
                       "    ;; bc1%s%s\n",
                       cond ? "t" : "f",
                       likely ? "l" : "");

                if (!likely)
                    emit_simple_instr(&block->wat, &block->wat_size, delay);

                append(&block->wat, &block->wat_size,
                       "    local.get $base i32.load offset=%zu\n"
                       "    i32.const %u\n"
                       "    i32.and\n"
                       "    i32.const 0\n"
                       "    i32.%s\n",
                       (size_t)CP1_FCR31_OFFSET,
                       FCR31_CMP_BIT,
                       cond ? "ne" : "eq");
                append(&block->wat, &block->wat_size, "    if\n");
                if (likely)
                    emit_simple_instr(&block->wat, &block->wat_size, delay);
                append(&block->wat, &block->wat_size,
                       "      local.get $base\n      call $block_%08x\n    else\n      local.get $base\n      call $block_%08x\n    end\n",
                       target, fallthrough);
                if (target_count < 32) {
                    int known = 0;
                    for (size_t ti = 0; ti < target_count; ++ti)
                        if (targets[ti] == target) { known = 1; break; }
                    if (!known) targets[target_count++] = target;
                }
                if (target_count < 32) {
                    int known = 0;
                    for (size_t ti = 0; ti < target_count; ++ti)
                        if (targets[ti] == fallthrough) { known = 1; break; }
                    if (!known) targets[target_count++] = fallthrough;
                }
                i++;
            } else {
                emit_simple_instr(&block->wat, &block->wat_size, inst);
            }
        }
            break;

        default:
            emit_simple_instr(&block->wat, &block->wat_size, inst);
            break;
        }
    }

    append(&block->wat, &block->wat_size, "  )\n");

    for (size_t ti = 0; ti < target_count; ++ti) {
        append(&block->wat, &block->wat_size,
               "  (func $block_%x (param $base i32))\n",
               targets[ti]);
    }

    append(&block->wat, &block->wat_size, ")\n");

    DebugMessage(M64MSG_INFO, "Recompiled block %08x to WebAssembly", address);
}

void wasm_dynarec_exec(struct r4300_core *r4300, uint32_t address)
{
    struct wasm_dynarec_block *block = get_block(address);
    if (!block || !block->wat) {
        DebugMessage(M64MSG_WARNING, "No WebAssembly block for %08x; using interpreter", address);
        return;
    }

    DebugMessage(M64MSG_INFO, "Executing WebAssembly block %08x", address);
    DebugMessage(M64MSG_VERBOSE, "\n%s", block->wat);

    char wat_path[64];
    char wasm_path[64];
    snprintf(wat_path, sizeof(wat_path), "/tmp/block_%08x.wat", address);
    snprintf(wasm_path, sizeof(wasm_path), "/tmp/block_%08x.wasm", address);

    FILE *f = fopen(wat_path, "w");
    if (!f)
        return;
    size_t len = strlen(block->wat);
    if (len > 2 && block->wat[len-2] == ')' && block->wat[len-1] == '\n')
        len -= 2;
    fwrite(block->wat, 1, len, f);
    fprintf(f, "  (export \"memory\" (memory 0))\n");
    fprintf(f, "  (export \"entry\" (func $block_%08x))\n)", address);
    fclose(f);

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "wat2wasm %s -o %s", wat_path, wasm_path);
    if (system(cmd) != 0)
        return;

    FILE *wf = fopen(wasm_path, "rb");
    if (!wf)
        return;
    fseek(wf, 0, SEEK_END);
    size_t wasm_size = ftell(wf);
    fseek(wf, 0, SEEK_SET);
    uint8_t *wasm = malloc(wasm_size);
    fread(wasm, 1, wasm_size, wf);
    fclose(wf);

    IM3Environment env = m3_NewEnvironment();
    IM3Runtime runtime = m3_NewRuntime(env, 64*1024, NULL);
    IM3Module module = NULL;
    M3Result m3res = m3_ParseModule(env, &module, wasm, wasm_size);
    if (!m3res) m3res = m3_LoadModule(runtime, module);
    if (m3res) { free(wasm); m3_FreeRuntime(runtime); m3_FreeEnvironment(env); return; }

    g_current_cpu = r4300;
    m3_LinkRawFunction(module, "env", "wasm_dynarec_dispatch", "v(ii)", wasm_dynarec_dispatch_import);
    m3_LinkRawFunction(module, "env", "mem_read32", "i(ii)", wasm_dynarec_read_word);
    m3_LinkRawFunction(module, "env", "mem_read64", "I(ii)", wasm_dynarec_read_dword);
    m3_LinkRawFunction(module, "env", "mem_write32", "v(iiii)", wasm_dynarec_write_word);
    m3_LinkRawFunction(module, "env", "mem_write64", "v(iiII)", wasm_dynarec_write_dword);

    IM3Function entry;
    m3_FindFunction(&entry, runtime, "entry");

    uint8_t *mem = m3_GetMemory(runtime, NULL, 0);
    struct new_dynarec_hot_state *state = (struct new_dynarec_hot_state*)mem;

    memcpy(mem, &r4300->new_dynarec_hot_state, sizeof(*state));
    for (int i = 0; i < 32; i++) {
        uint64_t *p = (uint64_t*)(mem + 0x8000 + i*8);
        *p = r4300->cp1.regs[i].dword;
        *(uint64_t*)(mem + CP1_SIMPLE_OFFSET(i)) = 0x8000 + i*8;
        *(uint64_t*)(mem + CP1_DOUBLE_OFFSET(i)) = 0x8000 + i*8;
    }

    m3_CallV(entry, 0);

    memcpy(&r4300->new_dynarec_hot_state, mem, sizeof(*state));
    for (int i = 0; i < 32; i++)
        r4300->cp1.regs[i].dword = *(uint64_t*)(mem + 0x8000 + i*8);

    g_current_cpu = NULL;

    m3_FreeRuntime(runtime);
    m3_FreeEnvironment(env);
    free(wasm);
    unlink(wasm_path);
    unlink(wat_path);
}

void wasm_dynarec_dump(uint32_t address)
{
    struct wasm_dynarec_block *block = get_block(address);
    if (!block || !block->wat) {
        DebugMessage(M64MSG_INFO, "No WebAssembly block for %08x", address);
        return;
    }
    DebugMessage(M64MSG_INFO, "WebAssembly block %08x:\n%s", address, block->wat);
}

const char *wasm_dynarec_get_wat(uint32_t address)
{
    struct wasm_dynarec_block *block = get_block(address);
    return block ? block->wat : NULL;
}

/* Simple dispatcher that recompiles and prints blocks using the new_dynarec_hot_state */
void wasm_dynarec_dispatch(struct r4300_core *r4300, uint32_t address)
{
    const uint32_t *code = fast_mem_access(r4300, address);
    if (!code)
        return;

    if (!get_block(address))
        wasm_dynarec_recompile_block(r4300, code, 4, address);

    wasm_dynarec_exec(r4300, address);
}

void wasm_dynarec_entry(struct r4300_core *r4300)
{
    wasm_dynarec_dispatch(r4300, r4300->new_dynarec_hot_state.pcaddr);
}
