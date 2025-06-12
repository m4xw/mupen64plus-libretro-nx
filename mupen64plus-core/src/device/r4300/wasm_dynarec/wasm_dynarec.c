#include "wasm_dynarec.h"

#include "api/callbacks.h"
#include "device/r4300/r4300_core.h"
#include "device/r4300/new_dynarec/new_dynarec.h"
#include "device/r4300/fpu.h"
#include "device/r4300/cp0.h"
#include "device/r4300/cached_interp.h"
#include "device/r4300/interrupt.h"
#include "device/rcp/mi/mi_controller.h"
#ifdef __EMSCRIPTEN__
# include <emscripten/emscripten.h>
#else
#include "wasm3.h"
#include "m3_env.h"
#endif
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

unsigned int stop_after_jal;

struct wasm_dynarec_block
{
    uint32_t address;
    char *wat;
    size_t wat_size;
    uint32_t mem_pages;
    size_t iw_count;
    int idle;
};

static struct wasm_dynarec_block *g_blocks = NULL;
static size_t g_blocks_count = 0;

static struct r4300_core *g_current_cpu = NULL;
static uint32_t g_dispatch_pc = 0;
static int g_dispatch_pending = 0;
unsigned int using_tlb = 0;

static void wasm_dynarec_update_count(struct r4300_core *r4300,
                                      uint32_t start_pc)
{
    struct cp0 *cp0 = &r4300->cp0;
    uint32_t *cp0_regs = r4300->new_dynarec_hot_state.cp0_regs;
    int *cycle_count = &r4300->new_dynarec_hot_state.cycle_count;

    uint32_t pc = r4300->new_dynarec_hot_state.pcaddr;
    uint32_t diff = (pc >= start_pc) ? (pc - start_pc) : (start_pc - pc);
    uint32_t count = (diff >> 2) * cp0->count_per_op;
    if (cp0->count_per_op_denom_pot) {
        count += (1U << cp0->count_per_op_denom_pot) - 1;
        count >>= cp0->count_per_op_denom_pot;
    }

    cp0_regs[CP0_COUNT_REG] += count;
    *cycle_count += count;
    cp0->last_addr = pc;
}

static void wasm_tlb_unmap(struct tlb* tlb, size_t entry)
{
    unsigned int i;
    const struct tlb_entry* e = &tlb->entries[entry];

    if (e->v_even)
    {
        for (i=e->start_even; i<e->end_even; i += 0x1000)
            tlb->LUT_r[i>>12] = 0;
        if (e->d_even)
            for (i=e->start_even; i<e->end_even; i += 0x1000)
                tlb->LUT_w[i>>12] = 0;
    }

    if (e->v_odd)
    {
        for (i=e->start_odd; i<e->end_odd; i += 0x1000)
            tlb->LUT_r[i>>12] = 0;
        if (e->d_odd)
            for (i=e->start_odd; i<e->end_odd; i += 0x1000)
                tlb->LUT_w[i>>12] = 0;
    }
}

static void wasm_tlb_map(struct tlb* tlb, size_t entry)
{
    unsigned int i;
    const struct tlb_entry* e = &tlb->entries[entry];

    if (e->v_even)
    {
        if (e->start_even < e->end_even &&
            !(e->start_even >= 0x80000000 && e->end_even < 0xC0000000) &&
            e->phys_even < 0x20000000)
        {
            for (i=e->start_even;i<e->end_even;i+=0x1000)
                tlb->LUT_r[i>>12] = UINT32_C(0x80000000) | (e->phys_even + (i - e->start_even) + 0xFFF);
            if (e->d_even)
                for (i=e->start_even;i<e->end_even;i+=0x1000)
                    tlb->LUT_w[i>>12] = UINT32_C(0x80000000) | (e->phys_even + (i - e->start_even) + 0xFFF);
        }
    }

    if (e->v_odd)
    {
        if (e->start_odd < e->end_odd &&
            !(e->start_odd >= 0x80000000 && e->end_odd < 0xC0000000) &&
            e->phys_odd < 0x20000000)
        {
            for (i=e->start_odd;i<e->end_odd;i+=0x1000)
                tlb->LUT_r[i>>12] = UINT32_C(0x80000000) | (e->phys_odd + (i - e->start_odd) + 0xFFF);
            if (e->d_odd)
                for (i=e->start_odd;i<e->end_odd;i+=0x1000)
                    tlb->LUT_w[i>>12] = UINT32_C(0x80000000) | (e->phys_odd + (i - e->start_odd) + 0xFFF);
        }
    }
}

static void wasm_tlb_write(struct r4300_core *r4300, unsigned int idx)
{
    uint32_t *cp0_regs = r4300->new_dynarec_hot_state.cp0_regs;

    wasm_tlb_unmap(&r4300->cp0.tlb, idx);

    r4300->cp0.tlb.entries[idx].g = (cp0_regs[CP0_ENTRYLO0_REG] & cp0_regs[CP0_ENTRYLO1_REG] & 1);
    r4300->cp0.tlb.entries[idx].pfn_even = (cp0_regs[CP0_ENTRYLO0_REG] & UINT32_C(0x3FFFFFC0)) >> 6;
    r4300->cp0.tlb.entries[idx].pfn_odd  = (cp0_regs[CP0_ENTRYLO1_REG] & UINT32_C(0x3FFFFFC0)) >> 6;
    r4300->cp0.tlb.entries[idx].c_even   = (cp0_regs[CP0_ENTRYLO0_REG] & UINT32_C(0x38)) >> 3;
    r4300->cp0.tlb.entries[idx].c_odd    = (cp0_regs[CP0_ENTRYLO1_REG] & UINT32_C(0x38)) >> 3;
    r4300->cp0.tlb.entries[idx].d_even   = (cp0_regs[CP0_ENTRYLO0_REG] & UINT32_C(0x4)) >> 2;
    r4300->cp0.tlb.entries[idx].d_odd    = (cp0_regs[CP0_ENTRYLO1_REG] & UINT32_C(0x4)) >> 2;
    r4300->cp0.tlb.entries[idx].v_even   = (cp0_regs[CP0_ENTRYLO0_REG] & UINT32_C(0x2)) >> 1;
    r4300->cp0.tlb.entries[idx].v_odd    = (cp0_regs[CP0_ENTRYLO1_REG] & UINT32_C(0x2)) >> 1;
    r4300->cp0.tlb.entries[idx].asid     = (cp0_regs[CP0_ENTRYHI_REG] & UINT32_C(0xFF));
    r4300->cp0.tlb.entries[idx].vpn2     = (cp0_regs[CP0_ENTRYHI_REG] & UINT32_C(0xFFFFE000)) >> 13;
    r4300->cp0.tlb.entries[idx].mask     = (cp0_regs[CP0_PAGEMASK_REG] & UINT32_C(0x1FFE000)) >> 13;

    r4300->cp0.tlb.entries[idx].start_even = r4300->cp0.tlb.entries[idx].vpn2 << 13;
    r4300->cp0.tlb.entries[idx].end_even = r4300->cp0.tlb.entries[idx].start_even +
        (r4300->cp0.tlb.entries[idx].mask << 12) + UINT32_C(0xFFF);
    r4300->cp0.tlb.entries[idx].phys_even = r4300->cp0.tlb.entries[idx].pfn_even << 12;

    r4300->cp0.tlb.entries[idx].start_odd = r4300->cp0.tlb.entries[idx].end_even + 1;
    r4300->cp0.tlb.entries[idx].end_odd = r4300->cp0.tlb.entries[idx].start_odd +
        (r4300->cp0.tlb.entries[idx].mask << 12) + UINT32_C(0xFFF);
    r4300->cp0.tlb.entries[idx].phys_odd = r4300->cp0.tlb.entries[idx].pfn_odd << 12;

    wasm_tlb_map(&r4300->cp0.tlb, idx);
}

static void wasm_cp0_update_count(struct r4300_core *r4300)
{
    struct cp0 *cp0 = &r4300->cp0;
    uint32_t *cp0_regs = r4300->new_dynarec_hot_state.cp0_regs;
    uint32_t pc = r4300->new_dynarec_hot_state.pcaddr;
    uint32_t count = ((pc - cp0->last_addr) >> 2) * cp0->count_per_op;
    if (cp0->count_per_op_denom_pot) {
        count += (1U << cp0->count_per_op_denom_pot) - 1;
        count >>= cp0->count_per_op_denom_pot;
    }
    cp0_regs[CP0_COUNT_REG] += count;
    r4300->new_dynarec_hot_state.cycle_count += count;
    cp0->last_addr = pc;
}

static uint32_t wasm_cp0_read32(struct r4300_core *r4300, unsigned int reg)
{
    uint32_t *cp0_regs = r4300->new_dynarec_hot_state.cp0_regs;
    uint64_t *cp0_latch = &r4300->new_dynarec_hot_state.cp0_latch;

    switch (reg) {
    case CP0_RANDOM_REG:
        wasm_cp0_update_count(r4300);
        cp0_regs[CP0_RANDOM_REG] =
            (cp0_regs[CP0_COUNT_REG] / r4300->cp0.count_per_op %
             (32 - cp0_regs[CP0_WIRED_REG])) + cp0_regs[CP0_WIRED_REG];
        return cp0_regs[reg];
    case CP0_COUNT_REG:
        wasm_cp0_update_count(r4300);
        return cp0_regs[reg];
    case CP0_UNUSED_7:
    case CP0_UNUSED_21:
    case CP0_UNUSED_22:
    case CP0_UNUSED_23:
    case CP0_UNUSED_24:
    case CP0_UNUSED_25:
    case CP0_UNUSED_31:
        return (uint32_t)(*cp0_latch);
    default:
        return cp0_regs[reg];
    }
}

static void wasm_cp0_write32(struct r4300_core *r4300, unsigned int reg,
                             uint32_t value)
{
    uint32_t *cp0_regs = r4300->new_dynarec_hot_state.cp0_regs;
    int *cycle_count = &r4300->new_dynarec_hot_state.cycle_count;

    r4300->new_dynarec_hot_state.cp0_latch = value;

    switch (reg) {
    case CP0_COUNT_REG:
        wasm_cp0_update_count(r4300);
        r4300->cp0.interrupt_unsafe_state |= INTR_UNSAFE_R4300;
        if (*cycle_count >= 0)
            gen_interrupt(r4300);
        r4300->cp0.interrupt_unsafe_state &= ~INTR_UNSAFE_R4300;
        translate_event_queue(&r4300->cp0, value);
        break;
    case CP0_COMPARE_REG:
        wasm_cp0_update_count(r4300);
        remove_event(&r4300->cp0.q, COMPARE_INT);
        cp0_regs[CP0_COUNT_REG] += r4300->cp0.count_per_op;
        *cycle_count += r4300->cp0.count_per_op;
        add_interrupt_event_count(&r4300->cp0, COMPARE_INT, value);
        cp0_regs[CP0_COUNT_REG] -= r4300->cp0.count_per_op;
        *cycle_count = cp0_regs[CP0_COUNT_REG] -
                       r4300->cp0.q.first->data.count;
        cp0_regs[CP0_COMPARE_REG] = value;
        cp0_regs[CP0_CAUSE_REG] &= ~CP0_CAUSE_IP7;
        break;
    case CP0_STATUS_REG:
        value &= ~UINT32_C(0x080000);
        if ((value & CP0_STATUS_FR) !=
            (cp0_regs[CP0_STATUS_REG] & CP0_STATUS_FR))
            set_fpr_pointers(&r4300->cp1, value);
        cp0_regs[CP0_STATUS_REG] = value;
        wasm_cp0_update_count(r4300);
        r4300_check_interrupt(r4300, CP0_CAUSE_IP2,
            r4300->mi->regs[MI_INTR_REG] &
            r4300->mi->regs[MI_INTR_MASK_REG]);
        r4300->cp0.interrupt_unsafe_state |= INTR_UNSAFE_R4300;
        if (*cycle_count >= 0)
            gen_interrupt(r4300);
        r4300->cp0.interrupt_unsafe_state &= ~INTR_UNSAFE_R4300;
        break;
    default:
        cp0_regs[reg] = value;
        break;
    }
}

static void wasm_tlbp(struct r4300_core *r4300)
{
    uint32_t *cp0_regs = r4300->new_dynarec_hot_state.cp0_regs;
    cp0_regs[CP0_INDEX_REG] |= UINT32_C(0x80000000);
    for (int i = 0; i < 32; ++i) {
        if (((r4300->cp0.tlb.entries[i].vpn2 & (~r4300->cp0.tlb.entries[i].mask)) ==
                (((cp0_regs[CP0_ENTRYHI_REG] & UINT32_C(0xFFFFE000)) >> 13) & (~r4300->cp0.tlb.entries[i].mask))) &&
            (r4300->cp0.tlb.entries[i].g || r4300->cp0.tlb.entries[i].asid == (cp0_regs[CP0_ENTRYHI_REG] & UINT32_C(0xFF)))) {
            cp0_regs[CP0_INDEX_REG] = i;
            break;
        }
    }
}

static void wasm_tlbr(struct r4300_core *r4300)
{
    uint32_t *cp0_regs = r4300->new_dynarec_hot_state.cp0_regs;
    int index = cp0_regs[CP0_INDEX_REG] & UINT32_C(0x1F);
    cp0_regs[CP0_PAGEMASK_REG] = r4300->cp0.tlb.entries[index].mask << 13;
    cp0_regs[CP0_ENTRYHI_REG] = (r4300->cp0.tlb.entries[index].vpn2 << 13) | r4300->cp0.tlb.entries[index].asid;
    cp0_regs[CP0_ENTRYLO0_REG] = (r4300->cp0.tlb.entries[index].pfn_even << 6) |
        (r4300->cp0.tlb.entries[index].c_even << 3) |
        (r4300->cp0.tlb.entries[index].d_even << 2) |
        (r4300->cp0.tlb.entries[index].v_even << 1) |
        r4300->cp0.tlb.entries[index].g;
    cp0_regs[CP0_ENTRYLO1_REG] = (r4300->cp0.tlb.entries[index].pfn_odd << 6) |
        (r4300->cp0.tlb.entries[index].c_odd << 3) |
        (r4300->cp0.tlb.entries[index].d_odd << 2) |
        (r4300->cp0.tlb.entries[index].v_odd << 1) |
        r4300->cp0.tlb.entries[index].g;
}

#ifndef __EMSCRIPTEN__
m3ApiRawFunction(wasm_dynarec_dispatch_import)
{
    m3ApiGetArg(uint32_t, base);
    m3ApiGetArg(uint32_t, addr);
    (void)base;
    if (g_current_cpu) {
        g_current_cpu->new_dynarec_hot_state.pcaddr = addr;
        g_dispatch_pc = addr;
        g_dispatch_pending = 1;
    }
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

m3ApiRawFunction(wasm_dynarec_cp0_read)
{
    m3ApiReturnType(uint64_t)
    m3ApiGetArg(uint32_t, base);
    m3ApiGetArg(uint32_t, reg);
    (void)base;
    uint64_t value = 0;
    if (g_current_cpu)
        value = wasm_cp0_read32(g_current_cpu, reg);
    m3ApiReturn(value);
}

m3ApiRawFunction(wasm_dynarec_cp0_write)
{
    m3ApiGetArg(uint32_t, base);
    m3ApiGetArg(uint32_t, reg);
    m3ApiGetArg(uint64_t, value);
    (void)base;
    if (g_current_cpu)
        wasm_cp0_write32(g_current_cpu, reg, (uint32_t)value);
    m3ApiSuccess();
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

m3ApiRawFunction(wasm_dynarec_tlbp)
{
    m3ApiGetArg(uint32_t, base);
    (void)base;
    if (g_current_cpu)
        wasm_tlbp(g_current_cpu);
    m3ApiSuccess();
}

m3ApiRawFunction(wasm_dynarec_tlbr)
{
    m3ApiGetArg(uint32_t, base);
    (void)base;
    if (g_current_cpu)
        wasm_tlbr(g_current_cpu);
    m3ApiSuccess();
}

m3ApiRawFunction(wasm_dynarec_tlbwi)
{
    m3ApiGetArg(uint32_t, base);
    (void)base;
    if (g_current_cpu) {
        wasm_tlb_write(g_current_cpu, g_current_cpu->new_dynarec_hot_state.cp0_regs[CP0_INDEX_REG] & 0x3F);
        invalidate_cached_code_wasm_dynarec(g_current_cpu, 0, 0);
    }
    m3ApiSuccess();
}

m3ApiRawFunction(wasm_dynarec_tlbwr)
{
    m3ApiGetArg(uint32_t, base);
    (void)base;
    if (g_current_cpu) {
        uint32_t *cp0_regs = g_current_cpu->new_dynarec_hot_state.cp0_regs;
        wasm_cp0_update_count(g_current_cpu);
        cp0_regs[CP0_RANDOM_REG] = (cp0_regs[CP0_COUNT_REG]/g_current_cpu->cp0.count_per_op % (32 - cp0_regs[CP0_WIRED_REG])) + cp0_regs[CP0_WIRED_REG];
        wasm_tlb_write(g_current_cpu, cp0_regs[CP0_RANDOM_REG]);
        invalidate_cached_code_wasm_dynarec(g_current_cpu, 0, 0);
    }
    m3ApiSuccess();
}
#else
EMSCRIPTEN_KEEPALIVE
void wasm_dynarec_dispatch_import(uint32_t base, uint32_t addr)
{
    (void)base;
    if (g_current_cpu) {
        g_current_cpu->new_dynarec_hot_state.pcaddr = addr;
        g_dispatch_pc = addr;
        g_dispatch_pending = 1;
    }
}

EMSCRIPTEN_KEEPALIVE
uint32_t wasm_dynarec_read_word(uint32_t base, uint32_t addr)
{
    (void)base;
    DebugMessage(M64MSG_VERBOSE, "wasm_dynarec_read_word: addr=0x%08X", addr);
    uint32_t value = 0;
    if (g_current_cpu)
        r4300_read_aligned_word(g_current_cpu, addr, &value);
    return value;
}

EMSCRIPTEN_KEEPALIVE
uint64_t wasm_dynarec_read_dword(uint32_t base, uint32_t addr)
{
    (void)base;
    DebugMessage(M64MSG_VERBOSE, "wasm_dynarec_read_dword: addr=0x%08X", addr);
    uint64_t value = 0;
    if (g_current_cpu)
        r4300_read_aligned_dword(g_current_cpu, addr, &value);
    return value;
}

EMSCRIPTEN_KEEPALIVE
void wasm_dynarec_write_word(uint32_t base, uint32_t addr,
                             uint32_t value, uint32_t mask)
{
    (void)base;
    DebugMessage(M64MSG_VERBOSE, "wasm_dynarec_write_word: addr=0x%08X, value=0x%08X, mask=0x%08X", addr, value, mask);
    if (g_current_cpu)
        r4300_write_aligned_word(g_current_cpu, addr, value, mask);
}

EMSCRIPTEN_KEEPALIVE
void wasm_dynarec_write_dword(uint32_t base, uint32_t addr,
                              uint64_t value, uint64_t mask)
{
    (void)base;
    DebugMessage(M64MSG_VERBOSE, "wasm_dynarec_write_dword: addr=0x%08X, value=0x%08X, mask=0x%08X", addr, value, mask);
    if (g_current_cpu)
        r4300_write_aligned_dword(g_current_cpu, addr, value, mask);
}

EMSCRIPTEN_KEEPALIVE
void wasm_dynarec_tlbp(uint32_t base)
{
    (void)base;
    if (g_current_cpu)
        wasm_tlbp(g_current_cpu);
}

EMSCRIPTEN_KEEPALIVE
void wasm_dynarec_tlbr(uint32_t base)
{
    (void)base;
    if (g_current_cpu)
        wasm_tlbr(g_current_cpu);
}

EMSCRIPTEN_KEEPALIVE
void wasm_dynarec_tlbwi(uint32_t base)
{
    (void)base;
    if (g_current_cpu) {
        wasm_tlb_write(g_current_cpu, g_current_cpu->new_dynarec_hot_state.cp0_regs[CP0_INDEX_REG] & 0x3F);
        invalidate_cached_code_wasm_dynarec(g_current_cpu, 0, 0);
    }
}

EMSCRIPTEN_KEEPALIVE
void wasm_dynarec_tlbwr(uint32_t base)
{
    (void)base;
    if (g_current_cpu) {
        uint32_t *cp0_regs = g_current_cpu->new_dynarec_hot_state.cp0_regs;
        wasm_cp0_update_count(g_current_cpu);
        cp0_regs[CP0_RANDOM_REG] = (cp0_regs[CP0_COUNT_REG]/g_current_cpu->cp0.count_per_op % (32 - cp0_regs[CP0_WIRED_REG])) + cp0_regs[CP0_WIRED_REG];
        wasm_tlb_write(g_current_cpu, cp0_regs[CP0_RANDOM_REG]);
        invalidate_cached_code_wasm_dynarec(g_current_cpu, 0, 0);
    }
}

EMSCRIPTEN_KEEPALIVE
uint64_t wasm_dynarec_cp0_read(uint32_t base, uint32_t reg)
{
    (void)base;
    uint64_t value = 0;
    if (g_current_cpu)
        value = wasm_cp0_read32(g_current_cpu, reg);
    return value;
}

EMSCRIPTEN_KEEPALIVE
void wasm_dynarec_cp0_write(uint32_t base, uint32_t reg, uint64_t value)
{
    (void)base;
    if (g_current_cpu)
        wasm_cp0_write32(g_current_cpu, reg, (uint32_t)value);
}
#endif

static struct wasm_dynarec_block *get_block(uint32_t address)
{
    for (size_t i = 0; i < g_blocks_count; ++i)
        if (g_blocks[i].address == address)
            return &g_blocks[i];
    return NULL;
}

/* Return remaining instruction count from an existing block that
 * covers the given address, or 4 if no such block exists. */
static size_t get_remaining_count(uint32_t address)
{
    for (size_t i = 0; i < g_blocks_count; ++i) {
        uint32_t start = g_blocks[i].address;
        uint32_t end = start + g_blocks[i].iw_count * 4;
        if (address >= start && address < end)
            return g_blocks[i].iw_count - (address - start) / 4;
    }
    return 4;
}

#ifdef __EMSCRIPTEN__
EM_JS(void, wasm_dynarec_exec_js,
      (uint32_t addr, uintptr_t state_ptr, uintptr_t cp1_ptr, const char *wat,
       size_t state_size, size_t cp1_simple_off, size_t cp1_double_off),
{
  const watStr = UTF8ToString(wat);
  Module.wasmBlockCache = Module.wasmBlockCache || {};
  let block = Module.wasmBlockCache[addr];
  let instance, entry, memU8, memDV;

  const heapU8 = HEAPU8;
  const heapDV = new DataView(HEAPU8.buffer);
  const CP1_BASE = 0x8000;

  const totalPages = ((state_size + 0x8000 + 0x1000 + 65535) >>> 16);
  if (!Module._dynarecMemory) {
    Module._dynarecMemory = new WebAssembly.Memory({
      initial: totalPages,
      maximum: totalPages,
      shared: true
    });
    Module._dynarecMemoryU8 = new Uint8Array(Module._dynarecMemory.buffer);
    Module._dynarecMemoryDV = new DataView(Module._dynarecMemory.buffer);
    for (let i = 0; i < 32; i++) {
      Module._dynarecMemoryDV.setBigUint64(cp1_simple_off + i * 8, BigInt(CP1_BASE + i * 8), true);
      Module._dynarecMemoryDV.setBigUint64(cp1_double_off + i * 8, BigInt(CP1_BASE + i * 8), true);
    }
  }

  if (!block) {
    let wasmBytes;
    if (Module['wabt']) {
      const mod = Module['wabt'].parseWat('block.wat', watStr, {
        features: { threads: true }
      });
      wasmBytes = mod.toBinary({}).buffer;
      mod.destroy();
    } else if (typeof Binaryen !== 'undefined') {
      const mod = Binaryen.parseText(watStr);
      if (Binaryen._BinaryenModuleSetFeatures)
        Binaryen._BinaryenModuleSetFeatures(mod,
          Binaryen._BinaryenFeatureAtomics() |
          Binaryen._BinaryenFeatureMutableGlobals());
      wasmBytes = mod.emitBinary();
      mod.dispose();
    } else {
      console.log(watStr);
      console.error('No WAT compiler available');
      while(true) {}
    }

    const dispatch_wrapper = function(base, a) {
      Module['_wasm_dynarec_dispatch_import'](base, a);
    };

    const imports = {
      env: {
        wasm_dynarec_dispatch: dispatch_wrapper,
        mem_read32: Module['_wasm_dynarec_read_word'],
        mem_read64: Module['_wasm_dynarec_read_dword'],
        mem_write32: Module['_wasm_dynarec_write_word'],
        mem_write64: Module['_wasm_dynarec_write_dword'],
        cp0_read: Module['_wasm_dynarec_cp0_read'],
        cp0_write: Module['_wasm_dynarec_cp0_write'],
        tlbp: Module['_wasm_dynarec_tlbp'],
        tlbr: Module['_wasm_dynarec_tlbr'],
        tlbwi: Module['_wasm_dynarec_tlbwi'],
        tlbwr: Module['_wasm_dynarec_tlbwr'],
        memory: Module._dynarecMemory
      }
    };

    instance = new WebAssembly.Instance(new WebAssembly.Module(wasmBytes), imports);
    entry = instance.exports.entry;

    block = {instance, entry};
    Module.wasmBlockCache[addr] = block;
    memU8 = Module._dynarecMemoryU8;
    memDV = Module._dynarecMemoryDV;
  } else {
    ({instance, entry} = block);
    memU8 = Module._dynarecMemoryU8;
    memDV = Module._dynarecMemoryDV;
  }

  for (let i = 0; i < state_size; i++)
    memU8[i] = heapU8[state_ptr + i];

  for (let i = 0; i < 32; i++) {
    const val = heapDV.getBigUint64(cp1_ptr + i * 8, true);
    memDV.setBigUint64(CP1_BASE + i * 8, val, true);
  }

  entry(0);

  for (let i = 0; i < state_size; i++)
    heapU8[state_ptr + i] = memU8[i];

  for (let i = 0; i < 32; i++) {
    const val = memDV.getBigUint64(CP1_BASE + i * 8, true);
    heapDV.setBigUint64(cp1_ptr + i * 8, val, true);
  }
});
#endif


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

    if (emit_arithmetic_instr(buf, size, inst) ||
        emit_loadstore_instr(buf, size, inst) ||
        emit_fpu_instr(buf, size, inst))
        return;

    switch (op) {
    case 0x00:
        switch (funct) {
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
                   "    local.get $base\n"
                   "    i32.const %u\n"
                   "    call $cp0_read\n"
                   "    i32.wrap_i64\n"
                   "    i64.extend_i32_s\n"
                   "    local.set $t\n"
                   "    local.get $base\n"
                   "    local.get $t\n"
                   "    i64.store offset=%zu\n"
                   "    local.get $base\n"
                   "    local.get $t\n"
                   "    i32.wrap_i64\n"
                   "    i32.store offset=%zu\n",
                   rt, rd,
                   rd,
                   (size_t)GPR_OFFSET(rt),
                   (size_t)CP0_OFFSET(rd));
            break;
        case 0x01:
            append(buf, size,
                   "    ;; dmfc0 r%u, c%u\n"
                   "    local.get $base\n"
                   "    i32.const %u\n"
                   "    call $cp0_read\n"
                   "    local.set $t\n"
                   "    local.get $base\n"
                   "    local.get $t\n"
                   "    i64.store offset=%zu\n"
                   "    local.get $base\n"
                   "    local.get $t\n"
                   "    i32.wrap_i64\n"
                   "    i32.store offset=%zu\n",
                   rt, rd,
                   rd,
                   (size_t)GPR_OFFSET(rt),
                   (size_t)CP0_OFFSET(rd));
            break;
        case 0x04:
        case 0x05:
            append(buf, size,
                   "    ;; mtc0/dmtc0 r%u, c%u\n"
                   "    local.get $base\n"
                   "    i32.const %u\n"
                   "    local.get $base i64.load offset=%zu\n"
                   "    call $cp0_write\n"
                   "    local.get $base\n"
                   "    local.get $base i64.load offset=%zu\n"
                   "    i32.wrap_i64\n"
                   "    i32.store offset=%zu\n",
                   rt, rd,
                   rd,
                   (size_t)GPR_OFFSET(rt),
                   (size_t)GPR_OFFSET(rt),
                   (size_t)CP0_OFFSET(rd));
            break;
        case 0x10:
            switch (funct) {
            case 0x01:
                append(buf, size,
                       "    ;; tlbr\n"
                       "    local.get $base\n"
                       "    call $tlbr\n");
                break;
            case 0x02:
                append(buf, size,
                       "    ;; tlbwi\n"
                       "    local.get $base\n"
                       "    call $tlbwi\n");
                break;
            case 0x06:
                append(buf, size,
                       "    ;; tlbwr\n"
                       "    local.get $base\n"
                       "    call $tlbwr\n");
                break;
            case 0x08:
                append(buf, size,
                       "    ;; tlbp\n"
                       "    local.get $base\n"
                       "    call $tlbp\n");
                break;
            default:
                append(buf, size,
                       "    ;; unsupported TLB subop %u\n", funct);
                break;
            }
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
            case 0x08:
                append(buf, size,
                       "    ;; round.l.s f%u, f%u\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    i32.load\n"
                       "    f32.reinterpret_i32\n"
                       "    f32.nearest\n"
                       "    i64.trunc_f32_s\n"
                       "    i64.store\n",
                       fd, fs,
                       (size_t)CP1_DOUBLE_OFFSET(fd),
                       (size_t)CP1_SIMPLE_OFFSET(fs));
                break;
            case 0x09:
                append(buf, size,
                       "    ;; trunc.l.s f%u, f%u\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    i32.load\n"
                       "    f32.reinterpret_i32\n"
                       "    f32.trunc\n"
                       "    i64.trunc_f32_s\n"
                       "    i64.store\n",
                       fd, fs,
                       (size_t)CP1_DOUBLE_OFFSET(fd),
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
            case 0x0c:
                append(buf, size,
                       "    ;; round.w.s f%u, f%u\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    i32.load\n"
                       "    f32.reinterpret_i32\n"
                       "    f32.nearest\n"
                       "    i32.trunc_f32_s\n"
                       "    i32.store\n",
                       fd, fs,
                       (size_t)CP1_SIMPLE_OFFSET(fd),
                       (size_t)CP1_SIMPLE_OFFSET(fs));
                break;
            case 0x0d:
                append(buf, size,
                       "    ;; trunc.w.s f%u, f%u\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    i32.load\n"
                       "    f32.reinterpret_i32\n"
                       "    f32.trunc\n"
                       "    i32.trunc_f32_s\n"
                       "    i32.store\n",
                       fd, fs,
                       (size_t)CP1_SIMPLE_OFFSET(fd),
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
            case 0x08:
                append(buf, size,
                       "    ;; round.l.d f%u, f%u\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    i64.load\n"
                       "    f64.reinterpret_i64\n"
                       "    f64.nearest\n"
                       "    i64.trunc_f64_s\n"
                       "    i64.store\n",
                       fd, fs,
                       (size_t)CP1_DOUBLE_OFFSET(fd),
                       (size_t)CP1_DOUBLE_OFFSET(fs));
                break;
            case 0x09:
                append(buf, size,
                       "    ;; trunc.l.d f%u, f%u\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    i64.load\n"
                       "    f64.reinterpret_i64\n"
                       "    f64.trunc\n"
                       "    i64.trunc_f64_s\n"
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
            case 0x0c:
                append(buf, size,
                       "    ;; round.w.d f%u, f%u\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    i64.load\n"
                       "    f64.reinterpret_i64\n"
                       "    f64.nearest\n"
                       "    i32.trunc_f64_s\n"
                       "    i32.store\n",
                       fd, fs,
                       (size_t)CP1_SIMPLE_OFFSET(fd),
                       (size_t)CP1_DOUBLE_OFFSET(fs));
                break;
            case 0x0d:
                append(buf, size,
                       "    ;; trunc.w.d f%u, f%u\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    local.get $base i64.load offset=%zu\n"
                       "    i32.wrap_i64\n"
                       "    i64.load\n"
                       "    f64.reinterpret_i64\n"
                       "    f64.trunc\n"
                       "    i32.trunc_f64_s\n"
                       "    i32.store\n",
                       fd, fs,
                       (size_t)CP1_SIMPLE_OFFSET(fd),
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
    }
}

void wasm_dynarec_init(struct r4300_core *r4300)
{
    g_blocks = NULL;
    g_blocks_count = 0;


#ifdef NEW_DYNAREC
    r4300->new_dynarec_hot_state.pc =
        &r4300->new_dynarec_hot_state.fake_pc;
    r4300->new_dynarec_hot_state.fake_pc.f.r.rs =
        &r4300->new_dynarec_hot_state.rs;
    r4300->new_dynarec_hot_state.fake_pc.f.r.rt =
        &r4300->new_dynarec_hot_state.rt;
    r4300->new_dynarec_hot_state.fake_pc.f.r.rd =
        &r4300->new_dynarec_hot_state.rd;
#endif

    r4300->new_dynarec_hot_state.pcaddr = 0xa4000040;

    DebugMessage(M64MSG_INFO, "Initializing experimental WebAssembly dynarec");
}

void invalidate_cached_code_wasm_dynarec(struct r4300_core *r4300,
                                         uint32_t address, size_t size)
{
    (void)r4300;

    if (g_blocks_count == 0)
        return;

    if (size == 0) {
        for (size_t i = 0; i < g_blocks_count; ++i) {
            free(g_blocks[i].wat);
            g_blocks[i].wat = NULL;
            g_blocks[i].wat_size = 0;
        }
        return;
    }

    uint32_t end = address + size - 1;
    for (size_t i = 0; i < g_blocks_count; ++i) {
        uint32_t addr = g_blocks[i].address;
        if (addr >= address && addr <= end) {
            free(g_blocks[i].wat);
            g_blocks[i].wat = NULL;
            g_blocks[i].wat_size = 0;
        }
    }
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

    size_t target_capacity = 32;
    uint32_t *targets = malloc(target_capacity * sizeof(uint32_t));
    size_t target_count = 0;

    DebugMessage(M64MSG_INFO, "Recompiling WebAssembly dynarec block at address %08x with %zu instructions", address, count);
    
    struct wasm_dynarec_block *block = get_block(address);
    if (block) {
        free(block->wat);
        block->wat = NULL;
        block->wat_size = 0;
        block->iw_count = count;
        block->idle = 0;
    } else {
        g_blocks = realloc(g_blocks, sizeof(*g_blocks) * (g_blocks_count + 1));
        block = &g_blocks[g_blocks_count++];
        block->address = address;
        block->wat = NULL;
        block->wat_size = 0;
        block->mem_pages = 0;
        block->iw_count = count;
        block->idle = 0;
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
           "  (import \"env\" \"cp0_read\" (func $cp0_read (param i32 i32) (result i64)))\n"
           "  (import \"env\" \"cp0_write\" (func $cp0_write (param i32 i32 i64)))\n"
           "  (import \"env\" \"tlbp\" (func $tlbp (param i32)))\n"
           "  (import \"env\" \"tlbr\" (func $tlbr (param i32)))\n"
           "  (import \"env\" \"tlbwi\" (func $tlbwi (param i32)))\n"
           "  (import \"env\" \"tlbwr\" (func $tlbwr (param i32)))\n"
           "  (import \"env\" \"memory\" (memory %u %u shared))\n"
           "  (func $block_%x (param $base i32) (local $t i64) (local $tmp i32)\n",
           block->mem_pages, block->mem_pages, address);

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
                append(&block->wat, &block->wat_size,
                       "    return\n");

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
            if (i == 0 && target == address && delay == 0)
                block->idle = 1;
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
                       "    call $dispatch\n"
                       "    return\n",
                       target,
                       (size_t)PC_OFFSET);
            } else {
                append(&block->wat, &block->wat_size,
                       "    local.get $base\n"
                       "    call $block_%08x\n"
                       "    return\n",
                       target);
                if (target != address) {
                    int known = 0;
                    for (size_t ti = 0; ti < target_count; ++ti)
                        if (targets[ti] == target) { known = 1; break; }
                    if (!known) {
                        if (target_count >= target_capacity) {
                            target_capacity *= 2;
                            targets = realloc(targets, target_capacity * sizeof(uint32_t));
                        }
                        targets[target_count++] = target;
                    }
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
            int32_t offset = ((int16_t)imm) << 2;
            uint32_t target = address + (i + 1) * 4 + offset;
            uint32_t fallthrough = address + (i + 2) * 4;
            uint32_t delay = (i + 1 < count) ? iw[i + 1] : 0;

            if (i == 0 && target == address && delay == 0)
                block->idle = 1;

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
            if (target < address || target >= address + count * 4) {
                append(&block->wat, &block->wat_size,
                       "      local.get $base\n      call $block_%08x\n      return\n",
                       target);
            } else {
                append(&block->wat, &block->wat_size,
                       "      i32.const %u\n"
                       "      local.set $tmp\n"
                       "      local.get $base\n"
                       "      local.get $tmp\n"
                       "      i32.store offset=%zu\n"
                       "      return\n",
                       target, (size_t)PC_OFFSET);
            }
            append(&block->wat, &block->wat_size, "    else\n");
            if (fallthrough < address || fallthrough >= address + count * 4) {
                append(&block->wat, &block->wat_size,
                       "      local.get $base\n      call $block_%08x\n      return\n",
                       fallthrough);
            }
            append(&block->wat, &block->wat_size, "    end\n");
            if (target != address) {
                int known = 0;
                for (size_t ti = 0; ti < target_count; ++ti)
                    if (targets[ti] == target) { known = 1; break; }
                if (!known) {
                    if (target_count >= target_capacity) {
                        target_capacity *= 2;
                        targets = realloc(targets, target_capacity * sizeof(uint32_t));
                    }
                    targets[target_count++] = target;
                }
            }
            if (fallthrough != address) {
                int known = 0;
                for (size_t ti = 0; ti < target_count; ++ti)
                    if (targets[ti] == fallthrough) { known = 1; break; }
                if (!known) {
                    if (target_count >= target_capacity) {
                        target_capacity *= 2;
                        targets = realloc(targets, target_capacity * sizeof(uint32_t));
                    }
                    targets[target_count++] = fallthrough;
                }
            }
            i++;
        }
            break;

        case 0x01: /* REGIMM */
        {
            uint32_t rtcode = rt;
            int32_t offset = ((int16_t)imm) << 2;
            uint32_t target = address + (i + 1) * 4 + offset;
            uint32_t fallthrough = address + (i + 2) * 4;
            uint32_t delay = (i + 1 < count) ? iw[i + 1] : 0;
            if (i == 0 && target == address && delay == 0)
                block->idle = 1;
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
                if (target < address || target >= address + count * 4) {
                    append(&block->wat, &block->wat_size,
                           "      call $block_%08x\n      return\n",
                           target);
                } else {
                    append(&block->wat, &block->wat_size,
                           "      i32.const %u\n"
                           "      local.set $tmp\n"
                           "      local.get $base\n"
                           "      local.get $tmp\n"
                           "      i32.store offset=%zu\n"
                           "      return\n",
                           target, (size_t)PC_OFFSET);
                }
                append(&block->wat, &block->wat_size, "    else\n");
            if (fallthrough < address || fallthrough >= address + count * 4) {
                append(&block->wat, &block->wat_size,
                       "      local.get $base\n      call $block_%08x\n      return\n",
                       fallthrough);
            }
                append(&block->wat, &block->wat_size, "    end\n");
                if (target != address) {
                    int known = 0;
                    for (size_t ti = 0; ti < target_count; ++ti)
                        if (targets[ti] == target) { known = 1; break; }
                    if (!known) {
                        if (target_count >= target_capacity) {
                            target_capacity *= 2;
                            targets = realloc(targets, target_capacity * sizeof(uint32_t));
                        }
                        targets[target_count++] = target;
                    }
                }
                if (fallthrough != address) {
                    int known = 0;
                    for (size_t ti = 0; ti < target_count; ++ti)
                        if (targets[ti] == fallthrough) { known = 1; break; }
                    if (!known) {
                        if (target_count >= target_capacity) {
                            target_capacity *= 2;
                            targets = realloc(targets, target_capacity * sizeof(uint32_t));
                        }
                        targets[target_count++] = fallthrough;
                    }
                }
                i++;
            }
        }
            break;

        case 0x11: /* COP1 */
        {
            if (rs == 0x08) {
                uint32_t rtcode = rt & 0x3;
                int32_t offset = ((int16_t)imm) << 2;
                uint32_t target = address + (i + 1) * 4 + offset;
                uint32_t fallthrough = address + (i + 2) * 4;
                uint32_t delay = (i + 1 < count) ? iw[i + 1] : 0;
                if (i == 0 && target == address && delay == 0)
                    block->idle = 1;

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
                if (target < address || target >= address + count * 4) {
                    append(&block->wat, &block->wat_size,
                           "      local.get $base\n      call $block_%08x\n      return\n",
                           target);
                } else {
                    append(&block->wat, &block->wat_size,
                           "      i32.const %u\n"
                           "      local.set $tmp\n"
                           "      local.get $base\n"
                           "      local.get $tmp\n"
                           "      i32.store offset=%zu\n"
                           "      return\n",
                           target, (size_t)PC_OFFSET);
                }
                append(&block->wat, &block->wat_size, "    else\n");
                if (fallthrough < address || fallthrough >= address + count * 4) {
                    append(&block->wat, &block->wat_size,
                           "      local.get $base\n      call $block_%08x\n      return\n",
                           fallthrough);
                }
                append(&block->wat, &block->wat_size, "    end\n");
                if (target != address) {
                    int known = 0;
                    for (size_t ti = 0; ti < target_count; ++ti)
                        if (targets[ti] == target) { known = 1; break; }
                    if (!known) {
                        if (target_count >= target_capacity) {
                            target_capacity *= 2;
                            targets = realloc(targets, target_capacity * sizeof(uint32_t));
                        }
                        targets[target_count++] = target;
                    }
                }
                if (fallthrough != address) {
                    int known = 0;
                    for (size_t ti = 0; ti < target_count; ++ti)
                        if (targets[ti] == fallthrough) { known = 1; break; }
                    if (!known) {
                        if (target_count >= target_capacity) {
                            target_capacity *= 2;
                            targets = realloc(targets, target_capacity * sizeof(uint32_t));
                        }
                        targets[target_count++] = fallthrough;
                    }
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

    uint32_t end_pc = address + count * 4;
    append(&block->wat, &block->wat_size,
           "    i32.const %u\n"
           "    local.set $tmp\n"
           "    local.get $base\n"
           "    local.get $tmp\n"
           "    i32.store offset=%zu\n",
           end_pc, (size_t)PC_OFFSET);

    append(&block->wat, &block->wat_size, "  )\n");

    for (size_t ti = 0; ti < target_count; ++ti) {
        append(&block->wat, &block->wat_size,
               "  (func $block_%x (param $base i32) (local $tmp i32)\n"
               "    i32.const %u\n"
               "    local.set $tmp\n"
               "    local.get $base\n"
               "    local.get $tmp\n"
               "    i32.store offset=%zu\n"
               "    local.get $base\n"
               "    local.get $tmp\n"
               "    call $dispatch\n"
               "  )\n",
               targets[ti], targets[ti], (size_t)PC_OFFSET);
    }

    append(&block->wat, &block->wat_size,
           "  (export \"entry\" (func $block_%x))\n"
           ")\n",
           address);

    free(targets);

    DebugMessage(M64MSG_INFO, "Recompiled block %08x to WebAssembly", address);
}

void wasm_dynarec_exec(struct r4300_core *r4300, uint32_t address)
{
    struct wasm_dynarec_block *block = get_block(address);
    if (!block || !block->wat) {
        DebugMessage(M64MSG_INFO, "No WebAssembly block for %08x; recompiling", address);
        const uint32_t *iw = fast_mem_access(r4300, address);
        if (!iw) {
            DebugMessage(M64MSG_WARNING, "No WebAssembly block for %08x; using interpreter", address);
            return;
        }
        size_t cnt = block ? block->iw_count : get_remaining_count(address);
        wasm_dynarec_recompile_block(r4300, iw, cnt, address);
        block = get_block(address);
        if (!block || !block->wat) {
            DebugMessage(M64MSG_WARNING, "No WebAssembly block for %08x; using interpreter", address);
            return;
        }
    }

    uint32_t start_pc = address;
    r4300->new_dynarec_hot_state.pcaddr = start_pc;

    DebugMessage(M64MSG_INFO, "Executing WebAssembly block %08x", address);
    //DebugMessage(M64MSG_VERBOSE, "\n%s", block->wat);
#ifdef __EMSCRIPTEN__
    g_current_cpu = r4300;
    wasm_dynarec_exec_js(address,
                         (uintptr_t)&r4300->new_dynarec_hot_state,
                         (uintptr_t)&r4300->cp1.regs[0].dword,
                         block->wat,
                         sizeof(struct new_dynarec_hot_state),
                         offsetof(struct new_dynarec_hot_state, cp1_regs_simple),
                         offsetof(struct new_dynarec_hot_state, cp1_regs_double));

    if (block->idle && r4300->new_dynarec_hot_state.cycle_count < 0) {
        r4300->new_dynarec_hot_state.cp0_regs[CP0_COUNT_REG] -=
            r4300->new_dynarec_hot_state.cycle_count;
        r4300->new_dynarec_hot_state.cycle_count = 0;
    }
    wasm_dynarec_update_count(r4300, start_pc);
    if (block->idle || r4300->new_dynarec_hot_state.cycle_count >= 0)
        gen_interrupt(r4300);
#else
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
    fprintf(f, "  \n)");
    fclose(f);

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "wat2wasm %s --enable-threads -o %s", wat_path, wasm_path);
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
    runtime->memory.maxPages = block->mem_pages;
    runtime->memory.pageSize = d_m3DefaultMemPageSize;
    ResizeMemory(runtime, block->mem_pages);
    IM3Module module = NULL;
    M3Result m3res = m3_ParseModule(env, &module, wasm, wasm_size);
    if (!m3res) m3res = m3_LoadModule(runtime, module);
    if (m3res) {
        if (module)
            m3_FreeModule(module);
        free(wasm);
        m3_FreeRuntime(runtime);
        m3_FreeEnvironment(env);
        return;
    }

    g_current_cpu = r4300;
    m3_LinkRawFunction(module, "env", "wasm_dynarec_dispatch", "v(ii)", wasm_dynarec_dispatch_import);
    m3_LinkRawFunction(module, "env", "mem_read32", "i(ii)", wasm_dynarec_read_word);
    m3_LinkRawFunction(module, "env", "mem_read64", "I(ii)", wasm_dynarec_read_dword);
    m3_LinkRawFunction(module, "env", "mem_write32", "v(iiii)", wasm_dynarec_write_word);
    m3_LinkRawFunction(module, "env", "mem_write64", "v(iiII)", wasm_dynarec_write_dword);
    m3_LinkRawFunction(module, "env", "cp0_read", "I(ii)", wasm_dynarec_cp0_read);
    m3_LinkRawFunction(module, "env", "cp0_write", "v(iiI)", wasm_dynarec_cp0_write);
    m3_LinkRawFunction(module, "env", "tlbp", "v(i)", wasm_dynarec_tlbp);
    m3_LinkRawFunction(module, "env", "tlbr", "v(i)", wasm_dynarec_tlbr);
    m3_LinkRawFunction(module, "env", "tlbwi", "v(i)", wasm_dynarec_tlbwi);
    m3_LinkRawFunction(module, "env", "tlbwr", "v(i)", wasm_dynarec_tlbwr);

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

    if (block->idle && r4300->new_dynarec_hot_state.cycle_count < 0) {
        r4300->new_dynarec_hot_state.cp0_regs[CP0_COUNT_REG] -=
            r4300->new_dynarec_hot_state.cycle_count;
        r4300->new_dynarec_hot_state.cycle_count = 0;
    }
    wasm_dynarec_update_count(r4300, start_pc);
    if (block->idle || r4300->new_dynarec_hot_state.cycle_count >= 0)
        gen_interrupt(r4300);

    g_current_cpu = NULL;

    m3_FreeRuntime(runtime);
    m3_FreeEnvironment(env);
    free(wasm);
    unlink(wasm_path);
    unlink(wat_path);
#endif
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

/* Simple dispatcher that recompiles and executes blocks using the new_dynarec_hot_state */
void wasm_dynarec_dispatch(struct r4300_core *r4300, uint32_t address)
{
    uint32_t pc = address;
    g_dispatch_pending = 1;
    g_dispatch_pc = address;

    while (g_dispatch_pending) {
        g_dispatch_pending = 0;

        const uint32_t *code = fast_mem_access(r4300, pc);
        if (!code)
            return;

        //DebugMessage(M64MSG_INFO, "Dispatching WebAssembly dynarec block at address %08x", pc);

        if (!get_block(pc))
            wasm_dynarec_recompile_block(r4300, code, 4, pc);

        wasm_dynarec_exec(r4300, pc);

        pc = g_dispatch_pc;
    }
}

void wasm_dynarec_entry(struct r4300_core *r4300)
{
    while (!r4300->new_dynarec_hot_state.stop)
    {
        uint32_t pc = r4300->new_dynarec_hot_state.pcaddr;
        wasm_dynarec_dispatch(r4300, pc);

        if (r4300->new_dynarec_hot_state.pending_exception)
            r4300->new_dynarec_hot_state.pending_exception = 0;
    }
}

#ifndef HAVE_GEN_INTERRUPT
__attribute__((weak)) void gen_interrupt(struct r4300_core* r4300) { (void)r4300; }
#endif

#ifndef HAVE_TRANSLATE_EVENT_QUEUE
__attribute__((weak)) void translate_event_queue(struct cp0* cp0, unsigned int base)
{ (void)cp0; (void)base; }
#endif
#ifndef HAVE_REMOVE_EVENT
__attribute__((weak)) void remove_event(struct interrupt_queue* q, int type)
{ (void)q; (void)type; }
#endif
#ifndef HAVE_ADD_INTERRUPT_EVENT_COUNT
__attribute__((weak)) void add_interrupt_event_count(struct cp0* cp0, int type, unsigned int count)
{ (void)cp0; (void)type; (void)count; }
#endif
#ifndef HAVE_SET_FPR_POINTERS
__attribute__((weak)) void set_fpr_pointers(struct cp1* cp1, uint32_t status)
{ (void)cp1; (void)status; }
#endif
#ifndef HAVE_R4300_CHECK_INTERRUPT
__attribute__((weak)) void r4300_check_interrupt(struct r4300_core* r4300, uint32_t cause_ip, int set)
{ (void)r4300; (void)cause_ip; (void)set; }
#endif
